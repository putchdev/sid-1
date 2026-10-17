#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// web/index.html's tune to MIDI (sidMidi, midiFile) against the FM-1's own player:
//   tests/song_test.js [tune.sid ...]      (default: the tunes in build/ref/hvsc, if fetched)
// Each tune's instruments go into slots, then its first 30 s plays as MIDI on the host build of firmware/src/sid1.c
// in voices mode. Its notes must start on the same voices at the same times, at the same pitch (a note's median over
// its gated frames, so slides as pitch bend count), as the tune played them. And sidSamples: Arkanoid's drums (played
// through the volume register) found as 5 samples, Commando and Monty on the Run with none.
"use strict";
const fs = require("fs"), path = require("path"), { execFileSync } = require("child_process");
const ROOT = path.resolve(__dirname, "..");
const page = fs.readFileSync(path.join(ROOT, "web/index.html"), "utf8");
const part = n => page.slice(page.indexOf(`// ==== ${n} begin`), page.indexOf(`// ==== ${n} end`));
const { psid, sidRun, sidInstruments, sidCalls, sidMidi, midiFile, notePitch, vreg, median, sidSamples } = new Function(
  part("c64") + part("sidpatch") + "; return { psid, sidRun, sidInstruments, sidCalls, sidMidi, midiFile, notePitch, vreg, median, sidSamples };")();

let failed = 0;
const check = (what, ok) => { console.log((ok ? "Passed  " : "FAILED  ") + what); failed += !ok; };
const OUT = path.join(ROOT, "build/host"), EXE = path.join(OUT, "sid1_song");
fs.mkdirSync(OUT, { recursive: true });
execFileSync("cc", ["-O2", "-Wall", "-Wno-unused-function", "-DSID1_HOST", '-DAPP_VER="dev"', "-Ifirmware/src", "-Ifirmware/gen",
  "-o", EXE, "firmware/src/sid1.c", "firmware/src/sid.c"], { cwd: ROOT });

const SECS = 30, START = 8000, FIRST_SLOT = 64;
const s8 = x => x << 24 >> 24;
function bytes(f) {                             // patch fields -> 50 bytes (docs/MIDI.md 3)
  const b = Array(50).fill(0);
  [..."SONGTEST"].forEach((c, i) => b[i] = c.charCodeAt(0));
  ["ad", "sr", "wave", "flags", "pw_hi", "pw_lo", "pw_speed", "pw_min", "pw_max", "mode", "res", "cut", "cut_speed", "cut_end",
   "transpose", "fine", "vib_delay", "vib_speed", "vib_depth", "wt_len", "wt_loop", "wt_abs"].forEach((k, i) => b[12 + i] = f[k] & 255);
  for (let k = 0; k < 8; k++) { b[34 + k] = f.wt_ctl[k] & 255; b[42 + k] = f.wt_note[k] & 255; }
  return b;
}

function midiEvents(file) {                     // a type 0 file back to [{ t, m }] (960 ticks a second, as midiFile)
  const b = file, out = [];
  if (String.fromCharCode(...b.slice(0, 4)) !== "MThd" || String.fromCharCode(...b.slice(14, 18)) !== "MTrk") return null;
  let i = 22, tick = 0, run = 0;
  const vlq = () => { let n = 0, x; do { x = b[i++]; n = n << 7 | x & 127; } while (x & 128); return n; };
  while (i < b.length) {
    tick += vlq();
    let st = b[i];
    if (st === 0xFF) { const ty = b[i + 1]; i += 2; const n = vlq(); i += n; if (ty === 0x2F) break; continue; }
    if (st & 0x80) i++; else st = run;
    run = st;
    const n = (st & 0xF0) === 0xC0 || (st & 0xF0) === 0xD0 ? 1 : 2;
    out.push({ t: tick / 960, m: [st, ...b.slice(i, i + n)] });
    i += n;
  }
  return out;
}

function playOnFm1(insts, events) {             // the firmware's notes: [{ vi, ms (its gate on), p (median while gated) }]
  const lines = [];
  let t = 100;
  insts.forEach((x, k) => {
    const b = bytes(x.fields), sum = b.reduce((a, v) => a + v, 0) & 0x3FFF;
    lines.push(`${t} midi 0xBF ${0x5B} ${FIRST_SLOT + k}`);
    for (const v of b) lines.push(`${t} midi 0xBF ${0x52} ${v & 127}`, `${t} midi 0xBF ${0x53} ${v >> 7}`);
    lines.push(`${t} midi 0xBF ${0x54} ${sum & 127}`, `${t} midi 0xBF ${0x55} ${sum >> 7}`);
    t += 1;
  });
  lines.push(`${t + 10} midi 0xBF ${0x26} 1`);
  for (const e of events) lines.push(`${START + Math.round(e.t * 1000)} midi 0x${e.m[0].toString(16)} ${e.m[1]} ${e.m[2] ?? 0}`);
  lines.push(`${START + SECS * 1000 + 500} end`);
  const script = path.join(OUT, "song_script.txt");
  fs.writeFileSync(script, lines.join("\n") + "\n");
  const out = execFileSync(EXE, [path.join(OUT, "song"), script], { cwd: ROOT, env: { ...process.env, SID_LOG: "1" }, maxBuffer: 1 << 30 }).toString();
  const regs = [new Uint8Array(32), new Uint8Array(32)], notes = [], open = [];
  for (const m of out.matchAll(/(\d+) ms {2}(FRAME|SID (\d) (\d+) (\d+))/g)) {
    if (m[2] === "FRAME") {
      for (const n of open) if (n) { const r = regs[n.vi / 3 | 0], o = n.vi % 3 * 7; n.ps.push(notePitch(r[o] | r[o + 1] << 8, 985248)); }
      continue;
    }
    const c = +m[3], r = +m[4], v = +m[5], vi = c * 3 + Math.floor(r / 7);
    if (r < 21 && r % 7 === 4) {
      if (v & 1 && !(regs[c][r] & 1)) { open[vi] = { vi, ms: +m[1] - START, ps: [] }; notes.push(open[vi]); }
      if (!(v & 1)) open[vi] = null;
    }
    regs[c][r] = v;
  }
  return notes.map(n => ({ vi: n.vi, ms: n.ms, p: median(n.ps.filter(p => p >= 0)) }));
}

const tunes = process.argv.slice(2);
if (!tunes.length) {
  const dir = path.join(ROOT, "build/ref/hvsc");
  if (fs.existsSync(dir)) for (const f of ["Commando.sid", "Monty_on_the_Run.sid", "Last_Ninja_2.sid"]) if (fs.existsSync(path.join(dir, f))) tunes.push(path.join(dir, f));
  if (!tunes.length) console.log("        no tunes: give .sid files, or fetch some into build/ref/hvsc");
}
for (const file of tunes) {
  const name = path.basename(file), t = psid(new Uint8Array(fs.readFileSync(file))), run = sidRun(t, t.start, SECS);
  const res = sidInstruments(t, run), insts = res.instruments.slice(0, 64);
  const events = sidMidi(t, run, res, k => k < insts.length ? FIRST_SLOT + k : -1);
  const back = midiEvents(midiFile(events, t.name));
  check(`${name}: the .mid reads back as the events`, back && back.length === events.length &&
    back.every((e, i) => Math.abs(e.t - events[i].t) < 0.002 && e.m.join() === events[i].m.join()));

  // the tune's notes: when, and their median pitch while gated
  const calls = sidCalls(run), clock = t.clock === 2 ? 1022727 : 985248, want = [];
  for (const n of res.notes) {
    if (n.inst >= insts.length) continue;
    const later = res.notes.filter(x => x.vi === n.vi && x.call > n.call).reduce((a, x) => Math.min(a, x.call), calls.length);
    const ps = [];
    for (let c = n.call; c < Math.min(n.off, later); c++) { const p = notePitch(vreg(calls[c], n.vi, 0) | vreg(calls[c], n.vi, 1) << 8, clock); if (p >= 0) ps.push(p); }
    want.push({ vi: n.vi, ms: n.call / run.rate * 1000, p: median(ps), drum: !!insts[n.inst].fields.wt_abs, steps: insts[n.inst].fields.wt_len > 1 });
  }
  const got = playOnFm1(insts, events);
  let timed = 0, pitched = 0, tonal = 0;
  const off = [];
  for (const w of want) {
    const g = got.find(x => x.vi === w.vi && Math.abs(x.ms - w.ms) <= 45);
    if (!g) continue;
    timed++;
    if (w.drum || w.steps) continue;
    tonal++;
    if (Math.abs(g.p - w.p) <= 0.35) pitched++; else off.push(`v${w.vi + 1} ${(w.ms / 1000).toFixed(2)}s ${w.p.toFixed(2)}/${g.p.toFixed(2)}`);
  }
  check(`${name}: ${timed} of ${want.length} notes start on their voice within 45 ms`, timed >= want.length * 0.97);
  check(`${name}: ${pitched} of ${tonal} at the tune's pitch (0.35 semitones)` + (off.length ? `; off: ${off.slice(0, 4).join(", ")}` : ""),
    pitched >= tonal * 0.95);
}

const hvsc = f => path.join(ROOT, "build/ref/hvsc", f);
if (fs.existsSync(hvsc("Arkanoid.sid"))) {
  const t = psid(new Uint8Array(fs.readFileSync(hvsc("Arkanoid.sid")))), r = sidSamples(t, sidRun(t, 2, SECS));
  // each a run of one step (mod 16), but for its last write
  const even = r.samples.every(x => new Set(x.writes.slice(1, -1).map((w, i) => w[0] - x.writes[i][0] & 15)).size === 1);
  check(`Arkanoid.sid song 2: ${r.samples.length} drum samples, each one step, ${r.hits.length} hits`, r.samples.length === 5 && even &&
    r.hits.length === r.samples.reduce((a, x) => a + x.hits, 0));
}
for (const f of ["Commando.sid", "Monty_on_the_Run.sid"]) if (fs.existsSync(hvsc(f))) {
  const t = psid(new Uint8Array(fs.readFileSync(hvsc(f))));
  check(`${f}: no samples`, !sidSamples(t, sidRun(t, t.start, SECS)).samples.length);
}
process.exit(failed ? 1 : 0);
