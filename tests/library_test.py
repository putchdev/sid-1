#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The page's protocol (docs/MIDI.md) in the host build of firmware/src/sid1.c: patches written to slots and read
back, refused when damaged, kept in flash across a reload, emptied; a patch played as the current one; the version;
the FM-1 telling the page its patch changed.
  tests/library_test.py"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/host"
SIZE = 50


def patch_bytes(name="TESTPAT", wt_len=3):
    """a patch with a top bit set in several bytes, as sid1.c's patch_t lays it out"""
    b = list(name.encode().ljust(12, b"\0"))
    b += [0x8A, 0xF3, 0x40, 3, 0x0A, 0xBC, 0xF0, 0x10, 0xE0, 0x30, 9, 0xC8, 0x85, 0x22, 0xF4, 5, 10, 200, 40]
    b += [wt_len, 1, 1]
    b += [0x80, 0x40, 0x20, 0, 0, 0, 0, 0]
    b += [96, 0xFC, 12, 0, 0, 0, 0, 0]
    assert len(b) == SIZE
    return b


def frame(begin, target, data, sum_fix=0):
    m = [(begin, target)]
    for v in data:
        m += [(0x52, v & 0x7F), (0x53, v >> 7)]
    s = (sum(data) + sum_fix) & 0x3FFF
    return m + [(0x54, s & 0x7F), (0x55, s >> 7)]


def run(events):
    """build the host app and run the events (CCs on channel 16, ("wait", ms), ("enc", id, steps)); the FM-1's
    channel 16 CCs, in order"""
    script, t = [], 100
    for e in events:
        if e[0] == "wait":
            t += e[1]
            continue
        script.append(f"{t} enc {e[1]} {e[2]}" if e[0] == "enc" else f"{t} midi 0xBF {e[0]} {e[1]}")
        t += 1
    script.append(f"{t + 300} end")
    OUT.mkdir(parents=True, exist_ok=True)
    path = OUT / "library_script.txt"
    path.write_text("\n".join(script) + "\n")
    exe = OUT / "sid1_test"
    subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DSID1_HOST", '-DAPP_VER="dev"',
                    "-Ifirmware/src", "-Ifirmware/gen", "-o", str(exe), "firmware/src/sid1.c", "firmware/src/sid.c"],
                   cwd=ROOT, check=True)
    out = subprocess.run([str(exe), str(OUT / "library"), str(path)], cwd=ROOT, check=True,
                         capture_output=True, text=True).stdout
    return [(int(a), int(b)) for a, b in re.findall(r"MIDI out BF (\d+) (\d+)", out)]


def dump(replies, begin, slot):
    """a dump (begin slot, 32/33 pairs, 34 35) from the replies: its bytes, None for an empty slot (36)"""
    for i, (d1, d2) in enumerate(replies):
        if d1 == 0x36 and d2 == slot and begin == 0x30:
            return None
        if d1 == begin and d2 == slot:
            data, j = [], i + 1
            while replies[j][0] != 0x34:
                lo, hi = replies[j], replies[j + 1]
                assert lo[0] == 0x32 and hi[0] == 0x33, "dump pairs"
                data.append(lo[1] | hi[1] << 7)
                j += 2
            s = replies[j][1] | replies[j + 1][1] << 7
            assert s == sum(data) & 0x3FFF, "dump sum"
            return data
    raise AssertionError(f"no dump {begin:#x} {slot}")


def after(replies, msg):
    return replies[replies.index(msg):]


def main():
    failed = 0

    def check(what, ok):
        nonlocal failed
        print(("Passed  " if ok else "FAILED  ") + what)
        failed += not ok

    p = patch_bytes()
    ev = [(0x1F, 0)]
    ev += frame(0x5B, 40, p) + frame(0x5B, 41, p, 1) + frame(0x5B, 42, p[:-1]) + frame(0x5B, 43, patch_bytes("WILD", 20))
    ev += [(0x5C, 40), (0x5C, 41), (0x5C, 42), (0x5C, 43), (0x5C, 0)]
    ev += [(0x4E, 0), ("wait", 100), (0x4C, 0), ("wait", 50), (0x5C, 40)]
    ev += [(0x5E, 40), ("wait", 5), (0x5C, 40)]
    ev += frame(0x50, 7, patch_bytes("PLAYED")) + [("wait", 300), (0x5D, 0)]
    ev += [("wait", 300), ("enc", 6, 1), ("wait", 400)]
    r = run(ev)

    check("version: SID-1 (1B 2), a DEV build (1C 127)", r[:2] == [(0x1B, 2), (0x1C, 127)])
    check("slot 40 taken", (0x3E, 40) in r)
    check("bad sum refused", (0x3F, 41) in r and (0x3E, 41) not in r)
    check("short frame refused", (0x3F, 42) in r and (0x3E, 42) not in r)
    first = r[:r.index((0x6E, 0x4E))]
    check("slot 40 reads back as sent", dump(first, 0x30, 40) == p)
    check("refused slots stay empty", dump(first, 0x30, 41) is None and dump(first, 0x30, 42) is None)
    wild = dump(first, 0x30, 43)
    check("a wavetable longer than 8 steps is cut to 8", wild is not None and wild[31] == 8)
    zero = dump(first, 0x30, 0)
    check("slot 0 is the factory PWM LEAD", zero is not None and bytes(zero[:8]) == b"PWM LEAD")
    check("kept in flash", (0x4D, 0) in r)
    reload = after(r, (0x4C, 0))
    check("slot 40 survives a reload", dump(reload, 0x30, 40) == p)
    emptied = after(reload, (0x3D, 40))
    check("emptied slot reads empty", dump(emptied, 0x30, 40) is None)
    check("a patch to play is taken", (0x56, 7) in r)
    playing = dump(after(r, (0x56, 7)), 0x37, 7)
    check("the patch playing reads back, as slot 7's", playing == patch_bytes("PLAYED"))
    check("... and as edited (38 1)", (0x38, 1) in after(r, (0x56, 7)))
    tail = after(r, (0x38, 1))
    check("no change told for the page's own patch; PRESETS on the FM-1: 3C the new slot (8)",
          [x for x in tail if x[0] == 0x3C] == [(0x3C, 8)])
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
