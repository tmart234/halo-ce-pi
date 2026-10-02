/*
NETWORK_DEDICATED.C

A dedicated host (network.dedicated in config.toml, HALO_DEDICATED): hosts
system link games from its configuration, with no player of its own, for
the players internet play brings it (stage H5 of the mmo repository's
docs/anticheat/08-reference-title-halo.md: with network.liveness, the
region's verified playlists).

- network.dedicated is the map rotation, "map[:variant],..." (bloodgulch,
  hangemhigh:ctf, ...; slayer by default, game_engine_get_variant_by_name).
- The host sets up a game as the pregame screen's fast setup does (as the
  automated network test, network_test.c, does), on the rotation's first
  map, but adds no player for a controller.
- It starts the game network.dedicated_start seconds after
  network.dedicated_min_players players have joined through the tunnel
  (p2p_player_count), and they stay.
- When the game ends, the host waits network.dedicated_postgame seconds
  on the postgame screen, takes the game back to the pregame lobby (what
  pressing A there does), moves to the rotation's next map, and starts
  again when the players are there.

The engine has always had a player on the hosting machine: a host without
one needs testing with the game's data (the build's tests have none).

Called from network_test_update every frame (main.c).
*/

#include "cseries.h"
#include "main/main.h"
#include "interface/player_ui.h"
#include "networking/network_game_globals.h"
#include "networking/network_client_manager.h"
#include "networking/network_server_manager.h"
#include "game/game.h"
#include "game/game_engine.h"

#include <stdio.h>
#include <string.h>

/* the platform layer's (port/linux/src/port_config.c, p2p.c) */
const char *config_string(char const *name);
double config_real(char const *name);
long config_integer(char const *name);
void platform_log(char const *format, ...);
int p2p_player_count(void);
/* network_server_manager.c's (its _internal header's) */
word network_game_server_get_state(struct network_game_server *server, short *substate);

enum
{
	MAXIMUM_ROTATION = 16,

	/* network_server_manager.c's states */
	_dedicated_server_pregame = 0,
	_dedicated_server_ingame,
	_dedicated_server_postgame,

	_dedicated_off = 0,
	/* waiting for the main menu, then setting the game up */
	_dedicated_setup,
	/* the lobby: the map set, waiting for players */
	_dedicated_lobby,
	_dedicated_playing,
	_dedicated_postgame,
};

static struct
{
	boolean checked;
	short state;
	char maps[MAXIMUM_ROTATION][64];
	char variants[MAXIMUM_ROTATION][64];
	short count;
	short next;
	real menu_seconds;
	real lobby_seconds;
	boolean map_set;
	real ready_seconds;
	real postgame_seconds;
	long last_players;
} dedicated;

static void dedicated_read_settings(
	void)
{
	const char *text = config_string("network.dedicated");

	dedicated.checked = TRUE;
	while (*text && dedicated.count < MAXIMUM_ROTATION)
	{
		char entry[128];
		char *colon;
		size_t length = strcspn(text, ", ");

		if (length && length < sizeof(entry))
		{
			memcpy(entry, text, length);
			entry[length] = 0;
			colon = strchr(entry, ':');
			if (colon)
				*colon = 0;
			if (entry[0])
			{
				snprintf(dedicated.maps[dedicated.count], sizeof(dedicated.maps[0]), "%s", entry);
				snprintf(dedicated.variants[dedicated.count], sizeof(dedicated.variants[0]), "%s",
					colon && colon[1] ? colon + 1 : "slayer");
				dedicated.count++;
			}
		}
		text += length;
		while (*text == ',' || *text == ' ')
			text++;
	}
	if (dedicated.count)
	{
		dedicated.state = _dedicated_setup;
		platform_log("dedicated host: %d maps in rotation, from %s:%s", (int)dedicated.count, dedicated.maps[0],
			dedicated.variants[0]);
	}
}

boolean network_dedicated_enabled(
	void)
{
	if (!dedicated.checked)
		dedicated_read_settings();
	return dedicated.state != _dedicated_off;
}

/* the rotation's next map and game variant, set on the server */
static void dedicated_set_map(
	struct network_game_server *server)
{
	char path[160];
	struct game_variant variant;
	short index = dedicated.next;

	main_set_multiplayer_map_name(dedicated.maps[index]);
	snprintf(path, sizeof(path), "levels\\test\\%s\\%s", dedicated.maps[index], dedicated.maps[index]);
	network_game_server_change_map_name(server, path);
	variant = *game_engine_get_variant_by_name(&variant, dedicated.variants[index]);
	player_ui_set_game_variant(&variant);
	network_game_server_change_game_variant(server, &variant);
	dedicated.next = (short)((index + 1) % dedicated.count);
	platform_log("dedicated host: next game %s:%s; waiting for %ld players", dedicated.maps[index],
		dedicated.variants[index], config_integer("network.dedicated_min_players"));
}

void network_dedicated_update(
	boolean main_menu_loaded,
	real seconds)
{
	struct network_game_server *server;
	long players;
	long needed;

	if (!network_dedicated_enabled())
		return;
	server = global_network_game_server_get();
	players = p2p_player_count();
	if (players != dedicated.last_players)
	{
		platform_log("dedicated host: %ld players", players);
		dedicated.last_players = players;
	}
	needed = config_integer("network.dedicated_min_players");
	if (needed < 1)
		needed = 1;

	switch (dedicated.state)
	{
	case _dedicated_setup:
		if (!main_menu_loaded)
			return;
		dedicated.menu_seconds += seconds;
		/* (the main menu settling first) */
		if (dedicated.menu_seconds < 2.0f)
			return;
		main_set_multiplayer_map_name(dedicated.maps[dedicated.next]);
		player_ui_fast_setup_network_server();
		platform_log("dedicated host: hosting");
		dedicated.state = _dedicated_lobby;
		dedicated.lobby_seconds = 0.0f;
		dedicated.map_set = FALSE;
		dedicated.ready_seconds = 0.0f;
		break;

	case _dedicated_lobby:
		dedicated.lobby_seconds += seconds;
		if (!server)
		{
			/* (the game went back to the menus) */
			if (dedicated.lobby_seconds > 5.0f)
			{
				dedicated.state = _dedicated_setup;
				dedicated.menu_seconds = 0.0f;
			}
			return;
		}
		/* the map (fast setup clears it) */
		if (!dedicated.map_set)
		{
			if (dedicated.lobby_seconds < 1.0f)
				return;
			dedicated_set_map(server);
			dedicated.map_set = TRUE;
		}
		if (players < needed)
		{
			dedicated.ready_seconds = 0.0f;
			return;
		}
		dedicated.ready_seconds += seconds;
		if (dedicated.ready_seconds >= (real)config_real("network.dedicated_start"))
		{
			network_game_client_request_immediate_start();
			platform_log("dedicated host: starting the game with %ld players", players);
			dedicated.state = _dedicated_playing;
		}
		break;

	case _dedicated_playing:
		if (!server)
		{
			dedicated.state = _dedicated_setup;
			dedicated.menu_seconds = 0.0f;
			return;
		}
		if (network_game_server_get_state(server, NULL) == _dedicated_server_postgame)
		{
			platform_log("dedicated host: the game is over");
			dedicated.state = _dedicated_postgame;
			dedicated.postgame_seconds = 0.0f;
		}
		break;

	case _dedicated_postgame:
		if (!server)
		{
			dedicated.state = _dedicated_setup;
			dedicated.menu_seconds = 0.0f;
			return;
		}
		dedicated.postgame_seconds += seconds;
		if (dedicated.postgame_seconds >= (real)config_real("network.dedicated_postgame"))
		{
			/* (what pressing A on the postgame screen does) */
			if (network_game_server_reset_to_pregame(server))
			{
				platform_log("dedicated host: back to the lobby");
				dedicated.state = _dedicated_lobby;
				dedicated.lobby_seconds = 0.0f;
				dedicated.map_set = FALSE;
				dedicated.ready_seconds = 0.0f;
			}
			else
			{
				platform_log("dedicated host: cannot go back to the lobby; hosting again");
				dedicated.state = _dedicated_setup;
				dedicated.menu_seconds = 0.0f;
			}
		}
		break;
	}
}
