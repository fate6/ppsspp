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
// Two backends: the fake one answers locally (one server, one world, rooms that only hold the local
// player), the network one talks to a matching server (docs/fork/matching-protocol.md in the PSP2i
// revival repo). [Network] NpMatching2Server = host:port selects the network one.
//
// Threading: everything here runs on the emu thread. Room requests are asynchronous: the backend calls
// `done` exactly once, either right away (fake) or later from PollEvent (network).

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

struct NpMatching2IntAttr {
	u16 id;
	u32 val;
};

struct NpMatching2BinAttr {
	u16 id;
	std::vector<u8> data;
};

struct NpMatching2RoomMember {
	u16 memberId = 0;  // 1-based, unique in the room
	std::string onlineId;
	u64 joinDate = 0;  // RTC ticks
	u32 flagAttr = 0;
};

// What RoomDataInternal (CreateJoinRoom/JoinRoom) and RoomDataExternal (SearchRoom) carry.
struct NpMatching2Room {
	u16 serverId = 0;
	u32 worldId = 0;
	u64 lobbyId = 0;
	u64 roomId = 0;
	u32 maxSlot = 0;
	u32 flagAttr = 0;
	u64 passwordSlotMask = 0;
	// Internal (we're a member)
	u16 ownerMemberId = 0;
	u16 myMemberId = 0;
	std::vector<NpMatching2RoomMember> members;
	// External (search results)
	u32 curMemberNum = 0;
	std::string ownerOnlineId;
	std::vector<NpMatching2IntAttr> intAttrsExt;
	std::vector<NpMatching2BinAttr> searchBinAttrsExt;
	std::vector<NpMatching2BinAttr> binAttrsExt;
};

struct NpMatching2CreateJoinRoomParams {
	u16 serverId = 0;
	u32 worldId = 0;
	u64 lobbyId = 0;
	u32 maxSlot = 0;
	u32 flagAttr = 0;
	std::string password;  // empty = none
	std::vector<NpMatching2IntAttr> intAttrsExt;
	std::vector<NpMatching2BinAttr> binAttrsInternal;
	std::vector<NpMatching2BinAttr> searchBinAttrsExt;
	std::vector<NpMatching2BinAttr> binAttrsExt;
	std::vector<NpMatching2BinAttr> memberBinAttrsInternal;
};

struct NpMatching2JoinRoomParams {
	u64 roomId = 0;
	std::string password;
	std::vector<NpMatching2BinAttr> memberBinAttrsInternal;
};

struct NpMatching2IntFilter {
	u8 op;  // SCE_NP_MATCHING2_OPERATOR_*: 1 EQ, 2 NE, 3 LT, 4 LE, 5 GT, 6 GE
	NpMatching2IntAttr attr;
};

struct NpMatching2SearchRoomParams {
	u32 worldId = 0;
	u64 lobbyId = 0;
	u32 startIndex = 0;  // 1-based
	u32 max = 0;
	u32 flagFilter = 0;
	u32 flagAttr = 0;
	std::vector<NpMatching2IntFilter> intFilters;
};

struct NpMatching2RoomDataExternalParams {
	u64 roomId = 0;
	std::vector<NpMatching2IntAttr> intAttrsExt;
	std::vector<NpMatching2BinAttr> searchBinAttrsExt;
	std::vector<NpMatching2BinAttr> binAttrsExt;
};

struct NpMatching2RoomMessageParams {
	u64 roomId = 0;
	u8 castType = 1;  // 1 = everyone else in the room, 2 = unicast
	std::vector<u16> dst;
	std::vector<u8> payload;
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
using NpMatching2RoomDone = std::function<void(int errorCode, const NpMatching2Room &room)>;
using NpMatching2RoomsDone = std::function<void(int errorCode, const std::vector<NpMatching2Room> &rooms, u32 total)>;

class NpMatching2Backend {
public:
	virtual ~NpMatching2Backend() {}

	virtual const char *Name() const = 0;
	// sceNpMatching2ContextStart/Stop. Start returns 0 or an error code for the game; it may block briefly.
	virtual int ContextStart(const char *communicationId) = 0;
	virtual void ContextStop() {}
	// Synchronous: sceNpMatching2GetServerIdListLocal, and the server/world info requests.
	virtual std::vector<u16> GetServerIdList() = 0;
	virtual int GetServerInfo(u16 serverId, NpMatching2ServerInfo *info) = 0;
	virtual int GetWorldInfoList(u16 serverId, std::vector<NpMatching2WorldInfo> *worlds) = 0;

	// Rooms (asynchronous, see above).
	virtual void SearchRoom(const NpMatching2SearchRoomParams &params, NpMatching2RoomsDone done) = 0;
	virtual void CreateJoinRoom(const NpMatching2CreateJoinRoomParams &params, const std::string &myOnlineId, NpMatching2RoomDone done) = 0;
	virtual void JoinRoom(const NpMatching2JoinRoomParams &params, NpMatching2RoomDone done) = 0;
	virtual void LeaveRoom(u64 roomId, NpMatching2Done done) = 0;
	virtual void KickoutRoomMember(u64 roomId, u16 memberId, bool blockKick, const std::vector<u8> &optData, NpMatching2Done done) = 0;
	virtual void SetRoomDataInternal(u64 roomId, u32 flagFilter, u32 flagAttr, NpMatching2Done done) = 0;
	virtual void SetRoomDataExternal(const NpMatching2RoomDataExternalParams &params, NpMatching2Done done) = 0;
	virtual void GetRoomDataExternalList(const std::vector<u64> &roomIds, NpMatching2RoomsDone done) = 0;
	virtual void SendRoomMessage(const NpMatching2RoomMessageParams &params, NpMatching2Done done) = 0;

	// Called from the callback pump. Returns the next pushed event, if any. A network backend also delivers
	// its pending `done` calls from here.
	virtual bool PollEvent(NpMatching2Event *event) = 0;
};

// The backend selected by the config. Re-evaluated at sceNpMatching2Init.
NpMatching2Backend &GetNpMatching2Backend();
void NpMatching2SelectBackend();
