/*
P2P.H

Internet play (p2p.c): machines that shared an invite reach each other's
system link games as if they were on one LAN. xnet.c routes the game's
traffic for them through here; sdl_platform.c passes invite links in and
out through the clipboard.

Addresses and ports are in network byte order.
*/

#ifndef __HALO_LINUX_P2P_H
#define __HALO_LINUX_P2P_H

/* starts internet play, if network.online is set, when the game starts
its networking; local_address is the address the game's sockets are
reached at (network.address, else 127.0.0.1) */
void p2p_initialize(unsigned long local_address);

/* on the desktop, before anything else: if this process was started with
an invite link (halo://join/...) and another copy of the game is running,
passes the link to it and returns nonzero (this one should quit) */
int p2p_hand_off_invite(void);

/* joins the game an invite link or code leads to; text may hold other
words around it. Returns nonzero if it held an invite */
int p2p_join_invite(const char *text);

/* this machine's identifier, which its XNADDR carries (6 bytes) */
const unsigned char *p2p_identifier(void);
/* the address the game reaches the machine with this identifier at, if it
is an internet play peer */
int p2p_peer_address(const unsigned char *identifier, unsigned long *address);

/* a destination the game sends to or connects to (stream: a TCP socket):
if it is a peer's address, the local address standing in for it */
int p2p_outgoing(int stream, unsigned long *address, unsigned short *port);
/* a source the game received from, accepted from or is connected to: if
it is one standing in for a peer, the peer's address */
int p2p_incoming(int stream, unsigned long *address, unsigned short *port);
/* the local addresses standing in for this port (a broadcast's) on every
peer; returns their count */
int p2p_broadcast_targets(unsigned short port, unsigned long *addresses, unsigned short *ports, int maximum_count);

/* the game listens (it is hosting) on socket, or closes a socket */
void p2p_socket_listening(int socket);
void p2p_socket_closed(int socket);

/* text for the clipboard (a new invite link), once; NULL if none. Called
from the main thread */
const char *p2p_take_clipboard_text(void);

/* evidence of an internet play game (p2p_evidence.c, stage H4), from the
game's thread; the ticks are the game's. A game starts (the host names it
for its players); each tick; a player's message to the host on the reliable
channel, with the count of units (hit reports) the host must account for;
the host's receipt of one from the machine at address (network byte order),
which returns the offset of its first unit in the tick's frame; and the
host's decision on a unit (applied, or rejected with a reason code) */
void p2p_evidence_start_match(int is_host, long tick);
void p2p_evidence_tick(long tick);
void p2p_evidence_client_frame(long tick, int units, const void *bytes, int size);
int p2p_evidence_host_frame(unsigned long address, long tick, int units, const void *bytes, int size);
void p2p_evidence_host_outcome(unsigned long address, long tick, int unit, int applied, int reason);

#endif
