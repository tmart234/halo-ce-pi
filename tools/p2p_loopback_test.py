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
one with a D1 result, one with none, one presenting the first's result
under its own key, one whose device the host bans (network.banned_devices,
finding H08) and one whose build it does not list (network.client_builds,
H07) are refused. The results come from the SDK's development
Verifier (cargo run -p fpp-ffi --example mint_ar, in the mmo checkout
tools/fpp_sdk.py builds from), so it needs Rust.

--verified tests a verified playlist (stage H5) instead: a development cell
of the region's services (the mmo checkout's dev-cell), a host blessed by
its Server Liveness (network.liveness), and players with tickets
from its Verifier and Broker (fpp-ticket). A player whose build the host
lists is admitted, checks the host's SAR chain and Checkpoints, and plays;
one with another build is refused (finding H07); when the cell goes away
the host is no longer blessed, and its player leaves.

    python tools/p2p_loopback_test.py [--binary build/linux/halo] [--trust]
"""

import argparse
import hashlib
import os
import re
import secrets
import signal
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import List, Optional

sys.path.insert(0, str(Path(__file__).resolve().parent))
import mqtt_test_broker  # noqa: E402
import fpp_sdk  # noqa: E402

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
    """the SDK's source (HALO_FPP_SOURCE, or the pinned commit, fetched if a
    cached library meant the build never needed it)"""
    return fpp_sdk.source_checkout()


def mint(*args: str, did: str = "", build: str = "") -> str:
    """the SDK's development Verifier (examples/mint_ar.rs); an AR's Device
    ID and client build if given"""
    env = dict(os.environ)
    if did:
        env["MINT_AR_DID"] = did
    if build:
        env["MINT_AR_BUILD"] = build
    result = subprocess.run(["cargo", "run", "-q", "--release", "--locked", "-p", "fpp-ffi", "--example", "mint_ar",
                             "--", *args], cwd=mmo_checkout(), check=True, capture_output=True, text=True, env=env)
    return result.stdout.strip()


def trust_test(args: argparse.Namespace, work: Path, common: dict) -> List[str]:
    """the host's trust policy: who is admitted and who is refused"""
    failures = []
    verifier = secrets.token_hex(32)
    work.mkdir(parents=True, exist_ok=True)
    joiners = {}
    # the host's lists: its release build, and a banned device (H07, H08)
    build, banned = "c0" * 32, "dd" * 32
    for name in ("trusted", "low", "none", "stolen", "banned", "modified"):
        seed = secrets.token_hex(32)
        (work / f"{name}.seed").write_text(seed)
        joiners[name] = {"seed": seed, "public": mint("session", seed)}
    mint("ar", verifier, joiners["trusted"]["public"], "2", str(work / "trusted.ar"), build=build)
    mint("ar", verifier, joiners["low"]["public"], "1", str(work / "low.ar"), build=build)
    mint("ar", verifier, joiners["banned"]["public"], "2", str(work / "banned.ar"), build=build, did=banned)
    mint("ar", verifier, joiners["modified"]["public"], "2", str(work / "modified.ar"), build="0e" * 32)
    processes = []
    try:
        host = start(args.binary, work / "host", {**common, "HALO_NET_ADDRESS": "127.0.0.200",
                                                  "HALO_NETWORK_TEST": "host:bloodgulch",
                                                  "HALO_NET_MINIMUM_TIER": "2",
                                                  "HALO_NET_CLIENT_BUILDS": build,
                                                  "HALO_NET_BANNED_DEVICES": banned,
                                                  "HALO_NET_VERIFIER_KEYS": mint("key", verifier)}, [])
        processes.append(host)
        invite = wait_for(work / "host" / "stdout.txt", INVITE.pattern, 20)
        if not invite:
            return ["the host made no invite"]
        attestations = {"trusted": "trusted.ar", "low": "low.ar", "none": "", "stolen": "trusted.ar",
                        "banned": "banned.ar", "modified": "modified.ar"}
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
            ("host", r"refused player \w+: its device is banned"),
            ("host", r"refused player \w+: its client build is not one network.client_builds lists"),
        ]
        for name, pattern in checks:
            found = wait_for(work / name / "stdout.txt", pattern, 30)
            print(f"{'ok  ' if found else 'FAIL'} {name}: {pattern}")
            if not found:
                failures.append(f"{name} never logged {pattern!r}")
        for name in ("low", "none", "stolen", "banned", "modified"):
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


def cargo_build(*packages: str) -> None:
    """binaries of the mmo checkout"""
    command = ["cargo", "build", "-q", "--locked"]
    for package in packages:
        command += ["-p", package]
    subprocess.run(command, cwd=mmo_checkout(), check=True)


def verified_test(args: argparse.Namespace, work: Path, common: dict) -> List[str]:
    """a verified playlist: a dev cell, a dedicated host it blesses, and
    players matched by its Broker"""
    failures: List[str] = []
    mmo = mmo_checkout()
    # (the cell's services, its runner, and the players' ticket tool)
    cargo_build("svc-liveness", "svc-verifier", "svc-broker", "svc-revocation", "svc-evidence", "svc-log",
                "fpp-svc", "tools", "client-core")
    cell_dir = work / "cell"
    cell_dir.mkdir(parents=True, exist_ok=True)
    keys = cell_dir / "keys"
    processes: List[subprocess.Popen] = []
    cell = subprocess.Popen([str(mmo / "target/debug/dev-cell")], cwd=cell_dir,
                            stdout=open(cell_dir / "stdout.txt", "w"), stderr=subprocess.STDOUT)
    try:
        if not wait_for(cell_dir / "stdout.txt", r"\[dev-cell\] ready", 60):
            return ["the dev cell did not start"]
        build = hashlib.sha256(args.binary.read_bytes()).hexdigest()
        game_address = "127.0.0.200:47400"
        host = start(args.binary, work / "host", {
            **common, "HALO_NET_ADDRESS": "127.0.0.200", "HALO_NETWORK_TEST": "host:bloodgulch",
            "HALO_NET_LIVENESS": "127.0.0.1:4444", "HALO_NET_GAME_ADDRESS": game_address,
            "HALO_NET_TUNNEL_PORT": "47400", "HALO_NET_KEY_BUNDLE": str(keys / "fpp_key_bundle.json"),
            "HALO_NET_CA_CERT": str(keys / "dev_ca.der"), "HALO_NET_CLIENT_BUILDS": build}, [])
        processes.append(host)
        if not wait_for(work / "host" / "stdout.txt", r"Verified play: blessed by Server Liveness", 30):
            return ["the host was not blessed by Server Liveness"]

        def ticket(name: str, client_build: str) -> Optional[Path]:
            path = work / f"{name}.ticket"
            result = subprocess.run([str(mmo / "target/debug/fpp-ticket"), "--client-build", client_build,
                                     "--out", str(path)], cwd=cell_dir, capture_output=True, text=True)
            print(result.stdout.strip() or result.stderr.strip())
            return path if result.returncode == 0 else None

        tickets = {"player": ticket("player", build), "modified": ticket("modified", "ab" * 32)}
        for name, path in tickets.items():
            if not path:
                failures.append(f"no ticket for {name}")
        if failures:
            return failures
        for index, (name, path) in enumerate(tickets.items()):
            processes.append(start(args.binary, work / name, {
                **common, "HALO_NET_ADDRESS": f"127.0.0.{201 + index}", "HALO_NETWORK_TEST": "join",
                "HALO_NET_TICKET": str(path)}, []))
        checks = [
            ("player", r"Verified play: the host's SAR verifies"),
            ("host", r"Verified play: admitted player \d+ to slot \d+: a D0 linux device"),
            ("player", r"Verified play: admitted to slot \d+"),
            ("player", r"Internet play: connected to host \w+ \(secure session\)"),
            ("player", r"Verified play: the host's Checkpoint \d+ verifies \(2 so far"),
            ("modified", r"Verified play: the host refused this machine: client build not admitted"),
            ("host", r"Verified play: refused player \d+: client build not admitted"),
        ]
        for name, pattern in checks:
            found = wait_for(work / name / "stdout.txt", pattern, 40)
            print(f"{'ok  ' if found else 'FAIL'} {name}: {pattern}")
            if not found:
                failures.append(f"{name} never logged {pattern!r}")
        if "connected to host" in (work / "modified" / "stdout.txt").read_text(errors="replace"):
            failures.append("the modified build was admitted")
        # the region's services go away: the host is not blessed any more,
        # and its player does not stay
        cell.send_signal(signal.SIGINT)
        cell.wait(20)
        gone = time.monotonic()
        lapsed = wait_for(work / "player" / "stdout.txt",
                          r"(SAR lapsed|not blessed \(SAR lapsed\))", 20)
        took = time.monotonic() - gone
        print(f"{'ok  ' if lapsed else 'FAIL'} player: left the host once it was no longer blessed ({took:.1f} s)")
        if not lapsed:
            failures.append("the player stayed with a host that is no longer blessed")
        unblessed = wait_for(work / "host" / "stdout.txt", r"this host is no longer blessed", 10)
        print(f"{'ok  ' if unblessed else 'FAIL'} host: knows it is no longer blessed")
        if not unblessed:
            failures.append("the host did not notice it lost its blessing")
        elif took > 10:
            failures.append(f"the player left {took:.1f} s after the host lost its blessing (more than one SAR lifetime)")
    finally:
        if cell.poll() is None:
            cell.send_signal(signal.SIGINT)
            try:
                cell.wait(20)
            except subprocess.TimeoutExpired:
                cell.kill()
        for process in processes:
            process.terminate()
        for process in processes:
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
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
    parser.add_argument("--verified", action="store_true",
                        help="test a verified playlist instead (needs Rust: the mmo checkout's dev cell)")
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
    if args.trust or args.evidence or args.verified:
        if args.trust:
            failures = trust_test(args, work, common)
        elif args.evidence:
            failures = evidence_test(args, work, common)
        else:
            failures = verified_test(args, work, common)
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
