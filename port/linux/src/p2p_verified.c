/*
P2P_VERIFIED.C

Verified playlists (stage H5 of the mmo repository's
docs/anticheat/08-reference-title-halo.md): a dedicated host blessed by a
region's trust plane, and players matched to it, instead of an invite.

- The host (network.liveness: Server Liveness's address) joins Server
  Liveness when its game starts hosting (fpp_gs_link_*, the fpp SDK's
  gs-link part, which only the Linux build has): with its long-term key
  (network.gs_key_file) and, on a machine with one, its TPM's evidence
  (network.gs_tpm2). Server Liveness gives it a match and a chain of
  Server Attestation Results (SARs) that certify its instance key (which
  signs its Checkpoints) and the static key of its end of the secure
  sessions, every two seconds while it stays blessed. Its invite is never
  used: players come with a place the region's Broker gave them.
- A player (network.ticket_file, written by the mmo repository's
  fpp-ticket) holds a session key, the host's address and key, an
  Attestation Result (AR) from the region's Verifier and a Session
  Admission Token (SAT) from its Broker. It dials the host directly (no
  signalling: a dedicated host has a public address), checks the host's
  SAR chain (it must name the key it dialled, and the key the host signs
  with), and presents its SAT and AR (Admit). It drops the host the moment
  the chain breaks or stops (SAR_LAPSED): a host that lost its blessing
  keeps no players.
- The host admits a player by the SAT and AR (fpp_admission_admit: signed
  by the region's Broker and Verifier, for this host and match, bound to
  the key the player's session proved, its device tier, its measured
  client build (network.client_builds; finding H07), its device not banned
  (network.banned_devices and the Revocation Feed's events; finding H08)),
  and only then lets the game see it. Revocation events Server Liveness
  relays remove the players they name.
- The host signs one Checkpoint every SESSION_EPOCH_MS for Server
  Liveness, which logs it in the region's Transparency Log, and sends it to
  every player (who checks it under the instance key the SAR certifies):
  the roster, and the digest of every evidence Checkpoint (p2p_evidence.c)
  signed since the last. Those keep their own match (a random one, as in
  internet play) and epochs (game time), so the two chains never sign two
  Checkpoints for one match and epoch; the logged chain anchors the other.

The control messages travel on the sessions' reliable channel with a 'V'
in front (p2p_evidence.c's messages have their own letters).

network.client_builds and network.banned_devices also apply to an
invite game's trust policy (p2p.c's admits()), to the AR a player
presents there.

Everything here runs on the p2p thread, under p2p_lock.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "fpp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
	VERIFIED_TAG = 'V',
	MAXIMUM_TOKEN = 4096,
	MAXIMUM_LISTED = 32,
	/* the host's logged Checkpoints: one this often */
	SESSION_EPOCH_MS = 5000,
	/* evidence Checkpoints one logged Checkpoint can anchor */
	MAXIMUM_ANCHORS = 64,
	/* a player: no SAR for this long is a lapse (three issue intervals,
	04-protocol.md §6.3) */
	SAR_GRACE_MS = 6000,
	/* fpp_types::Reason */
	REASON_SAR_LAPSED = 7,
	REASON_SAT_INVALID = 2,
	REASON_SERVER_FULL = 13,
};

static uint64_t unix_seconds(void)
{
	return (uint64_t)time(NULL);
}

/* whether ms milliseconds have passed since a p2p_now() reading */
static int elapsed_since(unsigned long since, unsigned long ms)
{
	return (unsigned long)(p2p_now() - since) >= ms;
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* hexadecimal text (up to a space, comma or line end) into bytes; their
count, or -1 */
static int parse_hex(const char *text, unsigned char *bytes, int maximum, const char **end)
{
	int count = 0;

	while (hex_digit(text[0]) >= 0 && hex_digit(text[1]) >= 0)
	{
		if (count == maximum)
			return -1;
		bytes[count++] = (unsigned char)(hex_digit(text[0]) << 4 | hex_digit(text[1]));
		text += 2;
	}
	if (end)
		*end = text;
	if (*text && *text != ' ' && *text != ',' && *text != '\r' && *text != '\n' && *text != '\t')
		return -1;
	return count;
}

/* a comma-separated list of 64-digit hexadecimal values; their count */
static int parse_list(const char *setting, const char *text, unsigned char (*values)[32], int maximum)
{
	int count = 0;

	while (*text)
	{
		const char *end;

		while (*text == ',' || *text == ' ')
			text++;
		if (!*text)
			break;
		if (count < maximum && parse_hex(text, values[count], 32, &end) == 32)
			count++;
		else
		{
			platform_log("Verified play: %s holds something that is not 64 hexadecimal digits: %.16s...", setting,
				text);
			end = text;
		}
		while (*end && *end != ',')
			end++;
		text = end;
	}
	return count;
}

/* ---------- the title's own lists (also for an invite game's trust policy) */

static struct
{
	int loaded;
	unsigned char builds[MAXIMUM_LISTED][32];
	int build_count;
	unsigned char banned[MAXIMUM_LISTED][32];
	int banned_count;
} lists;

static void load_lists(void)
{
	if (lists.loaded)
		return;
	lists.loaded = 1;
	lists.build_count = parse_list("network.client_builds", config_string("network.client_builds"), lists.builds,
		MAXIMUM_LISTED);
	lists.banned_count = parse_list("network.banned_devices", config_string("network.banned_devices"), lists.banned,
		MAXIMUM_LISTED);
	if (lists.build_count)
		platform_log("Verified play: %d client builds admitted (network.client_builds)", lists.build_count);
	if (lists.banned_count)
		platform_log("Verified play: %d devices banned (network.banned_devices)", lists.banned_count);
}

int p2p_verified_build_admitted(const unsigned char *build)
{
	int index;

	load_lists();
	if (!lists.build_count)
		return 1;
	for (index = 0; index < lists.build_count; index++)
	{
		if (!memcmp(lists.builds[index], build, 32))
			return 1;
	}
	return 0;
}

int p2p_verified_builds_listed(void)
{
	load_lists();
	return lists.build_count > 0;
}

int p2p_verified_device_banned(const unsigned char *did)
{
	int index;

	load_lists();
	for (index = 0; index < lists.banned_count; index++)
	{
		if (!memcmp(lists.banned[index], did, 32))
			return 1;
	}
	return 0;
}

static void send_control(int peer, const unsigned char *message, size_t size)
{
	unsigned char tagged[1 + MAXIMUM_TOKEN + 64];

	if (size + 1 > sizeof(tagged))
		return;
	tagged[0] = VERIFIED_TAG;
	memcpy(tagged + 1, message, size);
	p2p_evidence_send(peer, tagged, (int)size + 1);
}

/* ---------- the host */

struct verified_player
{
	/* joined, waiting for its Admit */
	int pending;
	int admitted;
	unsigned short slot;
};

static struct
{
	int checked;
	int enabled;
	int failed;
	int lapsed;
	FppKeys *keys;
	FppAdmission *admission;
#ifdef FPP_GS_LINK
	FppGsLink *link;
#endif
	unsigned char match_id[16];
	unsigned char sar[MAXIMUM_TOKEN];
	size_t sar_size;
	unsigned long sar_time;
	struct verified_player players[P2P_MAXIMUM_PEERS];
	int admitted_count;
	/* the logged chain */
	uint32_t epoch;
	int has_prev;
	unsigned char prev[32];
	unsigned long epoch_time;
	unsigned char anchors[MAXIMUM_ANCHORS][32];
	int anchor_count;
	unsigned long tick_time;
} host;

int p2p_verified_hosting(void)
{
	if (!host.checked)
	{
		host.checked = 1;
		host.enabled = config_string("network.liveness")[0] != 0;
	}
	return host.enabled;
}

int p2p_verified_host_ready(void)
{
	return host.admission != NULL && !host.lapsed;
}

#ifdef FPP_GS_LINK
static int read_seed(const char *path, unsigned char *seed)
{
	unsigned char bytes[160];
	FILE *file = fopen(path, "rb");
	int size;

	if (!file)
		return 0;
	size = (int)fread(bytes, 1, sizeof(bytes) - 1, file);
	fclose(file);
	if (size == 32)
	{
		memcpy(seed, bytes, 32);
		return 1;
	}
	if (size <= 0)
		return 0;
	bytes[size] = 0;
	{
		const char *text = (const char *)bytes;

		while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t')
			text++;
		return parse_hex(text, seed, 32, NULL) == 32;
	}
}
#endif

int p2p_verified_host_start(const unsigned char *noise_public_key, const char *game_address)
{
#ifdef FPP_GS_LINK
	FppGsLinkConfig link_config;
	FppGsLink *link = NULL;
	unsigned char instance_public_key[32];
	unsigned char seed[32];
	const char *seed_path = config_string("network.gs_key_file");
	int index;
	FppStatus status;

	if (host.admission || host.failed)
		return host.admission != NULL;
	load_lists();
	if (fpp_keys_load_bundle(config_string("network.key_bundle"), &host.keys) != FPP_STATUS_OK)
	{
		platform_log("Verified play: cannot read the region's key bundle %s (network.key_bundle)",
			config_string("network.key_bundle"));
		host.failed = 1;
		return 0;
	}
	if (!seed_path[0] || !read_seed(seed_path, seed))
	{
		/* (Server Liveness knows a host by this key: a development cell
		takes any) */
		platform_log("Verified play: no 32-byte key in network.gs_key_file; joining with a key for this run only");
		posix_random_bytes(seed, sizeof(seed));
	}
	p2p_instance_public_key(instance_public_key);
	memset(&link_config, 0, sizeof(link_config));
	link_config.liveness = config_string("network.liveness");
	link_config.ca_cert = config_string("network.ca_cert");
	link_config.bundle = config_string("network.key_bundle");
	link_config.gs_id = "halo-ce";
	link_config.game_addr = game_address;
	link_config.gs_key_seed = seed;
	link_config.instance_public_key = instance_public_key;
	link_config.noise_public_key = noise_public_key;
	link_config.sw_hash = NULL;
	link_config.tpm2 = config_boolean("network.gs_tpm2") ? 1 : 0;
	link_config.timeout_ms = 10000;
	platform_log("Verified play: joining Server Liveness at %s as %s", link_config.liveness, game_address);
	/* (it takes a network round trip or several: the game's threads must
	not wait on the lock meanwhile) */
	p2p_lock_leave();
	status = fpp_gs_link_connect(&link_config, &link);
	p2p_lock_enter();
	memset(seed, 0, sizeof(seed));
	if (status != FPP_STATUS_OK)
	{
		platform_log("Verified play: Server Liveness did not admit this host (%s); it hosts no verified games",
			fpp_status_str((int)status));
		host.failed = 1;
		return 0;
	}
	host.link = link;
	fpp_gs_link_match_id(link, host.match_id);
	status = fpp_admission_new(host.keys, instance_public_key, host.match_id,
		(uint8_t)(config_integer("network.minimum_tier") > 0 ? config_integer("network.minimum_tier") : 0),
		&host.admission);
	if (status != FPP_STATUS_OK)
	{
		platform_log("Verified play: admission failed to start (%s)", fpp_status_str((int)status));
		fpp_gs_link_free(link);
		host.link = NULL;
		host.failed = 1;
		return 0;
	}
	for (index = 0; index < lists.build_count; index++)
		fpp_admission_add_client_build(host.admission, lists.builds[index]);
	for (index = 0; index < lists.banned_count; index++)
		fpp_admission_ban_device(host.admission, lists.banned[index]);
	host.epoch_time = p2p_now();
	host.sar_time = p2p_now();
	{
		char text[33];

		p2p_hex(host.match_id, 16, text);
		platform_log("Verified play: blessed by Server Liveness; match %s (players come matched by the Broker)", text);
	}
	return 1;
#else
	(void)noise_public_key;
	(void)game_address;
	if (!host.failed)
		platform_log("Verified play: this build cannot host verified games (no gs-link in its fpp SDK)");
	host.failed = 1;
	return 0;
#endif
}

static void host_send_sar(int peer)
{
	unsigned char message[MAXIMUM_TOKEN + 64];
	size_t size;

	if (host.sar_size &&
		fpp_control_sar_update(host.sar, host.sar_size, message, sizeof(message), &size) == FPP_STATUS_OK)
	{
		send_control(peer, message, size);
	}
}

static void host_refuse(int peer, unsigned short reason, int kick)
{
	unsigned char message[64];
	size_t size;

	if (fpp_control_refuse(reason, (uint8_t)kick, message, sizeof(message), &size) == FPP_STATUS_OK)
		send_control(peer, message, size);
	if (host.players[peer].admitted)
	{
		/* (a revocation's removal has freed the slot already) */
		fpp_admission_remove(host.admission, host.players[peer].slot);
		host.admitted_count--;
	}
	memset(&host.players[peer], 0, sizeof(host.players[peer]));
	p2p_peer_refuse(peer, reason);
}

void p2p_verified_host_left(int peer);

void p2p_verified_host_joined(int peer)
{
	if (peer < 0 || peer >= P2P_MAXIMUM_PEERS)
		return;
	/* (the same machine again, in a new session: its old one is gone) */
	p2p_verified_host_left(peer);
	if (!p2p_verified_host_ready())
	{
		p2p_peer_refuse(peer, REASON_SAR_LAPSED);
		return;
	}
	host.players[peer].pending = 1;
	host_send_sar(peer);
}

void p2p_verified_host_left(int peer)
{
	if (peer < 0 || peer >= P2P_MAXIMUM_PEERS)
		return;
	if (host.players[peer].admitted)
	{
		if (host.admission)
			fpp_admission_remove(host.admission, host.players[peer].slot);
		host.admitted_count--;
	}
	memset(&host.players[peer], 0, sizeof(host.players[peer]));
}

int p2p_verified_host_admitted(int peer)
{
	return peer >= 0 && peer < P2P_MAXIMUM_PEERS && host.players[peer].admitted;
}

static void host_admit(int peer, const unsigned char *tokens, const FppControl *control)
{
	unsigned char key[FPP_SESSION_KEY_MAX];
	size_t key_size = sizeof(key);
	FppAdmitted admitted;
	FppStatus status;
	unsigned char message[64];
	size_t size;

	if (!p2p_peer_session_key(peer, key, &key_size))
	{
		host_refuse(peer, REASON_SAT_INVALID, 0);
		return;
	}
	status = fpp_admission_admit(host.admission, tokens, control->first_len, tokens + control->first_len,
		control->second_len, key, key_size, unix_seconds(), &admitted);
	if (status != FPP_STATUS_OK)
	{
		platform_log("Verified play: refused player %d: %s (%s)", peer, fpp_reason_str(admitted.reason),
			fpp_status_str((int)status));
		host_refuse(peer, admitted.reason, 0);
		return;
	}
	host.players[peer].pending = 0;
	host.players[peer].admitted = 1;
	host.players[peer].slot = admitted.slot;
	host.admitted_count++;
	{
		char did[17];

		p2p_hex(admitted.did, 8, did);
		platform_log("Verified play: admitted player %d to slot %u: a D%u %s device (%s..), queue %s", peer,
			(unsigned)admitted.slot, (unsigned)admitted.tier, (const char *)admitted.platform, did,
			(const char *)admitted.queue);
	}
	if (fpp_control_admitted(admitted.slot, 0, message, sizeof(message), &size) == FPP_STATUS_OK)
		send_control(peer, message, size);
	p2p_peer_admit(peer);
}

int p2p_verified_host_message(int peer, const unsigned char *data, int size)
{
	static unsigned char tokens[2 * MAXIMUM_TOKEN];
	FppControl control;

	if (size < 1 || data[0] != VERIFIED_TAG)
		return 0;
	if (peer < 0 || peer >= P2P_MAXIMUM_PEERS || !host.admission)
		return 1;
	if (fpp_control_decode(data + 1, (size_t)(size - 1), &control, tokens, sizeof(tokens)) != FPP_STATUS_OK)
		return 1;
	if (control.kind == FPP_CONTROL_KIND_ADMIT && host.players[peer].pending)
		host_admit(peer, tokens, &control);
	return 1;
}

void p2p_verified_anchor(const unsigned char *digest)
{
	if (!host.admission || host.anchor_count >= MAXIMUM_ANCHORS)
		return;
	memcpy(host.anchors[host.anchor_count++], digest, 32);
}

/* the logged chain's next Checkpoint: to Server Liveness, and every player */
static void host_checkpoint(void)
{
	FppCheckpointBuilder *builder = NULL;
	unsigned char build_id[32];
	unsigned char object[MAXIMUM_TOKEN];
	unsigned char message[MAXIMUM_TOKEN + 64];
	size_t object_size, size;
	int index;
	int signed_ok;

	memset(build_id, 0, sizeof(build_id));
	/* (its ticks: the game's 30 a second since the host was blessed) */
	if (fpp_checkpoint_begin(host.match_id, build_id, 0, host.epoch, host.epoch * (SESSION_EPOCH_MS * 30 / 1000),
		(host.epoch + 1) * (SESSION_EPOCH_MS * 30 / 1000) - 1, host.has_prev ? host.prev : NULL,
		&builder) != FPP_STATUS_OK)
	{
		return;
	}
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (host.players[index].admitted)
		{
			unsigned char leaf[2];

			leaf[0] = (unsigned char)host.players[index].slot;
			leaf[1] = (unsigned char)(host.players[index].slot >> 8);
			fpp_checkpoint_add_roster(builder, leaf, sizeof(leaf));
		}
	}
	/* the evidence chain's Checkpoints since the last ('E' and its digest) */
	for (index = 0; index < host.anchor_count; index++)
	{
		unsigned char event[33];

		event[0] = 'E';
		memcpy(event + 1, host.anchors[index], 32);
		fpp_checkpoint_add_event(builder, event, sizeof(event));
	}
	signed_ok = fpp_checkpoint_sign(builder, p2p_instance_key(), object, sizeof(object), &object_size) == FPP_STATUS_OK &&
		fpp_object_digest(object, object_size, host.prev) == FPP_STATUS_OK;
	fpp_checkpoint_free(builder);
	if (!signed_ok)
		return;
	host.has_prev = 1;
	host.anchor_count = 0;
	host.epoch++;
#ifdef FPP_GS_LINK
	if (host.link)
		fpp_gs_link_submit_checkpoint(host.link, object, object_size);
#endif
	if (fpp_control_checkpoint_head(object, object_size, message, sizeof(message), &size) == FPP_STATUS_OK)
	{
		for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
		{
			if (host.players[index].admitted)
				send_control(index, message, size);
		}
	}
}

static void host_lapse(const char *why)
{
	int index;

	if (host.lapsed)
		return;
	host.lapsed = 1;
	platform_log("Verified play: %s; this host is no longer blessed, and lets its players go", why);
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (host.players[index].pending || host.players[index].admitted)
			host_refuse(index, REASON_SAR_LAPSED, 1);
	}
}

void p2p_verified_host_update(void)
{
	int index;

	if (!host.admission || host.lapsed)
		return;
#ifdef FPP_GS_LINK
	{
		static unsigned char data[MAXIMUM_TOKEN];
		FppGsEvent event;

		while (host.link && fpp_gs_link_poll(host.link, &event, data, sizeof(data)) == FPP_STATUS_OK)
		{
			if (event.kind == FPP_GS_EVENT_KIND_SAR)
			{
				memcpy(host.sar, data, event.data_len);
				host.sar_size = event.data_len;
				host.sar_time = p2p_now();
				for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
				{
					if (host.players[index].pending || host.players[index].admitted)
						host_send_sar(index);
				}
			}
			else if (event.kind == FPP_GS_EVENT_KIND_REVOCATION)
			{
				FppRevocationOutcome outcome = FPP_REVOCATION_OUTCOME_IGNORED;

				if (fpp_admission_revocation(host.admission, data, event.data_len, unix_seconds(), &outcome) ==
					FPP_STATUS_OK)
				{
					platform_log("Verified play: a revocation event from the Revocation Feed (%d)", (int)outcome);
					if (outcome == FPP_REVOCATION_OUTCOME_INSTANCE_REVOKED)
					{
						host_lapse("Enforcement revoked this host");
						return;
					}
				}
			}
			else
			{
				fpp_gs_link_free(host.link);
				host.link = NULL;
				host_lapse("the link to Server Liveness closed");
				return;
			}
		}
	}
#endif
	/* (as its players do: SARs that stop are a lapse, whatever the link
	says yet) */
	if (elapsed_since(host.sar_time, SAR_GRACE_MS))
	{
		host_lapse("no SAR from Server Liveness for six seconds");
		return;
	}
	if (elapsed_since(host.tick_time, 1000))
	{
		host.tick_time = p2p_now();
		if (fpp_admission_tick(host.admission, unix_seconds()) == FPP_STATUS_TOKEN_REVOKED)
		{
			host_lapse("Enforcement revoked this host");
			return;
		}
	}
	{
		unsigned short slot, reason;

		while (fpp_admission_poll_removed(host.admission, &slot, &reason) == FPP_STATUS_OK)
		{
			for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
			{
				if (host.players[index].admitted && host.players[index].slot == slot)
				{
					platform_log("Verified play: removed the player in slot %u (%s)", (unsigned)slot,
						fpp_reason_str(reason));
					host_refuse(index, reason, 1);
				}
			}
		}
	}
	if (elapsed_since(host.epoch_time, SESSION_EPOCH_MS))
	{
		host.epoch_time += SESSION_EPOCH_MS;
		host_checkpoint();
	}
}

/* ---------- a player */

static struct
{
	int checked;
	int loaded;
	unsigned char seed[32];
	unsigned long address;
	unsigned short port;
	unsigned char noise_static[32];
	unsigned char sat[MAXIMUM_TOKEN];
	int sat_size;
	unsigned char ar[MAXIMUM_TOKEN];
	int ar_size;
	unsigned char liveness_key[32];

	int peer;
	/* its session with the host is up */
	int connected;
	int has_instance_key;
	unsigned char instance_key[32];
	FppKeys *keys;
	FppSarChain *chain;
	unsigned long sar_time;
	int admit_sent;
	int admitted;
	long heads;
	int has_match;
	unsigned char match_id[16];
} ticket = { .peer = -1 };

/* network.ticket_file: what fpp-ticket wrote */
static void load_ticket(void)
{
	const char *path = config_string("network.ticket_file");
	static char text[3 * MAXIMUM_TOKEN];
	FILE *file;
	size_t size;
	char *line;
	int have = 0;

	ticket.checked = 1;
	if (!path[0])
		return;
	file = fopen(path, "rb");
	if (!file)
	{
		platform_log("Verified play: cannot read network.ticket_file %s", path);
		return;
	}
	size = fread(text, 1, sizeof(text) - 1, file);
	fclose(file);
	text[size] = 0;
	if (strncmp(text, "fpp-ticket 1\n", 13))
	{
		platform_log("Verified play: %s is not a ticket (fpp-ticket 1)", path);
		return;
	}
	for (line = strtok(text + 13, "\n"); line; line = strtok(NULL, "\n"))
	{
		char *value = strchr(line, ' ');

		if (!value)
			continue;
		*value++ = 0;
		if (!strcmp(line, "session_seed") && parse_hex(value, ticket.seed, 32, NULL) == 32)
			have |= 1;
		else if (!strcmp(line, "gs_addr"))
		{
			unsigned int a, b, c, d, port;

			if (sscanf(value, "%u.%u.%u.%u:%u", &a, &b, &c, &d, &port) == 5 && a < 256 && b < 256 && c < 256 &&
				d < 256 && port && port < 65536)
			{
				unsigned char bytes[4] = { (unsigned char)a, (unsigned char)b, (unsigned char)c, (unsigned char)d };
				unsigned char port_bytes[2] = { (unsigned char)(port >> 8), (unsigned char)port };

				/* (network byte order, as struct p2p_candidate holds it) */
				memcpy(&ticket.address, bytes, 4);
				if (sizeof(ticket.address) > 4)
					memset((unsigned char *)&ticket.address + 4, 0, sizeof(ticket.address) - 4);
				memcpy(&ticket.port, port_bytes, 2);
				have |= 2;
			}
		}
		else if (!strcmp(line, "gs_noise_static") && parse_hex(value, ticket.noise_static, 32, NULL) == 32)
			have |= 4;
		else if (!strcmp(line, "sat") && (ticket.sat_size = parse_hex(value, ticket.sat, MAXIMUM_TOKEN, NULL)) > 0)
			have |= 8;
		else if (!strcmp(line, "ar") && (ticket.ar_size = parse_hex(value, ticket.ar, MAXIMUM_TOKEN, NULL)) > 0)
			have |= 16;
		else if (!strcmp(line, "liveness_key") && parse_hex(value, ticket.liveness_key, 32, NULL) == 32)
			have |= 32;
	}
	if (have != 63)
	{
		platform_log("Verified play: %s lacks something a ticket holds", path);
		return;
	}
	if (fpp_keys_new(&ticket.keys) != FPP_STATUS_OK ||
		fpp_keys_add(ticket.keys, FPP_KEY_LIVENESS, ticket.liveness_key) != FPP_STATUS_OK)
	{
		platform_log("Verified play: %s: its Server Liveness key is not a key", path);
		return;
	}
	ticket.loaded = 1;
}

int p2p_verified_ticket_seed(unsigned char *seed)
{
	if (!ticket.checked)
		load_ticket();
	if (!ticket.loaded)
		return 0;
	memcpy(seed, ticket.seed, 32);
	return 1;
}

int p2p_verified_ticket_host(struct p2p_candidate *address, unsigned char *host_key)
{
	if (!ticket.checked)
		load_ticket();
	if (!ticket.loaded)
		return 0;
	address->address = ticket.address;
	address->port = ticket.port;
	memcpy(host_key, ticket.noise_static, 32);
	return 1;
}

void p2p_verified_joiner_started(int peer)
{
	ticket.peer = peer;
	ticket.connected = 0;
	ticket.has_instance_key = 0;
	ticket.admit_sent = 0;
	ticket.admitted = 0;
	ticket.sar_time = p2p_now();
	fpp_sar_chain_free(ticket.chain);
	ticket.chain = NULL;
}

void p2p_verified_joiner_connected(int peer, const unsigned char *instance_key)
{
	if (peer != ticket.peer)
		return;
	ticket.connected = 1;
	ticket.has_instance_key = instance_key != NULL;
	if (instance_key)
		memcpy(ticket.instance_key, instance_key, 32);
	ticket.sar_time = p2p_now();
	platform_log("Verified play: secure session with the host; checking its SARs");
}

static void joiner_lapse(int peer, const char *why)
{
	platform_log("Verified play: %s: the host is not blessed (SAR lapsed); leaving it", why);
	fpp_sar_chain_free(ticket.chain);
	ticket.chain = NULL;
	ticket.peer = -1;
	p2p_peer_refuse(peer, REASON_SAR_LAPSED);
}

static void joiner_sar(int peer, const unsigned char *sar, size_t size)
{
	FppSarInfo info;
	FppStatus status;

	if (!ticket.chain)
		status = fpp_sar_chain_start(ticket.keys, sar, size, unix_seconds(), &ticket.chain);
	else
		status = fpp_sar_chain_update(ticket.chain, sar, size, unix_seconds());
	if (status != FPP_STATUS_OK)
	{
		joiner_lapse(peer, fpp_status_str((int)status));
		return;
	}
	fpp_sar_chain_info(ticket.chain, &info);
	/* the host is the one the Broker named (the key dialled), and signs
	with the key its session announced */
	if (!info.has_noise_static || memcmp(info.noise_static, ticket.noise_static, 32) ||
		!ticket.has_instance_key || memcmp(info.cnf, ticket.instance_key, 32))
	{
		joiner_lapse(peer, "its SAR is for another server");
		return;
	}
	ticket.sar_time = p2p_now();
	if (!ticket.admit_sent)
	{
		unsigned char message[2 * MAXIMUM_TOKEN + 64];
		size_t message_size;

		ticket.admit_sent = 1;
		platform_log("Verified play: the host's SAR verifies (a class %u server%s%s); presenting this machine's "
			"admission", (unsigned)info.server_class, info.region[0] ? " in region " : "", (const char *)info.region);
		if (fpp_control_admit(ticket.sat, (size_t)ticket.sat_size, ticket.ar, (size_t)ticket.ar_size, message,
			sizeof(message), &message_size) == FPP_STATUS_OK)
		{
			send_control(peer, message, message_size);
		}
	}
}

static void joiner_head(int peer, const unsigned char *checkpoint, size_t size)
{
	FppSarInfo info;
	FppCheckpointInfo checkpoint_info;

	if (!ticket.chain || fpp_sar_chain_info(ticket.chain, &info) != FPP_STATUS_OK)
		return;
	if (fpp_verify_checkpoint(checkpoint, size, info.cnf, &checkpoint_info) != FPP_STATUS_OK ||
		(ticket.has_match && memcmp(ticket.match_id, checkpoint_info.match_id, 16)))
	{
		joiner_lapse(peer, "it sent a Checkpoint its SAR's key did not sign for this match");
		return;
	}
	if (!ticket.has_match)
	{
		memcpy(ticket.match_id, checkpoint_info.match_id, 16);
		ticket.has_match = 1;
	}
	ticket.heads++;
	platform_log("Verified play: the host's Checkpoint %u verifies (%ld so far; roster %u, %u evidence anchored)",
		(unsigned)checkpoint_info.epoch, ticket.heads, (unsigned)checkpoint_info.roster_n,
		(unsigned)checkpoint_info.events_n);
}

int p2p_verified_joiner(int peer)
{
	return ticket.loaded && peer == ticket.peer;
}

int p2p_verified_joiner_message(int peer, const unsigned char *data, int size)
{
	static unsigned char bytes[MAXIMUM_TOKEN];
	FppControl control;

	if (!p2p_verified_joiner(peer))
		return 0;
	/* (nothing else counts before the host admits this machine) */
	if (size < 1 || data[0] != VERIFIED_TAG)
		return !ticket.admitted;
	if (fpp_control_decode(data + 1, (size_t)(size - 1), &control, bytes, sizeof(bytes)) != FPP_STATUS_OK)
		return 1;
	switch (control.kind)
	{
	case FPP_CONTROL_KIND_SAR_UPDATE:
		joiner_sar(peer, bytes, control.first_len);
		break;
	case FPP_CONTROL_KIND_ADMITTED:
		if (ticket.chain && !ticket.admitted)
		{
			ticket.admitted = 1;
			platform_log("Verified play: admitted to slot %u", (unsigned)control.slot);
			p2p_peer_admit(peer);
		}
		break;
	case FPP_CONTROL_KIND_REJECT:
	case FPP_CONTROL_KIND_KICK:
		platform_log("Verified play: the host %s this machine: %s", control.kind == FPP_CONTROL_KIND_KICK ?
			"removed" : "refused", fpp_reason_str(control.code));
		ticket.peer = -1;
		p2p_peer_refuse(peer, control.code);
		break;
	case FPP_CONTROL_KIND_CHECKPOINT_HEAD:
		joiner_head(peer, bytes, control.first_len);
		break;
	default:
		break;
	}
	return 1;
}

int p2p_verified_joiner_admitted(int peer)
{
	return !p2p_verified_joiner(peer) || ticket.admitted;
}

void p2p_verified_joiner_update(int peer)
{
	/* (from the session being up: a host that sends no SAR at all is no
	more blessed than one whose SARs stop) */
	if (!p2p_verified_joiner(peer) || !ticket.connected)
		return;
	if (elapsed_since(ticket.sar_time, SAR_GRACE_MS))
		joiner_lapse(peer, "no SAR for six seconds");
	else if (ticket.chain && fpp_sar_chain_live(ticket.chain, unix_seconds()) != FPP_STATUS_OK)
		joiner_lapse(peer, "its SAR expired");
}

void p2p_verified_joiner_gone(int peer)
{
	if (peer == ticket.peer)
		ticket.peer = -1;
}
