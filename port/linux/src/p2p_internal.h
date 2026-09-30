/*
P2P_INTERNAL.H

Shared internals of internet play (p2p.c, p2p_signal.c, p2p_crypto.c,
p2p_discord.c; see p2p.c for the design).
*/

#ifndef __HALO_LINUX_P2P_INTERNAL_H
#define __HALO_LINUX_P2P_INTERNAL_H

#include "p2p.h"

enum
{
	/* a machine's identifier: random, and also its XNADDR's abEnet */
	P2P_IDENTIFIER_SIZE = 6,
	/* an invite's secret */
	P2P_TOKEN_SIZE = 16,
	/* a host's static X25519 public key, which its invites carry */
	P2P_PUBLIC_KEY_SIZE = 32,
	/* the addresses a machine offers to be reached at */
	P2P_MAXIMUM_CANDIDATES = 4,
	/* an invite link's text: "halo://join/", the host's identifier, the
	token and the host's key in hexadecimal (108 digits, within the 128 of
	a Discord secret), and a terminator */
	P2P_LINK_SIZE = 128,
	/* the most machines one tunnels to: a host and the rest of a system
	link game's 128 machines (include/halo_port_limits.h) */
	P2P_MAXIMUM_PEERS = 127,
};

struct p2p_candidate
{
	/* network byte order */
	unsigned long address;
	unsigned short port;
};

/* ---------- p2p.c: what the signalling side calls back */

/* the milliseconds of a monotonic clock */
unsigned long p2p_now(void);
/* looks up a host name (posix_resolve_ipv4), letting go of the p2p lock
while it waits; the p2p thread's */
unsigned long p2p_resolve(const char *host);
/* registers this executable for links of scheme (posix_register_url_scheme),
unless it is an automated run (debug.exit_after, a hidden window, no
renderer), which must not take the links over */
void p2p_register_url_scheme(const char *scheme, const char *description);
/* formats bytes as lower-case hexadecimal (text holds 2 * size + 1) */
void p2p_hex(const unsigned char *bytes, int size, char *text);
/* the addresses this machine can be reached at; returns their count */
int p2p_local_candidates(struct p2p_candidate *candidates, int maximum_count);
/* a joiner (on the host) or the host (on a joiner) offered its addresses
through signalling; the tunnel starts reaching it. Nothing in signalling
is trusted: a host's key comes from the invite, and a session proves who is
at the other end */
void p2p_peer_offered(const unsigned char *identifier, const struct p2p_candidate *candidates, int count,
	int is_host);
/* an invite that arrived on the p2p thread (from Discord, or another copy
of the game) */
void p2p_invite_received(const char *text);

/* the p2p lock, for p2p_evidence.c's calls from the game */
void p2p_lock_enter(void);
void p2p_lock_leave(void);
/* this machine's session key, and the host's instance key (which signs its
Checkpoints), with their public halves (P2P_PUBLIC_KEY_SIZE bytes) */
struct FppSigner *p2p_session_key(void);
struct FppSigner *p2p_instance_key(void);
void p2p_session_public_key(unsigned char *key);
void p2p_instance_public_key(unsigned char *key);
/* the peer (its index) whose address the game has (network byte order), or
-1 */
int p2p_peer_index_for_address(unsigned long address);
/* sends the peer an evidence message on its session's reliable channel */
int p2p_evidence_send(int peer, const void *data, int size);

/* ---------- p2p_evidence.c: what a host was sent and decided (stage H4) */

/* each pass of the p2p thread (hosting: whether this machine hosts) */
void p2p_evidence_update(int hosting);
/* the host: a player admitted (its session key, its slot), or gone */
void p2p_evidence_peer_admitted(int peer, const unsigned char *session_key, unsigned long slot);
void p2p_evidence_peer_left(int peer);
/* a player: the host's instance key, from its session's handshake */
void p2p_evidence_host_key(int peer, const unsigned char *instance_key);
/* a message on a session's reliable channel (from_host: on a player) */
void p2p_evidence_message(int peer, int from_host, const unsigned char *data, int size);
/* (p2p_evidence_start_match with p2p_lock held) */
void p2p_evidence_start_match_locked(int is_host, long tick);

/* ---------- p2p_signal.c: signalling through public MQTT brokers */

/* connects to the brokers, if not already; called from the p2p thread */
void p2p_signal_start(void);
/* adds the signalling sockets to the p2p thread's select lists */
void p2p_signal_select_sets(int *read, int *read_count, int *write, int *write_count, int maximum_count);
/* services the sockets and timers; called from the p2p thread each pass */
void p2p_signal_update(const int *read, int read_count, const int *write, int write_count);
/* hosting: listen for joiners who hold this token */
void p2p_signal_host(const unsigned char *token);
void p2p_signal_stop_hosting(void);
/* joining: ask the host with this identifier, holding this token, until it
answers (or p2p_signal_stop_joining) */
void p2p_signal_join(const unsigned char *host_identifier, const unsigned char *token);
void p2p_signal_stop_joining(void);
/* whether any broker is connected */
int p2p_signal_connected(void);

/* ---------- p2p_crypto.c */

enum
{
	P2P_SHA256_SIZE = 32,
	/* what p2p_seal adds: a 12-byte nonce and a 16-byte tag */
	P2P_SEAL_OVERHEAD = 28,
};

void p2p_sha256(const void *data, int size, unsigned char *digest);
void p2p_hmac_sha256(const unsigned char *key, int key_size, const void *data, int size, unsigned char *digest);
/* encrypts and authenticates plaintext with key into sealed (size +
P2P_SEAL_OVERHEAD bytes); returns the sealed size */
int p2p_seal(const unsigned char *key, const void *plaintext, int size, unsigned char *sealed);
/* the reverse, if sealed was made with key: returns the plaintext size, or
-1 if it was not (or was altered) */
int p2p_open(const unsigned char *key, const unsigned char *sealed, int size, unsigned char *plaintext);

/* ---------- p2p_discord.c: rich presence and invites through the Discord
desktop client */

/* called from the p2p thread each pass */
void p2p_discord_update(void);
/* what to show: hosting with an invite link's secret and player counts, or
not (secret NULL) */
void p2p_discord_set_hosting(const char *secret, int player_count, int maximum_player_count);

#endif
