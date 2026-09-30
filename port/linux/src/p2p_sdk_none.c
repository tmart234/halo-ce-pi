/*
P2P_SDK_NONE.C

The fpp SDK calls internet play makes (p2p.c), for a build without the SDK
(Android, whose game code runs as a 32-bit guest the Rust SDK has no target
for). Each fails, so p2p_identifier makes no session key, and
p2p_initialize leaves internet play off: its old tunnel, whose keys every
invite holder could read, is gone, and there is no insecure one to fall
back to. The Linux and Windows builds link the SDK itself (HALO_FPP,
tools/fpp_sdk.py), and this file is empty in them.
*/

#ifndef HALO_FPP

#include "fpp.h"

enum FppStatus fpp_signer_generate(struct FppSigner **out)
{
	*out = NULL;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_signer_public_key(const struct FppSigner *signer, uint8_t *out)
{
	(void)signer;
	(void)out;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_signer_from_seed(const uint8_t *seed, struct FppSigner **out)
{
	(void)seed;
	*out = NULL;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_ar_verify(const uint8_t *ar, size_t len, const uint8_t *verifier_keys, size_t verifier_key_count,
	const uint8_t *session_public_key, uint64_t now_s, uint8_t minimum_tier, struct FppArInfo *info)
{
	(void)ar;
	(void)len;
	(void)verifier_keys;
	(void)verifier_key_count;
	(void)session_public_key;
	(void)now_s;
	(void)minimum_tier;
	(void)info;
	return FPP_STATUS_INTERNAL;
}

const char *fpp_status_str(int status)
{
	(void)status;
	return "no SDK in this build";
}

void fpp_signer_free(struct FppSigner *signer)
{
	(void)signer;
}

enum FppStatus fpp_p2p_keypair_generate(uint8_t *private_out, uint8_t *public_out)
{
	(void)private_out;
	(void)public_out;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_host_new(const uint8_t *static_private, const uint8_t *invite_secret,
	const uint8_t *instance_public_key, const uint8_t *hello, size_t hello_len, uint32_t max_peers,
	struct FppP2pHost **out)
{
	(void)static_private;
	(void)invite_secret;
	(void)instance_public_key;
	(void)hello;
	(void)hello_len;
	(void)max_peers;
	*out = NULL;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_host_recv(struct FppP2pHost *host, const uint8_t *from, size_t from_len,
	const uint8_t *datagram, size_t len)
{
	(void)host;
	(void)from;
	(void)from_len;
	(void)datagram;
	(void)len;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_host_send(struct FppP2pHost *host, uint32_t peer, const uint8_t *payload, size_t len)
{
	(void)host;
	(void)peer;
	(void)payload;
	(void)len;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_host_tick(struct FppP2pHost *host, uint64_t now_ms)
{
	(void)host;
	(void)now_ms;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_host_disconnect(struct FppP2pHost *host, uint32_t peer, uint16_t reason)
{
	(void)host;
	(void)peer;
	(void)reason;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_host_poll_transmit(struct FppP2pHost *host, uint8_t *to, size_t to_cap, size_t *to_len,
	uint8_t *packet, size_t cap, size_t *len)
{
	(void)host;
	(void)to;
	(void)to_cap;
	(void)to_len;
	(void)packet;
	(void)cap;
	(void)len;
	return FPP_STATUS_EMPTY;
}

enum FppStatus fpp_p2p_host_poll_event(struct FppP2pHost *host, struct FppP2pEvent *event, uint8_t *data,
	size_t cap)
{
	(void)host;
	(void)event;
	(void)data;
	(void)cap;
	return FPP_STATUS_EMPTY;
}

enum FppStatus fpp_p2p_joiner_new(const uint8_t *host_static_public, const uint8_t *invite_secret,
	const struct FppSigner *session_key, const uint8_t *attestation, size_t attestation_len, const uint8_t *hello,
	size_t hello_len, const uint8_t *host_addr, size_t host_addr_len, struct FppP2pJoiner **out)
{
	(void)host_static_public;
	(void)invite_secret;
	(void)session_key;
	(void)attestation;
	(void)attestation_len;
	(void)hello;
	(void)hello_len;
	(void)host_addr;
	(void)host_addr_len;
	*out = NULL;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_joiner_recv(struct FppP2pJoiner *joiner, const uint8_t *from, size_t from_len,
	const uint8_t *datagram, size_t len)
{
	(void)joiner;
	(void)from;
	(void)from_len;
	(void)datagram;
	(void)len;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_joiner_send(struct FppP2pJoiner *joiner, const uint8_t *payload, size_t len)
{
	(void)joiner;
	(void)payload;
	(void)len;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_joiner_tick(struct FppP2pJoiner *joiner, uint64_t now_ms)
{
	(void)joiner;
	(void)now_ms;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_joiner_retry(struct FppP2pJoiner *joiner)
{
	(void)joiner;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_joiner_close(struct FppP2pJoiner *joiner, uint16_t reason)
{
	(void)joiner;
	(void)reason;
	return FPP_STATUS_INTERNAL;
}

enum FppStatus fpp_p2p_joiner_poll_transmit(struct FppP2pJoiner *joiner, uint8_t *to, size_t to_cap,
	size_t *to_len, uint8_t *packet, size_t cap, size_t *len)
{
	(void)joiner;
	(void)to;
	(void)to_cap;
	(void)to_len;
	(void)packet;
	(void)cap;
	(void)len;
	return FPP_STATUS_EMPTY;
}

enum FppStatus fpp_p2p_joiner_poll_event(struct FppP2pJoiner *joiner, struct FppP2pEvent *event, uint8_t *data,
	size_t cap)
{
	(void)joiner;
	(void)event;
	(void)data;
	(void)cap;
	return FPP_STATUS_EMPTY;
}

void fpp_p2p_joiner_free(struct FppP2pJoiner *joiner)
{
	(void)joiner;
}

#endif
