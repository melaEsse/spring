/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "System/mmgr.h"

#include "PathManager.h"
#include "PathConstants.h"
#include "PathFinder.h"
#include "PathEstimator.h"
#include "Map/MapInfo.h"
#include "Sim/Misc/GlobalSynced.h"
#include "Sim/MoveTypes/MoveDefHandler.h"
#include "Sim/MoveTypes/MoveMath/MoveMath.h"
#include "System/Log/ILog.h"
#include "System/myMath.h"
#include "System/TimeProfiler.h"

#define PM_UNCONSTRAINED_MAXRES_FALLBACK_SEARCH 0
#define PM_UNCONSTRAINED_MEDRES_FALLBACK_SEARCH 1
#define PM_UNCONSTRAINED_LOWRES_FALLBACK_SEARCH 1



CPathManager::CPathManager(): nextPathID(0)
{
	maxResPF = new CPathFinder();
	medResPE = new CPathEstimator(maxResPF,  8, "pe",  mapInfo->map.name);
	lowResPE = new CPathEstimator(maxResPF, 32, "pe2", mapInfo->map.name);

	LOG("[CPathManager] pathing data checksum: %08x", GetPathCheckSum());

	#ifdef SYNCDEBUG
	// clients may have a non-writable cache directory (which causes
	// the estimator path-file checksum to remain zero), so we can't
	// update the sync-checker with this in normal builds
	// NOTE: better to just checksum the in-memory data and broadcast
	// that instead of relying on the zip-file CRC?
	{ SyncedUint tmp(GetPathCheckSum()); }
	#endif
}

CPathManager::~CPathManager()
{
	delete lowResPE;
	delete medResPE;
	delete maxResPF;
}



/*
Help-function.
Turns a start->goal-request into a well-defined request.
*/
unsigned int CPathManager::RequestPath(
	const MoveDef* moveDef,
	const float3& startPos,
	const float3& goalPos,
	float goalRadius,
	CSolidObject* caller,
	bool synced
) {
	float3 sp(startPos); sp.ClampInBounds();
	float3 gp(goalPos); gp.ClampInBounds();

	// Create an estimator definition.
	CRangedGoalWithCircularConstraint* pfDef = new CRangedGoalWithCircularConstraint(sp, gp, goalRadius, 3.0f, 2000);

	// Make request.
	return RequestPath(moveDef, sp, gp, pfDef, caller, synced);
}

/*
Request a new multipath, store the result and return a handle-id to it.
*/
unsigned int CPathManager::RequestPath(
	const MoveDef* md,
	const float3& startPos,
	const float3& goalPos,
	CPathFinderDef* pfDef,
	CSolidObject* caller,
	bool synced
) {
	SCOPED_TIMER("PathManager::RequestPath");

	MoveDef* moveDef = moveDefHandler->moveDefs[md->pathType];

	// Creates a new multipath.
	IPath::SearchResult result = IPath::Error;
	MultiPath* newPath = new MultiPath(startPos, pfDef, moveDef);
	newPath->finalGoal = goalPos;
	newPath->caller = caller;

	if (caller) {
		caller->UnBlock();
	}

	unsigned int pathID = 0;

	// choose the PF or the PE depending on the projected 2D goal-distance
	// NOTE: this distance can be far smaller than the actual path length!
	// FIXME: Why are we taking the height difference into consideration?
	// It seems more logical to subtract goalRadius / SQUARE_SIZE here
	const float goalDist2D = pfDef->Heuristic(startPos.x / SQUARE_SIZE, startPos.z / SQUARE_SIZE) + math::fabs(goalPos.y - startPos.y) / SQUARE_SIZE;

	if (goalDist2D < DETAILED_DISTANCE) {
		result = maxResPF->GetPath(*moveDef, startPos, *pfDef, newPath->maxResPath, true, false, MAX_SEARCHED_NODES_PF >> 3, true, caller, synced);

		#if (PM_UNCONSTRAINED_MAXRES_FALLBACK_SEARCH == 1)
		// unnecessary so long as a fallback path exists within the
		// {med, low}ResPE's restricted search region (in many cases
		// where it does not, the goal position is unreachable anyway)
		pfDef->DisableConstraint(true);
		#endif

		// fallback (note that this uses the estimators as backup,
		// unconstrained PF queries are too expensive on average)
		if (result != IPath::Ok) {
			result = medResPE->GetPath(*moveDef, startPos, *pfDef, newPath->medResPath, MAX_SEARCHED_NODES_PE >> 3, synced);
		}
		if (result != IPath::Ok) {
			result = lowResPE->GetPath(*moveDef, startPos, *pfDef, newPath->lowResPath, MAX_SEARCHED_NODES_PE >> 3, synced);
		}
	} else if (goalDist2D < ESTIMATE_DISTANCE) {
		result = medResPE->GetPath(*moveDef, startPos, *pfDef, newPath->medResPath, MAX_SEARCHED_NODES_PE >> 3, synced);

		// CantGetCloser may be a false positive due to PE approximations and large goalRadius
		if (result == IPath::CantGetCloser && (startPos - goalPos).SqLength2D() > pfDef->sqGoalRadius)
			result = maxResPF->GetPath(*moveDef, startPos, *pfDef, newPath->maxResPath, true, false, MAX_SEARCHED_NODES_PF >> 3, true, caller, synced);

		#if (PM_UNCONSTRAINED_MEDRES_FALLBACK_SEARCH == 1)
		pfDef->DisableConstraint(true);
		#endif

		// fallback
		if (result != IPath::Ok) {
			result = medResPE->GetPath(*moveDef, startPos, *pfDef, newPath->medResPath, MAX_SEARCHED_NODES_PE >> 3, synced);
		}
	} else {
		result = lowResPE->GetPath(*moveDef, startPos, *pfDef, newPath->lowResPath, MAX_SEARCHED_NODES_PE >> 3, synced);

		// CantGetCloser may be a false positive due to PE approximations and large goalRadius
		if (result == IPath::CantGetCloser && (startPos - goalPos).SqLength2D() > pfDef->sqGoalRadius) {
			result = medResPE->GetPath(*moveDef, startPos, *pfDef, newPath->medResPath, MAX_SEARCHED_NODES_PE >> 3, synced);
			if (result == IPath::CantGetCloser) // Same thing again
				result = maxResPF->GetPath(*moveDef, startPos, *pfDef, newPath->maxResPath, true, false, MAX_SEARCHED_NODES_PF >> 3, true, caller, synced);
		}

		#if (PM_UNCONSTRAINED_LOWRES_FALLBACK_SEARCH == 1)
		pfDef->DisableConstraint(true);
		#endif

		// fallback
		if (result != IPath::Ok) {
			result = lowResPE->GetPath(*moveDef, startPos, *pfDef, newPath->lowResPath, MAX_SEARCHED_NODES_PE >> 3, synced);
		}
	}

	if (result != IPath::Error) {
		if (result != IPath::CantGetCloser) {
			LowRes2MedRes(*newPath, startPos, caller, synced);
			MedRes2MaxRes(*newPath, startPos, caller, synced);
		} else {
			// add one dummy waypoint so that the calling MoveType
			// does not consider this request a failure, which can
			// happen when startPos is very close to goalPos
			//
			// otherwise, code relying on MoveType::progressState
			// (eg. BuilderCAI::MoveInBuildRange) would misbehave
			// (eg. reject build orders)
			if (newPath->maxResPath.path.empty()) {
				newPath->maxResPath.path.push_back(startPos);
				newPath->maxResPath.squares.push_back(int2(startPos.x / SQUARE_SIZE, startPos.z / SQUARE_SIZE));
			}
		}

		newPath->searchResult = result;
		pathID = Store(newPath);
	} else {
		delete newPath;
	}

	if (caller) {
		caller->Block();
	}

	return pathID;
}


/*
Store a new multipath into the pathmap.
*/
unsigned int CPathManager::Store(MultiPath* path)
{
	pathMap[++nextPathID] = path;
	return nextPathID;
}


// converts part of a med-res path into a high-res path
void CPathManager::MedRes2MaxRes(MultiPath& multiPath, const float3& startPos, const CSolidObject* owner, bool synced) const
{
	IPath::Path& maxResPath = multiPath.maxResPath;
	IPath::Path& medResPath = multiPath.medResPath;
	IPath::Path& lowResPath = multiPath.lowResPath;

	if (medResPath.path.empty())
		return;

	medResPath.path.pop_back();

	// Remove estimate waypoints until
	// the next one is far enough.
	while (!medResPath.path.empty() &&
		medResPath.path.back().SqDistance2D(startPos) < Square(DETAILED_DISTANCE * SQUARE_SIZE))
		medResPath.path.pop_back();

	// get the goal of the detailed search
	float3 goalPos;

	if (medResPath.path.empty()) {
		goalPos = medResPath.pathGoal;
	} else {
		goalPos = medResPath.path.back();
	}

	// define the search
	CRangedGoalWithCircularConstraint rangedGoalPFD(startPos, goalPos, 0.0f, 2.0f, 1000);

	// Perform the search.
	// If this is the final improvement of the path, then use the original goal.
	IPath::SearchResult result = IPath::Error;

	if (medResPath.path.empty() && lowResPath.path.empty()) {
		result = maxResPF->GetPath(*multiPath.moveDef, startPos, *multiPath.peDef, maxResPath, true, false, MAX_SEARCHED_NODES_PF >> 3, true, owner, synced);
	} else {
		result = maxResPF->GetPath(*multiPath.moveDef, startPos, rangedGoalPFD, maxResPath, true, false, MAX_SEARCHED_NODES_PF >> 3, true, owner, synced);
	}

	// If no refined path could be found, set goal as desired goal.
	if (result == IPath::CantGetCloser || result == IPath::Error) {
		maxResPath.pathGoal = goalPos;
	}
}

// converts part of a low-res path into a med-res path
void CPathManager::LowRes2MedRes(MultiPath& multiPath, const float3& startPos, const CSolidObject* owner, bool synced) const
{
	IPath::Path& medResPath = multiPath.medResPath;
	IPath::Path& lowResPath = multiPath.lowResPath;

	if (lowResPath.path.empty())
		return;

	lowResPath.path.pop_back();

	// Remove estimate2 waypoints until
	// the next one is far enough
	while (!lowResPath.path.empty() &&
		lowResPath.path.back().SqDistance2D(startPos) < Square(ESTIMATE_DISTANCE * SQUARE_SIZE)) {
		lowResPath.path.pop_back();
	}

	//Get the goal of the detailed search.
	float3 goalPos;

	if (lowResPath.path.empty()) {
		goalPos = lowResPath.pathGoal;
	} else {
		goalPos = lowResPath.path.back();
	}

	// define the search
	CRangedGoalWithCircularConstraint rangedGoal(startPos, goalPos, 0.0f, 2.0f, 20);

	// Perform the search.
	// If there is no estimate2 path left, use original goal.
	IPath::SearchResult result = IPath::Error;

	if (lowResPath.path.empty()) {
		result = medResPE->GetPath(*multiPath.moveDef, startPos, *multiPath.peDef, medResPath, MAX_SEARCHED_NODES_ON_REFINE, synced);
	} else {
		result = medResPE->GetPath(*multiPath.moveDef, startPos, rangedGoal, medResPath, MAX_SEARCHED_NODES_ON_REFINE, synced);
	}

	// If no refined path could be found, set goal as desired goal.
	if (result == IPath::CantGetCloser || result == IPath::Error) {
		medResPath.pathGoal = goalPos;
	}
}


/*
Removes and return the next waypoint in the multipath corresponding to given id.
*/
float3 CPathManager::NextWayPoint(
	unsigned int pathID,
	float3 callerPos,
	float minDistance,
	int numRetries,
	const CSolidObject* owner,
	bool synced
) {
	SCOPED_TIMER("PathManager::NextWayPoint");

	const float3 noPathPoint = float3(-1.0f, 0.0f, -1.0f);

	// 0 indicates a no-path id
	if (pathID == 0)
		return noPathPoint;

	if (numRetries > 4)
		return noPathPoint;

	// Find corresponding multipath.
	MultiPath* multiPath = GetMultiPath(pathID);
	if (multiPath == NULL)
		return noPathPoint;

	if (callerPos == ZeroVector) {
		if (!multiPath->maxResPath.path.empty())
			callerPos = multiPath->maxResPath.path.back();
	}

	// check if detailed path needs bettering
	if (!multiPath->medResPath.path.empty() &&
		(multiPath->medResPath.path.back().SqDistance2D(callerPos) < Square(MIN_DETAILED_DISTANCE * SQUARE_SIZE) ||
		multiPath->maxResPath.path.size() <= 2)) {

		if (!multiPath->lowResPath.path.empty() &&  // if so, check if estimated path also needs bettering
			(multiPath->lowResPath.path.back().SqDistance2D(callerPos) < Square(MIN_ESTIMATE_DISTANCE * SQUARE_SIZE) ||
			multiPath->medResPath.path.size() <= 2)) {

			LowRes2MedRes(*multiPath, callerPos, owner, synced);
		}

		if (multiPath->caller) {
			multiPath->caller->UnBlock();
		}

		MedRes2MaxRes(*multiPath, callerPos, owner, synced);

		if (multiPath->caller) {
			multiPath->caller->Block();
		}
	}

	float3 waypoint;

	do {
		// get the next waypoint from the high-res path
		//
		// if this is not possible, then either we are
		// at the goal OR the path could not reach all
		// the way to it (ie. a GoalOutOfRange result)
		// OR we are stuck on an impassable square
		if (multiPath->maxResPath.path.empty()) {
			if (multiPath->lowResPath.path.empty() && multiPath->medResPath.path.empty()) {
				if (multiPath->searchResult == IPath::Ok) {
					waypoint = multiPath->finalGoal; break;
				} else {
					// note: unreachable?
					waypoint = noPathPoint; break;
				}
			} else {
				waypoint = NextWayPoint(pathID, callerPos, minDistance, numRetries + 1, owner, synced);
				break;
			}
		} else {
			waypoint = multiPath->maxResPath.path.back();
			multiPath->maxResPath.path.pop_back();
		}
	} while (callerPos.SqDistance2D(waypoint) < Square(minDistance) && waypoint != multiPath->maxResPath.pathGoal);

	// indicate this is not a temporary waypoint
	// (the default PFS does not queue requests)
	waypoint.y = 0.0f;

	return waypoint;
}


// Delete a given multipath from the collection.
void CPathManager::DeletePath(unsigned int pathID) {
	// 0 indicate a no-path id.
	if (pathID == 0)
		return;

	const std::map<unsigned int, MultiPath*>::const_iterator pi = pathMap.find(pathID);
	if (pi == pathMap.end())
		return;
	MultiPath* multiPath = pi->second;
	pathMap.erase(pathID);
	delete multiPath;
}



// Tells estimators about changes in or on the map.
void CPathManager::TerrainChange(unsigned int x1, unsigned int z1, unsigned int x2, unsigned int z2, unsigned int /*type*/) {
	medResPE->MapChanged(x1, z1, x2, z2);
	lowResPE->MapChanged(x1, z1, x2, z2);
}



void CPathManager::Update()
{
	SCOPED_TIMER("PathManager::Update");
	maxResPF->UpdateHeatMap();
	medResPE->Update();
	lowResPE->Update();
}

// used to deposit heat on the heat-map as a unit moves along its path
void CPathManager::UpdatePath(const CSolidObject* owner, unsigned int pathID)
{
	if (pathID == 0)
		return;
	if (!owner->moveDef->heatMapping)
		return;

#ifndef USE_GML
	static std::vector<int2> points;
#else
	std::vector<int2> points;
#endif

	GetDetailedPathSquares(pathID, points);

	if (!points.empty()) {
		float scale = 1.0f / points.size();
		unsigned int i = points.size();

		for (std::vector<int2>::const_iterator it = points.begin(); it != points.end(); ++it) {
			SetHeatOnSquare(it->x, it->y, i * scale * owner->moveDef->heatProduced, owner); i--;
		}
	}
}



void CPathManager::SetHeatMappingEnabled(bool enabled) { maxResPF->SetHeatMapState(enabled); }
bool CPathManager::GetHeatMappingEnabled() { return maxResPF->GetHeatMapState(); }

void CPathManager::SetHeatOnSquare(int x, int y, int value, const CSolidObject* owner) { maxResPF->UpdateHeatValue(x, y, value, owner); }
const int CPathManager::GetHeatOnSquare(int x, int y) { return maxResPF->GetHeatValue(x, y); }



// get the waypoints in world-coordinates
void CPathManager::GetDetailedPath(unsigned pathID, std::vector<float3>& points) const
{
	points.clear();

	MultiPath* multiPath = GetMultiPath(pathID);
	if (multiPath == NULL)
		return;

	const IPath::path_list_type& maxResPoints = multiPath->maxResPath.path;

	points.reserve(maxResPoints.size());

	for (IPath::path_list_type::const_reverse_iterator pvi = maxResPoints.rbegin(); pvi != maxResPoints.rend(); ++pvi) {
		points.push_back(*pvi);
	}
}

void CPathManager::GetDetailedPathSquares(unsigned pathID, std::vector<int2>& points) const
{
	points.clear();

	MultiPath* multiPath = GetMultiPath(pathID);
	if (multiPath == NULL)
		return;

	const IPath::square_list_type& maxResSquares = multiPath->maxResPath.squares;

	points.reserve(maxResSquares.size());

	for (IPath::square_list_type::const_reverse_iterator pvi = maxResSquares.rbegin(); pvi != maxResSquares.rend(); ++pvi) {
		points.push_back(*pvi);
	}
}



void CPathManager::GetPathWayPoints(
	unsigned int pathID,
	std::vector<float3>& points,
	std::vector<int>& starts
) const {
	points.clear();
	starts.clear();

	MultiPath* multiPath = GetMultiPath(pathID);
	if (multiPath == NULL)
		return;

	const IPath::path_list_type& maxResPoints = multiPath->maxResPath.path;
	const IPath::path_list_type& medResPoints = multiPath->medResPath.path;
	const IPath::path_list_type& lowResPoints = multiPath->lowResPath.path;

	points.reserve(maxResPoints.size() + medResPoints.size() + lowResPoints.size());
	starts.reserve(3);
	starts.push_back(points.size());

	for (IPath::path_list_type::const_reverse_iterator pvi = maxResPoints.rbegin(); pvi != maxResPoints.rend(); ++pvi) {
		points.push_back(*pvi);
	}

	starts.push_back(points.size());

	for (IPath::path_list_type::const_reverse_iterator pvi = medResPoints.rbegin(); pvi != medResPoints.rend(); ++pvi) {
		points.push_back(*pvi);
	}

	starts.push_back(points.size());

	for (IPath::path_list_type::const_reverse_iterator pvi = lowResPoints.rbegin(); pvi != lowResPoints.rend(); ++pvi) {
		points.push_back(*pvi);
	}
}



boost::uint32_t CPathManager::GetPathCheckSum() const {
	return (medResPE->GetPathChecksum() + lowResPE->GetPathChecksum());
}



bool CPathManager::SetNodeExtraCost(unsigned int x, unsigned int z, float cost, bool synced) {
	if (x >= gs->mapx) { return false; }
	if (z >= gs->mapy) { return false; }

	PathNodeStateBuffer& maxResBuf = maxResPF->GetNodeStateBuffer();
	PathNodeStateBuffer& medResBuf = medResPE->GetNodeStateBuffer();
	PathNodeStateBuffer& lowResBuf = lowResPE->GetNodeStateBuffer();

	maxResBuf.SetNodeExtraCost(x, z, cost, synced);
	medResBuf.SetNodeExtraCost(x, z, cost, synced);
	lowResBuf.SetNodeExtraCost(x, z, cost, synced);
	return true;
}

bool CPathManager::SetNodeExtraCosts(const float* costs, unsigned int sizex, unsigned int sizez, bool synced) {
	if (sizex < 1 || sizex > gs->mapx) { return false; }
	if (sizez < 1 || sizez > gs->mapy) { return false; }

	PathNodeStateBuffer& maxResBuf = maxResPF->GetNodeStateBuffer();
	PathNodeStateBuffer& medResBuf = medResPE->GetNodeStateBuffer();
	PathNodeStateBuffer& lowResBuf = lowResPE->GetNodeStateBuffer();

	// make all buffers share the same cost-overlay
	maxResBuf.SetNodeExtraCosts(costs, sizex, sizez, synced);
	medResBuf.SetNodeExtraCosts(costs, sizex, sizez, synced);
	lowResBuf.SetNodeExtraCosts(costs, sizex, sizez, synced);
	return true;
}

float CPathManager::GetNodeExtraCost(unsigned int x, unsigned int z, bool synced) const {
	if (x >= gs->mapx) { return 0.0f; }
	if (z >= gs->mapy) { return 0.0f; }

	const PathNodeStateBuffer& maxResBuf = maxResPF->GetNodeStateBuffer();
	const float cost = maxResBuf.GetNodeExtraCost(x, z, synced);
	return cost;
}

const float* CPathManager::GetNodeExtraCosts(bool synced) const {
	const PathNodeStateBuffer& buf = maxResPF->GetNodeStateBuffer();
	const float* costs = buf.GetNodeExtraCosts(synced);
	return costs;
}
