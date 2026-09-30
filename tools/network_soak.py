"""Soak and red-team runs of the distributed netcode (stages H1 and H2).

Plays automated system link games on one computer (the copies on their own
loopback addresses, as in port/linux/README.md "Play on one computer") and
checks what the host's Signals (signals.jsonl, port/linux/game/network_signals.c)
and the machines' logs say:

- ``soak``: a host and honest scripted bots under simulated latency and loss,
  shooting each other (debug.network_test_shoot) and dying
  (debug.network_test_kill). Passes when the host dealt hits and wrote no
  Signal at all: any rejection of an honest bot is a false reject.
- ``movement``: one client runs debug.cheat_movement_step. Passes when the host
  flags it (position_report_ignored).
- ``wall_hits``: one client runs debug.cheat_wall_hits. Passes when the host
  rejects its hits as obstructed.
- ``radar``: one client runs debug.cheat_radar against a host filtering by
  relevance (network.relevance, H6). Passes when the host withheld players
  from it; says how many it still saw behind walls (near ones, and those the
  level's coarse visibility could not rule out).
- ``radar_open``: the same with network.relevance = false. Passes when the
  radar saw players behind walls and none was withheld: the attack works
  against a host that sends everyone everything, as the Xbox did.
- ``host_immunity``: the host runs debug.cheat_host_immunity. Passes when it
  dropped hits on its own player: the attack works, as it will until
  evidence and the replay auditor (H4) prove it.
- ``forged_scale``, ``forged_multiplier``, ``forged_kill``, ``forged_area``: one
  client runs debug.cheat_damage, its hit reports claiming ten times the
  damage, a kill-instantly flag, or a bullet as an explosion. Passes when
  the host rejects them for that reason, and nothing else.

The cheats are in debug builds only (ninja linux). You need the game's data:
--data is the folder holding maps/ (paths.data). Each copy gets a folder of
its own under --work holding links to the data, so each keeps its own
debug.txt and signals.jsonl there.

    python tools/network_soak.py --data ~/halo --minutes 10 --clients 3
    python tools/network_soak.py --data ~/halo --scenario movement --scenario wall_hits
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

# debug.cheat_damage's forgeries, and the reason the host rejects each with
FORGERIES = {
    "forged_scale": ("scale", "forged_scale"),
    "forged_multiplier": ("multiplier", "forged_scale"),
    "forged_kill": ("kill", "forged_flags"),
    "forged_area": ("area", "forged_area"),
}
SCENARIOS = ("soak", "movement", "wall_hits", "radar", "radar_open", "host_immunity", *FORGERIES)
FIRST_ADDRESS = 200
# the copies' own files in a data root, which each keeps apart
OWN_FILES = {"debug.txt", "signals.jsonl", "config.toml"}
# a log line of network_test_log_players (network_test.c)
STATUS = re.compile(r"network test: tick (\d+).*\| hits (\d+) dealt (\d+) rejected (\d+) replayed (\d+) "
                    r"\| authority (\w+) ignored (\d+) reconciled (\d+)")


@dataclass
class Machine:
    name: str
    address: str
    env: Dict[str, str]
    root: Path
    process: Optional[subprocess.Popen] = None
    log: str = ""

    def signals(self) -> List[dict]:
        path = self.root / "signals.jsonl"
        if not path.exists():
            return []
        return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]

    def statuses(self) -> List[re.Match]:
        return list(STATUS.finditer(self.log))


@dataclass
class Result:
    scenario: str
    passed: bool
    details: List[str] = field(default_factory=list)


def make_root(work: Path, name: str, data: Path) -> Path:
    root = work / name
    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    for entry in data.iterdir():
        if entry.name not in OWN_FILES:
            (root / entry.name).symlink_to(entry.resolve())
    return root


def plan(args: argparse.Namespace, scenario: str, work: Path) -> List[Machine]:
    """the host and its clients for a scenario, with their settings"""
    clients = args.clients if scenario == "soak" else max(args.clients, 2)
    addresses = [f"127.0.0.{FIRST_ADDRESS + index}" for index in range(clients + 1)]
    seconds = args.minutes * 60 if scenario == "soak" else args.attack_seconds
    common = {
        "HALO_NULL_RENDERER": "1",
        "HALO_HIDDEN_WINDOW": "1",
        "HALO_EXIT_AFTER": str(seconds + args.start_delay + 30),
        "HALO_NETWORK_TEST_START": str(args.start_delay),
        "HALO_NETWORK_TEST_SHOOT": str(args.shoot),
        "HALO_NETWORK_LATENCY": str(args.latency),
        "HALO_NETWORK_LOSS": str(args.loss),
    }
    machines = []
    for index, address in enumerate(addresses):
        name = "host" if index == 0 else f"client{index}"
        env = dict(common)
        env["HALO_NET_ADDRESS"] = address
        env["HALO_NET_BROADCAST"] = ",".join(a for a in addresses if a != address) if index == 0 else addresses[0]
        env["HALO_TEST_INPUT"] = f"bot:{index + 1}"
        if index == 0:
            env["HALO_NETWORK_TEST"] = f"host:{args.map}"
            env["HALO_NETWORK_TEST_KILL"] = str(args.kill)
            if scenario == "host_immunity":
                env["HALO_CHEAT_HOST_IMMUNITY"] = "1"
            env["HALO_NET_RELEVANCE"] = "false" if scenario == "radar_open" else "true"
        else:
            env["HALO_NETWORK_TEST"] = "join"
            # the last client is the cheat
            if index == len(addresses) - 1:
                if scenario == "movement":
                    env["HALO_CHEAT_MOVEMENT_STEP"] = str(args.movement_step)
                elif scenario == "wall_hits":
                    env["HALO_CHEAT_WALL_HITS"] = "1"
                elif scenario in ("radar", "radar_open"):
                    env["HALO_CHEAT_RADAR"] = "1"
                elif scenario in FORGERIES:
                    env["HALO_CHEAT_DAMAGE"] = FORGERIES[scenario][0]
        machines.append(Machine(name, address, env, make_root(work, f"{scenario}/{name}", args.data)))
    return machines


def run(args: argparse.Namespace, machines: List[Machine]) -> None:
    for index, machine in enumerate(machines):
        env = dict(os.environ)
        env.update(machine.env)
        env["HALO_DATA_ROOT"] = str(machine.root)
        machine.process = subprocess.Popen([str(args.binary)], env=env, stdout=subprocess.PIPE,
                                           stderr=subprocess.STDOUT, text=True, errors="replace")
        # (the host up before the clients search)
        time.sleep(5 if index == 0 else 1)
    deadline = time.monotonic() + float(machines[0].env["HALO_EXIT_AFTER"]) + 60
    for machine in machines:
        try:
            machine.log, _ = machine.process.communicate(timeout=max(1.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            machine.process.send_signal(signal.SIGTERM)
            machine.log, _ = machine.process.communicate()
        (machine.root / "stdout.txt").write_text(machine.log)


def judge(scenario: str, machines: List[Machine]) -> Result:
    """what the run shows, from the logs and the host's Signals"""
    host, clients = machines[0], machines[1:]
    signals = host.signals()
    kinds: Dict[str, int] = {}
    for entry in signals:
        key = f"{entry.get('kind')}:{entry.get('reason')}"
        kinds[key] = kinds.get(key, 0) + int(entry.get("count", 1))
    details = [f"host Signals: {kinds or 'none'}"]
    statuses = host.statuses()
    if not statuses:
        return Result(scenario, False, details + ["the host logged no game (is the data there? "
                                                  "look at the stdout.txt files)"])
    last = statuses[-1]
    dealt, rejected = int(last.group(3)), int(last.group(4))
    details.append(f"host at tick {last.group(1)}: dealt {dealt} hits, rejected {rejected}")
    playing = [c for c in clients if c.statuses()]
    if len(playing) < len(clients):
        return Result(scenario, False, details + [f"only {len(playing)} of {len(clients)} clients played"])
    for client in clients:
        authority = client.statuses()[-1].group(6)
        if authority != "host":
            details.append(f"{client.name} played under {authority} authority")
            return Result(scenario, False, details)

    if scenario == "soak":
        passed = not signals and rejected == 0 and dealt > 0
        if dealt == 0:
            details.append("no hits were dealt: raise the game's length or lower --shoot")
        if signals:
            details.append("false rejects: an honest bot's hits or reports were rejected")
        return Result(scenario, passed, details)
    if scenario == "movement":
        flagged = kinds.get("position_report_ignored:host_authority", 0)
        return Result(scenario, flagged > 0, details + [f"cheat flagged {flagged} times"])
    if scenario == "wall_hits":
        obstructed = kinds.get("hit_report_rejected:obstructed", 0)
        return Result(scenario, obstructed > 0, details + [f"{obstructed} hits rejected as obstructed"])
    if scenario in ("radar", "radar_open"):
        seen = sum(int(count) for count in re.findall(r"cheat: radar sees (\d+) hidden", clients[-1].log))
        withheld = sum(int(count) for count in re.findall(r"cheat: radar withheld (\d+)", clients[-1].log))
        details.append(f"the radar saw {seen} hidden players; the host withheld {withheld}")
        if scenario == "radar_open":
            return Result(scenario, seen > 0 and withheld == 0, details)
        return Result(scenario, withheld > 0, details)
    if scenario in FORGERIES:
        reason = FORGERIES[scenario][1]
        rejected_forged = kinds.get(f"hit_report_rejected:{reason}", 0)
        others = [key for key in kinds if key.startswith("hit_report_rejected:") and key != f"hit_report_rejected:{reason}"]
        if others:
            details.append(f"other rejections (false rejects of the honest client?): {others}")
        return Result(scenario, rejected_forged > 0 and not others,
                      details + [f"{rejected_forged} forged hits rejected as {reason}"])
    if scenario == "host_immunity":
        dropped = len(re.findall(r"cheat: host immunity dropped", host.log))
        return Result(scenario, dropped > 0,
                      details + [f"the host dropped {dropped} hits on its player (H09 open until H4)"])
    raise ValueError(scenario)


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", type=Path, required=True, help="the folder holding maps/")
    parser.add_argument("--binary", type=Path, default=Path("build/linux/halo"))
    parser.add_argument("--work", type=Path, default=Path("build/network_soak"))
    parser.add_argument("--scenario", action="append", choices=SCENARIOS,
                        help="run only these (repeatable); all by default")
    parser.add_argument("--map", default="bloodgulch")
    parser.add_argument("--clients", type=int, default=3)
    parser.add_argument("--minutes", type=float, default=10.0, help="the soak's length")
    parser.add_argument("--attack-seconds", type=float, default=90.0, help="each red-team game's length")
    parser.add_argument("--latency", type=float, default=60.0, help="ms each machine holds what it receives")
    parser.add_argument("--loss", type=float, default=2.0, help="percent of datagrams dropped")
    parser.add_argument("--shoot", type=float, default=3.0, help="seconds between scripted shots")
    parser.add_argument("--kill", type=float, default=20.0, help="seconds between scripted kills")
    parser.add_argument("--start-delay", type=float, default=8.0)
    parser.add_argument("--movement-step", type=float, default=3.0)
    args = parser.parse_args(argv)

    if not (args.data / "maps").is_dir():
        parser.error(f"{args.data} has no maps/ folder")
    if not args.binary.exists():
        parser.error(f"{args.binary} is missing: build it with ninja linux (a debug build, for the cheats)")
    results = []
    for scenario in args.scenario or SCENARIOS:
        print(f"== {scenario}", flush=True)
        machines = plan(args, scenario, args.work)
        run(args, machines)
        result = judge(scenario, machines)
        results.append(result)
        for line in result.details:
            print(f"   {line}")
        print(f"   {'PASS' if result.passed else 'FAIL'}  (logs: {args.work / scenario})", flush=True)
    failed = [r.scenario for r in results if not r.passed]
    print("all passed" if not failed else f"failed: {', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
