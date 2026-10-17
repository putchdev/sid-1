#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The sample kit in the host build of firmware/src/sid1.c: channel 10's notes play the test kit through chip 1's
volume register, as a C64 tune's drums. Heard on the 6581, next to silent on the 8580 (no DC offset to move), and
not taken by a part on All. A sample sent as docs/MIDI.md 2.6 has it (an Arkanoid drum: its writes each at their own
cycle), plays each write within a sample of its time, and is kept in flash; bad frames are refused.
  tests/digi_test.py          (the sound: build/host/digi.wav, digi8580.wav)"""
import math
import re
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "build/host"
EXE = OUT / "sid1_digi"
HITS = [(500, 36, "kick"), (1000, 38, "snare"), (1500, 42, "hat")]
failed = 0


def check(what, ok):
    global failed
    print(("Passed  " if ok else "FAILED  ") + what)
    failed += not ok


def run(name, lines):
    path = OUT / f"{name}_script.txt"
    path.write_text("\n".join(lines + ["2200 end"]) + "\n")
    log = subprocess.run([str(EXE), str(OUT / name), str(path)], cwd=ROOT, check=True, capture_output=True, text=True,
                         env={"SID_LOG": "1"}).stdout
    raw = (OUT / f"{name}.wav").read_bytes()
    n = (len(raw) - 44) // 4
    left = struct.unpack(f"<{n * 2}h", raw[44:44 + n * 4])[::2]
    return left, log


def rms(x, a_ms, b_ms):
    s = x[a_ms * 441 // 10:b_ms * 441 // 10]
    return math.sqrt(sum(v * v for v in s) / max(1, len(s)))


def crossings(x, a_ms, b_ms):                    # sign changes a second: low for a kick, high for a hat
    s = x[a_ms * 441 // 10:b_ms * 441 // 10]
    return sum((s[i - 1] < 0) != (s[i] < 0) for i in range(1, len(s))) * 1000 / (b_ms - a_ms)


OUT.mkdir(parents=True, exist_ok=True)
subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-function", "-DSID1_HOST", '-DAPP_VER="dev"', "-Ifirmware/src",
                "-Ifirmware/gen", "-o", str(EXE), "firmware/src/sid1.c", "firmware/src/sid.c"], cwd=ROOT, check=True)
hits = [f"{t} midi 0x99 {n} 100" for t, n, _ in HITS]
x, log = run("digi", hits)
quiet = rms(x, 300, 480)
level = {name: rms(x, t + 10, t + 150) for t, _, name in HITS}
check(f"before the hits: quiet (rms {quiet:.0f})", quiet < 50)
for name, r in level.items():
    check(f"the {name} is heard on the 6581 (rms {r:.0f})", r > 20 * max(quiet, 1))
zk, zh = crossings(x, 520, 700), crossings(x, 1510, 1540)
check(f"the kick is low ({zk:.0f} crossings/s), the hat high ({zh:.0f})", zk < 600 and zh > 2000)
onset = next(i for i in range(500 * 441 // 10, 600 * 441 // 10) if abs(x[i]) > 10 * max(quiet, 1)) * 10 / 441 - 500
check(f"the kick starts {onset:.1f} ms after its note (a buffer is 5.8 ms)", 0 <= onset <= 13)
check("channel 10 plays no voice (part 1 on All)", not re.search(r"SID \d (4|11|18) \d*[13579]\b", log))

x8, _ = run("digi8580", ["100 btn 7", "150 enc 2 1"] + hits)
l8 = max(rms(x8, t + 10, t + 150) for t, _, _ in HITS)
check(f"on the 8580 next to silent ({20 * math.log10(max(l8, 1) / level['kick']):.0f} dB)", l8 < level["kick"] / 30)

xv, _ = run("digivoice", ["500 midi 0x90 60 100", "700 midi 0x80 60 0"])
print(f"        for scale: part 1's patch at C4, rms {rms(xv, 510, 650):.0f}")

x2, _ = run("digi1x", ["100 btn 7", "150 enc 3 -1"] + hits)
check(f"the same without 2x (kick rms {rms(x2, 510, 650):.0f})", 0.7 < rms(x2, 510, 650) / level["kick"] < 1.4)

# Arkanoid's song 1 drum, as the page finds it: 6 runs of 25 writes, the value up 5 (mod 16) each, the gap growing
GAPS = [31] * 25 + [38] * 25 + [52] * 25 + [80] * 25 + [136] * 25 + [248] * 25
DRUM = [((5 + 5 * i) % 16, g) for i, g in enumerate(GAPS)]


def frame(begin, k, at, writes=(), sum_fix=0):
    vals = [at & 127, at >> 7]
    for v, g in writes:
        e = v << 10 | g
        vals += [e & 127, e >> 7]
    cc = [(begin, k), (0x61, vals[0]), (0x62, vals[1])]
    cc += [(0x63 + i % 2, x) for i, x in enumerate(vals[2:])]
    s = (sum(vals) + sum_fix) & 0x3FFF
    return cc + [(0x65, s & 127), (0x66, s >> 7)]


def upload(k, writes, t=100):
    cc = frame(0x67, k, len(writes))
    for i in range(0, len(writes), 128):
        cc += frame(0x60, k, i, writes[i:i + 128])
    return cc


def lines(cc, t):
    return [f"{t + i // 40} midi 0xBF {a} {b}" for i, (a, b) in enumerate(cc)]


def replies(log):
    return [(int(a), int(b)) for a, b in re.findall(r"MIDI out BF (\d+) (\d+)", log)]


cc = [(0x68, 0)] + upload(0, DRUM) + upload(1, [(15, 123)] * 9000)
cc += frame(0x60, 0, 0, DRUM[:4], sum_fix=1) + frame(0x67, 0, 10)
x, log = run("digiup", lines(cc, 100) + ["600 midi 0x99 36 100", "700 midi 0xBF 78 0", "900 midi 0xBF 76 0", "1000 midi 0x99 36 100"])
r = replies(log)
check("kit emptied (6C), the drum's length and its 2 frames taken (6A 0)", r[:4] == [(0x6C, 0), (0x6A, 0), (0x6A, 0), (0x6A, 0)])
big = 1 + (9000 + 127) // 128                   # (its length frame and every data frame)
check("refused: a sample past the kit's room, a bad sum, a second length (6B)",
      r[4:4 + big] == [(0x6B, 1)] * big and r[4 + big:6 + big] == [(0x6B, 0)] * 2)
digs = [tuple(map(int, m)) for m in re.findall(r"DIG (\d) (\d+) (\d+)", log)]
first = digs[:len(DRUM)]
step = 88200 / 985248
times_ok = all(abs((first[i + 1][2] - first[i][2]) - DRUM[i][1] * step) <= 1.01 for i in range(len(DRUM) - 1))
check(f"the drum plays its {len(DRUM)} writes, each within a sample of its cycle", [d[1] for d in first] == [v for v, _ in DRUM] and times_ok)
check("kept in flash (4D 0), and still there after a reload", (0x4D, 0) in r and len(digs) == 2 * len(DRUM) and
      [d[1] for d in digs[len(DRUM):]] == [v for v, _ in DRUM])
dur = sum(GAPS) / 985248 * 1000
on = rms(x, 605, 625)
check(f"heard ({dur:.1f} ms, rms {on:.0f})", on > 200)
sys.exit(1 if failed else 0)
