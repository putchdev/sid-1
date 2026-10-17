#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The parts (docs/MIDI.md 1 and 2.5) in the host build of firmware/src/sid1.c: the setup CCs and their replies,
notes and program changes by channel, the voices mode, pitch bend and its range, filtered parts kept to a chip
each, and the setup kept in flash.
  tests/parts_test.py"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/host"
EXE = OUT / "sid1_parts"
# factory slots: (AD, filter resonance) as sid1.c's FACTORY has them
PWM_LEAD, SAW_BASS, SWEEP_PAD, FLUTE, ORGAN, BELL = 0, 1, 3, 4, 5, 6
failed = 0


def check(what, ok):
    global failed
    print(("Passed  " if ok else "FAILED  ") + what)
    failed += not ok


def run(events):
    """events: (st, d1, d2) MIDI, or ("wait", ms). The FM-1's channel 16 CCs, and its SID writes as (dev, reg, val)
    with the index of the event they followed"""
    script, t = [], 100
    for e in events:
        if e[0] == "wait":
            t += e[1]
            continue
        script.append(f"{t} midi {e[0]} {e[1]} {e[2]}")
        t += 5
    script.append(f"{t + 200} end")
    path = OUT / "parts_script.txt"
    path.write_text("\n".join(script) + "\n")
    out = subprocess.run([str(EXE), str(OUT / "parts"), str(path)], cwd=ROOT, check=True, capture_output=True,
                         text=True, env={"SID_LOG": "1"}).stdout
    cc = [(int(a), int(b)) for a, b in re.findall(r"MIDI out BF (\d+) (\d+)", out)]
    sid = [(int(m[0]), int(m[1]), int(m[2]), int(m[3])) for m in re.findall(r"(\d+) ms  SID (\d) (\d+) (\d+)", out)]
    return cc, sid


def gates(sid, after=0):
    """voices gated on after a time: [(voice, AD written for it at its start)]"""
    out, ad, gate = [], {}, {}
    for t, d, r, v in sid:
        if r > 20:
            continue
        k = d * 3 + r // 7
        if r % 7 == 5:
            ad[k] = v
        if r % 7 == 4:
            if v & 1 and not gate.get(k) and t >= after:
                out.append((k, ad.get(k)))
            gate[k] = v & 1
    return out


def freq(sid, voice, before):
    lo = hi = 0
    for t, d, r, v in sid:
        if t <= before and d == voice // 3 and r == voice % 3 * 7:
            lo = v
        if t <= before and d == voice // 3 and r == voice % 3 * 7 + 1:
            hi = v
    return lo | hi << 8


def part(p, chan, slot):
    return [(0xBF, 0x20, p), (0xBF, 0x21, chan), (0xBF, 0x25, slot)]


OUT.mkdir(parents=True, exist_ok=True)
subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DSID1_HOST", '-DAPP_VER="dev"', "-Ifirmware/src",
                "-Ifirmware/gen", "-o", str(EXE), "firmware/src/sid1.c", "firmware/src/sid.c"], cwd=ROOT, check=True)

# defaults: part 1 on All, the rest off; a read is 20 p, 21, 25, 26, 2F p
cc, sid = run([(0xBF, 0x27, 0), (0xBF, 0x27, 1)] + [(0x90, 60 + k, 100) for k in range(6)])
check("part 1 reads as All, slot 1, parts mode", cc[-12:-6] == [(0x20, 0), (0x21, 17), (0x25, 0), (0x26, 0), (0x2E, 0), (0x2F, 0)])
check("part 2 reads as off", cc[-6:] == [(0x20, 1), (0x21, 16), (0x25, 0), (0x26, 0), (0x2E, 0), (0x2F, 1)])
check("single: six notes on six voices", sorted(k for k, _ in gates(sid)) == list(range(6)))

# split: part 1 PWM LEAD on channel 1, part 2 BELL on channel 2; a program change on channel 2 is part 2's
cc, sid = run(part(0, 0, PWM_LEAD) + part(1, 1, BELL) + [("wait", 100), (0x90, 60, 100), (0x91, 60, 100), ("wait", 200),
              (0x80, 60, 0), ("wait", 100), (0xC1, FLUTE, 0), (0x91, 67, 100), ("wait", 100)])
check("setup CCs answered 2F p; part 2's new slot told (29 6)",
      cc[:5] == [(0x2F, 0), (0x2F, 0), (0x2F, 1), (0x29, BELL), (0x2F, 1)] and (0x29, FLUTE) in cc)
g = gates(sid, 150)
check("split: channel 1 plays PWM LEAD, channel 2 BELL, on two voices",
      len(g) >= 2 and g[0][1] == 0x08 and g[1][1] == 0x09 and g[0][0] != g[1][0])
off = [(t, d * 3 + r // 7, v) for t, d, r, v in sid if t > 400 and t < 500 and r < 21 and r % 7 == 4]
check("note off on channel 1 releases only part 1's note",
      any(k == g[0][0] and not v & 1 for _, k, v in off) and not any(k == g[1][0] for _, k, _ in off))
check("program change on channel 2: part 2 plays FLUTE", gates(sid, 500)[-1][1] == 0x48)

# channel 16 is the page's: the edited part, whatever its channel
cc, sid = run(part(0, 0, PWM_LEAD) + part(1, 1, BELL) + [(0xBF, 0x2E, 1), ("wait", 50), (0x9F, 60, 100), ("wait", 50),
              (0xCF, FLUTE, 0), (0x8F, 60, 0), (0x9F, 62, 100), ("wait", 50), (0xBF, 0x27, 0)])
g = gates(sid)
check("2E picks the edited part (2F 7E; reads as 2E 1)", (0x2F, 0x7E) in cc and (0x2E, 1) in cc[-4:])
check("channel 16 notes and program changes are the edited part's", [a for _, a in g] == [0x09, 0x48] and (0x29, FLUTE) in cc)

# voices mode: channel n is voice n, with its own patch
cc, sid = run([(0xBF, 0x26, 1), (0xC2, BELL, 0), (0x92, 60, 100), (0x93, 62, 100), (0x90, 64, 100), (0x90, 65, 100)])
check("mode change answered 2F 7F", (0x2F, 0x7F) in cc)
g = gates(sid)
check("voices mode: channels 3, 4, 1 on voices 3, 4, 1; channel 3 plays BELL; channel 1 one note at a time",
      [k for k, _ in g] == [2, 3, 0, 0] and g[0][1] == 0x09)

# pitch bend: 2 semitones, then 12 after RPN 0
cc, sid = run([(0xBF, 0x26, 1), (0xC0, ORGAN, 0), (0x90, 60, 100), ("wait", 100), (0xE0, 127, 127), ("wait", 100),
               (0xB0, 101, 0), (0xB0, 100, 0), (0xB0, 6, 12), (0xE0, 127, 127), ("wait", 100), (0xE0, 0, 64)])
f0, f2, f12 = freq(sid, 0, 200), freq(sid, 0, 320), freq(sid, 0, 430)
check(f"bend up 2 semitones ({f2 / f0:.4f})", abs(f2 / f0 - 2 ** (2 / 12)) < 0.01)
check(f"bend range 12: up an octave ({f12 / f0:.4f})", abs(f12 / f0 - 2) < 0.01)
check("bend centre: back to the note", freq(sid, 0, 10 ** 6) == f0)

# a layer of two filtered parts: each keeps to a chip, its filter its own
cc, sid = run(part(0, 17, SAW_BASS) + part(1, 17, SWEEP_PAD) + [("wait", 50), (0x90, 48, 100), (0x90, 55, 100),
              (0x90, 52, 100), ("wait", 100)])
g = gates(sid, 120)
chips = {ad: {k // 3 for k, a in g if a == ad} for ad in (0x08, 0x8A)}
check(f"layer: SAW BASS on one chip, SWEEP PAD on the other ({chips})",
      len(g) == 6 and len(chips[0x08]) == 1 and len(chips[0x8A]) == 1 and chips[0x08] != chips[0x8A])
res = {d: v >> 4 for t, d, r, v in sid if r == 0x17}
check("each chip's resonance is its part's (10, 12)",
      res.get(min(chips[0x08])) == 10 and res.get(min(chips[0x8A])) == 12)

# kept: 4E, then a reload from the store alone
cc, sid = run(part(1, 4, BELL) + [(0xBF, 0x26, 1), (0xBF, 0x4E, 0), (0xBF, 0x4C, 0), (0xBF, 0x27, 1)])
check("kept in flash (4D 0)", (0x4D, 0) in cc)
check("part 2 survives a reload: channel 5, BELL, voices mode",
      cc[-6:] == [(0x20, 1), (0x21, 4), (0x25, BELL), (0x26, 1), (0x2E, 0), (0x2F, 1)])
sys.exit(1 if failed else 0)
