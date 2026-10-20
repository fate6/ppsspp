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

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "Common/Data/Encoding/Base64.h"
#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Format/JSONWriter.h"
#include "Common/File/FileDescriptor.h"
#include "Common/Log.h"
#include "Common/Net/HTTPClient.h"
#include "Common/Net/Resolve.h"
#include "Common/Net/SocketCompat.h"
#include "Common/Swap.h"
#include "Common/Thread/ThreadUtil.h"
#include "Core/Config.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/MemMapHelpers.h"
#include "Core/System.h"
#include "Core/HLE/NpMatching2Backend.h"
#include "Core/HLE/sceNp.h"
#include "Core/HLE/sceNp2.h"
#include "Core/HLE/sceRtc.h"

namespace {

// ---------------------------------------------------------------------------------------------------------
// Fake: answers locally. One server, available, with one world. No other players: searches find nothing,
// and a created room only ever holds the local player, as its owner.

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

	void SearchRoom(const NpMatching2SearchRoomParams &params, NpMatching2RoomsDone done) override {
		done(0, {}, 0);
	}

	void CreateJoinRoom(const NpMatching2CreateJoinRoomParams &params, const std::string &myOnlineId, NpMatching2RoomDone done) override {
		NpMatching2Room room{};
		room.serverId = params.serverId;
		room.worldId = params.worldId;
		room.lobbyId = params.lobbyId;
		room.roomId = nextRoomId_++;
		room.maxSlot = params.maxSlot;
		room.flagAttr = params.flagAttr;
		room.ownerMemberId = 1;
		room.myMemberId = 1;
		NpMatching2RoomMember me;
		me.memberId = 1;
		me.onlineId = myOnlineId;
		me.joinDate = __RtcGetCurrentTick();
		room.members = { me };
		currentRoomId_ = room.roomId;
		INFO_LOG(Log::sceNet, "NpMatching2 (fake): created room %llu in world %u, maxSlot %u", (unsigned long long)room.roomId, params.worldId, params.maxSlot);
		done(0, room);
	}

	void JoinRoom(const NpMatching2JoinRoomParams &params, NpMatching2RoomDone done) override {
		done(SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM, {});
	}

	void LeaveRoom(u64 roomId, NpMatching2Done done) override {
		if (roomId != currentRoomId_)
			return done(SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM);
		currentRoomId_ = 0;
		done(0);
	}

	void KickoutRoomMember(u64 roomId, u16 memberId, bool blockKick, const std::vector<u8> &optData, NpMatching2Done done) override {
		// Nobody else is in the room.
		done(InRoom(roomId) < 0 ? InRoom(roomId) : SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_USER);
	}

	void SetRoomDataInternal(u64 roomId, u32 flagFilter, u32 flagAttr, NpMatching2Done done) override {
		done(InRoom(roomId));
	}

	void SetRoomDataExternal(const NpMatching2RoomDataExternalParams &params, NpMatching2Done done) override {
		done(InRoom(params.roomId));
	}

	void GetRoomDataExternalList(const std::vector<u64> &roomIds, NpMatching2RoomsDone done) override {
		done(0, {}, 0);
	}

	void SendRoomMessage(const NpMatching2RoomMessageParams &params, NpMatching2Done done) override {
		// Nobody else is in the room.
		done(InRoom(params.roomId));
	}

	bool PollEvent(NpMatching2Event *event) override {
		return false;
	}

private:
	int InRoom(u64 roomId) const {
		return roomId == currentRoomId_ ? 0 : SCE_NP_MATCHING2_SERVER_ERROR_NO_SUCH_ROOM;
	}

	static constexpr u16 SERVER_ID = 1;
	static constexpr u32 WORLD_ID = 1;
	u64 nextRoomId_ = 1;
	u64 currentRoomId_ = 0;
};

// ---------------------------------------------------------------------------------------------------------
// Network: talks to a matching server over TCP. Frames are {u32 length LE (bytes after it); u16 type;
// u16 flags; u32 seq; UTF-8 JSON body}. Responses use type | 0x8000 and echo seq; pushes use seq 0.
// See docs/fork/matching-protocol.md (PSP2i revival repo) and server/matching/protocol.py there.

enum : u16 {
	MSG_HELLO = 0x0001,
	MSG_PING = 0x0002,
	MSG_GET_USER_INFO_LIST = 0x0004,
	MSG_SET_ROOM_DATA_EXTERNAL = 0x0006,
	MSG_GET_ROOM_DATA_EXTERNAL_LIST = 0x0007,
	MSG_CREATE_JOIN_ROOM = 0x0101,
	MSG_JOIN_ROOM = 0x0102,
	MSG_LEAVE_ROOM = 0x0103,
	MSG_KICKOUT_ROOM_MEMBER = 0x0104,
	MSG_SEARCH_ROOM = 0x0106,
	MSG_SEND_ROOM_MESSAGE = 0x0108,
	MSG_SET_ROOM_DATA_INTERNAL = 0x0109,
	MSG_RESPONSE = 0x8000,
	MSG_PUSH_ROOM_EVENT = 0x9001,
	MSG_PUSH_ROOM_MESSAGE = 0x9002,
	MSG_PUSH_SIGNALING = 0x9003,
	MSG_CONNECTION_LOST = 0xFFFF,  // local only: the reader thread lost the connection
};

static const u32 MAX_FRAME = 256 * 1024;
static const double CONNECT_TIMEOUT_S = 5.0;
static const int HELLO_TIMEOUT_MS = 5000;

struct Frame {
	u16 type;
	u32 seq;
	std::string body;
};

u64 JsonNum(const json::JsonGet &g, const char *name, u64 def = 0) {
	const JsonNode *node = g.get(name, JSON_NUMBER);
	return node ? (u64)(s64)node->value.toNumber() : def;
}

std::string JsonStr(const json::JsonGet &g, const char *name) {
	const char *s = g.getStringOrNull(name);
	return s ? s : "";
}

template <typename F>
void JsonForEach(const json::JsonGet &g, const char *name, F f) {
	const JsonNode *arr = g.getArray(name);
	if (!arr)
		return;
	for (const JsonNode *item : arr->value) {
		if (item->value.getTag() == JSON_OBJECT)
			f(json::JsonGet(item->value));
	}
}

std::vector<NpMatching2IntAttr> JsonIntAttrs(const json::JsonGet &g, const char *name) {
	std::vector<NpMatching2IntAttr> attrs;
	JsonForEach(g, name, [&](const json::JsonGet &a) {
		attrs.push_back({ (u16)JsonNum(a, "id"), (u32)JsonNum(a, "val") });
	});
	return attrs;
}

std::vector<NpMatching2BinAttr> JsonBinAttrs(const json::JsonGet &g, const char *name) {
	std::vector<NpMatching2BinAttr> attrs;
	JsonForEach(g, name, [&](const json::JsonGet &a) {
		const std::string data = JsonStr(a, "data");
		attrs.push_back({ (u16)JsonNum(a, "id"), Base64Decode(data.data(), data.size()) });
	});
	return attrs;
}

NpMatching2RoomMember JsonMember(const json::JsonGet &g) {
	NpMatching2RoomMember m;
	m.memberId = (u16)JsonNum(g, "memberId");
	m.onlineId = JsonStr(g, "onlineId");
	m.joinDate = JsonNum(g, "joinDate");
	m.flagAttr = (u32)JsonNum(g, "flagAttr");
	return m;
}

NpMatching2Room JsonRoomInternal(const json::JsonGet &g) {
	NpMatching2Room room;
	room.serverId = (u16)JsonNum(g, "serverId");
	room.worldId = (u32)JsonNum(g, "worldId");
	room.lobbyId = JsonNum(g, "lobbyId");
	room.roomId = JsonNum(g, "roomId");
	room.maxSlot = (u32)JsonNum(g, "maxSlot");
	room.flagAttr = (u32)JsonNum(g, "flagAttr");
	room.passwordSlotMask = JsonNum(g, "passwordSlotMask");
	room.ownerMemberId = (u16)JsonNum(g, "ownerMemberId");
	room.myMemberId = (u16)JsonNum(g, "meMemberId");
	JsonForEach(g, "members", [&](const json::JsonGet &m) {
		room.members.push_back(JsonMember(m));
	});
	room.curMemberNum = (u32)room.members.size();
	return room;
}

NpMatching2Room JsonRoomExternal(const json::JsonGet &g) {
	NpMatching2Room room;
	room.serverId = (u16)JsonNum(g, "serverId");
	room.worldId = (u32)JsonNum(g, "worldId");
	room.lobbyId = JsonNum(g, "lobbyId");
	room.roomId = JsonNum(g, "roomId");
	room.maxSlot = (u32)JsonNum(g, "maxSlot");
	room.curMemberNum = (u32)JsonNum(g, "curMemberNum");
	room.flagAttr = (u32)JsonNum(g, "flagAttr");
	room.passwordSlotMask = JsonNum(g, "passwordSlotMask");
	if (const JsonNode *owner = g.get("owner", JSON_OBJECT))
		room.ownerOnlineId = JsonStr(json::JsonGet(owner->value), "onlineId");
	room.intAttrsExt = JsonIntAttrs(g, "intAttrsExt");
	room.searchBinAttrsExt = JsonBinAttrs(g, "searchBinAttrsExt");
	room.binAttrsExt = JsonBinAttrs(g, "binAttrsExt");
	return room;
}

void WriteU64(json::JsonWriter &w, const char *name, u64 value) {
	w.writeRaw(name, std::to_string(value));
}

void WriteIntAttrs(json::JsonWriter &w, const char *name, const std::vector<NpMatching2IntAttr> &attrs) {
	w.pushArray(name);
	for (const auto &a : attrs) {
		w.pushDict();
		w.writeUint("id", a.id);
		w.writeUint("val", a.val);
		w.pop();
	}
	w.pop();
}

void WriteBinAttrs(json::JsonWriter &w, const char *name, const std::vector<NpMatching2BinAttr> &attrs) {
	w.pushArray(name);
	for (const auto &a : attrs) {
		w.pushDict();
		w.writeUint("id", a.id);
		w.writeString("data", Base64Encode(a.data.data(), a.data.size()));
		w.pop();
	}
	w.pop();
}

class NetworkNpMatching2Backend : public NpMatching2Backend {
public:
	explicit NetworkNpMatching2Backend(const std::string &server) : server_(server) {}
	~NetworkNpMatching2Backend() override {
		Disconnect();
	}

	const std::string &Server() const { return server_; }

	const char *Name() const override { return "network"; }

	int ContextStart(const char *communicationId) override {
		if (connected_)
			return 0;
		std::string host = server_;
		int port = 12021;
		const size_t colon = server_.rfind(':');
		if (colon != std::string::npos) {
			host = server_.substr(0, colon);
			port = atoi(server_.c_str() + colon + 1);
		}

		conn_ = std::make_unique<net::Connection>(nullptr);
		if (!conn_->Resolve(host.c_str(), port, net::DNSType::IPV4) || !conn_->Connect(1, CONNECT_TIMEOUT_S)) {
			ERROR_LOG(Log::sceNet, "NpMatching2 (network): can't connect to %s", server_.c_str());
			conn_.reset();
			return SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
		}
		sock_ = (SOCKET)conn_->sock();
		fd_util::SetNonBlocking((int)sock_, false);
		int one = 1;
		setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

		helloDone_ = false;
		connected_ = true;
		reader_ = std::thread([this] { ReaderLoop(); });

		SceNpId npId{};
		NpGetNpId(&npId);
		json::JsonWriter w;
		w.begin();
		w.writeUint("proto", 1);
		w.writeString("game", g_paramSFO.GetDiscID());
		w.writeString("commId", communicationId);
		w.writeString("onlineId", std::string(npId.handle.data, strnlen(npId.handle.data, sizeof(npId.handle.data))));
		if (!g_Config.sNpLoopbackHost.empty())
			w.writeString("localAddr", g_Config.sNpLoopbackHost);
		w.end();
		if (!SendFrame(MSG_HELLO, 1, w.str())) {
			Disconnect();
			return SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
		}

		std::string hello;
		{
			std::unique_lock<std::mutex> lock(helloMutex_);
			if (!helloCv_.wait_for(lock, std::chrono::milliseconds(HELLO_TIMEOUT_MS), [this] { return helloDone_; }) || helloBody_.empty()) {
				lock.unlock();
				ERROR_LOG(Log::sceNet, "NpMatching2 (network): no Hello reply from %s", server_.c_str());
				Disconnect();
				return SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
			}
			hello = helloBody_;
		}

		json::JsonReader reader(hello.data(), hello.size());
		const json::JsonGet root = reader.root();
		if (!reader.ok() || root.get("error")) {
			ERROR_LOG(Log::sceNet, "NpMatching2 (network): Hello refused: %s", hello.c_str());
			Disconnect();
			return SCE_NP_MATCHING2_ERROR_SERVER_NOT_AVAILABLE;
		}
		servers_.clear();
		worlds_.clear();
		JsonForEach(root, "servers", [&](const json::JsonGet &s) {
			servers_.push_back({ (u16)JsonNum(s, "id"), (u8)JsonNum(s, "status") });
		});
		if (const JsonNode *worlds = root.get("worlds", JSON_OBJECT)) {
			for (const JsonNode *server : worlds->value) {
				const u16 serverId = (u16)atoi(server->key);
				json::JsonGet holder(worlds->value);
				JsonForEach(holder, server->key, [&](const json::JsonGet &wi) {
					NpMatching2WorldInfo info{};
					info.worldId = (u32)JsonNum(wi, "worldId");
					info.numOfLobby = (u32)JsonNum(wi, "lobbyNum");
					info.maxNumOfTotalLobbyMember = (u32)JsonNum(wi, "maxUserNum");
					info.curNumOfTotalLobbyMember = (u32)JsonNum(wi, "curUserNum");
					info.curNumOfRoom = (u32)JsonNum(wi, "curRoomNum");
					info.curNumOfTotalRoomMember = (u32)JsonNum(wi, "curUserNum");
					worlds_[serverId].push_back(info);
				});
			}
		}
		INFO_LOG(Log::sceNet, "NpMatching2 (network): connected to %s as user %llu, %d server(s), public address %s",
			server_.c_str(), (unsigned long long)JsonNum(root, "userId"), (int)servers_.size(), JsonStr(root, "publicAddr").c_str());
		return 0;
	}

	void ContextStop() override {
		Disconnect();
	}

	std::vector<u16> GetServerIdList() override {
		std::vector<u16> ids;
		for (const auto &s : servers_)
			ids.push_back(s.serverId);
		return ids;
	}

	int GetServerInfo(u16 serverId, NpMatching2ServerInfo *info) override {
		for (const auto &s : servers_) {
			if (s.serverId == serverId) {
				*info = s;
				return 0;
			}
		}
		return SCE_NP_MATCHING2_ERROR_SERVER_NOT_FOUND;
	}

	int GetWorldInfoList(u16 serverId, std::vector<NpMatching2WorldInfo> *worlds) override {
		auto it = worlds_.find(serverId);
		if (it == worlds_.end())
			return SCE_NP_MATCHING2_ERROR_SERVER_NOT_FOUND;
		*worlds = it->second;
		return 0;
	}

	void SearchRoom(const NpMatching2SearchRoomParams &params, NpMatching2RoomsDone done) override {
		json::JsonWriter w;
		w.begin();
		w.writeUint("worldId", params.worldId);
		WriteU64(w, "lobbyId", params.lobbyId);
		w.writeUint("startIndex", params.startIndex ? params.startIndex : 1);
		w.writeUint("max", params.max ? params.max : 20);
		w.writeUint("flagFilter", params.flagFilter);
		w.writeUint("flagAttr", params.flagAttr);
		w.pushArray("intFilters");
		for (const auto &f : params.intFilters) {
			w.pushDict();
			w.writeUint("op", f.op);
			w.writeUint("id", f.attr.id);
			w.writeUint("val", f.attr.val);
			w.pop();
		}
		w.pop();
		w.end();
		Request(MSG_SEARCH_ROOM, w.str(), [done](int err, const json::JsonGet &body) {
			std::vector<NpMatching2Room> rooms;
			if (err >= 0) {
				JsonForEach(body, "rooms", [&](const json::JsonGet &r) {
					rooms.push_back(JsonRoomExternal(r));
				});
			}
			done(err, rooms, err >= 0 ? (u32)JsonNum(body, "total") : 0);
		});
	}

	void CreateJoinRoom(const NpMatching2CreateJoinRoomParams &params, const std::string &myOnlineId, NpMatching2RoomDone done) override {
		json::JsonWriter w;
		w.begin();
		w.writeUint("serverId", params.serverId);
		w.writeUint("worldId", params.worldId);
		WriteU64(w, "lobbyId", params.lobbyId);
		w.writeUint("maxSlot", params.maxSlot);
		w.writeUint("flagAttr", params.flagAttr);
		if (!params.password.empty())
			w.writeString("password", params.password);
		WriteIntAttrs(w, "intAttrsExt", params.intAttrsExt);
		WriteBinAttrs(w, "binAttrsInternal", params.binAttrsInternal);
		WriteBinAttrs(w, "searchBinAttrsExt", params.searchBinAttrsExt);
		WriteBinAttrs(w, "binAttrsExt", params.binAttrsExt);
		WriteBinAttrs(w, "memberBinAttrsInternal", params.memberBinAttrsInternal);
		w.end();
		Request(MSG_CREATE_JOIN_ROOM, w.str(), [this, done](int err, const json::JsonGet &body) {
			RoomResponse(err, body, done);
		});
	}

	void JoinRoom(const NpMatching2JoinRoomParams &params, NpMatching2RoomDone done) override {
		json::JsonWriter w;
		w.begin();
		WriteU64(w, "roomId", params.roomId);
		if (!params.password.empty())
			w.writeString("password", params.password);
		WriteBinAttrs(w, "memberBinAttrsInternal", params.memberBinAttrsInternal);
		w.end();
		Request(MSG_JOIN_ROOM, w.str(), [this, done](int err, const json::JsonGet &body) {
			RoomResponse(err, body, done);
		});
	}

	void LeaveRoom(u64 roomId, NpMatching2Done done) override {
		json::JsonWriter w;
		w.begin();
		WriteU64(w, "roomId", roomId);
		w.end();
		Request(MSG_LEAVE_ROOM, w.str(), [this, roomId, done](int err, const json::JsonGet &) {
			if (err >= 0)
				rooms_.erase(roomId);
			done(err);
		});
	}

	void KickoutRoomMember(u64 roomId, u16 memberId, bool blockKick, const std::vector<u8> &optData, NpMatching2Done done) override {
		json::JsonWriter w;
		w.begin();
		WriteU64(w, "roomId", roomId);
		w.writeUint("memberId", memberId);
		w.writeUint("blockKick", blockKick ? 1 : 0);
		w.writeString("optData", Base64Encode(optData.data(), optData.size()));
		w.end();
		Request(MSG_KICKOUT_ROOM_MEMBER, w.str(), [done](int err, const json::JsonGet &) { done(err); });
	}

	void SetRoomDataInternal(u64 roomId, u32 flagFilter, u32 flagAttr, NpMatching2Done done) override {
		json::JsonWriter w;
		w.begin();
		WriteU64(w, "roomId", roomId);
		w.writeUint("flagFilter", flagFilter);
		w.writeUint("flagAttr", flagAttr);
		w.end();
		Request(MSG_SET_ROOM_DATA_INTERNAL, w.str(), [done](int err, const json::JsonGet &) { done(err); });
	}

	void SetRoomDataExternal(const NpMatching2RoomDataExternalParams &params, NpMatching2Done done) override {
		json::JsonWriter w;
		w.begin();
		WriteU64(w, "roomId", params.roomId);
		WriteIntAttrs(w, "intAttrsExt", params.intAttrsExt);
		WriteBinAttrs(w, "searchBinAttrsExt", params.searchBinAttrsExt);
		WriteBinAttrs(w, "binAttrsExt", params.binAttrsExt);
		w.end();
		Request(MSG_SET_ROOM_DATA_EXTERNAL, w.str(), [done](int err, const json::JsonGet &) { done(err); });
	}

	void GetRoomDataExternalList(const std::vector<u64> &roomIds, NpMatching2RoomsDone done) override {
		json::JsonWriter w;
		w.begin();
		w.pushArray("roomIds");
		for (u64 id : roomIds)
			w.writeRaw(std::to_string(id));
		w.pop();
		w.end();
		Request(MSG_GET_ROOM_DATA_EXTERNAL_LIST, w.str(), [done](int err, const json::JsonGet &body) {
			std::vector<NpMatching2Room> rooms;
			if (err >= 0) {
				JsonForEach(body, "rooms", [&](const json::JsonGet &r) {
					rooms.push_back(JsonRoomExternal(r));
				});
			}
			done(err, rooms, (u32)rooms.size());
		});
	}

	void SendRoomMessage(const NpMatching2RoomMessageParams &params, NpMatching2Done done) override {
		json::JsonWriter w;
		w.begin();
		WriteU64(w, "roomId", params.roomId);
		w.writeUint("castType", params.castType);
		w.pushArray("dst");
		for (u16 d : params.dst)
			w.writeUint(d);
		w.pop();
		w.writeString("payload", Base64Encode(params.payload.data(), params.payload.size()));
		w.end();
		Request(MSG_SEND_ROOM_MESSAGE, w.str(), [done](int err, const json::JsonGet &) { done(err); });
	}

	bool PollEvent(NpMatching2Event *event) override {
		std::deque<Frame> frames;
		{
			std::lock_guard<std::mutex> guard(inboxMutex_);
			frames.swap(inbox_);
		}
		for (Frame &frame : frames)
			HandleFrame(frame);

		if (events_.empty())
			return false;
		*event = std::move(events_.front());
		events_.pop_front();
		return true;
	}

private:
	using ResponseHandler = std::function<void(int err, const json::JsonGet &body)>;

	void RoomResponse(int err, const json::JsonGet &body, const NpMatching2RoomDone &done) {
		NpMatching2Room room;
		if (err >= 0) {
			const JsonNode *r = body.get("room", JSON_OBJECT);
			if (!r) {
				err = SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT;
			} else {
				room = JsonRoomInternal(json::JsonGet(r->value));
				rooms_.insert(room.roomId);
			}
		}
		done(err, room);
	}

	void Request(u16 type, const std::string &body, ResponseHandler handler) {
		if (!connected_) {
			handler(SCE_NP_MATCHING2_ERROR_CONNECTION_CLOSED_BY_SERVER, json::JsonGet(JsonValue(JSON_NULL)));
			return;
		}
		u32 seq = nextSeq_++;
		if (seq <= 1) {  // 1 is the Hello's
			nextSeq_ = 3;
			seq = 2;
		}
		pending_[seq] = std::move(handler);
		if (!SendFrame(type, seq, body)) {
			ResponseHandler h = std::move(pending_[seq]);
			pending_.erase(seq);
			h(SCE_NP_MATCHING2_ERROR_CONNECTION_CLOSED_BY_SERVER, json::JsonGet(JsonValue(JSON_NULL)));
		}
	}

	void HandleFrame(const Frame &frame) {
		if (frame.type == MSG_CONNECTION_LOST) {
			ConnectionLost();
			return;
		}
		json::JsonReader reader(frame.body.data(), frame.body.size());
		const json::JsonGet root = reader.root();
		if (!reader.ok()) {
			ERROR_LOG(Log::sceNet, "NpMatching2 (network): bad JSON in frame %04x", frame.type);
			return;
		}

		// Pushes (0x9001-0x9003) also have the 0x8000 bit, so check them first.
		const bool isPush = frame.type >= MSG_PUSH_ROOM_EVENT && frame.type <= MSG_PUSH_SIGNALING;
		if (!isPush && (frame.type & MSG_RESPONSE)) {
			auto it = pending_.find(frame.seq);
			if (it == pending_.end()) {
				WARN_LOG(Log::sceNet, "NpMatching2 (network): response %04x for unknown seq %u", frame.type, frame.seq);
				return;
			}
			ResponseHandler handler = std::move(it->second);
			pending_.erase(it);
			const int err = (int)(u32)JsonNum(root, "error", 0);
			if (err < 0)
				INFO_LOG(Log::sceNet, "NpMatching2 (network): request %04x failed with %08x", frame.type & ~MSG_RESPONSE, err);
			handler(err, root);
			return;
		}

		NpMatching2Event ev;
		ev.roomId = JsonNum(root, "roomId");
		switch (frame.type) {
		case MSG_PUSH_ROOM_EVENT:
			ev.kind = NpMatching2Event::Kind::Room;
			ev.event = (u16)JsonNum(root, "event");
			if (const JsonNode *m = root.get("member", JSON_OBJECT))
				ev.member = JsonMember(json::JsonGet(m->value));
			ev.prevOwner = (u16)JsonNum(root, "prevOwner");
			ev.newOwner = (u16)JsonNum(root, "newOwner");
			ev.errorCode = (s32)(u32)JsonNum(root, "errorCode");
			if (ev.event == 0x1103 || ev.event == 0x1104)  // Kickedout / RoomDestroyed
				rooms_.erase(ev.roomId);
			break;
		case MSG_PUSH_ROOM_MESSAGE: {
			ev.kind = NpMatching2Event::Kind::RoomMessage;
			ev.event = 0x2102;
			ev.srcMemberId = (u16)JsonNum(root, "src");
			ev.castType = (u8)JsonNum(root, "castType", 1);
			const std::string payload = JsonStr(root, "payload");
			ev.payload = Base64Decode(payload.data(), payload.size());
			break;
		}
		case MSG_PUSH_SIGNALING: {
			ev.kind = NpMatching2Event::Kind::Signaling;
			ev.event = (u16)JsonNum(root, "event");
			ev.memberId = (u16)JsonNum(root, "memberId");
			ev.errorCode = (s32)(u32)JsonNum(root, "errorCode");
			const std::string addr = JsonStr(root, "addr");
			in_addr in{};
			if (inet_pton(AF_INET, addr.c_str(), &in) == 1)
				ev.addr = in.s_addr;
			ev.port = htons((u16)JsonNum(root, "port"));  // the game copies it raw into sin_port
			break;
		}
		default:
			WARN_LOG(Log::sceNet, "NpMatching2 (network): unexpected frame %04x", frame.type);
			return;
		}
		events_.push_back(std::move(ev));
	}

	// Fails what's pending and tells the game its rooms are gone.
	void ConnectionLost() {
		ERROR_LOG(Log::sceNet, "NpMatching2 (network): lost the connection to %s", server_.c_str());
		std::map<u32, ResponseHandler> pending;
		pending.swap(pending_);
		for (auto &it : pending)
			it.second(SCE_NP_MATCHING2_ERROR_CONNECTION_CLOSED_BY_SERVER, json::JsonGet(JsonValue(JSON_NULL)));
		for (u64 roomId : rooms_) {
			NpMatching2Event ev;
			ev.kind = NpMatching2Event::Kind::Room;
			ev.roomId = roomId;
			ev.event = 0x1104;  // RoomDestroyed
			ev.errorCode = SCE_NP_MATCHING2_ERROR_CONNECTION_CLOSED_BY_SERVER;
			events_.push_back(ev);
		}
		rooms_.clear();
	}

	bool SendFrame(u16 type, u32 seq, const std::string &body) {
		std::string frame(12, '\0');
		const u32 length = 8 + (u32)body.size();
		const u16 flags = 0;
		memcpy(&frame[0], &length, 4);
		memcpy(&frame[4], &type, 2);
		memcpy(&frame[6], &flags, 2);
		memcpy(&frame[8], &seq, 4);
		frame += body;

		std::lock_guard<std::mutex> guard(sendMutex_);
		if (!connected_)
			return false;
		size_t sent = 0;
		while (sent < frame.size()) {
			int n = send(sock_, frame.data() + sent, (int)(frame.size() - sent), 0);
			if (n <= 0)
				return false;
			sent += n;
		}
		return true;
	}

	bool RecvAll(char *buf, size_t size) {
		size_t got = 0;
		while (got < size) {
			int n = recv(sock_, buf + got, (int)(size - got), 0);
			if (n <= 0)
				return false;
			got += n;
		}
		return true;
	}

	void ReaderLoop() {
		SetCurrentThreadName("NpMatching2Net");
		while (connected_) {
			char head[12];
			if (!RecvAll(head, 4))
				break;
			u32 length;
			memcpy(&length, head, 4);
			if (length < 8 || length > MAX_FRAME) {
				ERROR_LOG(Log::sceNet, "NpMatching2 (network): bad frame length %u", length);
				break;
			}
			std::string rest(length, '\0');
			if (!RecvAll(&rest[0], length))
				break;
			Frame frame;
			memcpy(&frame.type, &rest[0], 2);
			memcpy(&frame.seq, &rest[4], 4);
			frame.body = rest.substr(8);
			if (frame.body.empty())
				frame.body = "{}";

			if (frame.type == MSG_PING) {
				SendFrame(MSG_PING | MSG_RESPONSE, frame.seq, "{}");
			} else if (frame.type == (MSG_HELLO | MSG_RESPONSE)) {
				std::lock_guard<std::mutex> guard(helloMutex_);
				helloBody_ = frame.body;
				helloDone_ = true;
				helloCv_.notify_all();
			} else {
				std::lock_guard<std::mutex> guard(inboxMutex_);
				inbox_.push_back(std::move(frame));
			}
		}
		const bool wasConnected = connected_.exchange(false);
		{
			std::lock_guard<std::mutex> guard(helloMutex_);
			helloDone_ = true;
			helloCv_.notify_all();
		}
		if (wasConnected) {
			std::lock_guard<std::mutex> guard(inboxMutex_);
			inbox_.push_back({ MSG_CONNECTION_LOST, 0, "{}" });
		}
	}

	void Disconnect() {
		if (conn_)
			INFO_LOG(Log::sceNet, "NpMatching2 (network): disconnecting from %s", server_.c_str());
		{
			std::lock_guard<std::mutex> guard(sendMutex_);
			connected_ = false;
			if (conn_)
				shutdown(sock_, SD_BOTH);
		}
		if (reader_.joinable())
			reader_.join();
		if (conn_) {
			conn_->Disconnect();
			conn_.reset();
		}
		sock_ = INVALID_SOCKET;
		{
			std::lock_guard<std::mutex> guard(inboxMutex_);
			inbox_.clear();
		}
		// Whatever was still pending won't be answered.
		std::map<u32, ResponseHandler> pending;
		pending.swap(pending_);
		for (auto &it : pending)
			it.second(SCE_NP_MATCHING2_ERROR_CONNECTION_CLOSED_BY_SERVER, json::JsonGet(JsonValue(JSON_NULL)));
		rooms_.clear();
		events_.clear();
	}

	std::string server_;
	std::unique_ptr<net::Connection> conn_;
	SOCKET sock_ = INVALID_SOCKET;
	std::atomic<bool> connected_{ false };
	std::thread reader_;
	std::mutex sendMutex_;

	std::mutex inboxMutex_;
	std::deque<Frame> inbox_;

	std::mutex helloMutex_;
	std::condition_variable helloCv_;
	bool helloDone_ = false;
	std::string helloBody_;

	// Emu thread only.
	std::map<u32, ResponseHandler> pending_;
	u32 nextSeq_ = 2;
	std::deque<NpMatching2Event> events_;
	std::set<u64> rooms_;
	std::vector<NpMatching2ServerInfo> servers_;
	std::map<u16, std::vector<NpMatching2WorldInfo>> worlds_;
};

FakeNpMatching2Backend g_fakeBackend;
std::unique_ptr<NetworkNpMatching2Backend> g_networkBackend;
NpMatching2Backend *g_backend = &g_fakeBackend;

}  // namespace

void NpMatching2SelectBackend() {
	const std::string &server = g_Config.sNpMatching2Server;
	if (server.empty()) {
		g_networkBackend.reset();
		g_backend = &g_fakeBackend;
	} else {
		if (!g_networkBackend || g_networkBackend->Server() != server)
			g_networkBackend = std::make_unique<NetworkNpMatching2Backend>(server);
		g_backend = g_networkBackend.get();
	}
	INFO_LOG(Log::sceNet, "NpMatching2 backend: %s%s%s", g_backend->Name(), server.empty() ? "" : " ", server.c_str());
}

NpMatching2Backend &GetNpMatching2Backend() {
	return *g_backend;
}
