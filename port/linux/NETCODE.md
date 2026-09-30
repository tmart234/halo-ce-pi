# Distributed netcode (the default)

The Xbox game plays system link in lockstep: clients send their input to
the host, the host sends every machine every player's input for each 30 Hz
tick, and every machine simulates the whole game from them, waiting for
each tick's update. A client therefore sees its own movement and shots a
full round trip late, and any machine whose simulation differs in the last
bit goes out of sync.

`network.netcode = "distributed"` replaces that with the model of later
Halo engines (the "distributed" simulation of the MonkeyNuts/Ares source)
with ideas from VALORANT's netcode articles, keeping the 30 Hz tick:

- **Every machine ticks on its own clock.** Nobody waits for anybody: a
  client no longer runs only the ticks the host has sent.
- **Own player predicted.** A client drives its own player (and the
  vehicle it drives) from its local input at once. Remote players are
  driven by the inputs the host relays every tick, the latest one held
  until a newer arrives.
- **Host authoritative.** The host alone decides damage, deaths, spawns,
  pickups, scores and the game's objects; clients do not decide them but
  apply what the host sends.
- **Corrections.** The host sends each client the authoritative state of
  the players' units and the game's moving objects; a client moves its
  copies toward it, a small error half of the way each tick, a larger one
  at once, drawn gliding from where they were. A client's own unit and
  vehicle are only corrected past a tolerance, so prediction does not
  rubber-band.
- **Shooter's hits.** A client reports what its own players hit; the host
  checks the report (the player's, a weapon they carry, the target where
  the host had it when the shooter saw it, no faster than weapons fire) and
  deals the damage. What the shooter saw hit, hits.

A client plays whichever netcode its host plays (the host's
`network.netcode`, which its game's advertisement carries).

## Versions

The native builds' network code has a version, an unsigned 16-bit number
(`HALO_PORT_NETWORK_VERSION` in `port/linux/include/halo_port_limits.h`),
raised with any change to what the machines send each other. A host puts it
in its game's advertisement (reserved bytes that hosts built before there
was a version send as zeros, so they are version 0). A client does not join
a host of another version: it shows a message box that says which of the
two is newer, with both versions ("update the game" or "ask the host to
update"), and stays in the list of games. Version 1 was the first of this
netcode; version 2 lets a machine join a game in progress; version 3 puts
each player in its slot of the host's player list on every machine;
version 4 plays the European (PAL) maps as the North American ones
(`port/linux/game/pal_tags.c`); version 5 adds host authority (below).

## Host authority

The game's own model trusts each client's word on where its own player is.
The host accepts a client's reported position if it is within 3.5 world
units of the host's copy, and then snaps to it. The next report is measured
from there, so a modified client can move 3.5 units a tick further than
physics allows, and through walls. `network.authority = "host"` (the
default; the host's setting applies) closes this. It is the server
authority of the `mmo` framework's Fair-Play Protocol ("don't trust, don't
send"):

- **The host ignores reports.** It moves every player, and every vehicle a
  player drives, from that player's input only (the input it already
  simulates each tick). It drops the players' and vehicles' position
  reports and counts them (`ignored` in the test report).
- **Each state carries the input tick it follows from.** In every unit
  state (`input_tick`, formerly padding) and in every state of a vehicle
  that a client's player drives, the host puts the low 16 bits of the
  latest tick of that player's input that it has run. A flag bit says the
  host is authoritative. A client learns it from these states, then stops
  sending position reports.
- **Clients reconcile.** A client keeps where its own unit (or the vehicle
  it drives) was after each of its last 64 ticks of input. When the host's
  state for input tick *t* arrives, the difference between the host's
  position and the client's prediction at *t* is the client's error. The
  client moves its unit by that error, drawn gliding
  (`network_objects_correct`), and shifts its predictions after *t* by the
  same amount so the error is not corrected twice. Errors under 0.02 world
  units are ignored. An error over 3 units (a respawn, a teleport), or a
  tick the client no longer has, falls back to the old correction: put
  where the host has it.

The price is the one every server-authoritative shooter pays: the host
runs a client's input when it arrives, not when the client ran it. A small
**input buffer** on the host (`player_queues_new.c`) keeps that from costing
inputs. Each of a client's actions waits in a queue, and each host tick runs
the oldest one. Two actions that arrive between two host ticks run on two
ticks, where before the second replaced the first. With none waiting, the
last is run again. With more than three waiting, the oldest are dropped, so
a burst does not leave the player behind for good. The input tick the host
reports is the one it actually ran, so the client's reconciliation compares
like with like. What remains is a late input: it runs a tick late, and the
client is corrected by that tick's difference.

Shooter's hits are still the client's, checked by the host
(`network_damage.c`). The checks cover the player, a weapon they carry, the
target where the host had it, the impact at the target and the fire rate.
They now also include **a path through the level**. The host casts a ray
from where it had the shooter's unit, at any of the last 32 ticks (about a
second: a round trip and a projectile's flight, sampled every fourth tick),
to the target, the damage's origin or its epicenter. The ray stops 0.25
units short. A report passes if any such ray is clear. The ray tests only
the level's solid one-sided surfaces. It ignores objects, invisible player
clipping, breakable glass and two-sided fences and grates, which shots may
pass. A shooter who had no line to the hit during the last second (a shot
through a wall) is rejected. An explosion is tested from where it went
off to the target (explosions reach round corners from there): the
engine's own test runs where the explosion is simulated, which for a
reported hit is the client. A melee blow passes only if the host had the
attacker within 4 units (and the target's size) of the target in the last
second.

A report must also carry the damage as the engine makes it, since the
client chose these numbers before:

- **Flags**: only those a player's own shot carries (an explosion, a
  localized effect, from a weapon, one object damaged). Kill instantly,
  bypasses shields, silent and no statistics the engine sets only on the
  host or for deaths no player deals: `forged_flags`.
- **An explosion only for damage the weapon deals as one**: a bullet
  marked an explosion would reach further and skip the path test:
  `forged_area`.
- **Scale and multiplier**: the engine's scale runs from 0 to 1 (a
  projectile's by its speed, an explosion's by distance), 1.5 for a melee
  blow from the air, and the multiplier is 1 until the host deals the
  damage. A report claiming more (ten times the damage, a one-shot kill)
  is `forged_scale`.

### Signals

Each rejection is also a **Signal**, a JSON line in `signals.jsonl` next to
`debug.txt` (`port/linux/game/network_signals.c`), for detection to read:

```json
{"tick":1234,"kind":"hit_report_rejected","machine":2,"player":5,"reason":"obstructed","count":3}
```

Kinds and reasons:

| Kind | Reasons |
| --- | --- |
| `hit_report_rejected` | `not_its_player`, `bad_target`, `weapon_not_carried`, `forged_flags`, `forged_area`, `forged_scale`, `target_not_there`, `impact_off_target`, `obstructed`, `melee_out_of_reach`, `rate`, `bad_damage` |
| `position_report_ignored` | `host_authority`: a machine kept sending position reports more than 3 seconds' worth after the host said it decides. A client of this version stops once it has the host's first state. |

The host writes each machine's kind and reason at most once a second, with
the count since its last line, so a modified client cannot flood the file.

`debug.cheat_movement_step` (debug builds) is the red-team client that
proves the gap and the fix. Its reports run the given number of units
ahead of its player. Against `network.authority = "client"`, a step under
3.5 moves the player that far each tick. Against `"host"`, the reports are
ignored:

```bash
HALO_NET_AUTHORITY=client HALO_NETWORK_TEST=host:bloodgulch ./halo &
HALO_CHEAT_MOVEMENT_STEP=3.0 HALO_TEST_INPUT=bot:1 HALO_NETWORK_TEST=join ./halo
```

`debug.cheat_wall_hits` (debug builds) is the same for shots.
`debug.network_test_shoot` normally hits the next player only when the
level leaves a line between them. With the cheat, it hits through walls,
and the host rejects those hits as `obstructed`:

```bash
HALO_NETWORK_TEST=host:bloodgulch HALO_NETWORK_TEST_SHOOT=2 ./halo &
HALO_CHEAT_WALL_HITS=1 HALO_TEST_INPUT=bot:1 HALO_NETWORK_TEST=join HALO_NETWORK_TEST_SHOOT=2 ./halo
grep obstructed signals.jsonl
```

Without the cheat, a soak of scripted bots (with `debug.network_latency`
and `debug.network_loss`) should show no rejected hits. Any rejected hit
there is a false reject to fix.

`debug.cheat_damage` (debug builds) forges the client's reports:
`scale` and `multiplier` (ten times the damage), `kill` (kill instantly),
`area` (a bullet as an explosion). The host rejects each.

Two more red-team switches (debug builds) show what host authority does
not fix, for the stages that do:

- `debug.cheat_radar`: a client that logs, every second, each player that
  no line through the level reaches from its own, with where the host says
  they are (`cheat: radar sees ...`). The host sends every client every
  unit, seen or not, so this works against every version so far. Relevance
  filtering on the host (the mmo framework's stage H6) is the fix.
- `debug.cheat_host_immunity`: a host that drops the clients' hits on its
  own players after the checks pass and counts them as dealt
  (`cheat: host immunity dropped ...`). The clients see the host's player
  unhurt, and nothing they can check at the time shows why. Only evidence
  of what the host was sent and what it decided (stage H4: signed inputs,
  host checkpoints and a replay auditor) proves it.

`tools/network_soak.py` runs all of this on one computer, each copy with
a data folder of its own (links to the game data) for its `debug.txt` and
`signals.jsonl`, and says what passed:

```bash
python tools/network_soak.py --data ~/halo            # everything
python tools/network_soak.py --data ~/halo --scenario soak --minutes 30 --latency 80 --loss 3
```

| Scenario | Passes when |
| --- | --- |
| `soak` | Honest bots under latency and loss: the host dealt hits and wrote no Signal (no false rejects) |
| `movement` | `debug.cheat_movement_step`: the host flags the client (`position_report_ignored`) |
| `wall_hits` | `debug.cheat_wall_hits`: the host rejects its hits as `obstructed` |
| `radar` | `debug.cheat_radar`: the radar saw hidden players (the attack works: H6 is open) |
| `host_immunity` | `debug.cheat_host_immunity`: the host dropped hits on its player (the attack works: H4 is open) |
| `forged_scale`, `forged_multiplier`, `forged_kill`, `forged_area` | `debug.cheat_damage`: the host rejects the forged reports for that reason, and no other hit |

## Joining a game in progress

A distributed game stays open when it starts (a lockstep one closes, as on
the Xbox, since every machine must simulate it from its first tick), and
the game list shows it. A machine that joins it is accepted as in the
pregame, and its players are added as the game adds a player in game:
every machine in the game spawns them, told by the game's own
`_message_server_add_player_ingame`. Then the host sends that machine alone
the game's settings and its start, with the host's game time (the start
message carries 16 bits of it); the machine loads the game, sets its clock
to that time and the ticks it spent loading, and takes the rest of the time
from the first game update if it is ahead (so that the game's timers read
as the host's). It takes up the host's count of updates where it is. When it has loaded, the distributed netcode
gives it the host's objects (network_objects.c), every player's statistics
and the game type's state, and it plays on as any other client.

The netcode names a player by its datum's index, which must be the same on
every machine, the one that joined too. That machine has not the players
who left (their datums stay until the game ends), nor the order in which
the others added players. So in a distributed game each player's datum is
its slot in the host's player list: every machine makes it there, and the
host gives a player added to the game in progress a slot whose datum is
free (`network_game_manager.c`). A player added to the game in progress
also gets its team and the game type's data, as the players at the start
do (in free for all, a team of its own).

Until it has loaded, the machine hears none of the game's messages (which a
machine in the pregame refuses, and which the others no longer need), only
a pregame keep-alive every five seconds from the host
(`network_server_manager.c`, `network_server_message_handler.c`).

## Stages

1. (Done) Decoupled ticks: clients tick on their own clock with local input
   for local players and the latest relayed input for remote ones; the host
   no longer waits for clients; taps are accumulated so a quick button
   press is never lost (lockstep too); out-of-sync checks off.
2. (Done) Authority: clients skip damage, deaths, spawns, pickups, item
   spawns, and scoring, and apply the host's state for them
   (`port/linux/game/network_distributed.c`):
   - every tick, every player's unit: which it is, alive or not, the seat
     it rides, shields and health (down, recharging, the damage they show),
     its powerups (camouflage and how long each has left), where it is (a
     client's own player's position is its own, within a tolerance, and the
     host takes it), and when dead who killed it;
   - what a client's players pick up, which the host decides: the client
     shows it (the HUD's message, the sound, a powerup's screen flash);
   - twice a second and with every kill, the players' statistics; five
     times a second, the game type's state (scores, the flags, the balls
     and their carriers, the king's hill) and whether the game is over.

   The messages are a kind of their own (the game's unused "data" message
   type), unreliable per tick, reliable for what must not be lost.
3. (Done) Object identity (`port/linux/game/network_objects.c`): the
   game's units, vehicles, weapons and equipment are the host's, at the same
   datum index (identifier and all) on every machine, so a message names
   one by its index.
   - The host tells its clients (reliably) of each such object it makes
     (what it is, where, how it looks) and each it deletes; clients make
     and delete theirs to match. A client that has loaded asks for all the
     host's objects and is told when it has them; from then on it deletes
     any such object the host has not told it of, and nothing but the
     host's word deletes the host's.
   - The objects placed when the map loads are placed alike everywhere:
     the host's word finds a client's already there. Past loading, a
     client's own objects (projectiles, effects: what only it sees) take
     indices from the upper half of the object array, clear of the host's.
   - Ten times a second, what every unit carries (the host's weapons, slot
     for slot, their ammunition, the weapon in hand, the grenades); a
     client moves the same weapon objects in and out of its units.
   - Players take the units the host spawns them with, seats are the
     host's (a client's own player's once it has ridden otherwise for
     longer than a round trip), and the CTF flags and oddballs are the same
     objects everywhere.
4. (Done) Corrections: every tick the host sends where its moving objects
   are (vehicles, items, bodies) and a few of those at rest, round them
   all. A client puts its copies there, and the difference is drawn fading
   over a few ticks (`render_interpolation.c`) instead of a jump. A client
   drives its own player's vehicle and sends where it is, which the host
   takes within a tolerance, as it does its own player's unit.
5. (Done) Hits (`port/linux/game/network_damage.c`):
   - A client deals no damage. What its own players' shots, grenades,
     melee and vehicles hit, it reports to the host (reliably).
   - The host deals a report once it has checked it: from that machine's
     player; damage one of their weapons (now or in the last ten seconds),
     their grenades or their vehicle can deal (its projectiles' impacts and
     detonations, followed through the tags); the target within a few
     world units of where the shooter saw it (more for a fast one); the
     impact at the target (an explosion within its reach); and no more
     reports than any weapon fires. Its own copies of a client's
     projectiles deal nothing (the report does).
   - The host sends its clients the damage it dealt to units, and a client
     replays what it does besides the harm (which the units' states
     bring): the player's screen flash and shake, the unit's flinch, pain
     sound, knockback and stun, the scope it knocks the player out of, and
     who the HUD shows hit them. A killing blow it replays whole, so the
     body falls as the shot had it and the kill is announced with the
     host's killer.

## Transport

What reaches the other machines, and how, decides how the game feels over
a real network as much as the model does (compared with Quake III, Source,
Unity's Netcode for Entities, lightyear, netfox and the Ares source):

- **Nothing held back.** The game's connections (the reliable messages:
  objects made and deleted, the game type's state, hits, pickups) send each
  write at once (`TCP_NODELAY`, in `xnet.c` for the game's sockets and in
  `p2p.c` for internet play's): with Nagle's algorithm a small write waited
  for the other end's delayed acknowledgement, up to 200 ms on Windows.
- **Input every tick, unreliably, each tick's buttons several times.** A
  client sends the host its players' input after each tick, with the
  buttons of the three ticks before it, and the host sends every client
  every player's input as its tick ran it, the same way. Each tick's
  buttons are taken once, from whichever message brings them first
  (`player_queues_new.c`), so a press is lost only with four datagrams lost
  in a row, and nothing waits for a lost one to be sent again (the game's
  own per-tick update, reliable and so held up by any loss, now carries no
  input). The host's clock still reaches the clients in it, and the game's
  own client update, whose input the host no longer takes, goes ten times
  a second instead of sixty.
- **One datagram a tick.** The unreliable messages of a tick to a machine
  go together (`_distributed_message_batch`), saving each one's headers
  (internet play's tunnel adds 35 bytes to every datagram).
- **Stamped with their tick.** Every message carries the sender's tick
  (its header's game time); an unreliable one that arrives after a newer of
  its kind is dropped, so a late datagram never puts anything back.
- **Taken at the tick.** The host takes a client's players' and vehicles'
  positions (the latest of each) at its next tick, not as each arrives.
- **Fewer bytes.** Vectors travel in 16 bits a part, shields and health in
  16 bits; what a unit carries is sent when it changes (and once a second);
  the objects at rest are sent round all of them, four a tick; the players'
  statistics when they change (with every kill, twice a second), with
  sixteen more players' each time round them all.
- **The players each client needs, when it needs them.** The host sends a
  client every player's unit and input every tick when they are within 25
  world units of the client's own players, every second tick within 60,
  every third within 120, every fourth further off, and every sixth when no
  cluster of the client's players' can see theirs (the map's potentially
  visible set, which errs on the side of seeing: Halo's are coarse, and
  most of a map sees most of it). None is ever left out. Whatever changes
  what a client sees goes at once:
  - a player's death, spawn or seat, and a player coming into sight;
  - a player's input every tick while their buttons or weapon choices
    change (the ticks it carries), so no jump, grenade or melee of theirs
    is missed, even out of sight;
  - a player a client's player aims at within 35 degrees at least every
    second tick, and within 20 degrees through a scope every tick, so a
    sniper sees a far player move as smoothly as a near one (as Ares
    raises the priority of what a player zooms onto).

  A client's own players' units go to it every tick, their input never (it
  has its own).
- **The round trip.** A client's input messages tell the host the latest
  host tick the client has had, which gives the host each client's round
  trip (smoothed as TCP smooths its own). The host keeps a second of where
  players' units and vehicles were, and checks a client's hit against where
  the target was as far back as that round trip, instead of against where
  it is now with a wide margin.

## Testing

`debug.network_test` (`port/linux/game/network_test.c`) hosts or joins a
game without the menus (in a team game the joining player takes the other
team), and `debug.test_input` plays controller 1 with a scripted bot; each
machine logs every player's position, health and shields, weapons,
grenades, score, kills and deaths every second, with the objects made and
removed and the hits reported, dealt, rejected and replayed, so two
machines' views of one game can be compared. `debug.network_test_kill`,
`debug.network_test_shoot`, `debug.network_test_vehicle` and
`debug.network_test_pickup` script kills, hits, a vehicle ride and a weapon
swap the bots' wandering does not reach. `debug.network_latency` and
`debug.network_loss` hold back what a machine receives and drop some of its
datagrams, to test as over the internet.

The host logs to `debug.txt` when a player on another machine presses the
action button where the host has nothing for them to pick up, with where it
has them and the nearest item: a client that sees a pickup the host does
not.
