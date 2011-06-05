/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#ifndef UNSYNCED_ACTION_EXECUTOR_H
#define UNSYNCED_ACTION_EXECUTOR_H

#include "Action.h"
#include "IActionExecutor.h"

#include <string>


class UnsyncedAction
{
public:
	UnsyncedAction(const Action& action, unsigned int key, bool repeat)
		: action(action)
		, key(key)
		, repeat(repeat)
	{}

	/**
	 * Returns the action arguments.
	 */
	const std::string& GetArgs() const { return action.extra; }

	/**
	 * Returns the normalized key symbol.
	 */
	unsigned int GetKey() const { return key; }

	/**
	 * Returns whether the action is to be executed repeatedly.
	 */
	bool IsRepeat() const { return repeat; }

	const Action& GetInnerAction() const { return action; }

private:
	const Action& action;
	unsigned int key;
	bool repeat;
};


class IUnsyncedActionExecutor : public IActionExecutor<UnsyncedAction, false>
{
protected:
	IUnsyncedActionExecutor(const std::string& command, const std::string& description, bool cheatRequired = false)
		: IActionExecutor<UnsyncedAction, false>(command, description, cheatRequired)
	{}
	IUnsyncedActionExecutor(const std::string& command, bool cheatRequired = false)
		: IActionExecutor<UnsyncedAction, false>(command, "", cheatRequired)
	{}

public:
	virtual ~IUnsyncedActionExecutor() {}
};

#endif // UNSYNCED_ACTION_EXECUTOR_H