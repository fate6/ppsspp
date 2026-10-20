// Copyright (c) 2022- PPSSPP Project.

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

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <StringUtils.h>
#include "Core/MemMapHelpers.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceNet.h"
#include "Core/HLE/proAdhoc.h"
#include "Core/HLE/sceNp.h"
#include "Core/HLE/sceNp2.h"
#include "Core/HLE/NpMatching2Backend.h"

bool npMatching2Inited = false;
SceNpAuthMemoryStat npMatching2MemStat = {};

std::recursive_mutex npMatching2EvtMtx;
std::deque<NpMatching2Args> npMatching2Events;
std::map<int, NpMatching2Handler> npMatching2Handlers;
//std::map<int, NpMatching2Context> npMatching2Contexts;

// Request functions take (ctxId, reqParam, optParam, &assignedReqId). optParam is
// SceNpMatching2RequestOptParam {u32 cbFunc; u32 cbArg; u32 timeout; u16 appReqId;}, and the result is
// delivered later as cbFunc(ctxId, reqId, event, errorCode, data, cbArg) (checked against PSP2i).
// A request is begun in the HLE function (reqId assigned) and finished when the backend answers.
struct NpMatching2PendingRequest {
	u32 cbFunc;  // 0 = no callback
	u32 cbArg;
	u32 ctxId;
	u32 reqId;
	u32 event;
	u64 issuedAtUs;
};

// Writes a callback's data to PSP memory right before it runs, fills in its arguments and returns the
// function to call (0 = skip). Writing late keeps bursts of events from reusing a data slot too early.
using NpMatching2CallbackBuilder = std::function<u32(u32 *args, int *argc)>;

// Request callbacks and pushed events, in order. NpMatching2ProcessEvents runs them one at a time on the
// ApctlThread pump.
struct NpMatching2Callback {
	u64 readyAtUs;  // CoreTiming time before which it must not run
	u32 reqId;      // request callbacks only (for AbortRequest), else 0
	NpMatching2CallbackBuilder build;
};
static std::deque<NpMatching2Callback> npMatching2Callbacks;
// Requests the backend hasn't answered yet, and the ones of those the game aborted.
static std::set<u32> npMatching2InFlightReqIds;
static std::set<u32> npMatching2AbortedReqIds;

// Games mark a request as pending *after* the request function returns (PSP2i: call, then state = 2,
// and the callback sets state = 1). An answer delivered in the same instant can run first, if the
// callback thread gets switched in at the syscall return; then the game overwrites "done" with "pending"
// and waits forever. A real server always takes a while, so never answer faster than this.
static const u64 NP_MATCHING2_MIN_LATENCY_US = 50000;
static u32 npMatching2NextReqId = 1;

// The room we're in (0 = none), its context, and the per-room callbacks from CreateJoinRoom/JoinRoom:
// {u32 cbFunc; u32 cbArg; ...}.
static u64 npMatching2RoomId = 0;
static u32 npMatching2RoomCtxId = 0;
static u32 npMatching2RoomEventCb = 0, npMatching2RoomEventCbArg = 0;
static u32 npMatching2RoomMessageCb = 0, npMatching2RoomMessageCbArg = 0;

// Callback data lives in PSP memory. It only has to stay valid while the callback runs, so a small ring
// of slots is enough. A SearchRoom page (20 rooms with attributes) needs several KB. TODO: savestates.
static const u32 NP_MATCHING2_DATA_SLOT_SIZE = 0x4000;
static const int NP_MATCHING2_DATA_SLOTS = 8;

// Signaling state of the other members of our room, from Established/Dead events, for
// SignalingGetConnectionStatus. Updated when an event arrives, so it's current by the time the game's
// signaling callback runs and asks.
struct NpMatching2PeerStatus {
	int status;  // 0 inactive, 1 pending, 2 active
	u32 addr;    // network byte order
	u16 port;
};
static std::map<u16, NpMatching2PeerStatus> npMatching2Peers;
static u32 npMatching2DataAddr = 0;
static int npMatching2NextDataSlot = 0;

static u32 NpMatching2AllocResponse() {
	if (!npMatching2DataAddr) {
		u32 size = NP_MATCHING2_DATA_SLOT_SIZE * NP_MATCHING2_DATA_SLOTS;
		npMatching2DataAddr = kernelMemory.Alloc(size, true, "NpMatching2Data");
		if (npMatching2DataAddr == (u32)-1) {
			npMatching2DataAddr = 0;
			return 0;
		}
	}
	const u32 addr = npMatching2DataAddr + npMatching2NextDataSlot * NP_MATCHING2_DATA_SLOT_SIZE;
	npMatching2NextDataSlot = (npMatching2NextDataSlot + 1) % NP_MATCHING2_DATA_SLOTS;
	Memory::Memset(addr, 0, NP_MATCHING2_DATA_SLOT_SIZE, "NpMatching2Data");
	return addr;
}

static void NpMatching2FreeResponses() {
	if (npMatching2DataAddr) {
		kernelMemory.Free(npMatching2DataAddr);
		npMatching2DataAddr = 0;
	}
	npMatching2NextDataSlot = 0;
}

static void NpMatching2ResetState() {
	std::lock_guard<std::recursive_mutex> guard(npMatching2EvtMtx);
	npMatching2Callbacks.clear();
	npMatching2InFlightReqIds.clear();
	npMatching2AbortedReqIds.clear();
	npMatching2RoomId = 0;
	npMatching2RoomCtxId = 0;
	npMatching2RoomEventCb = npMatching2RoomMessageCb = 0;
	npMatching2Peers.clear();
}

// Leaves the room on our side (after LeaveRoom, a kick, or the room going away).
static void NpMatching2ForgetRoom() {
	npMatching2RoomId = 0;
	npMatching2RoomEventCb = npMatching2RoomMessageCb = 0;
	npMatching2Peers.clear();
}

// ---- PSP request structs (PS3 layouts; int attr {u16 id; u32 val} = 8 bytes, bin attr {u16 id; u8 *ptr;
// u32 size} = 12 bytes, checked against PSP2i's TRoomAttrBuilder).

static const u32 NP_MATCHING2_MAX_ATTRS = 64;
static const u32 NP_MATCHING2_MAX_BIN_ATTR_SIZE = 0x1000;

static std::vector<NpMatching2IntAttr> NpMatching2ReadIntAttrs(u32 ptr, u32 num) {
	std::vector<NpMatching2IntAttr> attrs;
	if (!ptr || num > NP_MATCHING2_MAX_ATTRS || !Memory::IsValidRange(ptr, num * 8))
		return attrs;
	for (u32 i = 0; i < num; i++)
		attrs.push_back({ Memory::Read_U16(ptr + i * 8), Memory::Read_U32(ptr + i * 8 + 4) });
	return attrs;
}

static std::vector<NpMatching2BinAttr> NpMatching2ReadBinAttrs(u32 ptr, u32 num) {
	std::vector<NpMatching2BinAttr> attrs;
	if (!ptr || num > NP_MATCHING2_MAX_ATTRS || !Memory::IsValidRange(ptr, num * 12))
		return attrs;
	for (u32 i = 0; i < num; i++) {
		NpMatching2BinAttr attr;
		attr.id = Memory::Read_U16(ptr + i * 12);
		const u32 data = Memory::Read_U32(ptr + i * 12 + 4);
		const u32 size = std::min(Memory::Read_U32(ptr + i * 12 + 8), NP_MATCHING2_MAX_BIN_ATTR_SIZE);
		if (size && Memory::IsValidRange(data, size)) {
			attr.data.resize(size);
			Memory::Memcpy(attr.data.data(), data, size, "NpMatching2BinAttr");
		}
		attrs.push_back(std::move(attr));
	}
	return attrs;
}

// Room passwords: PSP2i passes a C string.
static std::string NpMatching2ReadPassword(u32 ptr) {
	if (!ptr || !Memory::IsValidAddress(ptr))
		return "";
	const char *s = Memory::GetCharPointer(ptr);
	return std::string(s, strnlen(s, 16));
}

static std::string NpMatching2MyOnlineId() {
	SceNpId npId{};
	NpGetNpId(&npId);
	return std::string(npId.handle.data, strnlen(npId.handle.data, sizeof(npId.handle.data)));
}

// Assigns the request id, writes it for the game and reads the callback from optParam.
static NpMatching2PendingRequest NpMatching2BeginRequest(int ctxId, u32 optParamPtr, u32 assignedReqIdPtr, u32 event) {
	NpMatching2PendingRequest req{};
	req.ctxId = (u32)ctxId;
	req.event = event;
	req.issuedAtUs = CoreTiming::GetGlobalTimeUs();
	// Never 0: PSP2i drops a callback whose reqId equals its "last aborted" field, which is 0 by default.
	req.reqId = npMatching2NextReqId++;
	if (npMatching2NextReqId == 0)
		npMatching2NextReqId = 1;
	if (Memory::IsValidRange(assignedReqIdPtr, 4))
		Memory::Write_U32(req.reqId, assignedReqIdPtr);

	if (Memory::IsValidRange(optParamPtr, 8)) {
		req.cbFunc = Memory::Read_U32(optParamPtr);
		req.cbArg = Memory::Read_U32(optParamPtr + 4);
	}
	if (!Memory::IsValidAddress(req.cbFunc)) {
		// Fine for fire-and-forget requests (PSP2i's SendRoomMessage and KickoutRoomMember pass no callback).
		// TODO: fall back to the context's default request callback, once we track contexts.
		DEBUG_LOG(Log::sceNet, "NpMatching2: request %d (event %04x) has no callback", req.reqId, event);
		req.cbFunc = 0;
	}
	std::lock_guard<std::recursive_mutex> guard(npMatching2EvtMtx);
	npMatching2InFlightReqIds.insert(req.reqId);
	return req;
}

// Writes the response into `data` (a zeroed slot) and returns the error code to report (it may turn a
// success into an error, e.g. when the response doesn't fit).
using NpMatching2ResponseWriter = std::function<s32(u32 data, s32 errorCode)>;

// Queues the request's callback. On success the callback gets a data slot, filled by `writer` if given,
// else zeroed; on error it gets data = 0.
static void NpMatching2FinishRequest(const NpMatching2PendingRequest &req, s32 errorCode, NpMatching2ResponseWriter writer = nullptr) {
	std::lock_guard<std::recursive_mutex> guard(npMatching2EvtMtx);
	npMatching2InFlightReqIds.erase(req.reqId);
	if (npMatching2AbortedReqIds.erase(req.reqId)) {
		INFO_LOG(Log::sceNet, "NpMatching2: request %d was aborted, dropping its answer", req.reqId);
		return;
	}
	if (!req.cbFunc)
		return;
	npMatching2Callbacks.push_back({ req.issuedAtUs + NP_MATCHING2_MIN_LATENCY_US, req.reqId, [req, errorCode, writer](u32 *args, int *argc) -> u32 {
		s32 err = errorCode;
		u32 data = 0;
		if (err >= 0) {
			data = NpMatching2AllocResponse();
			if (!data)
				err = SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY;
			else if (writer)
				err = writer(data, err);
			if (err < 0)
				data = 0;
		}
		const u32 values[6] = { req.ctxId, req.reqId, req.event, (u32)err, data, req.cbArg };
		memcpy(args, values, sizeof(values));
		*argc = 6;
		INFO_LOG(Log::sceNet, "NpMatching2 request callback %08x(ctx=%d, req=%d, event=%04x, error=%08x, data=%08x, arg=%08x)",
			req.cbFunc, req.ctxId, req.reqId, req.event, err, data, req.cbArg);
		return req.cbFunc;
	} });
}

// PSP SceNpMatching2RoomMemberDataInternal, as PSP2i reads it: next @0, SceNpId @4, onlineName ptr @0x28,
// memberId u16 @0x38. joinDate @0x30 and flagAttr @0x3C follow the PS3 order but are unverified.
static const u32 NP_MATCHING2_ROOM_MEMBER_SIZE = 0x50;
// PSP SceNpMatching2RoomDataInternal: same layout as the PS3 one (PSP2i reads roomId @0x10 and the member
// list @0x24..0x30).
static const u32 NP_MATCHING2_ROOM_DATA_INTERNAL_SIZE = 0x48;

static void NpMatching2WriteMember(u32 m, const NpMatching2RoomMember &member, bool isMe, u32 next) {
	SceNpId npId{};
	if (isMe)
		NpGetNpId(&npId);
	memset(&npId.handle, 0, sizeof(npId.handle));
	memcpy(npId.handle.data, member.onlineId.data(), std::min(member.onlineId.size(), sizeof(npId.handle.data)));
	Memory::Write_U32(next, m);
	Memory::Memcpy(m + 0x04, &npId, sizeof(npId), "NpMatching2RoomMember");
	Memory::Write_U32(m + 0x04, m + 0x28);  // onlineName -> the handle inside npId
	Memory::Write_U64(member.joinDate, m + 0x30);
	Memory::Write_U16(member.memberId, m + 0x38);
	Memory::Write_U32(member.flagAttr, m + 0x3C);
}

// Writes the room at addr, followed by its members. Returns false if it doesn't fit in size.
static bool NpMatching2WriteRoomDataInternal(u32 addr, u32 size, const NpMatching2Room &room) {
	const u32 membersAddr = addr + NP_MATCHING2_ROOM_DATA_INTERNAL_SIZE;
	if (NP_MATCHING2_ROOM_DATA_INTERNAL_SIZE + room.members.size() * NP_MATCHING2_ROOM_MEMBER_SIZE > size)
		return false;

	u32 meAddr = 0, ownerAddr = 0;
	for (size_t i = 0; i < room.members.size(); i++) {
		const NpMatching2RoomMember &member = room.members[i];
		const u32 m = membersAddr + (u32)i * NP_MATCHING2_ROOM_MEMBER_SIZE;
		NpMatching2WriteMember(m, member, member.memberId == room.myMemberId, i + 1 < room.members.size() ? m + NP_MATCHING2_ROOM_MEMBER_SIZE : 0);
		if (member.memberId == room.myMemberId)
			meAddr = m;
		if (member.memberId == room.ownerMemberId)
			ownerAddr = m;
	}

	Memory::Write_U16(room.serverId, addr + 0x00);
	Memory::Write_U32(room.worldId, addr + 0x04);
	Memory::Write_U64(room.lobbyId, addr + 0x08);
	Memory::Write_U64(room.roomId, addr + 0x10);
	Memory::Write_U64(0, addr + 0x18);  // passwordSlotMask
	Memory::Write_U32(room.maxSlot, addr + 0x20);
	Memory::Write_U32(room.members.empty() ? 0 : membersAddr, addr + 0x24);
	Memory::Write_U32((u32)room.members.size(), addr + 0x28);
	Memory::Write_U32(meAddr, addr + 0x2C);
	Memory::Write_U32(ownerAddr, addr + 0x30);
	Memory::Write_U32(0, addr + 0x34);  // roomGroup
	Memory::Write_U32(0, addr + 0x38);  // roomGroupNum
	Memory::Write_U32(room.flagAttr, addr + 0x3C);
	Memory::Write_U32(0, addr + 0x40);  // roomBinAttrInternal
	Memory::Write_U32(0, addr + 0x44);  // roomBinAttrInternalNum
	return true;
}

// Turns a pushed event into a game callback. ABIs (matching2.md section 6, a u64 roomId takes a2:a3):
//   room event   (ctxId, -, roomId, event, data, arg)                      from CreateJoinRoom/JoinRoom
//   room message (ctxId, -, roomId, srcMemberId, event, data, arg)         from CreateJoinRoom/JoinRoom
//   signaling    (ctxId, -, roomId, memberId, event, errorCode, arg)       from RegisterSignalingCallback
static void NpMatching2QueueEvent(const NpMatching2Event &ev) {
	NpMatching2CallbackBuilder build;
	switch (ev.kind) {
	case NpMatching2Event::Kind::Room:
		build = [ev](u32 *args, int *argc) -> u32 {
			if (!npMatching2RoomEventCb || ev.roomId != npMatching2RoomId)
				return 0;
			const u32 data = NpMatching2AllocResponse();
			if (!data)
				return 0;
			switch (ev.event) {
			case 0x1101:  // MemberJoined
			case 0x1102:  // MemberLeft: {member*, u8 eventCause, optData}; PSP2i reads (*data)+0x38
				NpMatching2WriteMember(data + 0x20, ev.member, false, 0);
				Memory::Write_U32(data + 0x20, data);
				Memory::Write_U8(ev.eventCause, data + 4);
				break;
			case 0x1103:  // Kickedout
			case 0x1104:  // RoomDestroyed: {u8 eventCause; s32 errorCode; optData}
				Memory::Write_U8(ev.eventCause, data);
				Memory::Write_U32((u32)ev.errorCode, data + 4);
				break;
			case 0x1105:  // RoomOwnerChanged: {u16 prevOwner; u16 newOwner; u8 eventCause; optData}
				Memory::Write_U16(ev.prevOwner, data);
				Memory::Write_U16(ev.newOwner, data + 2);
				Memory::Write_U8(ev.eventCause, data + 4);
				break;
			default:  // 0x1106-0x1108: PSP2i only logs them. TODO: layouts.
				break;
			}
			const u32 values[7] = { npMatching2RoomCtxId, 0, (u32)ev.roomId, (u32)(ev.roomId >> 32), ev.event, data, npMatching2RoomEventCbArg };
			memcpy(args, values, sizeof(values));
			*argc = 7;
			INFO_LOG(Log::sceNet, "NpMatching2 room event %04x for room %llu", ev.event, (unsigned long long)ev.roomId);
			const u32 cb = npMatching2RoomEventCb;
			if (ev.event == 0x1103 || ev.event == 0x1104)  // Kickedout / RoomDestroyed: we're out
				NpMatching2ForgetRoom();
			return cb;
		};
		break;

	case NpMatching2Event::Kind::RoomMessage:
		build = [ev](u32 *args, int *argc) -> u32 {
			if (!npMatching2RoomMessageCb || ev.roomId != npMatching2RoomId)
				return 0;
			const u32 data = NpMatching2AllocResponse();
			if (!data)
				return 0;
			// SceNpMatching2RoomMessageInfo: {bool filtered; u8 castType; pad; dst*; srcMember*; msg* @0x0C; msgLen @0x10}
			const u32 maxLen = NP_MATCHING2_DATA_SLOT_SIZE - 0x20;
			u32 len = (u32)ev.payload.size();
			if (len > maxLen) {
				WARN_LOG(Log::sceNet, "NpMatching2 room message of %u bytes truncated to %u", len, maxLen);
				len = maxLen;
			}
			Memory::Write_U8(ev.castType, data + 1);
			Memory::Write_U32(data + 0x20, data + 0x0C);
			Memory::Write_U32(len, data + 0x10);
			if (len)
				Memory::Memcpy(data + 0x20, ev.payload.data(), len, "NpMatching2RoomMessage");
			const u32 values[8] = { npMatching2RoomCtxId, 0, (u32)ev.roomId, (u32)(ev.roomId >> 32), ev.srcMemberId, 0x2102, data, npMatching2RoomMessageCbArg };
			memcpy(args, values, sizeof(values));
			*argc = 8;
			DEBUG_LOG(Log::sceNet, "NpMatching2 room message from member %d, %u bytes", ev.srcMemberId, len);
			return npMatching2RoomMessageCb;
		};
		break;

	case NpMatching2Event::Kind::Signaling:
		if (ev.roomId == npMatching2RoomId) {
			if (ev.event == 0x5102)  // Established
				npMatching2Peers[ev.memberId] = { 2, ev.addr, ev.port };
			else if (ev.event == 0x5101)  // Dead
				npMatching2Peers.erase(ev.memberId);
		}
		build = [ev](u32 *args, int *argc) -> u32 {
			if (npMatching2Handlers.empty())
				return 0;
			auto it = npMatching2Handlers.find((int)npMatching2RoomCtxId - 1);
			const NpMatching2Handler &handler = it != npMatching2Handlers.end() ? it->second : npMatching2Handlers.begin()->second;
			const u32 values[8] = { npMatching2RoomCtxId, 0, (u32)ev.roomId, (u32)(ev.roomId >> 32), ev.memberId, ev.event, (u32)ev.errorCode, handler.argument };
			memcpy(args, values, sizeof(values));
			*argc = 8;
			INFO_LOG(Log::sceNet, "NpMatching2 signaling event %04x for member %d (error %08x)", ev.event, ev.memberId, ev.errorCode);
			return handler.entryPoint;
		};
		break;
	}

	std::lock_guard<std::recursive_mutex> guard(npMatching2EvtMtx);
	npMatching2Callbacks.push_back({ CoreTiming::GetGlobalTimeUs(), 0, build });
}

// serverId: 0 on 0x0103/0x0104/0x0105/0x0107/0x0108/0x0109/0x010a/0x010b/0x010c/0x010d (ie. when already joined to a server?)
// unk1~unk5 usually 0, 
// unk1: 1st 32-bit of LeaveRoom/etc's Arg2 on 0x0103/0x0104/0x0105/0x0107/0x0108/0x0109/0x010a/0x010b/0x010c/0x010d/0x010e
// unk2: 2nd 32-bit of LeaveRoom/etc's Arg2 on 0x0103/0x0104/0x0105/0x0107/0x0108/0x0109/0x010a/0x010b/0x010c/0x010d/0x010e
// unk5: 1 on 0x0002/0x0003/0x0005/0x0006/0x0007/0x0101/0x0102/0x0106
// unk6 (new state?): 8-bit?(masked with 0xff) 0x01 on 0x0001, 0x03 on 0x0002, 0x04 on 0x0003, 0x05 on 0x0004, 0x06 on 0x0005, 0x07 on 0x0006, 0x08 on 0x0007,
//		0x09 on 0x0101, 0x0A on 0x0102, 0x0C on 0x0103, 0x0D on 0x0104, 0x0E on 0x0105, 0x0F on 0x0106, 0x10 on 0x0107, 0x11 on 0x0108,
//		0x12 on 0x0109, 0x13 on 0x010a, 0x14 on 0x010b, 0x15 on 0x010c, 0x16 on 0x010d, 0x17 on 0x010e, 0x18 on 0xa102
void notifyNpMatching2Handlers(NpMatching2Args &args, u32 ctxId, u32 serverId, u32 cbFuncAddr, u32 cbArgAddr, u32 unk3, u32 unk4, u32 unk5, u8 unk6) {
	std::lock_guard<std::recursive_mutex> npMatching2Guard(npMatching2EvtMtx);
	// TODO: separate/map each list per ctxId
	npMatching2Events.push_back(args);
}

bool NpMatching2ProcessEvents() {
	if (npMatching2Inited) {
		// Lets the backend deliver answers (FinishRequest) and hand over pushed events.
		NpMatching2Event ev;
		while (GetNpMatching2Backend().PollEvent(&ev))
			NpMatching2QueueEvent(ev);
	}

	while (!npMatching2Callbacks.empty() && CoreTiming::GetGlobalTimeUs() >= npMatching2Callbacks.front().readyAtUs) {
		NpMatching2Callback cb = std::move(npMatching2Callbacks.front());
		npMatching2Callbacks.pop_front();
		u32 args[8]{};
		int argc = 0;
		const u32 func = cb.build(args, &argc);
		if (!Memory::IsValidAddress(func))
			continue;  // nobody to tell (e.g. the room was left meanwhile)
		hleEnqueueCall(func, argc, args);
		return true;
	}

	if (npMatching2Events.empty()) {
		return false;
	}

	auto& args = npMatching2Events.front();
	auto& event = args.data[0];
	auto& stat = args.data[1];
	auto& serverIdPtr = args.data[2];
	auto& inStructPtr = args.data[3];
	auto& newStat = args.data[5];
	npMatching2Events.pop_front();

	//int handlerID = id - 1;
	for (std::map<int, NpMatching2Handler>::iterator it = npMatching2Handlers.begin(); it != npMatching2Handlers.end(); ++it) {
		//if (it->first == handlerID)
		{
			DEBUG_LOG(Log::sceNet, "NpMatching2Callback [HandlerID=%i][EventID=%04x][State=%04x][ArgsPtr=%08x]", it->first, event, stat, it->second.argument);
			hleEnqueueCall(it->second.entryPoint, 7, args.data);
		}
	}

	// Per npMatching2 function callback
	u32* inStruct = (u32*)Memory::GetPointer(inStructPtr);
	if (Memory::IsValidAddress(inStruct[0])) {
		DEBUG_LOG(Log::sceNet, "NpMatching2Callback [ServerID=%i][EventID=%04x][State=%04x][FuncAddr=%08x][ArgsPtr=%08x]", *(u32*)Memory::GetPointer(serverIdPtr), event, stat, inStruct[0], inStruct[1]);
		hleEnqueueCall(inStruct[0], 7, args.data);
	}
	return true;
}

static int sceNpMatching2Init(int poolSize, int threadPriority, int cpuAffinityMask, int threadStackSize)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d, %d, %d, %d) at %08x", __FUNCTION__, poolSize, threadPriority, cpuAffinityMask, threadStackSize, currentMIPS->pc);
	//if (npMatching2Inited)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_ALREADY_INITIALIZED);

	npMatching2MemStat.npMemSize = poolSize - 0x20;
	npMatching2MemStat.npMaxMemSize = 0x4050; // Dummy maximum foot print
	npMatching2MemStat.npFreeMemSize = npMatching2MemStat.npMemSize;

	// A new session: drop any server connection left from before (the game can re-Init without Term), so
	// the server forgets the rooms we were in.
	GetNpMatching2Backend().ContextStop();
	npMatching2Handlers.clear();
	npMatching2Events.clear();
	NpMatching2ResetState();
	NpMatching2SelectBackend();
	npMatching2Inited = true;
	return 0;
}

static int sceNpMatching2Term()
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s() at %08x", __FUNCTION__, currentMIPS->pc);
	npMatching2Inited = false;
	GetNpMatching2Backend().ContextStop();
	npMatching2Handlers.clear();
	npMatching2Events.clear();
	NpMatching2ResetState();
	NpMatching2FreeResponses();

	return 0;
}

static int sceNpMatching2CreateContext(u32 communicationIdPtr, u32 passPhrasePtr, u32 ctxIdPtr, int unknown)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%08x[%s], %08x[%08x], %08x[%hu], %i) at %08x", __FUNCTION__, communicationIdPtr, safe_string(Memory::GetCharPointer(communicationIdPtr)), passPhrasePtr, Memory::Read_U32(passPhrasePtr), ctxIdPtr, Memory::Read_U16(ctxIdPtr), unknown, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	if (!Memory::IsValidAddress(communicationIdPtr) || !Memory::IsValidAddress(passPhrasePtr) || !Memory::IsValidAddress(ctxIdPtr))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX);

	// FIXME: It seems Context are mapped to TitleID? may return 0x80550C05 or 0x80550C06 when finding an existing context
	SceNpCommunicationId* titleid = (SceNpCommunicationId*)Memory::GetCharPointer(communicationIdPtr);
	memcpy(npTitleId.data, titleid->data, sizeof(npTitleId));

	SceNpCommunicationPassphrase* passph = (SceNpCommunicationPassphrase*)Memory::GetCharPointer(passPhrasePtr);

	SceNpId npid{};
	int retval = NpGetNpId(&npid);
	if (retval < 0)
		return hleLogError(Log::sceNet, retval);

	INFO_LOG(Log::sceNet, "%s - Title ID: %s", __FUNCTION__, titleid->data);
	INFO_LOG(Log::sceNet, "%s - Online ID: %s", __FUNCTION__, npid.handle.data);
	std::string datahex;
	DataToHexString(npid.opt, sizeof(npid.opt), &datahex);
	INFO_LOG(Log::sceNet, "%s - Options?: %s", __FUNCTION__, datahex.c_str());
	datahex.clear();
	DataToHexString(10, 0, passph->data, sizeof(passph->data), &datahex);
	INFO_LOG(Log::sceNet, "%s - Passphrase: \n%s", __FUNCTION__, datahex.c_str());

	// TODO: Allocate & zeroed a memory of 68 bytes where npId (36 bytes) is copied to offset 8, offset 44 = 0x00026808, offset 48 = 0

	// Returning dummy Id, a 16-bit variable according to JPCSP
	// FIXME: It seems ctxId need to be in the range of 1 to 7 to be valid ?
	Memory::Write_U16(1, ctxIdPtr);
	return 0;
}

static int sceNpMatching2ContextStart(int ctxId)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d) at %08x", __FUNCTION__, ctxId, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	//if (!npMatching2Ctx)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND); //SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID

	//if (npMatching2Ctx.started)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_ALREADY_STARTED);

	// TODO: use sceNpGetUserProfile and check server availability using sceNpService_76867C01
	//npMatching2Ctx.started = true;
	// Upstream fetched http://static-resource.np.community.playstation.net/np/resource/psp-title/<commId>_00/matching/<commId>_00-matching.xml
	// here, only to log the agent FQDNs it lists (blocking for up to 20 s). The backend decides now.
	// Real firmware returns 0x805508A6 (from sceNpService_76867C01) if it can't check server availability.
	int err = GetNpMatching2Backend().ContextStart(npTitleId.data);
	if (err < 0)
		return hleLogError(Log::sceNet, err);
	hleEatMicro(1000);
	return 0;
}

static int sceNpMatching2ContextStop(int ctxId)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d) at %08x", __FUNCTION__, ctxId, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	//if (!npMatching2Ctx)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND); //SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID

	//if (!npMatching2Ctx.started)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_STARTED);

	//npMatching2Ctx.started = false;
	GetNpMatching2Backend().ContextStop();

	return 0;
}

static int sceNpMatching2DestroyContext(int ctxId)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d) at %08x", __FUNCTION__, ctxId, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	//if (!npMatching2Ctx)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND); //SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID

	GetNpMatching2Backend().ContextStop();
	NpMatching2ResetState();

	// Remove callback handler
	int handlerID = ctxId - 1;
	if (npMatching2Handlers.find(handlerID) != npMatching2Handlers.end()) {
		npMatching2Handlers.erase(handlerID);
		WARN_LOG(Log::sceNet, "%s: Deleted handler %d", __FUNCTION__, handlerID);
	}
	else {
		ERROR_LOG(Log::sceNet, "%s: Invalid Context ID %d", __FUNCTION__, ctxId);
	}

	return 0;
}

static int sceNpMatching2GetMemoryStat(u32 memStatPtr)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%08x) at %08x", __FUNCTION__, memStatPtr, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	auto memStat = PSPPointer<SceNpAuthMemoryStat>::Create(memStatPtr);
	if (!memStat.IsValid())
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	*memStat = npMatching2MemStat;
	memStat.NotifyWrite("NpMatching2GetMemoryStat");

	return 0;
}

static int sceNpMatching2RegisterSignalingCallback(int ctxId, u32 callbackFunctionAddr, u32 callbackArgument)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d, %08x, %08x) at %08x", __FUNCTION__, ctxId, callbackFunctionAddr, callbackArgument, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	if (ctxId <= 0)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID);

	int id = ctxId - 1;
	if (callbackFunctionAddr != 0) {
		bool foundHandler = false;

		struct NpMatching2Handler handler;
		memset(&handler, 0, sizeof(handler));

		handler.entryPoint = callbackFunctionAddr;
		handler.argument = callbackArgument;

		for (std::map<int, NpMatching2Handler>::iterator it = npMatching2Handlers.begin(); it != npMatching2Handlers.end(); it++) {
			if (it->second.entryPoint == handler.entryPoint) {
				foundHandler = true;
				id = it->first;
				break;
			}
		}

		if (!foundHandler && Memory::IsValidAddress(handler.entryPoint)) {
			npMatching2Handlers[id] = handler;
			WARN_LOG(Log::sceNet, "%s - Added handler(%08x, %08x) : %d", __FUNCTION__, handler.entryPoint, handler.argument, id);
		}
		else {
			ERROR_LOG(Log::sceNet, "%s - Same handler(%08x, %08x) already exists", __FUNCTION__, handler.entryPoint, handler.argument);
		}

		//u32 dataLength = 4097; 
		//notifyNpMatching2Handlers(retval, dataLength, handler.argument);

		// callback struct have 57 * u32? where [0]=0, [40]=flags, [55]=callbackFunc, and [56]=callbackArgs?
		//hleEnqueueCall(callbackFunctionAddr, 7, (u32*)Memory::GetPointer(callbackArgument), nullptr); // 7 args? since the callback handler is trying to use t2 register
	}
	return 0;
}

static int sceNpMatching2GetServerIdListLocal(int ctxId, u32 serverIdsPtr, int maxServerIds)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d, %08x, %d) at %08x", __FUNCTION__, ctxId, serverIdsPtr, maxServerIds, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	if (!Memory::IsValidAddress(serverIdsPtr))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	// Server ids are 16-bit according to JPCSP
	const std::vector<u16> ids = GetNpMatching2Backend().GetServerIdList();
	int count = 0;
	for (; count < (int)ids.size() && count < maxServerIds; count++)
		Memory::Write_U16(ids[count], serverIdsPtr + count * 2);

	return hleLogInfo(Log::sceNet, count);
}

// reqParam = {u16 serverId}. The callback data is SceNpMatching2Server {u16 serverId; u8 status; u8 padding;}.
static int sceNpMatching2GetServerInfo(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	if (!Memory::IsValidRange(reqParamPtr, 2) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	const u16 serverId = Memory::Read_U16(reqParamPtr);
	if (serverId == 0)
		return hleLogError(Log::sceNet, 0x80550CBF); // Should be SCE_NP_MATCHING2_ERROR_INVALID_SERVER_ID ?

	NpMatching2ServerInfo info{};
	const int err = GetNpMatching2Backend().GetServerInfo(serverId, &info);
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0001);
	NpMatching2FinishRequest(req, err, [info](u32 data, s32 err) {
		Memory::Write_U16(info.serverId, data);
		Memory::Write_U8(info.status, data + 2);
		return err;
	});
	return hleLogInfo(Log::sceNet, 0, "serverId=%d -> reqId=%d, status=%d, error=%08x", serverId, req.reqId, info.status, err);
}

// reqParam = {u16 serverId}. The callback data is {u32 worldPtr; u32 worldNum;}. PSP2i reads the u32 worldId
// at worldPtr + 4 and needs it to be non-zero. The rest of the PSP world struct is a guess: a "next" pointer
// at +0 (like the PSP room lists), then the PS3 SceNpMatching2World fields.
static int sceNpMatching2GetWorldInfoList(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	if (!Memory::IsValidRange(reqParamPtr, 2) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	const u16 serverId = Memory::Read_U16(reqParamPtr);
	if (serverId == 0)
		return hleLogError(Log::sceNet, 0x80550CBF); // Should be SCE_NP_MATCHING2_ERROR_INVALID_SERVER_ID ?

	const u32 WORLD_SIZE = 0x40;
	std::vector<NpMatching2WorldInfo> worlds;
	const int err = GetNpMatching2Backend().GetWorldInfoList(serverId, &worlds);
	const size_t maxWorlds = (NP_MATCHING2_DATA_SLOT_SIZE - 8) / WORLD_SIZE;
	if (worlds.size() > maxWorlds)
		worlds.resize(maxWorlds);

	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0002);
	NpMatching2FinishRequest(req, err, [worlds, WORLD_SIZE](u32 data, s32 err) {
		Memory::Write_U32(worlds.empty() ? 0 : data + 8, data);
		Memory::Write_U32((u32)worlds.size(), data + 4);
		for (size_t i = 0; i < worlds.size(); i++) {
			const u32 w = data + 8 + (u32)i * WORLD_SIZE;
			const NpMatching2WorldInfo &info = worlds[i];
			Memory::Write_U32(i + 1 < worlds.size() ? w + WORLD_SIZE : 0, w);
			Memory::Write_U32(info.worldId, w + 0x04);
			Memory::Write_U32(info.numOfLobby, w + 0x08);
			Memory::Write_U32(info.maxNumOfTotalLobbyMember, w + 0x0C);
			Memory::Write_U32(info.curNumOfTotalLobbyMember, w + 0x10);
			Memory::Write_U32(info.curNumOfRoom, w + 0x14);
			Memory::Write_U32(info.curNumOfTotalRoomMember, w + 0x18);
		}
		return err;
	});
	return hleLogInfo(Log::sceNet, 0, "serverId=%d -> reqId=%d, %d world(s), error=%08x", serverId, req.reqId, (int)worlds.size(), err);
}

// Drops the request's callback: queued ones now, unanswered ones when the answer arrives.
static int sceNpMatching2AbortRequest(int ctxId, u32 reqId)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	std::lock_guard<std::recursive_mutex> guard(npMatching2EvtMtx);
	for (auto it = npMatching2Callbacks.begin(); it != npMatching2Callbacks.end(); ) {
		if (it->reqId == reqId)
			it = npMatching2Callbacks.erase(it);
		else
			++it;
	}
	if (npMatching2InFlightReqIds.count(reqId))
		npMatching2AbortedReqIds.insert(reqId);
	return hleLogInfo(Log::sceNet, 0);
}

// Room functions PPSSPP didn't know (names from PSP2i's debug strings). They need a real backend, so for
// now they fail through the request callback instead of leaving the game waiting.
static int NpMatching2UnimplementedRequest(const char *name, int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr, u32 event)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, event);
	NpMatching2FinishRequest(req, SCE_NP_MATCHING2_SERVER_ERROR_SERVICE_UNAVAILABLE);
	return hleLogError(Log::sceNet, 0, "UNIMPL %s(%d, %08x, %08x, %08x) -> reqId=%d fails with SERVICE_UNAVAILABLE", name, ctxId, reqParamPtr, optParamPtr, assignedReqIdPtr, req.reqId);
}

static int sceNpMatching2GetUserInfoList(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	return NpMatching2UnimplementedRequest(__FUNCTION__, ctxId, reqParamPtr, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0004);
}

// PSP SceNpMatching2RoomDataExternal (search results), as PSP2i reads it: next @0, serverId @4, worldId @8,
// lobbyId @0x10, roomId @0x18, u32 maxSlot @0x20, u32 curMemberNum @0x24, passwordSlotMask @0x28,
// owner (SceNpUserInfo2*: SceNpId, then the name pointer @0x24) @0x30, flagAttr @0x3C, searchable int attrs
// @0x40/0x44, searchable bin attrs @0x48/0x4C, bin attrs @0x50/0x54. PSP2i reads 0x20/0x24 as u32 (the
// PS3 struct has u16 slot counts there).
static const u32 NP_MATCHING2_ROOM_DATA_EXTERNAL_SIZE = 0x58;

// Bump allocator over a callback data slot.
struct NpMatching2SlotWriter {
	u32 next;
	u32 end;
	u32 Alloc(u32 size) {
		size = (size + 3) & ~3;
		if (next + size > end)
			return 0;
		const u32 addr = next;
		next += size;
		return addr;
	}
};

static bool NpMatching2WriteBinAttrs(NpMatching2SlotWriter &out, const std::vector<NpMatching2BinAttr> &attrs, u32 *ptr) {
	*ptr = 0;
	if (attrs.empty())
		return true;
	const u32 table = out.Alloc((u32)attrs.size() * 12);
	if (!table)
		return false;
	for (size_t i = 0; i < attrs.size(); i++) {
		const u32 data = attrs[i].data.empty() ? 0 : out.Alloc((u32)attrs[i].data.size());
		if (!attrs[i].data.empty() && !data)
			return false;
		if (data)
			Memory::Memcpy(data, attrs[i].data.data(), (u32)attrs[i].data.size(), "NpMatching2BinAttr");
		Memory::Write_U16(attrs[i].id, table + (u32)i * 12);
		Memory::Write_U32(data, table + (u32)i * 12 + 4);
		Memory::Write_U32((u32)attrs[i].data.size(), table + (u32)i * 12 + 8);
	}
	*ptr = table;
	return true;
}

// Writes a linked list of RoomDataExternal entries. Returns how many fit.
static u32 NpMatching2WriteRoomDataExternalList(NpMatching2SlotWriter &out, const std::vector<NpMatching2Room> &rooms, u32 *first) {
	*first = 0;
	u32 prev = 0, count = 0;
	for (const NpMatching2Room &room : rooms) {
		NpMatching2SlotWriter attempt = out;
		const u32 r = attempt.Alloc(NP_MATCHING2_ROOM_DATA_EXTERNAL_SIZE);
		const u32 owner = attempt.Alloc(0x2C);
		const u32 ints = room.intAttrsExt.empty() ? 0 : attempt.Alloc((u32)room.intAttrsExt.size() * 8);
		u32 searchBins = 0, bins = 0;
		if (!r || !owner || (!room.intAttrsExt.empty() && !ints) ||
			!NpMatching2WriteBinAttrs(attempt, room.searchBinAttrsExt, &searchBins) ||
			!NpMatching2WriteBinAttrs(attempt, room.binAttrsExt, &bins)) {
			WARN_LOG(Log::sceNet, "NpMatching2: only %u of %u rooms fit in the callback data", count, (u32)rooms.size());
			break;
		}
		out = attempt;

		Memory::Memset(r, 0, NP_MATCHING2_ROOM_DATA_EXTERNAL_SIZE, "NpMatching2RoomDataExternal");
		Memory::Memset(owner, 0, 0x2C, "NpMatching2RoomOwner");
		Memory::Memcpy(owner, room.ownerOnlineId.data(), (u32)std::min(room.ownerOnlineId.size(), (size_t)16), "NpMatching2RoomOwner");
		Memory::Write_U32(owner, owner + 0x24);  // onlineName -> the handle inside the SceNpId
		for (size_t i = 0; i < room.intAttrsExt.size(); i++) {
			Memory::Write_U16(room.intAttrsExt[i].id, ints + (u32)i * 8);
			Memory::Write_U32(room.intAttrsExt[i].val, ints + (u32)i * 8 + 4);
		}
		Memory::Write_U16(room.serverId, r + 0x04);
		Memory::Write_U32(room.worldId, r + 0x08);
		Memory::Write_U64(room.lobbyId, r + 0x10);
		Memory::Write_U64(room.roomId, r + 0x18);
		Memory::Write_U32(room.maxSlot, r + 0x20);
		Memory::Write_U32(room.curMemberNum, r + 0x24);
		Memory::Write_U64(room.passwordSlotMask, r + 0x28);
		Memory::Write_U32(owner, r + 0x30);
		Memory::Write_U32(room.flagAttr, r + 0x3C);
		Memory::Write_U32(ints, r + 0x40);
		Memory::Write_U32((u32)room.intAttrsExt.size(), r + 0x44);
		Memory::Write_U32(searchBins, r + 0x48);
		Memory::Write_U32((u32)room.searchBinAttrsExt.size(), r + 0x4C);
		Memory::Write_U32(bins, r + 0x50);
		Memory::Write_U32((u32)room.binAttrsExt.size(), r + 0x54);
		if (prev)
			Memory::Write_U32(r, prev);
		else
			*first = r;
		prev = r;
		count++;
	}
	return count;
}

// Remembers the room we're now in, with its per-room callbacks.
static void NpMatching2EnterRoom(const NpMatching2PendingRequest &req, const NpMatching2Room &room, u32 eventCb, u32 eventCbArg, u32 messageCb, u32 messageCbArg) {
	npMatching2RoomId = room.roomId;
	npMatching2RoomCtxId = req.ctxId;
	npMatching2RoomEventCb = eventCb;
	npMatching2RoomEventCbArg = eventCbArg;
	npMatching2RoomMessageCb = messageCb;
	npMatching2RoomMessageCbArg = messageCbArg;
	npMatching2Peers.clear();
}

// Finishes CreateJoinRoom/JoinRoom: the callback data is {SceNpMatching2RoomDataInternal *roomDataInternal}.
static void NpMatching2FinishRoomRequest(const NpMatching2PendingRequest &req, int err, const NpMatching2Room &room) {
	NpMatching2FinishRequest(req, err, [room](u32 data, s32 err) -> s32 {
		if (!NpMatching2WriteRoomDataInternal(data + 0x10, NP_MATCHING2_DATA_SLOT_SIZE - 0x10, room))
			return SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY;
		Memory::Write_U32(data + 0x10, data);
		return err;
	});
}

static void NpMatching2ReadRoomCallbacks(u32 roomEventCbParamPtr, u32 roomMessageCbParamPtr, u32 cbs[4]) {
	cbs[0] = cbs[1] = cbs[2] = cbs[3] = 0;
	if (Memory::IsValidRange(roomEventCbParamPtr, 8)) {
		cbs[0] = Memory::Read_U32(roomEventCbParamPtr);
		cbs[1] = Memory::Read_U32(roomEventCbParamPtr + 4);
	}
	if (Memory::IsValidRange(roomMessageCbParamPtr, 8)) {
		cbs[2] = Memory::Read_U32(roomMessageCbParamPtr);
		cbs[3] = Memory::Read_U32(roomMessageCbParamPtr + 4);
	}
}

// reqParam (0x20) = PS3 SceNpMatching2SetRoomDataExternalRequest: roomId @0, searchable int attrs @8/0xC,
// searchable bin attrs @0x10/0x14, bin attrs @0x18/0x1C (PSP2i: 8 int attrs 0x4C-0x53, name 0x54, 0x55/0x56).
static int sceNpMatching2SetRoomDataExternal(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x20) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	NpMatching2RoomDataExternalParams params;
	params.roomId = Memory::Read_U64(reqParamPtr);
	params.intAttrsExt = NpMatching2ReadIntAttrs(Memory::Read_U32(reqParamPtr + 0x08), Memory::Read_U32(reqParamPtr + 0x0C));
	params.searchBinAttrsExt = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x10), Memory::Read_U32(reqParamPtr + 0x14));
	params.binAttrsExt = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x18), Memory::Read_U32(reqParamPtr + 0x1C));
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0006);
	GetNpMatching2Backend().SetRoomDataExternal(params, [req](int err) {
		NpMatching2FinishRequest(req, err);
	});
	return hleLogInfo(Log::sceNet, 0, "roomId=%llu, %d int / %d searchable bin / %d bin attrs -> reqId=%d", (unsigned long long)params.roomId,
		(int)params.intAttrsExt.size(), (int)params.searchBinAttrsExt.size(), (int)params.binAttrsExt.size(), req.reqId);
}

// reqParam (0x20) = PS3 SceNpMatching2SendRoomMessageRequest: roomId @0, u8 castType @8 (1 = everyone else,
// 2 = unicast), u16 dst @0xC (unicast), msg* @0x14, msgLen @0x18. PSP2i sends with no callback.
static int sceNpMatching2SendRoomMessage(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x1C) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	NpMatching2RoomMessageParams params;
	params.roomId = Memory::Read_U64(reqParamPtr);
	params.castType = Memory::Read_U8(reqParamPtr + 0x08);
	if (params.castType == 2)
		params.dst.push_back(Memory::Read_U16(reqParamPtr + 0x0C));
	const u32 msg = Memory::Read_U32(reqParamPtr + 0x14);
	const u32 len = std::min(Memory::Read_U32(reqParamPtr + 0x18), NP_MATCHING2_MAX_BIN_ATTR_SIZE);
	if (len && Memory::IsValidRange(msg, len)) {
		params.payload.resize(len);
		Memory::Memcpy(params.payload.data(), msg, len, "NpMatching2RoomMessage");
	}
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0108);
	GetNpMatching2Backend().SendRoomMessage(params, [req](int err) {
		NpMatching2FinishRequest(req, err);
	});
	return hleLogDebug(Log::sceNet, 0, "roomId=%llu cast=%d dst=%d len=%u -> reqId=%d", (unsigned long long)params.roomId, params.castType,
		params.dst.empty() ? 0 : params.dst[0], len, req.reqId);
}

// reqParam = {u64 roomId; u32 flagFilter; u32 flagAttr; ...} (PSP2i).
static int sceNpMatching2SetRoomDataInternal(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x10) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	const u64 roomId = Memory::Read_U64(reqParamPtr);
	const u32 flagFilter = Memory::Read_U32(reqParamPtr + 8);
	const u32 flagAttr = Memory::Read_U32(reqParamPtr + 12);
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0109);
	GetNpMatching2Backend().SetRoomDataInternal(roomId, flagFilter, flagAttr, [req](int err) {
		NpMatching2FinishRequest(req, err);
	});
	return hleLogInfo(Log::sceNet, 0, "roomId=%llu flagFilter=%08x flagAttr=%08x -> reqId=%d", (unsigned long long)roomId, flagFilter, flagAttr, req.reqId);
}

// reqParam = {u64 roomId; u8 optData[0x14]...}.
static int sceNpMatching2LeaveRoom(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 8) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	const u64 roomId = Memory::Read_U64(reqParamPtr);
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0103);
	GetNpMatching2Backend().LeaveRoom(roomId, [req, roomId](int err) {
		if (err >= 0 && roomId == npMatching2RoomId)
			NpMatching2ForgetRoom();
		NpMatching2FinishRequest(req, err);
	});
	return hleLogInfo(Log::sceNet, 0, "roomId=%llu -> reqId=%d", (unsigned long long)roomId, req.reqId);
}

// reqParam (0x20) = PS3 SceNpMatching2KickoutRoomMemberRequest: roomId @0, u16 memberId @8,
// u8 blockKickFlag @0xA, optData {u8 data[16] @0xC; u32 len @0x1C}. PSP2i kicks with "BYE" and no callback.
static int sceNpMatching2KickoutRoomMember(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x20) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	const u64 roomId = Memory::Read_U64(reqParamPtr);
	const u16 memberId = Memory::Read_U16(reqParamPtr + 0x08);
	const bool blockKick = Memory::Read_U8(reqParamPtr + 0x0A) != 0;
	std::vector<u8> optData(std::min(Memory::Read_U32(reqParamPtr + 0x1C), 16U));
	if (!optData.empty())
		Memory::Memcpy(optData.data(), reqParamPtr + 0x0C, (u32)optData.size(), "NpMatching2KickOptData");
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0105);
	GetNpMatching2Backend().KickoutRoomMember(roomId, memberId, blockKick, optData, [req](int err) {
		NpMatching2FinishRequest(req, err);
	});
	return hleLogInfo(Log::sceNet, 0, "roomId=%llu member=%d block=%d -> reqId=%d", (unsigned long long)roomId, memberId, blockKick, req.reqId);
}

// (ctxId, reqParam, optParam, roomEventCbParam, roomMessageCbParam, &assignedReqId), checked against PSP2i.
// reqParam (0x70 bytes) = PS3 SceNpMatching2CreateJoinRoomRequest: worldId @0, lobbyId u64 @8, maxSlot @0x10,
// flagAttr @0x14, bin attrs internal @0x18/0x1C, searchable int attrs @0x20/0x24, searchable bin attrs
// @0x28/0x2C, bin attrs ext @0x30/0x34, password (char*) @0x38, ..., member bin attrs internal @0x5C/0x60.
// The callback data is {SceNpMatching2RoomDataInternal *roomDataInternal}.
static int sceNpMatching2CreateJoinRoom(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 roomEventCbParamPtr, u32 roomMessageCbParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x64) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	NpMatching2CreateJoinRoomParams params;
	params.serverId = GetNpMatching2Backend().GetServerIdList().empty() ? 0 : GetNpMatching2Backend().GetServerIdList()[0];
	params.worldId = Memory::Read_U32(reqParamPtr + 0x00);
	params.lobbyId = Memory::Read_U64(reqParamPtr + 0x08);
	params.maxSlot = Memory::Read_U32(reqParamPtr + 0x10);
	params.flagAttr = Memory::Read_U32(reqParamPtr + 0x14);
	params.binAttrsInternal = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x18), Memory::Read_U32(reqParamPtr + 0x1C));
	params.intAttrsExt = NpMatching2ReadIntAttrs(Memory::Read_U32(reqParamPtr + 0x20), Memory::Read_U32(reqParamPtr + 0x24));
	params.searchBinAttrsExt = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x28), Memory::Read_U32(reqParamPtr + 0x2C));
	params.binAttrsExt = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x30), Memory::Read_U32(reqParamPtr + 0x34));
	params.password = NpMatching2ReadPassword(Memory::Read_U32(reqParamPtr + 0x38));
	params.memberBinAttrsInternal = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x5C), Memory::Read_U32(reqParamPtr + 0x60));
	if (params.worldId == 0)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT, "worldId 0");

	u32 cbs[4];
	NpMatching2ReadRoomCallbacks(roomEventCbParamPtr, roomMessageCbParamPtr, cbs);
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0101);
	GetNpMatching2Backend().CreateJoinRoom(params, NpMatching2MyOnlineId(), [req, cbs = std::vector<u32>(cbs, cbs + 4)](int err, const NpMatching2Room &room) {
		if (err >= 0)
			NpMatching2EnterRoom(req, room, cbs[0], cbs[1], cbs[2], cbs[3]);
		INFO_LOG(Log::sceNet, "NpMatching2 CreateJoinRoom reqId=%d -> roomId=%llu, error=%08x", req.reqId, (unsigned long long)room.roomId, err);
		NpMatching2FinishRoomRequest(req, err, room);
	});
	return hleLogInfo(Log::sceNet, 0, "world=%u maxSlot=%u flagAttr=%08x attrs %d/%d/%d/%d%s -> reqId=%d",
		params.worldId, params.maxSlot, params.flagAttr, (int)params.intAttrsExt.size(), (int)params.binAttrsInternal.size(),
		(int)params.searchBinAttrsExt.size(), (int)params.binAttrsExt.size(), params.password.empty() ? "" : ", password", req.reqId);
}

// Same arguments as CreateJoinRoom (PSP2i call site). reqParam (0x30) = PS3 SceNpMatching2JoinRoomRequest:
// roomId @0, password (char*) @8, joinRoomGroupLabel* @0xC, member bin attrs internal @0x10/0x14, optData @0x18.
static int sceNpMatching2JoinRoom(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 roomEventCbParamPtr, u32 roomMessageCbParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x18) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	NpMatching2JoinRoomParams params;
	params.roomId = Memory::Read_U64(reqParamPtr);
	params.password = NpMatching2ReadPassword(Memory::Read_U32(reqParamPtr + 0x08));
	params.memberBinAttrsInternal = NpMatching2ReadBinAttrs(Memory::Read_U32(reqParamPtr + 0x10), Memory::Read_U32(reqParamPtr + 0x14));

	u32 cbs[4];
	NpMatching2ReadRoomCallbacks(roomEventCbParamPtr, roomMessageCbParamPtr, cbs);
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0102);
	GetNpMatching2Backend().JoinRoom(params, [req, cbs = std::vector<u32>(cbs, cbs + 4)](int err, const NpMatching2Room &room) {
		if (err >= 0)
			NpMatching2EnterRoom(req, room, cbs[0], cbs[1], cbs[2], cbs[3]);
		INFO_LOG(Log::sceNet, "NpMatching2 JoinRoom reqId=%d -> roomId=%llu, me=%d owner=%d, %d member(s), error=%08x", req.reqId,
			(unsigned long long)room.roomId, room.myMemberId, room.ownerMemberId, (int)room.members.size(), err);
		NpMatching2FinishRoomRequest(req, err, room);
	});
	return hleLogInfo(Log::sceNet, 0, "roomId=%llu%s -> reqId=%d", (unsigned long long)params.roomId, params.password.empty() ? "" : " (password)", req.reqId);
}

// reqParam (0x38) = PS3 SceNpMatching2SearchRoomRequest: option @0, worldId @4, lobbyId u64 @8,
// rangeFilter {startIndex, max} @0x10, flagFilter @0x18, flagAttr @0x1C, int filters @0x20/0x24
// ({u8 op; pad[3]; {u16 id; u32 val}} = 12 bytes), bin filters @0x28/0x2C, attr ids @0x30/0x34.
// The callback data is {u32 startIndex; u32 total; u32 resultCount; SceNpMatching2RoomDataExternal *rooms}.
static int sceNpMatching2SearchRoom(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x38) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	NpMatching2SearchRoomParams params;
	params.worldId = Memory::Read_U32(reqParamPtr + 0x04);
	params.lobbyId = Memory::Read_U64(reqParamPtr + 0x08);
	params.startIndex = Memory::Read_U32(reqParamPtr + 0x10);
	params.max = Memory::Read_U32(reqParamPtr + 0x14);
	params.flagFilter = Memory::Read_U32(reqParamPtr + 0x18);
	params.flagAttr = Memory::Read_U32(reqParamPtr + 0x1C);
	const u32 filters = Memory::Read_U32(reqParamPtr + 0x20), numFilters = Memory::Read_U32(reqParamPtr + 0x24);
	if (filters && numFilters <= NP_MATCHING2_MAX_ATTRS && Memory::IsValidRange(filters, numFilters * 12)) {
		for (u32 i = 0; i < numFilters; i++) {
			const u32 f = filters + i * 12;
			params.intFilters.push_back({ Memory::Read_U8(f), { Memory::Read_U16(f + 4), Memory::Read_U32(f + 8) } });
		}
	}

	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0106);
	GetNpMatching2Backend().SearchRoom(params, [req, params](int err, const std::vector<NpMatching2Room> &rooms, u32 total) {
		INFO_LOG(Log::sceNet, "NpMatching2 SearchRoom reqId=%d -> %d of %u room(s), error=%08x", req.reqId, (int)rooms.size(), total, err);
		NpMatching2FinishRequest(req, err, [params, rooms, total](u32 data, s32 err) {
			NpMatching2SlotWriter out{ data + 0x10, data + NP_MATCHING2_DATA_SLOT_SIZE };
			u32 first = 0;
			const u32 count = NpMatching2WriteRoomDataExternalList(out, rooms, &first);
			Memory::Write_U32(params.startIndex ? params.startIndex : 1, data);
			Memory::Write_U32(total, data + 4);
			Memory::Write_U32(count, data + 8);  // resultCount
			Memory::Write_U32(first, data + 12);
			return err;
		});
	});
	return hleLogInfo(Log::sceNet, 0, "world=%u range=%u+%u flags=%08x/%08x, %d filter(s) -> reqId=%d",
		params.worldId, params.startIndex, params.max, params.flagFilter, params.flagAttr, (int)params.intFilters.size(), req.reqId);
}

// reqParam = PS3 SceNpMatching2GetRoomDataExternalListRequest: {u64 *roomId; u32 roomIdNum; u16 *attrId;
// u32 attrIdNum}. The callback data is {SceNpMatching2RoomDataExternal *rooms; u32 roomNum}.
static int sceNpMatching2GetRoomDataExternalList(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x10) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	std::vector<u64> roomIds;
	const u32 ids = Memory::Read_U32(reqParamPtr), num = Memory::Read_U32(reqParamPtr + 4);
	if (ids && num <= NP_MATCHING2_MAX_ATTRS && Memory::IsValidRange(ids, num * 8)) {
		for (u32 i = 0; i < num; i++)
			roomIds.push_back(Memory::Read_U64(ids + i * 8));
	}
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0007);
	GetNpMatching2Backend().GetRoomDataExternalList(roomIds, [req](int err, const std::vector<NpMatching2Room> &rooms, u32) {
		NpMatching2FinishRequest(req, err, [rooms](u32 data, s32 err) {
			NpMatching2SlotWriter out{ data + 0x10, data + NP_MATCHING2_DATA_SLOT_SIZE };
			u32 first = 0;
			const u32 count = NpMatching2WriteRoomDataExternalList(out, rooms, &first);
			Memory::Write_U32(first, data);
			Memory::Write_U32(count, data + 4);
			return err;
		});
	});
	return hleLogInfo(Log::sceNet, 0, "%d room id(s) -> reqId=%d", (int)roomIds.size(), req.reqId);
}

// (ctxId, roomId u64 in a2:a3, memberId, int *connStatus, SceNetInAddr *peerAddr, u16 *peerPort): synchronous,
// from the Established/Dead events we've seen. PSP2i needs status 2 (active) before it stores the address.
static int sceNpMatching2SignalingGetConnectionStatus(int ctxId, u64 roomId, u32 memberId, u32 connStatusPtr, u32 peerAddrPtr, u32 peerPortPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	NpMatching2PeerStatus peer{ 0, 0, 0 };
	auto it = npMatching2Peers.find((u16)memberId);
	if (roomId == npMatching2RoomId && it != npMatching2Peers.end())
		peer = it->second;
	if (Memory::IsValidRange(connStatusPtr, 4))
		Memory::Write_U32(peer.status, connStatusPtr);
	if (Memory::IsValidRange(peerAddrPtr, 4))
		Memory::Write_U32(peer.addr, peerAddrPtr);
	if (Memory::IsValidRange(peerPortPtr, 2))
		Memory::Write_U16(peer.port, peerPortPtr);
	in_addr peerIn{};
	peerIn.s_addr = peer.addr;
	return hleLogInfo(Log::sceNet, 0, "room=%llu member=%d -> status %d, %s:%d", (unsigned long long)roomId, memberId, peer.status,
		ip2str(peerIn).c_str(), ntohs(peer.port));
}

static void WrapI_SignalingGetConnectionStatus() {
	int retval = sceNpMatching2SignalingGetConnectionStatus(PARAM(0), PARAM64(2), PARAM(4), PARAM(5), PARAM(6), PARAM(7));
	RETURN(retval);
}

static int sceNpMatching2SendRoomChatMessage(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d, %08x, %08x, %08x[%08x]) at %08x", __FUNCTION__, ctxId, reqParamPtr, optParamPtr, assignedReqIdPtr, Memory::Read_U32(assignedReqIdPtr), currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	if (!Memory::IsValidAddress(reqParamPtr) || !Memory::IsValidAddress(assignedReqIdPtr))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	u32 cbFunc = Memory::Read_U32(reqParamPtr);
	u32 cbArg = Memory::Read_U32(reqParamPtr + 0x04);

	// Notify callback handler
	if (Memory::IsValidAddress(cbFunc)) {
		// There args are supposed to be constructed in the stack and the data need to be available even after returning from this function, so these args + optional data probably copied to somewhere
		NpMatching2Args args = {};
		args.data[0] = PSP_NP_MATCHING2_EVENT_0107;
		args.data[1] = PSP_NP_MATCHING2_STATE_3208;
		//args.data[2] = pointer to arg[8]
		args.data[3] = optParamPtr;
		args.data[4] = 0;
		args.data[5] = assignedReqIdPtr;
		args.data[6] = 0;
		//args.data[8] = reqParamPtr;

		notifyNpMatching2Handlers(args, ctxId, 0, cbFunc, cbArg, 0, 0, 0, 0x10);

		Memory::Write_U32(args.data[1], assignedReqIdPtr); // server status or flags?
	}

	// After returning, Fat Princess will loop for 64 times (increasing the address by 288 bytes on each loop) or until found a zero status byte (0x08BD4860 + 0x10), looking for empty/available entry to set?
	return 0;
}

const HLEFunction sceNpMatching2[] = {
	{0x2E61F6E1, &WrapI_IIII<sceNpMatching2Init>,						"sceNpMatching2Init",							'i', "iiii"   },
	{0x8BF37D8C, &WrapI_V<sceNpMatching2Term>,							"sceNpMatching2Term",							'i', ""       },
	{0x5030CC53, &WrapI_UUUI<sceNpMatching2CreateContext>,				"sceNpMatching2CreateContext",					'i', "xxxi"   },
	{0x190FF903, &WrapI_I<sceNpMatching2ContextStart>,					"sceNpMatching2ContextStart",					'i', "i"      },
	{0x2B3892FC, &WrapI_I<sceNpMatching2ContextStop>,					"sceNpMatching2ContextStop",					'i', "i"      },
	{0x3DE70241, &WrapI_I<sceNpMatching2DestroyContext>,				"sceNpMatching2DestroyContext",					'i', "i"      },
	{0x22F38DAF, &WrapI_U<sceNpMatching2GetMemoryStat>,					"sceNpMatching2GetMemoryStat",					'i', "x"      },
	{0xA3C298D1, &WrapI_IUU<sceNpMatching2RegisterSignalingCallback>,	"sceNpMatching2RegisterSignalingCallback",		'i', "ixx"    },
	{0xF47342FC, &WrapI_IUI<sceNpMatching2GetServerIdListLocal>,		"sceNpMatching2GetServerIdListLocal",			'i', "ixi"    },
	{0x4EE3A8EC, &WrapI_IUUU<sceNpMatching2GetServerInfo>,				"sceNpMatching2GetServerInfo",					'i', "ixxx"   },
	{0xC870535A, &WrapI_IUUU<sceNpMatching2LeaveRoom>,					"sceNpMatching2LeaveRoom",						'i', "ixxx"   },
	{0xAAD0946A, &WrapI_IUUUUU<sceNpMatching2CreateJoinRoom>,			"sceNpMatching2CreateJoinRoom",					'i', "ixxxxx" },
	{0x81C13E6D, &WrapI_IUUU<sceNpMatching2SearchRoom>,					"sceNpMatching2SearchRoom",						'i', "ixxx"   },
	{0x55F7837F, &WrapI_IUUU<sceNpMatching2SendRoomChatMessage>,		"sceNpMatching2SendRoomChatMessage",			'i', "ixxx"   },
	{0x12C5A111, &WrapI_IUUU<sceNpMatching2GetRoomDataExternalList>,	"sceNpMatching2GetRoomDataExternalList",		'i', "ixxx"   },
	{0x6D6D0C75, &WrapI_SignalingGetConnectionStatus,					"sceNpMatching2SignalingGetConnectionStatus",	'i', "iXixxx" },
	{0x7BBFC427, &WrapI_IUUUUU<sceNpMatching2JoinRoom>,					"sceNpMatching2JoinRoom",			            'i', "ixxxxx" },
	{0x97529ECC, &WrapI_IUUU<sceNpMatching2KickoutRoomMember>,			"sceNpMatching2KickoutRoomMember",			    'i', "ixxx"   },
	{0xA53E7C69, &WrapI_IUUU<sceNpMatching2GetWorldInfoList>,			"sceNpMatching2GetWorldInfoList",			    'i', "ixxx"   },
	{0xE6C93DBD, &WrapI_IUUU<sceNpMatching2SetRoomDataInternal>,		"sceNpMatching2SetRoomDataInternal",			'i', "ixxx"   },
	{0xFADBA9DB, &WrapI_IU<sceNpMatching2AbortRequest>,					"sceNpMatching2AbortRequest",			        'i', "ix"     },
	{0xC8FC5D41, &WrapI_IUUU<sceNpMatching2GetUserInfoList>,			"sceNpMatching2GetUserInfoList",				'i', "ixxx"   },
	{0xD7D4AEB2, &WrapI_IUUU<sceNpMatching2SetRoomDataExternal>,		"sceNpMatching2SetRoomDataExternal",			'i', "ixxx"   },
	{0xF940D9AD, &WrapI_IUUU<sceNpMatching2SendRoomMessage>,			"sceNpMatching2SendRoomMessage",				'i', "ixxx"   },
	{0x3892E9A6, nullptr,		                                        "sceNpMatching2_3892E9A6",			            'i', ""       },
};

void Register_sceNpMatching2()
{
	RegisterHLEModule("sceNpMatching2", ARRAY_SIZE(sceNpMatching2), sceNpMatching2);
}
