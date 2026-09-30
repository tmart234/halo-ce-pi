"""The Fair-Play Protocol SDK (libfpp), which internet play's tunnel uses for
its secure sessions (port/linux/src/p2p.c): built from the `mmo` repository
at a pinned commit, for the port's 32-bit targets.

The SDK is Rust with a C ABI (crates/fpp-ffi). Its header is vendored in
port/third_party/fpp/include/fpp.h; the library is built here, once per
commit, into build/third_party/fpp/<target>/. That needs Rust (rustup) and
the target: rustup target add i686-unknown-linux-gnu (Linux) or
i686-pc-windows-msvc (Windows).

HALO_FPP_SOURCE may name a local checkout of mmo to build from instead of
fetching the pinned commit (for work on both at once). Its header must
match the vendored one.

    python tools/fpp_sdk.py linux     # builds, and prints the library's path
    python tools/fpp_sdk.py commit    # prints the pinned commit
    python tools/fpp_sdk.py bump      # pins mmo's main (or: bump <ref|commit>)

The build files run it as a ninja step (ninja linux, ninja windows), so
configuring needs no Rust, and a build of another port never runs it.

The pin is not raised by hand: `bump` moves it to a newer commit when the
SDK's source changed there (not for mmo's documentation), copies that
commit's fpp.h over the vendored one and rewrites the Android stand-in
(tools/fpp_sdk_stub.py). The "Update the fpp SDK" workflow
(.github/workflows/fpp-sdk.yml) runs it on a schedule, when mmo's main
changes, or by hand, builds and tests every port with the result, and
merges it when they pass.
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import List, Optional

ROOT = Path(__file__).resolve().parent.parent
REPOSITORY = "https://github.com/tmart234/mmo.git"
# the mmo commit whose crates/fpp-ffi this port uses (and whose fpp.h is
# vendored); raised by `bump` (below), not by hand
COMMIT = "bbf885769fff46cb2fa4e453aa1d0006984c3034"
HEADER = ROOT / "port/third_party/fpp/include/fpp.h"
INCLUDE = HEADER.parent
THIRD_PARTY = ROOT / "build/third_party"

# what the SDK is built from in mmo: a commit changing none of these (its
# documentation, say) is not worth a new pin
SDK_PATHS = ["crates", "Cargo.toml", "Cargo.lock", "rust-toolchain.toml", "rust-toolchain"]

TARGETS = {
    "linux": ("i686-unknown-linux-gnu", "libfpp.a"),
    "windows": ("i686-pc-windows-msvc", "fpp.lib"),
}
# what the Rust standard library in libfpp needs from the system
# (cargo rustc -- --print native-static-libs)
LINUX_LIBRARIES = ["gcc_s", "util", "rt", "dl"]
WINDOWS_LIBRARIES = ["bcrypt", "advapi32", "kernel32", "ntdll", "userenv", "ws2_32", "dbghelp"]
# the C runtime each port links: the Windows build's clang (not clang-cl)
# links the static one (libcmt), so the SDK must too
RUSTFLAGS = {
    "linux": "",
    "windows": "-C target-feature=+crt-static",
}


class SdkError(Exception):
    pass


def _run(command: List[str], cwd: Optional[Path] = None) -> None:
    print("+", " ".join(command), flush=True)
    result = subprocess.run(command, cwd=cwd)
    if result.returncode != 0:
        raise SdkError(f"{' '.join(command)} failed ({result.returncode})")


def source_checkout(commit: str = COMMIT) -> Path:
    """a checkout of mmo: HALO_FPP_SOURCE, or the commit (the pinned one)
    fetched"""
    local = os.environ.get("HALO_FPP_SOURCE")
    if local:
        return Path(local).resolve()
    checkout = THIRD_PARTY / "mmo"
    head = checkout / ".git" / "HEAD"
    if head.is_file():
        current = subprocess.run(["git", "rev-parse", "HEAD"], cwd=checkout, capture_output=True, text=True).stdout
        if current.strip() == commit:
            return checkout
    if shutil.which("git") is None:
        raise SdkError("git is needed to fetch the SDK's source")
    checkout.mkdir(parents=True, exist_ok=True)
    if not head.is_file():
        _run(["git", "init", "-q"], checkout)
        _run(["git", "remote", "add", "origin", REPOSITORY], checkout)
    _run(["git", "fetch", "-q", "--depth", "1", "origin", commit], checkout)
    _run(["git", "checkout", "-q", "--detach", commit], checkout)
    return checkout


def resolve(ref: str) -> str:
    """a branch or tag of mmo (or a full commit) as a commit"""
    if re.fullmatch(r"[0-9a-f]{40}", ref):
        return ref
    result = subprocess.run(["git", "ls-remote", REPOSITORY, f"refs/heads/{ref}", f"refs/tags/{ref}"],
                            capture_output=True, text=True)
    if result.returncode != 0 or not result.stdout.split():
        raise SdkError(f"no {ref} in {REPOSITORY}")
    return result.stdout.split()[0]


def bump(ref: str) -> bool:
    """pins the SDK at mmo's ref (a branch, a tag or a commit) if the SDK's
    source differs there from the pinned commit's: the pin, the vendored
    header and the Android stand-in, as one change; whether it changed"""
    import fpp_sdk_stub

    if os.environ.get("HALO_FPP_SOURCE"):
        raise SdkError("bump pins a commit of mmo itself: unset HALO_FPP_SOURCE")
    commit = resolve(ref)
    if commit == COMMIT:
        print(f"the SDK is pinned at {ref} ({commit}) already")
        return False
    checkout = source_checkout(commit)
    # (the pinned commit too, to tell whether the SDK changed in between)
    _run(["git", "fetch", "-q", "--depth", "1", "origin", COMMIT], checkout)
    changed = subprocess.run(["git", "diff", "--quiet", COMMIT, commit, "--", *SDK_PATHS], cwd=checkout)
    if changed.returncode == 0:
        print(f"{ref} ({commit[:12]}) changes nothing the SDK is built from since {COMMIT[:12]}: pin kept")
        return False
    header = checkout / "crates/fpp-ffi/include/fpp.h"
    header_changed = header.read_bytes().replace(b"\r\n", b"\n") != HEADER.read_bytes().replace(b"\r\n", b"\n")
    shutil.copyfile(header, HEADER)
    script = Path(__file__)
    text = script.read_text()
    script.write_text(re.sub(r'^COMMIT = "[0-9a-f]{40}"$', f'COMMIT = "{commit}"', text, count=1, flags=re.M))
    fpp_sdk_stub.STUB.write_text(fpp_sdk_stub.render())
    print(f"pinned the SDK at {ref} ({commit}), was {COMMIT}"
          f"{'; fpp.h changed' if header_changed else ''}")
    print(f"changes: {REPOSITORY.removesuffix('.git')}/compare/{COMMIT}...{commit}")
    return True


def library_path(platform: str) -> Path:
    """where build() leaves a platform's library, relative to the repository
    (for the build files, which build it with a rule that runs this script)"""
    target, name = TARGETS[platform]
    return (THIRD_PARTY / "fpp" / target / name).relative_to(ROOT)


def build(platform: str) -> Path:
    """libfpp for a platform, built if needed; returns its path"""
    target, name = TARGETS[platform]
    out_dir = THIRD_PARTY / "fpp" / target
    library = out_dir / name
    stamp = out_dir / "commit"
    local = os.environ.get("HALO_FPP_SOURCE")
    if library.is_file() and not local and stamp.is_file() and stamp.read_text().strip() == COMMIT:
        # (built before, from the pinned commit)
        return library
    if shutil.which("cargo") is None:
        raise SdkError("Rust is needed for internet play's secure sessions (the fpp SDK): install rustup, "
                       f"then: rustup target add {target}")
    source = source_checkout()
    their_header = source / "crates/fpp-ffi/include/fpp.h"
    if their_header.read_bytes().replace(b"\r\n", b"\n") != HEADER.read_bytes().replace(b"\r\n", b"\n"):
        raise SdkError(f"{their_header} differs from the vendored {HEADER.relative_to(ROOT)}: "
                       "copy it over (and raise COMMIT in tools/fpp_sdk.py)")
    target_dir = THIRD_PARTY / "fpp" / "cargo-target"
    if RUSTFLAGS[platform]:
        os.environ["RUSTFLAGS"] = RUSTFLAGS[platform]
    try:
        # (the static library alone: the crate's cdylib would need the
        # target's linker, which a cross build does not have)
        _run(["cargo", "rustc", "--release", "--locked", "-p", "fpp-ffi", "--lib", "--crate-type", "staticlib",
              "--target", target, "--target-dir", str(target_dir)], source)
    finally:
        os.environ.pop("RUSTFLAGS", None)
    out_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(target_dir / target / "release" / name, library)
    stamp.write_text(COMMIT if not local else f"local {source}")
    return library


def main() -> int:
    arguments = sys.argv[1:]
    if arguments == ["commit"]:
        print(COMMIT)
        return 0
    if arguments[:1] == ["bump"] and len(arguments) <= 2:
        try:
            bump(arguments[1] if len(arguments) == 2 else "main")
        except SdkError as error:
            print(f"error: {error}", file=sys.stderr)
            return 1
        return 0
    if len(arguments) != 1 or arguments[0] not in TARGETS:
        print(__doc__)
        return 2
    try:
        print(build(arguments[0]))
    except SdkError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
