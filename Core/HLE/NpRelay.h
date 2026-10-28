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

// Peer relay through the matching server (PSP2i revival repo, docs/fork/matching-protocol.md §6 phase 2).
// The server gives each opted-in client a virtual address in 10.92.0.0/16 and shows relayed peers at theirs.
// The game's sceNetInet traffic to such an address goes over the Matching2 connection instead of the network:
//  - TCP: the game's socket becomes one end of a host loopback connection; the relay owns the other end and
//    moves its bytes as RelayData. So recv/send/select on the game's socket stay plain host calls.
//  - UDP: sendto becomes RelayDatagram; incoming datagrams wait in a queue per receiving socket.
// Ports are the game's own (sceNetInet doesn't apply PortOffset).

#include <cstddef>
#include <functional>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Net/SocketCompat.h"

namespace NpRelay {

// ---- Matching2 connection side (NpMatching2Backend)

// Sends one frame; thread-safe. flags bit 0 = raw body.
typedef std::function<bool(u16 type, u16 flags, u32 seq, const char *body, size_t size)> SendFrameFn;

// After HelloOk carried a vaddr. Addresses in network order.
void Start(u32 vaddr, u32 net, u32 mask, SendFrameFn send);
// Matching2 connection gone: every relayed socket fails (ECONNRESET), queued data is dropped. Idempotent.
void Stop();
// Reader thread: takes relay frames. Returns false for anything else.
bool HandleFrame(u16 type, u16 flags, u32 seq, const std::string &body);

// ---- sceNetInet side (emu thread). Socket ids are the game's.

bool IsRelayAddr(u32 addr);
bool IsOwnAddr(u32 addr);
u32 OwnAddr();

enum class Status { None, Opening, Open, Refused, Reset };
Status SocketStatus(int socket);

// connect() to a relay address on a TCP socket: replaces *hostSock (closing the old one) with the game's end of
// a loopback connection. Returns 0 when connected or (non-blocking) when the open is under way, else a host
// errno.
int Connect(int socket, SOCKET *hostSock, bool blocking, u32 dst, u16 dstPort, u16 srcPort);
// UDP connect(): later send()s go to dst.
void SetDefaultDest(int socket, u32 dst, u16 dstPort);
bool GetDefaultDest(int socket, u32 *dst, u16 *dstPort);
bool GetPeer(int socket, u32 *addr, u16 *port);
// connect() to our own vaddr goes over host loopback; these make both ends show the vaddr as the peer.
void NoteSelfConnect(int socket, u16 localPort, u16 dstPort);
// accept(): a host loopback connection from peerPort that NoteSelfConnect made. Returns our vaddr for it.
bool AcceptedFromSelf(int socket, u16 peerPort, u32 *vaddr);

void Listen(int socket, u16 port);
// A relayed connection queued on this listening socket: its host socket and the peer. The caller adopts it
// and then calls Accepted with the new socket id and the returned token.
bool Accept(int listenSocket, SOCKET *hostSock, u32 *peer, u16 *peerPort, u32 *token);
void Accepted(int socket, u32 token);

// The first UDP socket bound to a port receives that port's relayed datagrams.
void BindUdp(int socket, u16 port);
bool SendDatagram(u32 dst, u16 dstPort, u16 srcPort, const u8 *data, size_t size);
// Copies the oldest queued datagram (truncated to bufSize). Returns its size copied, or -1 if none.
int RecvDatagram(int socket, u8 *buf, int bufSize, u32 *src, u16 *srcPort);

// For select/poll: a datagram or an incoming connection is queued.
bool Readable(int socket);

void SocketClosed(int socket);
// sceNetInetTerm: every game socket is gone.
void SocketsReset();

}  // namespace NpRelay
