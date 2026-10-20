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
#include <mutex>
#include <set>
#include <StringUtils.h>
#include "Core/MemMapHelpers.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/FunctionWrappers.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceNet.h"
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
// of slots is enough. TODO: savestates.
static const u32 NP_MATCHING2_DATA_SLOT_SIZE = 0x400;
static const int NP_MATCHING2_DATA_SLOTS = 16;
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
		// TODO: fall back to the context's default request callback, once we track contexts.
		WARN_LOG(Log::sceNet, "NpMatching2: request %d (event %04x) has no callback", req.reqId, event);
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
			return npMatching2RoomEventCb;
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

	npMatching2Handlers.clear();
	npMatching2Events.clear();
	NpMatching2ResetState();
	npMatching2Inited = true;
	INFO_LOG(Log::sceNet, "NpMatching2 backend: %s", GetNpMatching2Backend().Name());
	return 0;
}

static int sceNpMatching2Term()
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s() at %08x", __FUNCTION__, currentMIPS->pc);
	npMatching2Inited = false;
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

	//TODO: Stop any in-progress HTTPClient communication used on sceNpMatching2ContextStart
	//npMatching2Ctx.started = false;

	return 0;
}

static int sceNpMatching2DestroyContext(int ctxId)
{
	ERROR_LOG(Log::sceNet, "UNIMPL %s(%d) at %08x", __FUNCTION__, ctxId, currentMIPS->pc);
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);

	//if (!npMatching2Ctx)
	//	return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_NOT_FOUND); //SCE_NP_MATCHING2_ERROR_INVALID_CONTEXT_ID

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

// Request functions on the room we're in: the reqParam starts with the u64 roomId. On success the
// callback gets a zeroed response (the games we know don't read it).
static int NpMatching2SimpleRoomRequest(const char *name, int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr, u32 event, void (NpMatching2Backend::*op)(u64, NpMatching2Done))
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 8) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT);

	const u64 roomId = Memory::Read_U64(reqParamPtr);
	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, event);
	(GetNpMatching2Backend().*op)(roomId, [req](int err) {
		NpMatching2FinishRequest(req, err);
	});
	return hleLogInfo(Log::sceNet, 0, "%s: roomId=%llu -> reqId=%d", name, (unsigned long long)roomId, req.reqId);
}

static int sceNpMatching2SetRoomDataExternal(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	// TODO: pass the searchable/bin attributes to the backend.
	return NpMatching2SimpleRoomRequest(__FUNCTION__, ctxId, reqParamPtr, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0006, &NpMatching2Backend::SetRoomDataExternal);
}

static int sceNpMatching2SendRoomMessage(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	// TODO: pass castType/destination/payload to the backend.
	return NpMatching2SimpleRoomRequest(__FUNCTION__, ctxId, reqParamPtr, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0108, &NpMatching2Backend::SendRoomMessage);
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
		if (err >= 0 && roomId == npMatching2RoomId) {
			npMatching2RoomId = 0;
			npMatching2RoomEventCb = npMatching2RoomMessageCb = 0;
		}
		NpMatching2FinishRequest(req, err);
	});
	return hleLogInfo(Log::sceNet, 0, "roomId=%llu -> reqId=%d", (unsigned long long)roomId, req.reqId);
}

// (ctxId, reqParam, optParam, roomEventCbParam, roomMessageCbParam, &assignedReqId), checked against PSP2i.
// reqParam (0x70 bytes) follows the PS3 SceNpMatching2CreateJoinRoomRequest: worldId @0, lobbyId u64 @8,
// maxSlot @0x10, flagAttr @0x14, then attribute/password/group pointers. The callback data is
// {SceNpMatching2RoomDataInternal *roomDataInternal}.
static int sceNpMatching2CreateJoinRoom(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 roomEventCbParamPtr, u32 roomMessageCbParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x18) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	NpMatching2CreateJoinRoomParams params{};
	params.serverId = GetNpMatching2Backend().GetServerIdList().empty() ? 0 : GetNpMatching2Backend().GetServerIdList()[0];
	params.worldId = Memory::Read_U32(reqParamPtr + 0x00);
	params.lobbyId = Memory::Read_U64(reqParamPtr + 0x08);
	params.maxSlot = Memory::Read_U32(reqParamPtr + 0x10);
	params.flagAttr = Memory::Read_U32(reqParamPtr + 0x14);
	if (params.worldId == 0)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT, "worldId 0");

	u32 eventCb = 0, eventCbArg = 0, messageCb = 0, messageCbArg = 0;
	if (Memory::IsValidRange(roomEventCbParamPtr, 8)) {
		eventCb = Memory::Read_U32(roomEventCbParamPtr);
		eventCbArg = Memory::Read_U32(roomEventCbParamPtr + 4);
	}
	if (Memory::IsValidRange(roomMessageCbParamPtr, 8)) {
		messageCb = Memory::Read_U32(roomMessageCbParamPtr);
		messageCbArg = Memory::Read_U32(roomMessageCbParamPtr + 4);
	}

	SceNpId myNpId{};
	NpGetNpId(&myNpId);
	const std::string myOnlineId(myNpId.handle.data, strnlen(myNpId.handle.data, sizeof(myNpId.handle.data)));

	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0101);
	GetNpMatching2Backend().CreateJoinRoom(params, myOnlineId, [req, eventCb, eventCbArg, messageCb, messageCbArg](int err, const NpMatching2Room &room) {
		if (err >= 0) {
			npMatching2RoomId = room.roomId;
			npMatching2RoomCtxId = req.ctxId;
			npMatching2RoomEventCb = eventCb;
			npMatching2RoomEventCbArg = eventCbArg;
			npMatching2RoomMessageCb = messageCb;
			npMatching2RoomMessageCbArg = messageCbArg;
		}
		INFO_LOG(Log::sceNet, "NpMatching2 CreateJoinRoom reqId=%d -> roomId=%llu, error=%08x", req.reqId, (unsigned long long)room.roomId, err);
		NpMatching2FinishRequest(req, err, [room](u32 data, s32 err) -> s32 {
			if (!NpMatching2WriteRoomDataInternal(data + 0x10, NP_MATCHING2_DATA_SLOT_SIZE - 0x10, room))
				return SCE_NP_MATCHING2_ERROR_OUT_OF_MEMORY;
			Memory::Write_U32(data + 0x10, data);
			return err;
		});
	});
	return hleLogInfo(Log::sceNet, 0, "world=%u maxSlot=%u flagAttr=%08x -> reqId=%d, roomEventCb=%08x, roomMessageCb=%08x",
		params.worldId, params.maxSlot, params.flagAttr, req.reqId, eventCb, messageCb);
}

// reqParam (0x38 bytes) follows the PS3 SceNpMatching2SearchRoomRequest: option @0, worldId @4,
// lobbyId u64 @8, rangeFilter {startIndex, max} @0x10, flagFilter @0x18, flagAttr @0x1C, then the
// int/bin filters and attribute ids. The callback data is
// {u32 startIndex; u32 total; u32 resultCount; SceNpMatching2RoomDataExternal *rooms}.
static int sceNpMatching2SearchRoom(int ctxId, u32 reqParamPtr, u32 optParamPtr, u32 assignedReqIdPtr)
{
	if (!npMatching2Inited)
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_NOT_INITIALIZED);
	if (!Memory::IsValidRange(reqParamPtr, 0x38) || !Memory::IsValidRange(assignedReqIdPtr, 4))
		return hleLogError(Log::sceNet, SCE_NP_MATCHING2_ERROR_CONTEXT_MAX); // Should be SCE_NP_MATCHING2_ERROR_INVALID_ARGUMENT ?

	NpMatching2SearchRoomParams params{};
	params.worldId = Memory::Read_U32(reqParamPtr + 0x04);
	params.lobbyId = Memory::Read_U64(reqParamPtr + 0x08);
	params.startIndex = Memory::Read_U32(reqParamPtr + 0x10);
	params.max = Memory::Read_U32(reqParamPtr + 0x14);
	params.flagFilter = Memory::Read_U32(reqParamPtr + 0x18);
	params.flagAttr = Memory::Read_U32(reqParamPtr + 0x1C);

	const NpMatching2PendingRequest req = NpMatching2BeginRequest(ctxId, optParamPtr, assignedReqIdPtr, PSP_NP_MATCHING2_EVENT_0106);
	GetNpMatching2Backend().SearchRoom(params, [req, params](int err, const std::vector<NpMatching2Room> &rooms, u32 total) {
		if (!rooms.empty()) {
			// TODO: write SceNpMatching2RoomDataExternal entries (matching2.md "LOBBY flow").
			WARN_LOG(Log::sceNet, "NpMatching2 SearchRoom: %d room(s) from the backend dropped, RoomDataExternal isn't implemented", (int)rooms.size());
			total = 0;
		}
		INFO_LOG(Log::sceNet, "NpMatching2 SearchRoom reqId=%d -> %u room(s), error=%08x", req.reqId, total, err);
		NpMatching2FinishRequest(req, err, [params, total](u32 data, s32 err) {
			Memory::Write_U32(params.startIndex ? params.startIndex : 1, data);
			Memory::Write_U32(total, data + 4);
			Memory::Write_U32(0, data + 8);  // resultCount
			Memory::Write_U32(0, data + 12);
			return err;
		});
	});
	return hleLogInfo(Log::sceNet, 0, "world=%u range=%u+%u flags=%08x/%08x -> reqId=%d",
		params.worldId, params.startIndex, params.max, params.flagFilter, params.flagAttr, req.reqId);
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
	{0x12C5A111, nullptr,		                                        "sceNpMatching2GetRoomDataExternalList",		'i', ""       },
	{0x6D6D0C75, nullptr,		                                        "sceNpMatching2SignalingGetConnectionStatus",	'i', ""       },
	{0x7BBFC427, nullptr,		                                        "sceNpMatching2JoinRoom",			            'i', ""       },
	{0x97529ECC, nullptr,		                                        "sceNpMatching2KickoutRoomMember",			    'i', ""       },
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
