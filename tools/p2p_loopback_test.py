"""Internet play on one computer: a host, a joiner that opens its invite, and
the test MQTT broker (tools/mqtt_test_broker.py) in place of the public ones.
No game data is needed: the tunnel starts when the game hosts (the automated
network test's fast setup), and the joiner's system link list gets the host's
game through it.

Passes when the joiner and the host log a secure session, keep it, and a
third copy whose invite has another host key (an impostor's, or a forged
signalling answer) reaches the host's address but gets no session.

--evidence tests the host's evidence (stage H4) instead: synthetic hit
reports (debug.evidence_synthetic) from a joined player, audited with the
SDK's auditor (crates/fpp-audit): an honest host's evidence is clean, and a
host that leaves out outcomes is caught from the player's bundle.

--trust tests the host's trust policy instead (network.minimum_tier 2): a
joiner with a D2 Attestation Result bound to its session key is admitted;
one with a D1 result, one with none, and one presenting the first's result
under its own key are refused. The results come from the SDK's development
Verifier (cargo run -p fpp-ffi --example mint_ar, in the mmo checkout
tools/fpp_sdk.py builds from), so it needs Rust.

    python tools/p2p_loopback_test.py [--binary build/linux/halo] [--trust]
"""

import argparse
import os
import re
import secrets
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mqtt_test_broker  # noqa: E402

INVITE = re.compile(r"halo://join/[0-9a-zA-Z_-]+")


def start(binary: Path, root: Path, env: dict, args: List[str]) -> subprocess.Popen:
    # (an empty maps/ is enough to reach the menus, and hosting)
    (root / "maps").mkdir(parents=True, exist_ok=True)
    full = dict(os.environ)
    full.update(env)
    full["HALO_DATA_ROOT"] = str(root)
    return subprocess.Popen([str(binary), *args], env=full, stdout=open(root / "stdout.txt", "w"),
                            stderr=subprocess.STDOUT)


def wait_for(path: Path, pattern: str, seconds: float) -> Optional[re.Match]:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        match = re.search(pattern, path.read_text(errors="replace")) if path.exists() else None
        if match:
            return match
        time.sleep(0.25)
    return None


def mmo_checkout() -> Path:
    local = os.environ.get("HALO_FPP_SOURCE")
    return Path(local) if local else Path(__file__).resolve().parent.parent / "build/third_party/mmo"


def mint(*args: str) -> str:
    """the SDK's development Verifier (examples/mint_ar.rs)"""
    result = subprocess.run(["cargo", "run", "-q", "--release", "--locked", "-p", "fpp-ffi", "--example", "mint_ar",
                             "--", *args], cwd=mmo_checkout(), check=True, capture_output=True, text=True)
    return result.stdout.strip()


def trust_test(args: argparse.Namespace, work: Path, common: dict) -> List[str]:
    """the host's trust policy: who is admitted and who is refused"""
    failures = []
    verifier = secrets.token_hex(32)
    work.mkdir(parents=True, exist_ok=True)
    joiners = {}
    for name in ("trusted", "low", "none", "stolen"):
        seed = secrets.token_hex(32)
        (work / f"{name}.seed").write_text(seed)
        joiners[name] = {"seed": seed, "public": mint("session", seed)}
    mint("ar", verifier, joiners["trusted"]["public"], "2", str(work / "trusted.ar"))
    mint("ar", verifier, joiners["low"]["public"], "1", str(work / "low.ar"))
    processes = []
    try:
        host = start(args.binary, work / "host", {**common, "HALO_NET_ADDRESS": "127.0.0.200",
                                                  "HALO_NETWORK_TEST": "host:bloodgulch",
                                                  "HALO_NET_MINIMUM_TIER": "2",
                                                  "HALO_NET_VERIFIER_KEYS": mint("key", verifier)}, [])
        processes.append(host)
        invite = wait_for(work / "host" / "stdout.txt", INVITE.pattern, 20)
        if not invite:
            return ["the host made no invite"]
        attestations = {"trusted": "trusted.ar", "low": "low.ar", "none": "", "stolen": "trusted.ar"}
        for index, name in enumerate(joiners):
            env = {**common, "HALO_NET_ADDRESS": f"127.0.0.{201 + index}", "HALO_NETWORK_TEST": "join",
                   "HALO_NET_SESSION_KEY": str(work / f"{name}.seed")}
            if attestations[name]:
                env["HALO_NET_ATTESTATION"] = str(work / attestations[name])
            processes.append(start(args.binary, work / name, env, [invite.group(0)]))
        checks = [
            ("trusted", r"connected to host \w+ \(secure session\)"),
            ("host", r"player \w+ is a D2 windows device"),
            ("low", r"host \w+: admits only more trusted devices"),
            ("host", r"refused player \w+: a D1 windows device"),
            ("none", r"host \w+: admits only more trusted devices"),
            ("host", r"refused player \w+: no attestation"),
            ("stolen", r"host \w+: could not verify this device's attestation"),
            ("host", r"refused player \w+: its attestation does not verify \(token: bound to another session key\)"),
        ]
        for name, pattern in checks:
            found = wait_for(work / name / "stdout.txt", pattern, 30)
            print(f"{'ok  ' if found else 'FAIL'} {name}: {pattern}")
            if not found:
                failures.append(f"{name} never logged {pattern!r}")
        for name in ("low", "none", "stolen"):
            if "connected to host" in (work / name / "stdout.txt").read_text(errors="replace"):
                failures.append(f"{name} was admitted")
        players = len(re.findall(r"connected to player", (work / "host" / "stdout.txt").read_text(errors="replace")))
        print(f"{'ok  ' if players == 1 else 'FAIL'} host: {players} player admitted")
        if players != 1:
            failures.append(f"the host admitted {players} players")
    finally:
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
    return failures


def audit(bundles: List[Path]) -> subprocess.CompletedProcess:
    """the SDK's evidence auditor (crates/fpp-audit), on bundles"""
    return subprocess.run(["cargo", "run", "-q", "--release", "--locked", "-p", "fpp-audit", "--",
                           *(str(b.resolve()) for b in bundles)],
                          cwd=mmo_checkout(), capture_output=True, text=True)


def evidence_test(args: argparse.Namespace, work: Path, common: dict) -> List[str]:
    """the host's evidence (stage H4), without a game: an honest host's
    evidence audits clean; a cheating host's (debug.evidence_synthetic
    omit: every other hit report left without an outcome) is caught from its
    player's bundle"""
    failures = []
    for mode, honest in (("on", True), ("omit", False)):
        base = work / mode
        env = {**common, "HALO_EVIDENCE_SYNTHETIC": mode}
        processes = []
        try:
            processes.append(start(args.binary, base / "host", {**env, "HALO_NET_ADDRESS": "127.0.0.200",
                                                                "HALO_NETWORK_TEST": "host:bloodgulch"}, []))
            invite = wait_for(base / "host" / "stdout.txt", INVITE.pattern, 20)
            if not invite:
                failures.append(f"{mode}: the host made no invite")
                continue
            processes.append(start(args.binary, base / "joiner", {**env, "HALO_NET_ADDRESS": "127.0.0.201",
                                                                  "HALO_NETWORK_TEST": "join"}, [invite.group(0)]))
            if not wait_for(base / "joiner" / "stdout.txt", r"evidence goes to", 30):
                failures.append(f"{mode}: the player never started its evidence")
                continue
            # (three epochs of five seconds, and the Checkpoints' grace)
            time.sleep(20)
        finally:
            for process in processes:
                process.terminate()
            for process in processes:
                try:
                    process.wait(10)
                except subprocess.TimeoutExpired:
                    process.kill()
        bundles = sorted(base.glob("*/evidence/*.fppb"))
        players = [b for b in bundles if "-player-" in b.name]
        if len(players) != 1 or len(bundles) != 2:
            failures.append(f"{mode}: expected a host and a player bundle, found {[b.name for b in bundles]}")
            continue
        result = audit(bundles)
        print(result.stdout.rstrip())
        if result.returncode not in (0, 1):
            failures.append(f"{mode}: the auditor failed: {result.stderr.strip()}")
            continue
        checkpoints = re.search(r"-player-.*?(\d+) checkpoints, (\d+) outcomes", result.stdout)
        if not checkpoints or int(checkpoints.group(1)) < 2 or int(checkpoints.group(2)) < 2:
            failures.append(f"{mode}: the player's bundle holds too little to audit")
        caught = "no outcome for unit" in result.stdout
        if honest and (result.returncode != 0 or caught):
            failures.append("the honest host's evidence did not audit clean")
        if not honest and (result.returncode != 1 or not caught):
            failures.append("the cheating host was not caught")
        print(f"{'ok  ' if not failures else 'FAIL'} {mode}: "
              f"{'clean' if result.returncode == 0 else 'findings'}{' (dropped outcomes caught)' if caught else ''}")
    return failures


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=Path("build/linux/halo"))
    parser.add_argument("--work", type=Path, default=None)
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--trust", action="store_true", help="test the host's trust policy instead")
    parser.add_argument("--synthetic", default="", help="(development) debug.evidence_synthetic for both copies")
    parser.add_argument("--evidence", action="store_true",
                        help="test the host's evidence and its audit instead (needs Rust: fpp-audit)")
    args = parser.parse_args(argv)

    work = args.work or Path(tempfile.mkdtemp(prefix="p2p_loopback_"))
    broker = mqtt_test_broker.Broker(0)
    threading.Thread(target=broker.serve, daemon=True).start()
    common = {
        "HALO_NET_BROKERS": f"127.0.0.1:{broker.port}",
        "HALO_NET_STUN": "",
        "HALO_NET_ALLOW_UPNP": "false",
        "HALO_NULL_RENDERER": "1",
        "HALO_HIDDEN_WINDOW": "1",
        "HALO_EXIT_AFTER": str(args.seconds),
        "HALO_NETWORK_TEST_START": "600",
        # (not over the LAN: the copies see each other only through the tunnel)
        "HALO_NET_BROADCAST": "127.0.0.254",
    }
    if args.synthetic:
        common["HALO_EVIDENCE_SYNTHETIC"] = args.synthetic
    if args.trust or args.evidence:
        failures = trust_test(args, work, common) if args.trust else evidence_test(args, work, common)
        print(f"logs: {work}")
        for failure in failures:
            print(f"FAIL: {failure}")
        print("passed" if not failures else "failed")
        return 1 if failures else 0
    processes = []
    failures = []
    try:
        host = start(args.binary, work / "host", {**common, "HALO_NET_ADDRESS": "127.0.0.200",
                                                  "HALO_NETWORK_TEST": "host:bloodgulch"}, [])
        processes.append(host)
        invite = wait_for(work / "host" / "stdout.txt", INVITE.pattern, 20)
        if not invite:
            failures.append("the host made no invite")
        else:
            print(f"invite: {invite.group(0)}")
            joiner = start(args.binary, work / "joiner", {**common, "HALO_NET_ADDRESS": "127.0.0.201",
                                                          "HALO_NETWORK_TEST": "join"}, [invite.group(0)])
            processes.append(joiner)
            # an invite whose host key is not the host's: signalling still
            # works (it needs only the token), but no session may come of it
            link = invite.group(0)
            forged = link[:-1] + ("0" if link[-1] != "0" else "1")
            impostor = start(args.binary, work / "forged", {**common, "HALO_NET_ADDRESS": "127.0.0.202",
                                                            "HALO_NETWORK_TEST": "join"}, [forged])
            processes.append(impostor)
            checks = [
                (work / "joiner" / "stdout.txt", r"Internet play: connected to host \w+ \(secure session\)"),
                (work / "host" / "stdout.txt", r"Internet play: connected to player \w+ \(secure session"),
                (work / "forged" / "stdout.txt", r"Internet play: host \w+ answers at"),
            ]
            for path, pattern in checks:
                found = wait_for(path, pattern, 30)
                print(f"{'ok  ' if found else 'FAIL'} {path.parent.name}: {pattern}")
                if not found:
                    failures.append(f"{path.parent.name} never logged {pattern!r}")
            # the session carries the tunnel's pings: still up a while later
            time.sleep(min(15.0, args.seconds / 2))
            for name in ("joiner", "host"):
                log = (work / name / "stdout.txt").read_text(errors="replace")
                if "lost the connection" in log:
                    failures.append(f"{name} lost the connection")
            forged_log = (work / "forged" / "stdout.txt").read_text(errors="replace")
            ok = "connected to host" not in forged_log
            print(f"{'ok  ' if ok else 'FAIL'} forged: no session with a host key that is not the host's")
            if not ok:
                failures.append("a joiner with a forged host key connected")
            host_log = (work / "host" / "stdout.txt").read_text(errors="replace")
            players = len(re.findall(r"connected to player", host_log))
            print(f"{'ok  ' if players == 1 else 'FAIL'} host: {players} player connected (the forged one is not)")
            if players != 1:
                failures.append(f"the host connected {players} players")
    finally:
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
    print(f"logs: {work}")
    for failure in failures:
        print(f"FAIL: {failure}")
    print("passed" if not failures else "failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
