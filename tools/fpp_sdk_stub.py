"""Writes port/linux/src/p2p_sdk_none.c: a stand-in for every fpp SDK call
internet play makes (p2p*.c), for builds without the SDK (Android), each
failing, so internet play stays off there (p2p_initialize).

    python tools/fpp_sdk_stub.py          # rewrite it
    python tools/fpp_sdk_stub.py --check  # fail if it is out of date
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "port/third_party/fpp/include/fpp.h"
SOURCES = sorted((ROOT / "port/linux/src").glob("p2p*.c"))
STUB = ROOT / "port/linux/src/p2p_sdk_none.c"

PREAMBLE = """/*
P2P_SDK_NONE.C

The fpp SDK calls internet play makes (p2p*.c), for a build without the SDK
(Android, whose game code runs as a 32-bit guest the Rust SDK has no target
for). Each fails, so p2p_identifier makes no session key, and
p2p_initialize leaves internet play off: its old tunnel, whose keys every
invite holder could read, is gone, and there is no insecure one to fall
back to. The Linux and Windows builds link the SDK itself (HALO_FPP,
tools/fpp_sdk.py), and this file is empty in them.

Written by tools/fpp_sdk_stub.py from port/third_party/fpp/include/fpp.h:
run it after using another SDK call.
*/

#ifndef HALO_FPP

#include "fpp.h"
"""


def without_gs_link(text):
    """the header without its FPP_GS_LINK parts (a dedicated host's link to
    Server Liveness, which only the Linux build has, and calls only under
    #ifdef FPP_GS_LINK): they have no stand-in"""
    return re.sub(r"^#if defined\(FPP_GS_LINK\)\n.*?^#endif\n", "", text, flags=re.M | re.S)


def prototypes():
    text = without_gs_link(HEADER.read_text())
    # (a prototype: a return type, fpp_name, the parameters, a semicolon)
    pattern = re.compile(r"^((?:enum FppStatus|void|const char \*|uint32_t))\s*(fpp_\w+)\(([^;]*?)\);", re.M | re.S)
    return {m.group(2): (m.group(1).strip(), " ".join(m.group(3).split())) for m in pattern.finditer(text)}


def used():
    names = set()
    for source in SOURCES:
        if source == STUB:
            continue
        # (calls under #ifdef FPP_GS_LINK: the Linux build's alone)
        text = re.sub(r"^#ifdef FPP_GS_LINK\n.*?^#endif[^\n]*\n", "", source.read_text(), flags=re.M | re.S)
        names.update(re.findall(r"\b(fpp_[a-z0-9_]+)\s*\(", text))
    return names


def stub(name, result, parameters):
    params = [p.strip() for p in parameters.split(",")] if parameters.strip() not in ("", "void") else []
    lines = [f"{result} {name}({', '.join(params) if params else 'void'})".replace("* ", "*"), "{"]
    for param in params:
        pname = re.findall(r"(\w+)$", param)[0]
        if pname == "out" and param.endswith("**out"):
            lines.append("\t*out = NULL;")
        else:
            lines.append(f"\t(void){pname};")
    if result == "enum FppStatus":
        empty = name.endswith("_poll_transmit") or name.endswith("_poll_event")
        lines.append(f"\treturn {'FPP_STATUS_EMPTY' if empty else 'FPP_STATUS_INTERNAL'};")
    elif result.startswith("const char"):
        lines.append('\treturn "no SDK in this build";')
    elif result == "uint32_t":
        lines.append("\treturn 0;")
    lines.append("}")
    return "\n".join(lines)


def render():
    known = prototypes()
    names = sorted(n for n in used() if n in known)
    missing = sorted(n for n in used() if n not in known)
    if missing:
        raise SystemExit(f"not in fpp.h: {', '.join(missing)}")
    body = "\n\n".join(stub(name, *known[name]) for name in names)
    return f"{PREAMBLE}\n{body}\n\n#endif\n"


def main():
    text = render()
    if "--check" in sys.argv:
        if STUB.read_text() != text:
            print(f"{STUB.relative_to(ROOT)} is out of date: run python tools/fpp_sdk_stub.py")
            return 1
        return 0
    STUB.write_text(text)
    print(f"wrote {STUB.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
