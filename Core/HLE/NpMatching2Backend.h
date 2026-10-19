// Copyright (c) 2026- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#pragma once

#include <vector>

#include "Common/CommonTypes.h"

// The "server side" of sceNpMatching2. sceNp2.cpp deals with PSP memory, request ids and callbacks;
// a backend only answers the requests. The fake backend answers locally with one server and one world,
// which is enough to get through a game's server select. A network backend can replace it later.

struct NpMatching2ServerInfo {
	u16 serverId;
	u8 status;  // 1 = available
};

struct NpMatching2WorldInfo {
	u32 worldId;  // must not be 0
	u32 numOfLobby;
	u32 maxNumOfTotalLobbyMember;
	u32 curNumOfTotalLobbyMember;
	u32 curNumOfRoom;
	u32 curNumOfTotalRoomMember;
};

class NpMatching2Backend {
public:
	virtual ~NpMatching2Backend() {}

	virtual const char *Name() const = 0;
	// sceNpMatching2ContextStart. Returns 0 or an error code for the game.
	virtual int ContextStart(const char *communicationId) = 0;
	// sceNpMatching2GetServerIdListLocal.
	virtual std::vector<u16> GetServerIdList() = 0;
	// Request functions return 0 or an error code. The error goes to the game's request callback.
	virtual int GetServerInfo(u16 serverId, NpMatching2ServerInfo *info) = 0;
	virtual int GetWorldInfoList(u16 serverId, std::vector<NpMatching2WorldInfo> *worlds) = 0;
};

NpMatching2Backend &GetNpMatching2Backend();
