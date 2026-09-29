/*
NETWORK_SIGNALS.C

The host's Signals (stage H2 of the mmo reference-title plan): each time it
rejects what a client machine claims, a JSON line in signals.jsonl beside
debug.txt, for detection to read. The rejection itself is the enforcement;
a Signal is the evidence that a machine keeps trying.

{"tick":1234,"kind":"hit_report_rejected","machine":2,"player":5,"reason":"obstructed","count":3}

Each kind and reason of rejection from a machine is written at most once a
second, with how many there were since its last line (count), so a
modified client cannot flood the file.
*/

#include "cseries.h"
#include "game/game.h"
#include "network_distributed.h"

#include <stdio.h>

/* the platform layer's (xbox_files.c) */
void platform_append_signal(char const *line);

enum
{
	MAXIMUM_SIGNAL_KINDS = 16,
	MAXIMUM_SIGNAL_MACHINES = HALO_PORT_MAXIMUM_NETWORK_MACHINES + 1,
	SIGNAL_INTERVAL_TICKS = TICKS_PER_SECOND,
};

/* by machine, kind and reason (by their strings' addresses): when the last
line was written, and how many since */
static struct
{
	char const *kind;
	char const *reason;
	long last_time;
	long pending;
	short player_index;
} signal_slots[MAXIMUM_SIGNAL_MACHINES][MAXIMUM_SIGNAL_KINDS];

static void signal_write(
	long machine_index,
	char const *kind,
	char const *reason,
	short player_index,
	long count)
{
	char line[256];

	sprintf(line, "{\"tick\":%ld,\"kind\":\"%s\",\"machine\":%ld,\"player\":%d,\"reason\":\"%s\",\"count\":%ld}",
		game_time_get(), kind, machine_index, (int)player_index, reason ? reason : "", count);
	platform_append_signal(line);
}

void network_signal(
	char const *kind,
	char const *reason,
	long machine_index,
	short player_index)
{
	long machine = machine_index >= 0 && machine_index < MAXIMUM_SIGNAL_MACHINES - 1 ?
		machine_index : MAXIMUM_SIGNAL_MACHINES - 1;
	short index;

	for (index = 0; index < MAXIMUM_SIGNAL_KINDS; index++)
	{
		if (!signal_slots[machine][index].kind ||
			(signal_slots[machine][index].kind == kind && signal_slots[machine][index].reason == reason))
		{
			break;
		}
	}
	if (index == MAXIMUM_SIGNAL_KINDS)
	{
		/* (more kinds than slots: write it unthrottled) */
		signal_write(machine_index, kind, reason, player_index, 1);
		return;
	}
	if (!signal_slots[machine][index].kind)
	{
		signal_slots[machine][index].kind = kind;
		signal_slots[machine][index].reason = reason;
		signal_slots[machine][index].last_time = NONE;
	}
	signal_slots[machine][index].pending++;
	signal_slots[machine][index].player_index = player_index;
	if (signal_slots[machine][index].last_time == NONE ||
		game_time_get() - signal_slots[machine][index].last_time >= SIGNAL_INTERVAL_TICKS ||
		game_time_get() < signal_slots[machine][index].last_time)
	{
		signal_write(machine_index, kind, reason, player_index, signal_slots[machine][index].pending);
		signal_slots[machine][index].last_time = game_time_get();
		signal_slots[machine][index].pending = 0;
	}
}

void network_signals_new_game(
	void)
{
	csmemset(signal_slots, 0, sizeof(signal_slots));
}
