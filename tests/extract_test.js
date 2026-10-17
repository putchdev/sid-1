#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// web/index.html's patch extraction (its "sidpatch" section) against the FM-1's own player:
//   tests/extract_test.js [tune.sid ...]
// The host build of firmware/src/sid1.c plays each factory patch at eight keys and logs its SID writes (SID_LOG);
// the extraction must find one instrument in them that plays as the patch does: the same waveform and pitch frame by
// frame, envelope, filter, pulse sweep and vibrato. Any .sid files given are run a minute and their instruments listed.
"use strict";
const fs = require("fs"), path = require("path"), { execFileSync } = require("child_process");
const ROOT = path.resolve(__dirname, "..");
const page = fs.readFileSync(path.join(ROOT, "web/index.html"), "utf8");
const part = n => page.slice(page.indexOf(`// ==== ${n} begin`), page.indexOf(`// ==== ${n} end`));
const { psid, sidRun, sidInstruments, tuneShort } = new Function(part("c64") + part("sidpatch") +
  "; return { psid, sidRun, sidInstruments, tuneShort };")();

let failed = 0;
const check = (what, ok) => { console.log((ok ? "Passed  " : "FAILED  ") + what); failed += !ok; };
const OUT = path.join(ROOT, "build/host"), EXE = path.join(OUT, "sid1_extract");
fs.mkdirSync(OUT, { recursive: true });
execFileSync("cc", ["-O2", "-Wall", "-Wno-unused-function", "-DSID1_HOST", '-DAPP_VER="dev"', "-Ifirmware/src", "-Ifirmware/gen",
  "-o", EXE, "firmware/src/sid1.c", "firmware/src/sid.c"], { cwd: ROOT });

const KEYS = [48, 55, 60, 64, 67, 50, 52, 57];   // (ZAP climbs 36 semitones: none near the top note)
function play(slot) {                           // the patch's bytes, and the player's writes as calls (a frame each)
  const lines = [`100 midi 0xBF ${0x5C} ${slot}`, `200 midi 0xC0 ${slot} 0`];
  KEYS.forEach((k, i) => lines.push(`${300 + i * 1800} midi 0x90 ${k} 100`, `${1800 + i * 1800} midi 0x80 ${k} 0`));
  lines.push(`${300 + KEYS.length * 1800} end`);
  const script = path.join(OUT, "extract_script.txt");
  fs.writeFileSync(script, lines.join("\n") + "\n");
  const out = execFileSync(EXE, [path.join(OUT, "extract"), script], { cwd: ROOT, env: { ...process.env, SID_LOG: "1" } }).toString();
  const cc = [...out.matchAll(/MIDI out BF (\d+) (\d+)/g)].map(m => [+m[1], +m[2]]);
  const bytes = [];
  for (let i = cc.findIndex(x => x[0] === 0x30) + 1; cc[i][0] === 0x32; i += 2) bytes.push(cc[i][1] | cc[i + 1][1] << 7);
  const run = { rate: 50, init: [], frames: [] };
  let cur = run.init;
  for (const m of out.matchAll(/ms {2}(FRAME|SID (\d) (\d+) (\d+))/g)) {
    if (m[1] === "FRAME") run.frames.push(cur = []);
    else cur.push([+m[2], +m[3], +m[4]]);
  }
  return { bytes, run };
}
const s8 = x => x << 24 >> 24;
function fields(b) {                            // docs/MIDI.md 3, read again here
  const f = { name: String.fromCharCode(...b.slice(0, 12)).replace(/\0.*/, "") };
  ["ad", "sr", "wave", "flags", "pw_hi", "pw_lo", "pw_speed", "pw_min", "pw_max", "mode", "res", "cut", "cut_speed", "cut_end",
   "transpose", "fine", "vib_delay", "vib_speed", "vib_depth", "wt_len", "wt_loop", "wt_abs"].forEach((k, i) => f[k] = b[12 + i]);
  for (const k of ["pw_speed", "cut_speed", "transpose", "fine"]) f[k] = s8(f[k]);
  f.wt_ctl = b.slice(34, 42); f.wt_note = b.slice(42, 50);
  return f;
}
function steps(f, key, n = 12) {                // waveform and pitch a frame, as the FM-1's player steps its wavetable
  const out = [];
  let pos = 0;
  for (let j = 0; j < n; j++) {
    const ctl = (f.wt_len && f.wt_ctl[pos] ? f.wt_ctl[pos] : f.wave) & 0xF0 | f.wave & 0x0E;
    const p = f.wt_len && f.wt_abs >> pos & 1 ? f.wt_note[pos] : key + f.transpose + (f.wt_len ? s8(f.wt_note[pos]) : 0);
    out.push(`${ctl.toString(16)}:${p}`);
    if (f.wt_len && ++pos >= f.wt_len) pos = f.wt_loop < f.wt_len ? f.wt_loop : f.wt_len - 1;
  }
  return out.join(" ");
}
const near = (a, b, tol) => Math.abs(a - b) <= tol;

for (let slot = 0; slot < 16; slot++) {
  const { bytes, run } = play(slot), want = fields(bytes), got = sidInstruments({ clock: 1 }, run);
  const g = got.instruments[0]?.fields, why = [];
  if (got.instruments.length !== 1) why.push(`${got.instruments.length} instruments`);
  if (g) {
    const n = got.notes.slice().sort((a, b) => a.call - b.call), same = (k, ok = g[k] === want[k]) => { if (!ok) why.push(`${k} ${g[k]} not ${want[k]}`); };
    const bad = n.find((x, i) => steps(g, x.base) !== steps(want, KEYS[i]));
    if (bad) why.push(`plays ${steps(g, bad.base)} not ${steps(want, KEYS[n.indexOf(bad)])}`);
    same("ad"); same("sr");
    if ((g.flags ^ want.flags) & 1) why.push(`filter ${g.flags & 1} not ${want.flags & 1}`);
    if (g.flags & 2 ^ want.flags & 2) why.push(`hard restart ${g.flags >> 1} not ${want.flags >> 1}`);
    if (want.wave & 0x40 || want.wt_ctl.some(c => c & 0x40)) {
      same("pw_hi", g.pw_hi === (want.pw_hi & 15)); same("pw_lo"); same("pw_speed");
      if (want.pw_speed) { same("pw_min", g.pw_min === want.pw_min || g.pw_min === 0); same("pw_max", g.pw_max === want.pw_max || g.pw_max === 255); }
    }
    if (want.flags & 1) {
      same("mode"); same("res"); same("cut"); same("cut_speed");
      // (a sweep longer than the frames looked at is still going: as far as it goes)
      const long = Math.abs(want.cut_end - want.cut) * 8 / Math.abs(want.cut_speed) > 63;
      if (want.cut_speed) same("cut_end", long ? g.cut_end === (want.cut_speed > 0 ? 255 : 0) : g.cut_end === want.cut_end);
    }
    same("fine", near(g.fine, want.fine, 1));
    if (want.vib_depth) {
      same("vib_depth", near(g.vib_depth, want.vib_depth, Math.max(2, want.vib_depth / 10)));
      same("vib_speed", near(g.vib_speed, want.vib_speed, want.vib_speed / 10));
      same("vib_delay", near(g.vib_delay, Math.max(1, want.vib_delay), 1));
    } else same("vib_depth");
  }
  check(`${want.name}: found as the FM-1 plays it` + (why.length ? ": " + why.join(", ") : ""), !why.length);
}
check("a tune's short name", tuneShort("Monty on the Run") === "MOTR" && tuneShort("Commando") === "COMMANDO" && tuneShort("Last Ninja 2") === "LN2");

for (const f of process.argv.slice(2)) {
  let t;
  try { t = psid(new Uint8Array(fs.readFileSync(f))); } catch (e) { console.log(`        ${path.basename(f)}: ${e.message}`); continue; }
  const run = sidRun(t, t.start, 60), r = sidInstruments(t, run);
  check(`${path.basename(f)}: ${r.notes.length} notes, ${r.instruments.length} instruments` + (run.error ? `: ${run.error}` : ""), !run.error && r.instruments.length > 0);
  for (const x of r.instruments.slice(0, 6)) {
    const f2 = x.fields;
    console.log(`          ${String(x.notes.length).padStart(4)} notes, voices ${x.voices.join("")}: AD ${f2.ad.toString(16).padStart(2, "0")} SR ${f2.sr.toString(16).padStart(2, "0")}` +
      ` wave ${f2.wave.toString(16)}${f2.flags & 1 ? " filter" : ""}${f2.flags & 2 ? " HR" : ""}` +
      (f2.wt_len ? ` steps ${f2.wt_len}/${f2.wt_loop}` : "") + (f2.pw_speed ? ` pw ${f2.pw_speed}` : "") + (f2.vib_depth ? ` vib ${f2.vib_depth}c` : ""));
  }
}
process.exit(failed ? 1 : 0);
