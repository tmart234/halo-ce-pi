/*
P2P_EVIDENCE.C

Evidence of what the host of an internet play game was sent and what it
decided (stage H4, milestone M3 of the mmo repository's
docs/anticheat/08-reference-title-halo.md), so that a player can later
prove what a cheating host did (finding H09), which nothing a player can
check at the time shows.

- A game is a match, named by 16 random bytes the host sends each player
  ('M', on the secure session's reliable channel). Its ticks (the game's
  30 a second) fall in epochs of EVIDENCE_EPOCH_TICKS.
- A player's frame is what it sent the host reliably in one tick (the
  netcode's hit reports: network_damage.c), with the count of accountable
  units (reports) in front. Each epoch, the player signs an InputCommit
  over its frames with its session key (the key its secure session proved)
  and sends it ('I').
- The host rebuilds each player's frames from what it received, and at
  each epoch's end (and a grace for late commits) signs a Checkpoint with
  its instance key: per player, the commit's digest if its frames match
  what arrived, and which ticks' frames arrived; and an outcome event for
  every unit it decided (a hit dealt, or rejected with a reason). It sends
  every player the Checkpoint with its leaves ('C', in parts).
- Every machine appends what it sent, signed and received to a bundle,
  evidence/<match>-<role>-<machine>.fppb in the data root: a log of
  records (a type byte, a 32-bit length, the bytes). The mmo repository's
  auditor (fpp-audit) checks a bundle: every signature and chain, that
  the host acknowledged each commit and every frame, and accounted for
  every unit. A host that silently drops a player's hits is caught from
  that player's bundle alone.

What the evidence cannot show without replaying the game (the auditor's
next step, which needs the game's data): a host that records a hit as
dealt or rejected and lies about it.

debug.evidence_synthetic runs the machinery without a game (no data
needed): a player sends a made-up frame each second ('F', standing in
for the netcode's reliable channel) and the host decides each; "omit"
makes the host leave every other one out, as a cheating host would
(tools/p2p_loopback_test.py --evidence).

Everything here runs under p2p_lock: the game's calls take it, and the
p2p thread's are made holding it.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "fpp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum
{
	EVIDENCE_EPOCH_TICKS = 150,
	/* the host signs an epoch's Checkpoint this long after it ends, for the
	players' commits (and frames) still on their way */
	CHECKPOINT_GRACE_TICKS = 90,
	/* a host finishes a player's frame for a tick this long after the tick
	if nothing later came (the reliable channel delivers a tick's messages
	together) */
	FRAME_SETTLE_TICKS = 60,
	/* epochs a host keeps open per player */
	HOST_EPOCHS = 4,
	MAXIMUM_FRAME_SIZE = 8192,
	MAXIMUM_EVENTS_PER_EPOCH = 1024,
	EVENT_SIZE = 11,
	APPLIED_SIZE = (EVIDENCE_EPOCH_TICKS + 7) / 8,
	MAXIMUM_OBJECT_SIZE = 4096,
	MAXIMUM_PACKAGE_SIZE = 65536,
	/* a Checkpoint package's part: its tag, epoch, index and count */
	PART_HEADER_SIZE = 1 + 4 + 2 + 2,
	PART_DATA_SIZE = 1200,
	SYNTHETIC_TICK_MS = 33,
	SYNTHETIC_FRAME_TICKS = 30,
};

/* the reliable channel's evidence messages */
enum
{
	_evidence_match = 'M',
	_evidence_commit = 'I',
	_evidence_checkpoint = 'C',
	_evidence_synthetic_frame = 'F',
};

/* a bundle's records */
enum
{
	_record_meta = 1,
	_record_frame = 2,
	_record_commit = 3,
	_record_checkpoint = 4,
	_record_received_frame = 5,
	_record_roster = 6,
};

enum
{
	_role_none,
	_role_host,
	_role_player,
};

struct frame
{
	int active;
	long tick;
	int units;
	int size;
	unsigned char bytes[MAXIMUM_FRAME_SIZE];
};

/* the host's record of one player's frames and commit for one epoch */
struct host_epoch
{
	int used;
	long epoch;
	FppInputCommitBuilder *builder;
	long last_tick;
	int frame_count;
	unsigned char applied[APPLIED_SIZE];
	/* its commit, verified at the Checkpoint */
	int has_commit;
	unsigned char commit[MAXIMUM_OBJECT_SIZE];
	int commit_size;
};

struct host_slot
{
	int used;
	int peer;
	unsigned short slot;
	unsigned char session_key[P2P_PUBLIC_KEY_SIZE];
	struct frame frame;
	struct host_epoch epochs[HOST_EPOCHS];
};

struct host_events
{
	long epoch;
	int count;
	unsigned char events[MAXIMUM_EVENTS_PER_EPOCH][EVENT_SIZE];
};

static struct
{
	int role;
	int has_match;
	unsigned char match_id[16];
	FILE *bundle;
	long tick;

	/* a player */
	int host_peer;
	unsigned short slot;
	int has_instance_key;
	unsigned char instance_key[P2P_PUBLIC_KEY_SIZE];
	struct frame frame;
	FppInputCommitBuilder *builder;
	long builder_epoch;
	int builder_frames;
	int has_prev_commit;
	unsigned char prev_commit[32];
	unsigned char package[MAXIMUM_PACKAGE_SIZE];
	int package_size;
	long package_epoch;
	int package_parts;
	int package_received;

	/* the host */
	struct host_slot slots[P2P_MAXIMUM_PEERS];
	struct host_events events[2];
	long next_checkpoint_epoch;
	int has_prev_checkpoint;
	unsigned char prev_checkpoint[32];

	/* debug.evidence_synthetic */
	int synthetic_checked;
	int synthetic;
	int synthetic_omit;
	unsigned long synthetic_start;
	long synthetic_offset;
	long synthetic_last_frame;
	unsigned long synthetic_count;
	unsigned long synthetic_omitted;
} evidence;

/* ---------- helpers */

static void put_u16(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
}

static void put_u32(unsigned char *bytes, unsigned long value)
{
	bytes[0] = (unsigned char)value;
	bytes[1] = (unsigned char)(value >> 8);
	bytes[2] = (unsigned char)(value >> 16);
	bytes[3] = (unsigned char)(value >> 24);
}

static unsigned long get_u16(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8;
}

static unsigned long get_u32(const unsigned char *bytes)
{
	return (unsigned long)bytes[0] | (unsigned long)bytes[1] << 8 | (unsigned long)bytes[2] << 16 |
		(unsigned long)bytes[3] << 24;
}

static long epoch_of(long tick)
{
	return tick >= 0 ? tick / EVIDENCE_EPOCH_TICKS : -1;
}

static void record(int type, const void *bytes, size_t size)
{
	unsigned char header[5];

	if (!evidence.bundle)
		return;
	header[0] = (unsigned char)type;
	put_u32(header + 1, (unsigned long)size);
	fwrite(header, 1, sizeof(header), evidence.bundle);
	fwrite(bytes, 1, size, evidence.bundle);
	fflush(evidence.bundle);
}

/* two parts in one record */
static void record2(int type, const void *first, size_t first_size, const void *second, size_t second_size)
{
	unsigned char buffer[MAXIMUM_FRAME_SIZE + 64];

	if (first_size + second_size > sizeof(buffer))
		return;
	memcpy(buffer, first, first_size);
	memcpy(buffer + first_size, second, second_size);
	record(type, buffer, first_size + second_size);
}

static void open_bundle(const char *role)
{
	char path[1024];
	char match[33];
	char machine[2 * P2P_IDENTIFIER_SIZE + 1];
	unsigned char meta[1 + 1 + 16 + 2 + 2 * P2P_PUBLIC_KEY_SIZE + 4];
	int size = 0;

	if (evidence.bundle)
		fclose(evidence.bundle);
	evidence.bundle = NULL;
	p2p_hex(evidence.match_id, 16, match);
	p2p_hex(p2p_identifier(), P2P_IDENTIFIER_SIZE, machine);
	snprintf(path, sizeof(path), "%s/evidence", platform_data_root());
	posix_make_directory(path);
	snprintf(path, sizeof(path), "%s/evidence/%s-%s-%s.fppb", platform_data_root(), match, role, machine);
	evidence.bundle = fopen(path, "wb");
	if (!evidence.bundle)
	{
		platform_log("Internet play: cannot write the match's evidence to %s", path);
		return;
	}
	fwrite("FPPB1\n", 1, 6, evidence.bundle);
	/* the bundle's version, its maker's role, the match, the player's
	slot, its session key and the host's instance key, the epoch's ticks */
	meta[size++] = 1;
	meta[size++] = (unsigned char)evidence.role;
	memcpy(meta + size, evidence.match_id, 16);
	size += 16;
	put_u16(meta + size, evidence.slot);
	size += 2;
	p2p_session_public_key(meta + size);
	size += P2P_PUBLIC_KEY_SIZE;
	if (evidence.role == _role_host)
		p2p_instance_public_key(meta + size);
	else
		memcpy(meta + size, evidence.instance_key, P2P_PUBLIC_KEY_SIZE);
	size += P2P_PUBLIC_KEY_SIZE;
	put_u32(meta + size, EVIDENCE_EPOCH_TICKS);
	size += 4;
	record(_record_meta, meta, (size_t)size);
	platform_log("Internet play: this game's evidence goes to %s", path);
}

/* ---------- the player */

/* the epoch's commit, signed and sent */
static void player_close_epoch(void)
{
	unsigned char object[MAXIMUM_OBJECT_SIZE + 1];
	size_t size = 0;
	unsigned char digest[32];

	if (!evidence.builder)
		return;
	if (evidence.builder_frames &&
		fpp_input_commit_sign(evidence.builder, p2p_session_key(), object + 1, MAXIMUM_OBJECT_SIZE, &size) ==
			FPP_STATUS_OK &&
		fpp_object_digest(object + 1, size, digest) == FPP_STATUS_OK)
	{
		record(_record_commit, object + 1, size);
		object[0] = _evidence_commit;
		if (evidence.host_peer >= 0)
			p2p_evidence_send(evidence.host_peer, object, (int)size + 1);
		memcpy(evidence.prev_commit, digest, sizeof(digest));
		evidence.has_prev_commit = 1;
	}
	fpp_input_commit_free(evidence.builder);
	evidence.builder = NULL;
	evidence.builder_frames = 0;
}

static void player_finish_frame(void)
{
	long epoch;
	unsigned char tick[4];

	if (!evidence.frame.active)
		return;
	evidence.frame.active = 0;
	epoch = epoch_of(evidence.frame.tick);
	if (epoch < 0)
		return;
	if (evidence.builder && evidence.builder_epoch != epoch)
		player_close_epoch();
	if (!evidence.builder)
	{
		if (fpp_input_commit_begin(evidence.match_id, evidence.slot, (uint32_t)epoch,
			(uint32_t)(epoch * EVIDENCE_EPOCH_TICKS), (uint32_t)(epoch * EVIDENCE_EPOCH_TICKS + EVIDENCE_EPOCH_TICKS - 1),
			evidence.has_prev_commit ? evidence.prev_commit : NULL, &evidence.builder) != FPP_STATUS_OK)
		{
			evidence.builder = NULL;
			return;
		}
		evidence.builder_epoch = epoch;
	}
	if (fpp_input_commit_add_frame(evidence.builder, (uint32_t)evidence.frame.tick, evidence.frame.bytes,
		(size_t)evidence.frame.size) == FPP_STATUS_OK)
	{
		evidence.builder_frames++;
		put_u32(tick, (unsigned long)evidence.frame.tick);
		record2(_record_frame, tick, sizeof(tick), evidence.frame.bytes, (size_t)evidence.frame.size);
	}
}

/* adds a message to the tick's frame; the offset of its first unit in it */
static int frame_add(struct frame *frame, long tick, int units, const void *bytes, int size)
{
	int offset;

	if (!frame->active || frame->tick != tick)
	{
		frame->active = 1;
		frame->tick = tick;
		frame->units = 0;
		frame->size = 1;
	}
	offset = frame->units;
	if (frame->size + size > MAXIMUM_FRAME_SIZE || frame->units + units > 255)
		return offset;
	memcpy(frame->bytes + frame->size, bytes, (size_t)size);
	frame->size += size;
	frame->units += units;
	frame->bytes[0] = (unsigned char)frame->units;
	return offset;
}

static void player_frame(long tick, int units, const void *bytes, int size)
{
	if (evidence.role != _role_player || !evidence.has_match || tick < 0)
		return;
	if (evidence.frame.active && evidence.frame.tick != tick)
		player_finish_frame();
	frame_add(&evidence.frame, tick, units, bytes, size);
}

static void player_tick(long tick)
{
	if (evidence.role != _role_player || !evidence.has_match)
		return;
	if (evidence.frame.active && evidence.frame.tick < tick)
		player_finish_frame();
	if (evidence.builder && epoch_of(tick) > evidence.builder_epoch)
		player_close_epoch();
}

static void player_match(int peer, const unsigned char *data, int size)
{
	if (size < 1 + 16 + 2 + 4)
		return;
	if (evidence.role == _role_player && evidence.has_match && !memcmp(evidence.match_id, data + 1, 16))
		return;
	/* (the last match's last epoch, then the new one) */
	player_finish_frame();
	player_close_epoch();
	evidence.role = _role_player;
	evidence.has_match = 1;
	evidence.host_peer = peer;
	memcpy(evidence.match_id, data + 1, 16);
	evidence.slot = (unsigned short)get_u16(data + 17);
	evidence.has_prev_commit = 0;
	evidence.package_size = 0;
	evidence.package_parts = 0;
	evidence.synthetic_offset = (long)get_u32(data + 19) - (long)((p2p_now() - evidence.synthetic_start) / SYNTHETIC_TICK_MS);
	evidence.synthetic_last_frame = -1;
	open_bundle("player");
}

/* a part of the host's Checkpoint package; the whole one checked and kept */
static void player_checkpoint_part(const unsigned char *data, int size)
{
	long epoch;
	int index, count;
	unsigned long object_size;
	FppCheckpointInfo info;

	if (size < PART_HEADER_SIZE || !evidence.has_match)
		return;
	epoch = (long)get_u32(data + 1);
	index = (int)get_u16(data + 5);
	count = (int)get_u16(data + 7);
	if (index == 0)
	{
		evidence.package_size = 0;
		evidence.package_epoch = epoch;
		evidence.package_parts = count;
		evidence.package_received = 0;
	}
	if (epoch != evidence.package_epoch || index != evidence.package_received ||
		evidence.package_size + size - PART_HEADER_SIZE > MAXIMUM_PACKAGE_SIZE)
	{
		return;
	}
	memcpy(evidence.package + evidence.package_size, data + PART_HEADER_SIZE, (size_t)(size - PART_HEADER_SIZE));
	evidence.package_size += size - PART_HEADER_SIZE;
	evidence.package_received++;
	if (evidence.package_received < evidence.package_parts)
		return;
	/* (the signed Checkpoint first: the auditor checks its leaves) */
	object_size = evidence.package_size >= 4 ? get_u32(evidence.package) : 0;
	if (object_size + 4 > (unsigned long)evidence.package_size ||
		!evidence.has_instance_key ||
		fpp_verify_checkpoint(evidence.package + 4, object_size, evidence.instance_key, &info) != FPP_STATUS_OK ||
		memcmp(info.match_id, evidence.match_id, 16))
	{
		platform_log("Internet play: the host sent a Checkpoint that does not verify (epoch %ld)", epoch);
	}
	record(_record_checkpoint, evidence.package, (size_t)evidence.package_size);
	evidence.package_size = 0;
}

/* ---------- the host */

static struct host_slot *host_slot_for_peer(int peer)
{
	int index;

	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (evidence.slots[index].used && evidence.slots[index].peer == peer)
			return &evidence.slots[index];
	}
	return NULL;
}

static struct host_epoch *host_epoch(struct host_slot *slot, long epoch, int create)
{
	struct host_epoch *oldest = NULL;
	int index;

	for (index = 0; index < HOST_EPOCHS; index++)
	{
		struct host_epoch *entry = &slot->epochs[index];

		if (entry->used && entry->epoch == epoch)
			return entry;
		if (!oldest || !entry->used || (oldest->used && entry->epoch < oldest->epoch))
			oldest = entry;
	}
	/* (an epoch already signed takes nothing more) */
	if (!create || epoch < evidence.next_checkpoint_epoch)
		return NULL;
	if (oldest->builder)
		fpp_input_commit_free(oldest->builder);
	memset(oldest, 0, sizeof(*oldest));
	oldest->used = 1;
	oldest->epoch = epoch;
	oldest->last_tick = -1;
	if (fpp_input_commit_begin(evidence.match_id, slot->slot, (uint32_t)epoch, (uint32_t)(epoch * EVIDENCE_EPOCH_TICKS),
		(uint32_t)(epoch * EVIDENCE_EPOCH_TICKS + EVIDENCE_EPOCH_TICKS - 1), NULL, &oldest->builder) != FPP_STATUS_OK)
	{
		oldest->builder = NULL;
	}
	return oldest;
}

static void host_finish_frame(struct host_slot *slot)
{
	struct host_epoch *entry;
	unsigned char header[6];

	if (!slot->frame.active)
		return;
	slot->frame.active = 0;
	entry = host_epoch(slot, epoch_of(slot->frame.tick), 1);
	if (!entry || !entry->builder || slot->frame.tick <= entry->last_tick)
		return;
	if (fpp_input_commit_add_frame(entry->builder, (uint32_t)slot->frame.tick, slot->frame.bytes,
		(size_t)slot->frame.size) != FPP_STATUS_OK)
	{
		return;
	}
	entry->last_tick = slot->frame.tick;
	entry->frame_count++;
	{
		long bit = slot->frame.tick - entry->epoch * EVIDENCE_EPOCH_TICKS;

		entry->applied[bit / 8] |= (unsigned char)(1 << (bit % 8));
	}
	put_u16(header, slot->slot);
	put_u32(header + 2, (unsigned long)slot->frame.tick);
	record2(_record_received_frame, header, sizeof(header), slot->frame.bytes, (size_t)slot->frame.size);
}

static int host_frame(int peer, long tick, int units, const void *bytes, int size)
{
	struct host_slot *slot = host_slot_for_peer(peer);

	if (!slot || tick < 0)
		return 0;
	if (slot->frame.active && slot->frame.tick != tick)
		host_finish_frame(slot);
	return frame_add(&slot->frame, tick, units, bytes, size);
}

static void host_outcome(int peer, long tick, int unit, int outcome, int reason)
{
	struct host_slot *slot = host_slot_for_peer(peer);
	struct host_events *events = &evidence.events[epoch_of(evidence.tick) & 1];
	unsigned char *event;

	if (!slot || evidence.tick < 0)
		return;
	if (events->epoch != epoch_of(evidence.tick))
	{
		events->epoch = epoch_of(evidence.tick);
		events->count = 0;
	}
	if (events->count == MAXIMUM_EVENTS_PER_EPOCH)
		return;
	/* an outcome: its tag, the player's slot, the frame's tick, the unit
	in it, what the host decided (1 applied, 2 rejected) and why */
	event = events->events[events->count++];
	event[0] = 'O';
	put_u16(event + 1, slot->slot);
	put_u32(event + 3, (unsigned long)tick);
	put_u16(event + 7, (unsigned long)unit);
	event[9] = (unsigned char)outcome;
	event[10] = (unsigned char)reason;
}

/* a player's commit: kept, and checked at the Checkpoint */
static void host_commit(int peer, const unsigned char *object, int size)
{
	struct host_slot *slot = host_slot_for_peer(peer);
	FppInputCommitInfo info;
	struct host_epoch *entry;

	if (!slot || size > MAXIMUM_OBJECT_SIZE ||
		fpp_verify_input_commit(object, (size_t)size, slot->session_key, &info) != FPP_STATUS_OK ||
		memcmp(info.match_id, evidence.match_id, 16) || info.slot != slot->slot)
	{
		platform_log("Internet play: a player's input commit does not verify");
		return;
	}
	record(_record_commit, object, (size_t)size);
	entry = host_epoch(slot, (long)info.epoch, 1);
	if (!entry)
	{
		platform_log("Internet play: player %u's commit for epoch %lu came after its Checkpoint", (unsigned int)slot->slot,
			(unsigned long)info.epoch);
		return;
	}
	memcpy(entry->commit, object, (size_t)size);
	entry->commit_size = size;
	entry->has_commit = 1;
}

/* one player's line in a Checkpoint */
struct input_leaf
{
	struct host_slot *slot;
	int acknowledged;
	unsigned char digest[32];
	const unsigned char *applied;
};

/* whether the player's commit for the epoch is what arrived: its frames
the ones the host received, no more and no fewer; its digest if so */
static int host_acknowledges(struct host_slot *slot, struct host_epoch *entry, unsigned char *digest)
{
	FppInputCommitInfo info;
	unsigned char root[32];

	if (!entry->has_commit ||
		fpp_verify_input_commit(entry->commit, (size_t)entry->commit_size, slot->session_key, &info) != FPP_STATUS_OK)
	{
		return 0;
	}
	if (!entry->builder || fpp_input_commit_frames_root(entry->builder, root) != FPP_STATUS_OK ||
		memcmp(root, info.frames_root, 32) || info.n != (uint32_t)entry->frame_count)
	{
		platform_log("Internet play: player %u's commit for epoch %ld is not what arrived (%u frames signed, %d arrived)",
			(unsigned int)slot->slot, entry->epoch, (unsigned int)info.n, entry->frame_count);
		return 0;
	}
	memcpy(digest, info.digest, 32);
	return 1;
}

/* the epoch's Checkpoint, signed, kept and sent every player: the package
is the signed Checkpoint (its size in front), then its input leaves (slot,
whether a commit is acknowledged, its digest, the applied bitset) and its
events, which the auditor checks against the Checkpoint's roots */
static void host_checkpoint(long epoch)
{
	static unsigned char package[MAXIMUM_PACKAGE_SIZE];
	static struct input_leaf leaves[P2P_MAXIMUM_PEERS];
	FppCheckpointBuilder *builder = NULL;
	struct host_events *events = &evidence.events[epoch & 1];
	int event_count = events->epoch == epoch ? events->count : 0;
	unsigned char build_id[32];
	size_t object_size = 0;
	int package_size;
	int leaf_count = 0;
	int index;

	/* the players' lines, slots ascending */
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		struct host_slot *slot = &evidence.slots[index];
		struct host_epoch *entry;
		struct input_leaf leaf;
		int at;

		if (!slot->used)
			continue;
		if (slot->frame.active && epoch_of(slot->frame.tick) <= epoch)
			host_finish_frame(slot);
		entry = host_epoch(slot, epoch, 0);
		if (!entry)
			continue;
		leaf.slot = slot;
		leaf.acknowledged = host_acknowledges(slot, entry, leaf.digest);
		leaf.applied = entry->applied;
		/* (nothing of this player's this epoch) */
		if (!leaf.acknowledged && !entry->frame_count)
			continue;
		for (at = leaf_count; at > 0 && leaves[at - 1].slot->slot > slot->slot; at--)
			leaves[at] = leaves[at - 1];
		leaves[at] = leaf;
		leaf_count++;
	}
	memset(build_id, 0, sizeof(build_id));
	if (fpp_checkpoint_begin(evidence.match_id, build_id, 0, (uint32_t)epoch, (uint32_t)(epoch * EVIDENCE_EPOCH_TICKS),
		(uint32_t)(epoch * EVIDENCE_EPOCH_TICKS + EVIDENCE_EPOCH_TICKS - 1),
		evidence.has_prev_checkpoint ? evidence.prev_checkpoint : NULL, &builder) != FPP_STATUS_OK)
	{
		return;
	}
	for (index = 0; index < leaf_count; index++)
	{
		fpp_checkpoint_add_input(builder, leaves[index].slot->slot, leaves[index].acknowledged ? leaves[index].digest : NULL,
			leaves[index].applied, APPLIED_SIZE);
	}
	for (index = 0; index < event_count; index++)
		fpp_checkpoint_add_event(builder, events->events[index], EVENT_SIZE);
	if (fpp_checkpoint_sign(builder, p2p_instance_key(), package + 4, MAXIMUM_OBJECT_SIZE, &object_size) != FPP_STATUS_OK ||
		fpp_object_digest(package + 4, object_size, evidence.prev_checkpoint) != FPP_STATUS_OK)
	{
		fpp_checkpoint_free(builder);
		return;
	}
	fpp_checkpoint_free(builder);
	evidence.has_prev_checkpoint = 1;
	put_u32(package, (unsigned long)object_size);
	package_size = 4 + (int)object_size;
	put_u16(package + package_size, (unsigned long)leaf_count);
	package_size += 2;
	for (index = 0; index < leaf_count; index++)
	{
		put_u16(package + package_size, leaves[index].slot->slot);
		package[package_size + 2] = (unsigned char)leaves[index].acknowledged;
		if (leaves[index].acknowledged)
			memcpy(package + package_size + 3, leaves[index].digest, 32);
		else
			memset(package + package_size + 3, 0, 32);
		memcpy(package + package_size + 35, leaves[index].applied, APPLIED_SIZE);
		package_size += 35 + APPLIED_SIZE;
	}
	put_u16(package + package_size, (unsigned long)event_count);
	package_size += 2;
	for (index = 0; index < event_count; index++)
	{
		memcpy(package + package_size, events->events[index], EVENT_SIZE);
		package_size += EVENT_SIZE;
	}
	record(_record_checkpoint, package, (size_t)package_size);
	/* to every player, in parts */
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		struct host_slot *slot = &evidence.slots[index];
		int parts = (package_size + PART_DATA_SIZE - 1) / PART_DATA_SIZE;
		int part;

		if (!slot->used)
			continue;
		for (part = 0; part < parts; part++)
		{
			unsigned char message[PART_HEADER_SIZE + PART_DATA_SIZE];
			int size = package_size - part * PART_DATA_SIZE;

			if (size > PART_DATA_SIZE)
				size = PART_DATA_SIZE;
			message[0] = _evidence_checkpoint;
			put_u32(message + 1, (unsigned long)epoch);
			put_u16(message + 5, (unsigned long)part);
			put_u16(message + 7, (unsigned long)parts);
			memcpy(message + PART_HEADER_SIZE, package + part * PART_DATA_SIZE, (size_t)size);
			p2p_evidence_send(slot->peer, message, PART_HEADER_SIZE + size);
		}
	}
	events->count = 0;
	events->epoch = -1;
}

static void host_tick(long tick)
{
	int index;

	if (evidence.role != _role_host || !evidence.has_match)
		return;
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		struct host_slot *slot = &evidence.slots[index];

		if (slot->used && slot->frame.active && tick - slot->frame.tick > FRAME_SETTLE_TICKS)
			host_finish_frame(slot);
	}
	while (evidence.next_checkpoint_epoch >= 0 &&
		tick >= (evidence.next_checkpoint_epoch + 1) * EVIDENCE_EPOCH_TICKS + CHECKPOINT_GRACE_TICKS)
	{
		host_checkpoint(evidence.next_checkpoint_epoch);
		evidence.next_checkpoint_epoch++;
	}
}

static void host_send_match(struct host_slot *slot)
{
	unsigned char message[1 + 16 + 2 + 4];

	message[0] = _evidence_match;
	memcpy(message + 1, evidence.match_id, 16);
	put_u16(message + 17, slot->slot);
	put_u32(message + 19, (unsigned long)(evidence.tick > 0 ? evidence.tick : 0));
	p2p_evidence_send(slot->peer, message, sizeof(message));
}

static void end_match(void)
{
	int index;

	if (evidence.role == _role_player)
	{
		player_finish_frame();
		player_close_epoch();
	}
	if (evidence.role == _role_host)
	{
		/* (the last epochs, signed as they stand) */
		if (evidence.tick >= 0)
			host_tick(evidence.tick + 2 * EVIDENCE_EPOCH_TICKS + CHECKPOINT_GRACE_TICKS);
		for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
		{
			int epoch;

			for (epoch = 0; epoch < HOST_EPOCHS; epoch++)
			{
				if (evidence.slots[index].epochs[epoch].builder)
					fpp_input_commit_free(evidence.slots[index].epochs[epoch].builder);
			}
			memset(&evidence.slots[index].frame, 0, sizeof(evidence.slots[index].frame));
			memset(evidence.slots[index].epochs, 0, sizeof(evidence.slots[index].epochs));
		}
	}
	if (evidence.bundle)
		fclose(evidence.bundle);
	evidence.bundle = NULL;
	evidence.has_match = 0;
}

/* ---------- debug.evidence_synthetic */

static void synthetic_check(void)
{
	const char *setting;

	if (evidence.synthetic_checked)
		return;
	evidence.synthetic_checked = 1;
	setting = config_string("debug.evidence_synthetic");
	evidence.synthetic = setting[0] != 0;
	evidence.synthetic_omit = !strcmp(setting, "omit");
	evidence.synthetic_start = p2p_now();
	evidence.synthetic_last_frame = -1;
}

static long synthetic_tick(void)
{
	return (long)((p2p_now() - evidence.synthetic_start) / SYNTHETIC_TICK_MS) + evidence.synthetic_offset;
}

/* ---------- the p2p layer's calls (p2p_lock held) */

void p2p_evidence_update(int hosting)
{
	long tick;

	synthetic_check();
	if (!evidence.synthetic)
		return;
	tick = synthetic_tick();
	/* (a machine that joined a host is its player; every copy of the game
	listens, so hosting alone says nothing) */
	if (hosting && evidence.role == _role_none)
		p2p_evidence_start_match_locked(1, tick);
	if (evidence.role == _role_host)
	{
		evidence.tick = tick;
		host_tick(tick);
	}
	else if (evidence.role == _role_player && evidence.has_match)
	{
		player_tick(tick);
		/* a made-up reliable message, each second: a unit to account for */
		if (tick - evidence.synthetic_last_frame >= SYNTHETIC_FRAME_TICKS)
		{
			unsigned char message[64];
			int size;

			evidence.synthetic_last_frame = tick;
			message[0] = _evidence_synthetic_frame;
			put_u32(message + 1, (unsigned long)tick);
			message[5] = 1;
			size = 6 + snprintf((char *)message + 6, sizeof(message) - 6, "synthetic report %lu",
				evidence.synthetic_count++);
			player_frame(tick, 1, message + 6, size - 6);
			p2p_evidence_send(evidence.host_peer, message, size);
		}
	}
}

void p2p_evidence_peer_admitted(int peer, const unsigned char *session_key, unsigned long slot)
{
	struct host_slot *entry = host_slot_for_peer(peer);
	int index;

	if (!entry)
	{
		for (index = 0; index < P2P_MAXIMUM_PEERS && evidence.slots[index].used; index++)
			;
		if (index == P2P_MAXIMUM_PEERS)
			return;
		entry = &evidence.slots[index];
		memset(entry, 0, sizeof(*entry));
		entry->used = 1;
		entry->peer = peer;
	}
	entry->slot = (unsigned short)slot;
	memcpy(entry->session_key, session_key, P2P_PUBLIC_KEY_SIZE);
	if (evidence.role == _role_host && evidence.has_match)
	{
		unsigned char roster[2 + P2P_PUBLIC_KEY_SIZE];

		put_u16(roster, entry->slot);
		memcpy(roster + 2, session_key, P2P_PUBLIC_KEY_SIZE);
		record(_record_roster, roster, sizeof(roster));
		host_send_match(entry);
	}
}

void p2p_evidence_peer_left(int peer)
{
	struct host_slot *slot = host_slot_for_peer(peer);

	if (slot)
	{
		int epoch;

		host_finish_frame(slot);
		for (epoch = 0; epoch < HOST_EPOCHS; epoch++)
		{
			if (slot->epochs[epoch].builder)
				fpp_input_commit_free(slot->epochs[epoch].builder);
		}
		memset(slot, 0, sizeof(*slot));
	}
	if (evidence.role == _role_player && evidence.host_peer == peer)
	{
		end_match();
		evidence.role = _role_none;
		evidence.host_peer = -1;
	}
}

void p2p_evidence_host_key(int peer, const unsigned char *instance_key)
{
	(void)peer;
	memcpy(evidence.instance_key, instance_key, P2P_PUBLIC_KEY_SIZE);
	evidence.has_instance_key = 1;
}

void p2p_evidence_message(int peer, int from_host, const unsigned char *data, int size)
{
	if (size < 1)
		return;
	if (from_host)
	{
		if (data[0] == _evidence_match)
			player_match(peer, data, size);
		else if (data[0] == _evidence_checkpoint && evidence.host_peer == peer)
			player_checkpoint_part(data, size);
		return;
	}
	if (evidence.role != _role_host)
		return;
	if (data[0] == _evidence_commit)
		host_commit(peer, data + 1, size - 1);
	else if (data[0] == _evidence_synthetic_frame && evidence.synthetic && size >= 6)
	{
		/* (what the netcode's reliable channel would carry, and the host's
		decision on its unit: applied, or left out by a cheating host) */
		long tick = (long)get_u32(data + 1);
		int offset = host_frame(peer, tick, data[5], data + 6, size - 6);

		if (evidence.synthetic_omit && (evidence.synthetic_count++ & 1))
		{
			evidence.synthetic_omitted++;
			platform_log("cheat: synthetic host left out an outcome (%lu so far)", evidence.synthetic_omitted);
		}
		else
			host_outcome(peer, tick, offset, 1, 0);
	}
}

void p2p_evidence_start_match_locked(int is_host, long tick)
{
	int index;

	if (!is_host)
		return;
	end_match();
	evidence.role = _role_host;
	evidence.has_match = 1;
	evidence.tick = tick;
	evidence.next_checkpoint_epoch = epoch_of(tick);
	evidence.has_prev_checkpoint = 0;
	evidence.events[0].epoch = evidence.events[1].epoch = -1;
	posix_random_bytes(evidence.match_id, sizeof(evidence.match_id));
	evidence.slot = 0;
	open_bundle("host");
	for (index = 0; index < P2P_MAXIMUM_PEERS; index++)
	{
		if (evidence.slots[index].used)
		{
			unsigned char roster[2 + P2P_PUBLIC_KEY_SIZE];

			put_u16(roster, evidence.slots[index].slot);
			memcpy(roster + 2, evidence.slots[index].session_key, P2P_PUBLIC_KEY_SIZE);
			record(_record_roster, roster, sizeof(roster));
			host_send_match(&evidence.slots[index]);
		}
	}
}

/* ---------- the game's calls (p2p.h) */

void p2p_evidence_start_match(int is_host, long tick)
{
	p2p_lock_enter();
	synthetic_check();
	if (!evidence.synthetic)
		p2p_evidence_start_match_locked(is_host, tick);
	p2p_lock_leave();
}

void p2p_evidence_tick(long tick)
{
	p2p_lock_enter();
	if (!evidence.synthetic)
	{
		evidence.tick = tick;
		host_tick(tick);
		player_tick(tick);
	}
	p2p_lock_leave();
}

void p2p_evidence_client_frame(long tick, int units, const void *bytes, int size)
{
	p2p_lock_enter();
	if (!evidence.synthetic)
		player_frame(tick, units, bytes, size);
	p2p_lock_leave();
}

int p2p_evidence_host_frame(unsigned long address, long tick, int units, const void *bytes, int size)
{
	int offset = 0;
	int peer;

	p2p_lock_enter();
	peer = p2p_peer_index_for_address(address);
	if (!evidence.synthetic && evidence.role == _role_host && peer >= 0)
		offset = host_frame(peer, tick, units, bytes, size);
	p2p_lock_leave();
	return offset;
}

void p2p_evidence_host_outcome(unsigned long address, long tick, int unit, int applied, int reason)
{
	int peer;

	p2p_lock_enter();
	peer = p2p_peer_index_for_address(address);
	if (!evidence.synthetic && evidence.role == _role_host && peer >= 0)
		host_outcome(peer, tick, unit, applied ? 1 : 2, reason);
	p2p_lock_leave();
}
