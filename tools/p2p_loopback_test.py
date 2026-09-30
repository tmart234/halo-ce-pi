"""Internet play on one computer: a host, a joiner that opens its invite, and
the test MQTT broker (tools/mqtt_test_broker.py) in place of the public ones.
No game data is needed: the tunnel starts when the game hosts (the automated
network test's fast setup), and the joiner's system link list gets the host's
game through it.

Passes when the joiner and the host log a secure session, keep it, and a
third copy whose invite has another host key (an impostor's, or a forged
signalling answer) reaches the host's address but gets no session.

    python tools/p2p_loopback_test.py [--binary build/linux/halo]
"""

import argparse
import os
import re
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


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=Path("build/linux/halo"))
    parser.add_argument("--work", type=Path, default=None)
    parser.add_argument("--seconds", type=float, default=40.0)
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
