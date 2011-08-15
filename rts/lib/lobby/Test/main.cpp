/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include "TestConnection.h"

int main() {
	TestConnection con;
	con.Connect("lobby.springrts.com", 8200);
	while (true) {
		con.Run();
	}

	return 0;
}

