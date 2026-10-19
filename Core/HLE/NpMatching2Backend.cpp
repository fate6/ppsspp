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

#include <mutex>

#include "Common/Log.h"
#include "Common/Swap.h"
#include "Core/HLE/NpMatching2Backend.h"
#include "Core/HLE/sceNp2.h"
#include "Core/HLE/sceRtc.h"

namespace {

// Answers locally: one server, available, with one world. No other players: searches find nothing, and a
// created room only ever holds the local player, as its owner.
class FakeNpMatching2Backend : public NpMatching2Backend {
public:
	const char *Name() const override { return "fake"; }

	int ContextStart(const char *communicationId) override {
		INFO_LOG(Log::sceNet, "NpMatching2 (fake): context started for %s", communicationId);
		return 0;
	}

	std::vector<u16> GetServerIdList() override {
		return { SERVER_ID };
	}

	int GetServerInfo(u16 serverId, NpMatching2ServerInfo *info) override {
		if (serverId != SERVER_ID)
			return SCE_NP_MATCHING2_ERROR_SERVER_NOT_FOUND;
		info->serverId = serverId;
		info->status = 1;
		return 0;
	}

	int GetWorldInfoList(u16 serverId, std::vector<NpMatching2WorldInfo> *worlds) override {
		if (serverId != SERVER_ID)
			return SCE_NP_MATCHING2_ERROR_SERVER_NOT_FOUND;
		NpMatching2WorldInfo world{};
		world.worldId = WORLD_ID;
		worlds->push_back(world);
		return 0;
	}

	int SearchRoom(const NpMatching2SearchRoomParams &params, std::vector<NpMatching2Room> *rooms, u32 *total) override {
		*total = 0;
		return 0;
	}

	int CreateJoinRoom(const NpMatching2CreateJoinRoomParams &params, const std::string &myOnlineId, NpMatching2Room *room) override {
		room->serverId = params.serverId;
		room->worldId = params.worldId;
		room->lobbyId = params.lobbyId;
		room->roomId = nextRoomId_++;
		room->maxSlot = params.maxSlot;
		room->flagAttr = params.flagAttr;
		room->ownerMemberId = 1;
		room->myMemberId = 1;
		room->members = { { 1, myOnlineId, __RtcGetCurrentTick(), 0 } };
		currentRoomId_ = room->roomId;
		INFO_LOG(Log::sceNet, "NpMatching2 (fake): created room %llu in world %u, maxSlot %u", (unsigned long long)room->roomId, params.worldId, params.maxSlot);
		return 0;
	}

	int LeaveRoom(u64 roomId) override {
		if (roomId != currentRoomId_)
			return SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
		currentRoomId_ = 0;
		return 0;
	}

	int SetRoomDataInternal(u64 roomId, u32 flagFilter, u32 flagAttr) override {
		return roomId == currentRoomId_ ? 0 : SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
	}

	int SetRoomDataExternal(u64 roomId) override {
		return roomId == currentRoomId_ ? 0 : SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
	}

	int SendRoomMessage(u64 roomId) override {
		// Nobody else is in the room.
		return roomId == currentRoomId_ ? 0 : SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
	}

private:
	u64 nextRoomId_ = 1;
	u64 currentRoomId_ = 0;

	static constexpr u16 SERVER_ID = 1;
	static constexpr u32 WORLD_ID = 1;
};

}  // namespace

NpMatching2Backend &GetNpMatching2Backend() {
	static FakeNpMatching2Backend fakeBackend;
	return fakeBackend;
}
