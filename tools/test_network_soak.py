"""The verdicts of tools/network_soak.py, from made-up logs and Signals (the
runs themselves need the game's data)."""

import argparse
import json
from pathlib import Path

import network_soak


def status(tick, dealt, rejected, authority="host"):
    return (f"network test: tick {tick} player 0: (1.000 2.000 3.000) h1.00/1.00 | items 0 (+0 -0 !0 x0) | playing"
            f" | sent 1 received 1 corrected 0 | hits 9 dealt {dealt} rejected {rejected} replayed 0"
            f" | authority {authority} ignored 0 reconciled 4\n")


def machines(tmp_path: Path, host_log: str, client_logs, signals=()):
    result = []
    for index, log in enumerate([host_log, *client_logs]):
        root = tmp_path / f"m{index}"
        root.mkdir(parents=True)
        result.append(network_soak.Machine(f"m{index}", f"127.0.0.{200 + index}", {}, root, log=log))
    (result[0].root / "signals.jsonl").write_text("".join(json.dumps(s) + "\n" for s in signals))
    return result


def test_soak_passes_without_signals(tmp_path):
    ms = machines(tmp_path, status(900, 12, 0), [status(900, 0, 0)])
    assert network_soak.judge("soak", ms).passed


def test_soak_fails_on_a_false_reject(tmp_path):
    ms = machines(tmp_path, status(900, 12, 1), [status(900, 0, 0)],
                  [{"tick": 5, "kind": "hit_report_rejected", "machine": 1, "player": 1, "reason": "obstructed",
                    "count": 1}])
    result = network_soak.judge("soak", ms)
    assert not result.passed
    assert any("false rejects" in line for line in result.details)


def test_soak_needs_hits(tmp_path):
    ms = machines(tmp_path, status(900, 0, 0), [status(900, 0, 0)])
    assert not network_soak.judge("soak", ms).passed


def test_a_client_under_client_authority_fails(tmp_path):
    ms = machines(tmp_path, status(900, 3, 0), [status(900, 0, 0, authority="client")])
    assert not network_soak.judge("soak", ms).passed


def test_no_game_fails(tmp_path):
    ms = machines(tmp_path, "no maps/ folder found\n", [""])
    assert not network_soak.judge("soak", ms).passed


def test_attacks(tmp_path):
    flagged = [{"kind": "position_report_ignored", "reason": "host_authority", "count": 30}]
    ms = machines(tmp_path / "a", status(900, 0, 0), [status(900, 0, 0), status(900, 0, 0)], flagged)
    assert network_soak.judge("movement", ms).passed
    assert not network_soak.judge("wall_hits", ms).passed

    obstructed = [{"kind": "hit_report_rejected", "reason": "obstructed", "count": 2}]
    ms = machines(tmp_path / "b", status(900, 0, 2), [status(900, 0, 0), status(900, 0, 0)], obstructed)
    assert network_soak.judge("wall_hits", ms).passed

    radar = status(900, 0, 0) + "cheat: radar sees 2 hidden (2 so far): player 0 (1 2 3) player 1 (4 5 6)\n"
    ms = machines(tmp_path / "c", status(900, 0, 0), [status(900, 0, 0), radar])
    # (against a host sending everything: the attack works)
    assert network_soak.judge("radar_open", ms).passed
    # (against relevance: the host must have withheld someone)
    assert not network_soak.judge("radar", ms).passed
    filtered = radar + "cheat: radar withheld 3 (3 so far)\n"
    ms = machines(tmp_path / "c2", status(900, 0, 0), [status(900, 0, 0), filtered])
    assert network_soak.judge("radar", ms).passed
    assert not network_soak.judge("radar_open", ms).passed

    host = status(900, 4, 0) + "cheat: host immunity dropped a hit on its player 0 (1 so far)\n"
    ms = machines(tmp_path / "d", host, [status(900, 0, 0), status(900, 0, 0)])
    assert network_soak.judge("host_immunity", ms).passed


def test_plan_puts_the_cheat_on_the_last_client(tmp_path):
    data = tmp_path / "data"
    (data / "maps").mkdir(parents=True)
    (data / "debug.txt").write_text("old")
    args = argparse.Namespace(clients=1, minutes=1.0, attack_seconds=60.0, start_delay=8.0, shoot=3.0, latency=60.0,
                              loss=2.0, map="bloodgulch", kill=20.0, movement_step=3.0, data=data)
    ms = network_soak.plan(args, "wall_hits", tmp_path / "work")
    assert len(ms) == 3
    assert "HALO_CHEAT_WALL_HITS" in ms[2].env and "HALO_CHEAT_WALL_HITS" not in ms[1].env
    assert ms[0].env["HALO_NETWORK_TEST"] == "host:bloodgulch"
    assert ms[0].env["HALO_NET_BROADCAST"] == "127.0.0.201,127.0.0.202"
    assert (ms[1].root / "maps").is_symlink() and not (ms[1].root / "debug.txt").exists()


def test_forgeries(tmp_path):
    forged = [{"kind": "hit_report_rejected", "reason": "forged_scale", "count": 4}]
    ms = machines(tmp_path / "a", status(900, 2, 4), [status(900, 0, 0), status(900, 0, 0)], forged)
    assert network_soak.judge("forged_scale", ms).passed
    assert network_soak.judge("forged_multiplier", ms).passed
    assert not network_soak.judge("forged_kill", ms).passed

    # a false reject of the honest client beside the forgery fails it
    mixed = forged + [{"kind": "hit_report_rejected", "reason": "obstructed", "count": 1}]
    ms = machines(tmp_path / "b", status(900, 2, 5), [status(900, 0, 0), status(900, 0, 0)], mixed)
    assert not network_soak.judge("forged_scale", ms).passed

    area = [{"kind": "hit_report_rejected", "reason": "forged_area", "count": 3}]
    ms = machines(tmp_path / "c", status(900, 2, 3), [status(900, 0, 0), status(900, 0, 0)], area)
    assert network_soak.judge("forged_area", ms).passed

    kill = [{"kind": "hit_report_rejected", "reason": "forged_flags", "count": 3}]
    ms = machines(tmp_path / "d", status(900, 2, 3), [status(900, 0, 0), status(900, 0, 0)], kill)
    assert network_soak.judge("forged_kill", ms).passed
