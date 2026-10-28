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

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "Common/Data/Format/JSONReader.h"
#include "Common/Data/Format/JSONWriter.h"
#include "Common/File/FileDescriptor.h"
#include "Common/Log.h"
#include "Common/Thread/ThreadUtil.h"
#include "Common/Net/SocketCompat.h"
#include "Core/HLE/NetInetConstants.h"
#include "Core/HLE/NpRelay.h"

namespace NpRelay {
namespace {

// Frame types and flags (server/matching/protocol.py in the PSP2i revival repo).
enum : u16 {
	MSG_RELAY_OPEN = 0x0201,
	MSG_RELAY_ACCEPT = 0x0202,
	MSG_RELAY_DATA = 0x0203,
	MSG_RELAY_CLOSE = 0x0204,
	MSG_RELAY_DATAGRAM = 0x0205,
	MSG_RESPONSE = 0x8000,
	MSG_PUSH_RELAY_INCOMING = 0x9004,
	FLAG_RAW = 0x0001,
};

const size_t MAX_DATA = 64 * 1024;  // per RelayData frame
const size_t MAX_QUEUED_DATAGRAMS = 256;  // per socket; the oldest are dropped
const int CONNECT_TIMEOUT_MS = 10000;  // blocking connect() waiting for the RelayOpen answer
const int WRITE_STALL_MS = 10000;  // the game stopped reading a relayed socket: give up on it

// A relayed TCP connection. `end` is our end of the loopback pair; the game's socket is the other end.
struct Stream {
	u32 id = 0;  // the server's stream id; 0 while our RelayOpen is unanswered
	SOCKET end = INVALID_SOCKET;
	u32 peer = 0;  // network order
	u16 peerPort = 0;
	u16 localPort = 0;
	// Under g_mutex.
	Status state = Status::Opening;
	bool remoteClosed = false;  // RelayClose came in: don't send one back
	bool reset = false;  // close `end` with RST rather than FIN
	std::atomic<bool> closeRequested{ false };  // the pump closes `end` (only the pump, or Stop after it)
	// The reader thread writes to `end` under it; closing takes it too.
	std::mutex writeMutex;
	bool endClosed = false;
};
typedef std::shared_ptr<Stream> StreamPtr;

struct QueuedAccept {
	SOCKET gameEnd;
	StreamPtr stream;
};

struct Datagram {
	u32 src;
	u16 srcPort;
	std::string data;
};

// Heap-held and never freed: the matching backend may stop the relay from its own destructor at exit.
std::mutex &g_mutex = *new std::mutex();
std::condition_variable &g_cv = *new std::condition_variable();  // a blocking connect() waits on it for the RelayOpen answer
bool g_active = false;
u32 g_vaddr = 0, g_net = 0, g_mask = 0;  // network order
SendFrameFn &g_send = *new SendFrameFn();
u32 g_nextSeq = 0x40000000;  // RelayOpen seqs, apart from the backend's requests (only RelayOpen is answered)
SOCKET g_wake = INVALID_SOCKET;  // UDP to itself: wakes the pump's select
sockaddr_in g_wakeAddr{};
std::vector<StreamPtr> &g_streams = *new std::vector<StreamPtr>();  // every stream whose `end` is still open
std::map<u32, StreamPtr> &g_byId = *new std::map<u32, StreamPtr>();
std::map<u32, StreamPtr> &g_opening = *new std::map<u32, StreamPtr>();  // RelayOpen seq -> stream
std::map<int, StreamPtr> &g_bySocket = *new std::map<int, StreamPtr>();  // game socket -> its stream (connected or accepted)
std::map<u32, StreamPtr> &g_accepting = *new std::map<u32, StreamPtr>();  // Accept token -> stream, until Accepted
u32 g_nextToken = 1;
std::map<u16, int> &g_listeners = *new std::map<u16, int>();  // port -> listening game socket
std::map<int, std::deque<QueuedAccept>> &g_acceptQueue = *new std::map<int, std::deque<QueuedAccept>>();
std::map<u16, int> &g_udpReceivers = *new std::map<u16, int>();  // port -> the UDP game socket that receives on it
std::map<int, std::deque<Datagram>> &g_datagrams = *new std::map<int, std::deque<Datagram>>();
std::map<int, std::pair<u32, u16>> &g_defaultDest = *new std::map<int, std::pair<u32, u16>>();  // connected UDP game sockets
// TCP connections to our own vaddr run over host loopback (so the loopback latency of sceNetInet applies). Both ends
// still show the vaddr as the peer: game socket -> (our vaddr, peer port); connecting socket's local port -> it.
std::map<int, std::pair<u32, u16>> &g_selfPeer = *new std::map<int, std::pair<u32, u16>>();
std::map<u16, int> &g_selfPorts = *new std::map<u16, int>();
int g_dropLogCount = 0;

std::mutex &g_startStopMutex = *new std::mutex();
std::thread &g_pump = *new std::thread();
std::atomic<bool> g_pumpRun{ false };

std::string AddrStr(u32 addr) {
	const u8 *b = (const u8 *)&addr;
	char buf[16];
	snprintf(buf, sizeof(buf), "%d.%d.%d.%d", b[0], b[1], b[2], b[3]);
	return buf;
}

bool ParseAddr(const char *s, u32 *addr) {
	in_addr in{};
	if (!s || inet_pton(AF_INET, s, &in) != 1)
		return false;
	*addr = in.s_addr;
	return true;
}

u64 JsonNum(const json::JsonGet &g, const char *name, u64 def = 0) {
	const JsonNode *node = g.get(name, JSON_NUMBER);
	return node ? (u64)(s64)node->value.toNumber() : def;
}

bool WouldBlock(int err) {
	return err == EAGAIN || err == EWOULDBLOCK;
}

void WakeLocked() {
	if (g_wake != INVALID_SOCKET) {
		const char c = 0;
		sendto(g_wake, &c, 1, 0, (const sockaddr *)&g_wakeAddr, sizeof(g_wakeAddr));
	}
}

bool Send(u16 type, u16 flags, u32 seq, const char *body, size_t size) {
	SendFrameFn send;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		send = g_send;
	}
	return send && send(type, flags, seq, body, size);
}

bool SendJson(u16 type, u32 seq, const std::string &body) {
	return Send(type, 0, seq, body.data(), body.size());
}

std::string StreamJson(u32 id) {
	json::JsonWriter w;
	w.begin();
	w.writeUint("stream", id);
	w.end();
	return w.str();
}

// A connected TCP pair on 127.0.0.1, both non-blocking: *gameEnd for the game's socket, *relayEnd for us.
bool MakePair(SOCKET *gameEnd, SOCKET *relayEnd) {
	SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET)
		return false;
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(addr);
	SOCKET a = INVALID_SOCKET, b = INVALID_SOCKET;
	if (bind(listener, (sockaddr *)&addr, sizeof(addr)) == 0 && listen(listener, 1) == 0 &&
		getsockname(listener, (sockaddr *)&addr, &len) == 0) {
		a = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (a != INVALID_SOCKET && connect(a, (sockaddr *)&addr, sizeof(addr)) == 0) {
			sockaddr_in mine{}, from{};
			socklen_t mineLen = sizeof(mine), fromLen = sizeof(from);
			getsockname(a, (sockaddr *)&mine, &mineLen);
			b = accept(listener, (sockaddr *)&from, &fromLen);
			// Someone else's connection to our listener would be a bug or an attack; don't relay it.
			if (b != INVALID_SOCKET && from.sin_port != mine.sin_port) {
				closesocket(b);
				b = INVALID_SOCKET;
			}
		}
	}
	closesocket(listener);
	if (a == INVALID_SOCKET || b == INVALID_SOCKET) {
		if (a != INVALID_SOCKET)
			closesocket(a);
		ERROR_LOG(Log::sceNet, "NpRelay: can't make a loopback pair (errno %d)", socket_errno);
		return false;
	}
	const int one = 1;
	for (SOCKET s : { a, b }) {
		fd_util::SetNonBlocking((int)s, true);
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	}
	*gameEnd = a;
	*relayEnd = b;
	return true;
}

void CloseEnd(Stream &s) {
	std::lock_guard<std::mutex> guard(s.writeMutex);
	if (s.endClosed)
		return;
	if (s.reset) {
		linger l{};
		l.l_onoff = 1;
		l.l_linger = 0;
		setsockopt(s.end, SOL_SOCKET, SO_LINGER, (const char *)&l, sizeof(l));
	}
	closesocket(s.end);
	s.endClosed = true;
}

void RequestCloseLocked(Stream &s, bool reset) {
	s.reset = s.reset || reset;
	s.closeRequested = true;
	if (s.id)
		g_byId.erase(s.id);
	WakeLocked();
}

// Reader thread: data for the game. Waits while the game's receive buffer is full (the server holds the sender
// back meanwhile).
void WriteToEnd(Stream &s, const char *data, size_t size) {
	std::lock_guard<std::mutex> guard(s.writeMutex);
	int stalledMs = 0;
	while (size > 0 && !s.endClosed && !s.closeRequested && g_pumpRun) {
		const int n = send(s.end, data, (int)size, MSG_NOSIGNAL);
		if (n > 0) {
			data += n;
			size -= n;
			stalledMs = 0;
			continue;
		}
		if (n < 0 && WouldBlock(socket_errno) && stalledMs < WRITE_STALL_MS) {
			fd_set wr;
			FD_ZERO(&wr);
			FD_SET(s.end, &wr);
			timeval tv{ 0, 50000 };
			select((int)s.end + 1, nullptr, &wr, nullptr, &tv);
			stalledMs += 50;
			continue;
		}
		WARN_LOG(Log::sceNet, "NpRelay: stream %u: can't pass %d bytes on to the game (errno %d), closing it", s.id, (int)size, socket_errno);
		std::lock_guard<std::mutex> lock(g_mutex);
		RequestCloseLocked(s, true);
		break;
	}
}

// Moves what the game sends on relayed sockets to the server, and closes ends.
void PumpLoop() {
	SetCurrentThreadName("NpRelayPump");
	std::vector<char> buf(4 + MAX_DATA);
	while (g_pumpRun) {
		std::vector<StreamPtr> reading, closing;
		SOCKET wake;
		{
			std::lock_guard<std::mutex> guard(g_mutex);
			wake = g_wake;
			for (auto it = g_streams.begin(); it != g_streams.end(); ) {
				if ((*it)->closeRequested) {
					closing.push_back(*it);
					it = g_streams.erase(it);
					continue;
				}
				// Unanswered opens aren't read: what the game sends meanwhile waits in the socket.
				if ((*it)->state == Status::Open)
					reading.push_back(*it);
				++it;
			}
		}
		for (const StreamPtr &s : closing)
			CloseEnd(*s);

		fd_set rd;
		FD_ZERO(&rd);
		FD_SET(wake, &rd);
		SOCKET maxSock = wake;
		for (const StreamPtr &s : reading) {
			FD_SET(s->end, &rd);
			maxSock = std::max(maxSock, s->end);
		}
		timeval tv{ 0, 200000 };
		if (select((int)maxSock + 1, &rd, nullptr, nullptr, &tv) <= 0)
			continue;
		if (FD_ISSET(wake, &rd)) {
			char drain[64];
			while (recv(wake, drain, sizeof(drain), 0) > 0) {
			}
		}
		for (const StreamPtr &s : reading) {
			if (!FD_ISSET(s->end, &rd))
				continue;
			memcpy(&buf[0], &s->id, 4);
			const int got = recv(s->end, &buf[4], (int)MAX_DATA, 0);
			if (got > 0) {
				Send(MSG_RELAY_DATA, FLAG_RAW, 0, buf.data(), 4 + got);
				continue;
			}
			if (got < 0 && WouldBlock(socket_errno))
				continue;
			// The game closed its socket (or it failed).
			bool tell;
			{
				std::lock_guard<std::mutex> guard(g_mutex);
				tell = !s->remoteClosed && g_active;
				RequestCloseLocked(*s, got < 0);
			}
			INFO_LOG(Log::sceNet, "NpRelay: stream %u to %s:%d closed by the game", s->id, AddrStr(s->peer).c_str(), s->peerPort);
			if (tell)
				SendJson(MSG_RELAY_CLOSE, 0, StreamJson(s->id));
		}
	}
}

void OnOpenAnswer(u32 seq, const json::JsonGet &root) {
	const u32 id = (u32)JsonNum(root, "stream");
	std::unique_lock<std::mutex> guard(g_mutex);
	auto it = g_opening.find(seq);
	if (it == g_opening.end()) {
		// A blocking connect() gave up meanwhile: close what the server opened.
		guard.unlock();
		if (id)
			SendJson(MSG_RELAY_CLOSE, 0, StreamJson(id));
		return;
	}
	StreamPtr s = it->second;
	g_opening.erase(it);
	if (id && s->state == Status::Opening) {
		s->id = id;
		s->state = Status::Open;
		g_byId[id] = s;
		INFO_LOG(Log::sceNet, "NpRelay: stream %u to %s:%d open", id, AddrStr(s->peer).c_str(), s->peerPort);
	} else {
		s->state = Status::Refused;
		RequestCloseLocked(*s, true);
		INFO_LOG(Log::sceNet, "NpRelay: connect to %s:%d refused (%08x)", AddrStr(s->peer).c_str(), s->peerPort, (u32)JsonNum(root, "error"));
	}
	WakeLocked();
	g_cv.notify_all();
}

void OnIncoming(const json::JsonGet &root) {
	const u32 id = (u32)JsonNum(root, "stream");
	const u16 srcPort = (u16)JsonNum(root, "srcPort");
	const u16 dstPort = (u16)JsonNum(root, "dstPort");
	u32 src = 0;
	ParseAddr(root.getStringOrNull("src"), &src);

	bool listening;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		listening = g_active && g_listeners.count(dstPort);
	}
	SOCKET gameEnd, relayEnd;
	bool ok = false;
	if (listening && MakePair(&gameEnd, &relayEnd)) {
		std::lock_guard<std::mutex> guard(g_mutex);
		auto l = g_listeners.find(dstPort);
		if (g_active && l != g_listeners.end()) {
			auto s = std::make_shared<Stream>();
			s->id = id;
			s->end = relayEnd;
			s->peer = src;
			s->peerPort = srcPort;
			s->localPort = dstPort;
			s->state = Status::Open;
			g_streams.push_back(s);
			g_byId[id] = s;
			g_acceptQueue[l->second].push_back({ gameEnd, s });
			WakeLocked();
			ok = true;
		} else {
			closesocket(gameEnd);
			closesocket(relayEnd);
		}
	}
	INFO_LOG(Log::sceNet, "NpRelay: %s:%d connects to our port %d: %s (stream %u)", AddrStr(src).c_str(), srcPort, dstPort,
		ok ? "queued for accept()" : "refused, nothing listens", id);
	json::JsonWriter w;
	w.begin();
	w.writeUint("stream", id);
	w.writeBool("ok", ok);
	w.end();
	SendJson(MSG_RELAY_ACCEPT, 0, w.str());
}

void OnRemoteClose(const json::JsonGet &root) {
	const u32 id = (u32)JsonNum(root, "stream");
	StreamPtr s;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		auto it = g_byId.find(id);
		if (it == g_byId.end())
			return;
		s = it->second;
		s->remoteClosed = true;
	}
	INFO_LOG(Log::sceNet, "NpRelay: stream %u to %s:%d closed by the peer", id, AddrStr(s->peer).c_str(), s->peerPort);
	// EOF for the game once it has read what came before. The pump closes our end when the game closes its socket.
	std::lock_guard<std::mutex> guard(s->writeMutex);
	if (!s->endClosed)
		shutdown(s->end, SHUT_WR);
}

void OnData(const std::string &body) {
	if (body.size() < 4)
		return;
	u32 id;
	memcpy(&id, body.data(), 4);
	StreamPtr s;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		auto it = g_byId.find(id);
		if (it == g_byId.end())
			return;
		s = it->second;
	}
	WriteToEnd(*s, body.data() + 4, body.size() - 4);
}

// For the UDP socket bound to dstPort, if there is one.
void QueueDatagramLocked(u16 dstPort, Datagram &&d) {
	auto it = g_udpReceivers.find(dstPort);
	if (it == g_udpReceivers.end()) {
		if (g_dropLogCount++ < 20)
			WARN_LOG(Log::sceNet, "NpRelay: datagram from %s:%d to our port %d dropped: no socket there", AddrStr(d.src).c_str(), d.srcPort, dstPort);
		return;
	}
	auto &queue = g_datagrams[it->second];
	if (queue.size() >= MAX_QUEUED_DATAGRAMS)
		queue.pop_front();
	queue.push_back(std::move(d));
}

void OnDatagram(const std::string &body) {
	if (body.size() < 8)
		return;
	Datagram d;
	u16 dstPort;
	memcpy(&d.src, body.data(), 4);
	memcpy(&dstPort, body.data() + 4, 2);
	memcpy(&d.srcPort, body.data() + 6, 2);
	d.data = body.substr(8);
	std::lock_guard<std::mutex> guard(g_mutex);
	QueueDatagramLocked(dstPort, std::move(d));
}

// Drops every game socket's relay state. Queued connections are closed, which ends their streams.
void ResetSocketsLocked() {
	for (auto &it : g_acceptQueue) {
		for (QueuedAccept &q : it.second)
			closesocket(q.gameEnd);
	}
	g_acceptQueue.clear();
	g_accepting.clear();
	g_bySocket.clear();
	g_listeners.clear();
	g_udpReceivers.clear();
	g_datagrams.clear();
	g_defaultDest.clear();
	g_selfPeer.clear();
	g_selfPorts.clear();
}

void StopLocked() {
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		if (!g_active && !g_pump.joinable())
			return;
		g_active = false;
		g_pumpRun = false;
		WakeLocked();
		g_opening.clear();
		for (auto &it : g_bySocket) {
			if (it.second->state == Status::Opening || it.second->state == Status::Open)
				it.second->state = Status::Reset;
		}
		g_cv.notify_all();
	}
	if (g_pump.joinable())
		g_pump.join();
	std::vector<StreamPtr> streams;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		streams.swap(g_streams);
		g_byId.clear();
		// Queued connections were never seen by the game: just drop them. Datagrams too.
		for (auto &it : g_acceptQueue) {
			for (QueuedAccept &q : it.second)
				closesocket(q.gameEnd);
		}
		g_acceptQueue.clear();
		g_accepting.clear();
		g_datagrams.clear();
		if (g_wake != INVALID_SOCKET)
			closesocket(g_wake);
		g_wake = INVALID_SOCKET;
		g_send = nullptr;
	}
	// The game's sockets get a reset (ECONNRESET).
	for (const StreamPtr &s : streams) {
		s->reset = true;
		CloseEnd(*s);
	}
	INFO_LOG(Log::sceNet, "NpRelay: stopped, %d relayed connection(s) reset", (int)streams.size());
}

}  // namespace

void Start(u32 vaddr, u32 net, u32 mask, SendFrameFn send) {
	std::lock_guard<std::mutex> startStop(g_startStopMutex);
	StopLocked();
	SOCKET wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	socklen_t len = sizeof(addr);
	if (wake == INVALID_SOCKET || bind(wake, (sockaddr *)&addr, sizeof(addr)) != 0 || getsockname(wake, (sockaddr *)&addr, &len) != 0) {
		ERROR_LOG(Log::sceNet, "NpRelay: can't set up (errno %d); peers stay unreachable through the relay", socket_errno);
		if (wake != INVALID_SOCKET)
			closesocket(wake);
		return;
	}
	fd_util::SetNonBlocking((int)wake, true);
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		g_wake = wake;
		g_wakeAddr = addr;
		g_vaddr = vaddr;
		g_net = net & mask;
		g_mask = mask;
		g_send = std::move(send);
		g_active = true;
	}
	g_pumpRun = true;
	g_pump = std::thread(PumpLoop);
	int prefix = 0;
	for (u32 m = ntohl(mask); m & 0x80000000; m <<= 1)
		prefix++;
	INFO_LOG(Log::sceNet, "NpRelay: our address is %s; peers in %s/%d go through the matching server", AddrStr(vaddr).c_str(),
		AddrStr(net & mask).c_str(), prefix);
}

void Stop() {
	std::lock_guard<std::mutex> startStop(g_startStopMutex);
	StopLocked();
}

bool HandleFrame(u16 type, u16 flags, u32 seq, const std::string &body) {
	const bool raw = (flags & FLAG_RAW) != 0;
	switch (type) {
	case MSG_RELAY_DATA:
		if (raw)
			OnData(body);
		return true;
	case MSG_RELAY_DATAGRAM:
		if (raw)
			OnDatagram(body);
		return true;
	case MSG_PUSH_RELAY_INCOMING:
	case MSG_RELAY_CLOSE:
	case MSG_RELAY_OPEN | MSG_RESPONSE:
		break;
	default:
		return false;
	}
	json::JsonReader reader(body.data(), body.size());
	if (raw || !reader.ok()) {
		ERROR_LOG(Log::sceNet, "NpRelay: bad body in frame %04x", type);
		return true;
	}
	const json::JsonGet root = reader.root();
	if (type == MSG_PUSH_RELAY_INCOMING)
		OnIncoming(root);
	else if (type == MSG_RELAY_CLOSE)
		OnRemoteClose(root);
	else
		OnOpenAnswer(seq, root);
	return true;
}

bool IsRelayAddr(u32 addr) {
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_active && (addr & g_mask) == g_net;
}

bool IsOwnAddr(u32 addr) {
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_active && addr == g_vaddr;
}

u32 OwnAddr() {
	std::lock_guard<std::mutex> guard(g_mutex);
	return g_active ? g_vaddr : 0;
}

Status SocketStatus(int socket) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto it = g_bySocket.find(socket);
	return it == g_bySocket.end() ? Status::None : it->second->state;
}

int Connect(int socket, SOCKET *hostSock, bool blocking, u32 dst, u16 dstPort, u16 srcPort) {
	SOCKET gameEnd, relayEnd;
	if (!MakePair(&gameEnd, &relayEnd))
		return ENOBUFS;
	auto s = std::make_shared<Stream>();
	s->end = relayEnd;
	s->peer = dst;
	s->peerPort = dstPort;
	s->localPort = srcPort;
	u32 seq;
	{
		std::lock_guard<std::mutex> guard(g_mutex);
		if (!g_active) {
			closesocket(gameEnd);
			closesocket(relayEnd);
			return ENETUNREACH;
		}
		seq = g_nextSeq++;
		if (g_nextSeq == 0)
			g_nextSeq = 0x40000000;
		g_opening[seq] = s;
		g_streams.push_back(s);
		g_bySocket[socket] = s;
	}
	closesocket(*hostSock);
	*hostSock = gameEnd;

	json::JsonWriter w;
	w.begin();
	w.writeString("dst", AddrStr(dst));
	w.writeUint("dstPort", dstPort);
	w.writeUint("srcPort", srcPort);
	w.end();
	INFO_LOG(Log::sceNet, "NpRelay: socket %d connects to %s:%d (from port %d)", socket, AddrStr(dst).c_str(), dstPort, srcPort);
	if (!SendJson(MSG_RELAY_OPEN, seq, w.str())) {
		std::lock_guard<std::mutex> guard(g_mutex);
		g_opening.erase(seq);
		s->state = Status::Reset;
		RequestCloseLocked(*s, true);
		return ECONNRESET;
	}
	if (!blocking)
		return 0;

	std::unique_lock<std::mutex> lock(g_mutex);
	g_cv.wait_for(lock, std::chrono::milliseconds(CONNECT_TIMEOUT_MS), [&] { return s->state != Status::Opening; });
	switch (s->state) {
	case Status::Open:
		return 0;
	case Status::Opening:
		g_opening.erase(seq);
		s->state = Status::Refused;
		RequestCloseLocked(*s, true);
		return ETIMEDOUT;
	case Status::Refused:
		return ECONNREFUSED;
	default:
		return ECONNRESET;
	}
}

void SetDefaultDest(int socket, u32 dst, u16 dstPort) {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_defaultDest[socket] = { dst, dstPort };
}

bool GetDefaultDest(int socket, u32 *dst, u16 *dstPort) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto it = g_defaultDest.find(socket);
	if (it == g_defaultDest.end())
		return false;
	*dst = it->second.first;
	*dstPort = it->second.second;
	return true;
}

bool GetPeer(int socket, u32 *addr, u16 *port) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto it = g_bySocket.find(socket);
	if (it != g_bySocket.end()) {
		*addr = it->second->peer;
		*port = it->second->peerPort;
		return true;
	}
	for (const auto *peers : { &g_defaultDest, &g_selfPeer }) {
		auto d = peers->find(socket);
		if (d != peers->end()) {
			*addr = d->second.first;
			*port = d->second.second;
			return true;
		}
	}
	return false;
}

void NoteSelfConnect(int socket, u16 localPort, u16 dstPort) {
	std::lock_guard<std::mutex> guard(g_mutex);
	if (!g_active)
		return;
	g_selfPeer[socket] = { g_vaddr, dstPort };
	if (localPort)
		g_selfPorts[localPort] = socket;
}

bool AcceptedFromSelf(int socket, u16 peerPort, u32 *vaddr) {
	std::lock_guard<std::mutex> guard(g_mutex);
	if (!g_active || !g_selfPorts.count(peerPort))
		return false;
	g_selfPeer[socket] = { g_vaddr, peerPort };
	*vaddr = g_vaddr;
	return true;
}

void Listen(int socket, u16 port) {
	if (port == 0)
		return;
	std::lock_guard<std::mutex> guard(g_mutex);
	g_listeners[port] = socket;
}

bool Accept(int listenSocket, SOCKET *hostSock, u32 *peer, u16 *peerPort, u32 *token) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto it = g_acceptQueue.find(listenSocket);
	if (it == g_acceptQueue.end() || it->second.empty())
		return false;
	QueuedAccept q = it->second.front();
	it->second.pop_front();
	*hostSock = q.gameEnd;
	*peer = q.stream->peer;
	*peerPort = q.stream->peerPort;
	*token = g_nextToken++;
	g_accepting[*token] = q.stream;
	return true;
}

void Accepted(int socket, u32 token) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto it = g_accepting.find(token);
	if (it == g_accepting.end())
		return;
	g_bySocket[socket] = it->second;
	g_accepting.erase(it);
}

void BindUdp(int socket, u16 port) {
	if (port == 0)
		return;
	std::lock_guard<std::mutex> guard(g_mutex);
	g_udpReceivers.emplace(port, socket);  // the first one keeps it
}

bool SendDatagram(u32 dst, u16 dstPort, u16 srcPort, const u8 *data, size_t size) {
	{
		// To ourselves: the server wouldn't pass it back, so deliver it here, from our vaddr.
		std::lock_guard<std::mutex> guard(g_mutex);
		if (g_active && dst == g_vaddr) {
			QueueDatagramLocked(dstPort, Datagram{ g_vaddr, srcPort, std::string((const char *)data, size) });
			return true;
		}
	}
	std::string body(8, '\0');
	memcpy(&body[0], &dst, 4);
	memcpy(&body[4], &dstPort, 2);
	memcpy(&body[6], &srcPort, 2);
	body.append((const char *)data, size);
	return Send(MSG_RELAY_DATAGRAM, FLAG_RAW, 0, body.data(), body.size());
}

int RecvDatagram(int socket, u8 *buf, int bufSize, u32 *src, u16 *srcPort) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto it = g_datagrams.find(socket);
	if (it == g_datagrams.end() || it->second.empty())
		return -1;
	Datagram &d = it->second.front();
	const int n = std::min(bufSize, (int)d.data.size());
	if (n > 0)
		memcpy(buf, d.data.data(), n);
	*src = d.src;
	*srcPort = d.srcPort;
	it->second.pop_front();
	return n;
}

bool Readable(int socket) {
	std::lock_guard<std::mutex> guard(g_mutex);
	auto d = g_datagrams.find(socket);
	if (d != g_datagrams.end() && !d->second.empty())
		return true;
	auto a = g_acceptQueue.find(socket);
	return a != g_acceptQueue.end() && !a->second.empty();
}

void SocketClosed(int socket) {
	std::lock_guard<std::mutex> guard(g_mutex);
	g_bySocket.erase(socket);
	g_defaultDest.erase(socket);
	g_selfPeer.erase(socket);
	g_datagrams.erase(socket);
	for (auto it = g_selfPorts.begin(); it != g_selfPorts.end(); ) {
		it = it->second == socket ? g_selfPorts.erase(it) : std::next(it);
	}
	for (auto it = g_listeners.begin(); it != g_listeners.end(); ) {
		it = it->second == socket ? g_listeners.erase(it) : std::next(it);
	}
	for (auto it = g_udpReceivers.begin(); it != g_udpReceivers.end(); ) {
		it = it->second == socket ? g_udpReceivers.erase(it) : std::next(it);
	}
	auto q = g_acceptQueue.find(socket);
	if (q != g_acceptQueue.end()) {
		for (QueuedAccept &a : q->second)
			closesocket(a.gameEnd);  // the pump sees EOF on our end and closes the stream
		g_acceptQueue.erase(q);
	}
}

void SocketsReset() {
	std::lock_guard<std::mutex> guard(g_mutex);
	ResetSocketsLocked();
}

}  // namespace NpRelay
