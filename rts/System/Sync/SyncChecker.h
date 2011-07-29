/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef SYNCCHECKER_H
#define SYNCCHECKER_H

#ifdef SYNCCHECK

#include <assert.h>
#include <deque>
#include <vector>

#ifdef TRACE_SYNC
	#include "SyncTracer.h"
#endif

#ifdef TRACE_SYNC_HEAVY
	#include "HsiehHash.h"
#endif

/**
 * @brief sync checker class
 *
 * Lightweight sync debugger that just keeps a running checksum over all
 * assignments to synced variables.
 */
class CSyncChecker {

	public:

		static unsigned GetChecksum() { return g_checksum; }
		static void NewFrame() { g_checksum = 0xfade1eaf; }

	private:

		static unsigned g_checksum;

		static inline void Sync(void* p, unsigned size) {
			// most common cases first, make it easy for compiler to optimize for it
			// simple xor is not enough to detect multiple zeroes, e.g.
#ifdef TRACE_SYNC_HEAVY
			g_checksum = HsiehHash((char*)p, size, g_checksum);
#else
			switch(size) {
			case 1:
				g_checksum += *(unsigned char*)p;
				g_checksum ^= g_checksum << 10;
				g_checksum += g_checksum >> 1;
				break;
			case 2:
				g_checksum += *(unsigned short*)(char*)p;
				g_checksum ^= g_checksum << 11;
				g_checksum += g_checksum >> 17;
				break;
			case 4:
				g_checksum += *(unsigned int*)(char*)p;
				g_checksum ^= g_checksum << 16;
				g_checksum += g_checksum >> 11;
				break;
			default:
			{
				unsigned i = 0;
				for (; i < (size & ~3); i += 4) {
					g_checksum += *(unsigned int*)(char*)p + i;
					g_checksum ^= g_checksum << 16;
					g_checksum += g_checksum >> 11;
				}
				for (; i < size; ++i) {
					g_checksum += *(unsigned char*)p + i;
					g_checksum ^= g_checksum << 10;
					g_checksum += g_checksum >> 1;
				}
				break;
			}
			}
#endif
		}

		friend class CSyncedPrimitiveBase;
};

#endif // SYNCDEBUG

#endif // SYNCDEBUGGER_H
