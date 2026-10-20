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

#include <functional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"

// The "server side" of sceNpMatching2. sceNp2.cpp deals with PSP memory, request ids and callbacks;
// a backend only answers the requests and reports what happens in the rooms (events).
//
// Threading: everything here runs on the emu thread. Room requests are asynchronous: the backend calls
// `done` exactly once, either right away (fake backend) or later from PollEvent (network backend). The fake backend answers locally with one server and one world,
// which is enough to get through a game's server select and to host a room alone. A network backend
// (a server holding shared room state) can replace it later.

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

struct NpMatching2RoomMember {
	u16 memberId;  // 1-based, unique in the room
	std::string onlineId;
	u64 joinDate;  // RTC ticks
	u32 flagAttr;
};

struct NpMatching2Room {
	u16 serverId;
	u32 worldId;
	u64 lobbyId;
	u64 roomId;
	u32 maxSlot;
	u32 flagAttr;
	u16 ownerMemberId;
	u16 myMemberId;
	std::vector<NpMatching2RoomMember> members;
};

// The parts of SceNpMatching2CreateJoinRoomRequest the backend needs.
struct NpMatching2CreateJoinRoomParams {
	u16 serverId;
	u32 worldId;
	u64 lobbyId;
	u32 maxSlot;
	u32 flagAttr;
};

struct NpMatching2SearchRoomParams {
	u32 worldId;
	u64 lobbyId;
	u32 startIndex;  // 1-based
	u32 max;
	u32 flagFilter;
	u32 flagAttr;
};

// Something the server pushes: a room event (0x1101 MemberJoined ...), a room message (0x2102) or a
// signaling event (0x5101 Dead / 0x5102 Established).
struct NpMatching2Event {
	enum class Kind { Room, RoomMessage, Signaling };
	Kind kind = Kind::Room;
	u64 roomId = 0;
	u16 event = 0;
	// Room: the member who joined/left (0x1101/0x1102), owner change (0x1105), cause and error.
	NpMatching2RoomMember member{};
	u16 prevOwner = 0, newOwner = 0;
	u8 eventCause = 0;
	s32 errorCode = 0;  // also Signaling
	// RoomMessage
	u16 srcMemberId = 0;
	u8 castType = 0;
	std::vector<u8> payload;
	// Signaling: the peer and its address (network byte order).
	u16 memberId = 0;
	u32 addr = 0;
	u16 port = 0;
};

using NpMatching2Done = std::function<void(int errorCode)>;

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

	// Rooms (asynchronous, see above). The attribute blobs and filters are not passed through yet.
	virtual void SearchRoom(const NpMatching2SearchRoomParams &params, std::function<void(int errorCode, const std::vector<NpMatching2Room> &rooms, u32 total)> done) = 0;
	virtual void CreateJoinRoom(const NpMatching2CreateJoinRoomParams &params, const std::string &myOnlineId, std::function<void(int errorCode, const NpMatching2Room &room)> done) = 0;
	virtual void LeaveRoom(u64 roomId, NpMatching2Done done) = 0;
	virtual void SetRoomDataInternal(u64 roomId, u32 flagFilter, u32 flagAttr, NpMatching2Done done) = 0;
	virtual void SetRoomDataExternal(u64 roomId, NpMatching2Done done) = 0;
	virtual void SendRoomMessage(u64 roomId, NpMatching2Done done) = 0;

	// Called from the callback pump. Returns the next pushed event, if any. A network backend also delivers
	// its pending `done` calls from here.
	virtual bool PollEvent(NpMatching2Event *event) = 0;
};

NpMatching2Backend &GetNpMatching2Backend();
