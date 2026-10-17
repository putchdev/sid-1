#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-only
// web/index.html's 6502 and PSID runner (its "c64" section), on their own:
//   tests/c64_test.js [tune.sid ...]
// - Klaus Dormann's 6502 functional test (every documented opcode, decimal mode included)
// - Tom Harte's per-opcode tests for the undocumented opcodes tunes use, and a few documented ones: registers,
//   memory and cycles after each instruction
// - made-up PSID tunes: one with a play address, one that installs an interrupt handler, one timed by the CIA
// - the CIA's timers, and made-up RSID tunes run by a raster interrupt, a CIA 1 timer and CIA 2 NMIs
// - any .sid files given: run a minute each, and say what they wrote
// The references go in build/ref (a missing one is reported as skipped):
//   dormann/6502_functional_test.bin  github.com/Klaus2m5/6502_65C02_functional_tests, bin_files/
//   harte/XX.json                     github.com/SingleStepTests/65x02, 6502/v1/ (any opcodes; 25 were used)
"use strict";
const fs = require("fs"), path = require("path");
const ROOT = path.resolve(__dirname, "..");
const page = fs.readFileSync(path.join(ROOT, "web/index.html"), "utf8");
const code = page.slice(page.indexOf("// ==== c64 begin"), page.indexOf("// ==== c64 end"));
const { C64, Cia, psid, sidRun } = new Function(code + "; return { C64, Cia, psid, sidRun };")();

let failed = 0;
const check = (what, ok) => { console.log((ok ? "Passed  " : "FAILED  ") + what); failed += !ok; };
const skip = what => console.log("Skipped " + what);

function dormann() {
  const f = path.join(ROOT, "build/ref/dormann/6502_functional_test.bin");
  if (!fs.existsSync(f)) return skip("Dormann's functional test: no " + path.relative(ROOT, f));
  const m = new C64();
  m.flat = true;
  m.ram.set(fs.readFileSync(f));
  m.pc = 0x400;
  let prev = -1, n = 0;
  while (m.pc !== prev && n < 1e8) { prev = m.pc; m.step(); n++; }   // (it ends, passed or not, in a JMP to itself)
  check(`Dormann's functional test: passed at $3469 (stopped at $${m.pc.toString(16)}, ${n} instructions)`, m.pc === 0x3469);
}

function harte() {
  const dir = path.join(ROOT, "build/ref/harte");
  const files = fs.existsSync(dir) ? fs.readdirSync(dir).filter(x => x.endsWith(".json")).sort() : [];
  if (!files.length) return skip("Harte's opcode tests: none in build/ref/harte");
  const m = new C64();
  m.flat = true;
  for (const f of files) {
    const tests = JSON.parse(fs.readFileSync(path.join(dir, f), "utf8"));
    let bad = 0, first = "";
    for (const t of tests) {
      const i = t.initial, e = t.final;
      for (const [a, v] of i.ram) m.ram[a] = v;
      Object.assign(m, { pc: i.pc, s: i.s, a: i.a, x: i.x, y: i.y, p: i.p, cycles: 0, jam: false });
      m.step();
      const why = [["pc", m.pc, e.pc], ["s", m.s, e.s], ["a", m.a, e.a], ["x", m.x, e.x], ["y", m.y, e.y], ["p", m.p, e.p],
        ["cycles", m.cycles, t.cycles.length], ...e.ram.map(([a, v]) => [`$${a.toString(16)}`, m.ram[a], v])]
        .filter(([, got, want]) => got !== want);
      for (const [a] of i.ram) m.ram[a] = 0;
      for (const [a] of e.ram) m.ram[a] = 0;
      if (why.length) { bad++; if (!first) first = `${t.name}: ` + why.map(([k, g, w]) => `${k} ${g} not ${w}`).join(", "); }
    }
    check(`opcode ${f.slice(0, 2)}: ${tests.length - bad}/${tests.length}` + (first ? ` (first: ${first})` : ""), !bad);
  }
}

function tune(load, init, play, code, extra = {}) {   // a PSID v2 file around some machine code (RSID: extra.rsid)
  const h = new Uint8Array(0x7C);
  const w = (o, v) => { h[o] = v >> 8; h[o + 1] = v & 255; };
  h.set([..."PSID"].map(c => c.charCodeAt(0)));
  if (extra.rsid) {                             // (the load address goes in front of the data)
    h.set([..."RSID"].map(c => c.charCodeAt(0)));
    code = [load & 255, load >> 8, ...code];
    load = 0;
  }
  w(4, 2); w(6, 0x7C); w(8, load); w(0xA, init); w(0xC, play); w(0xE, 1); w(0x10, 1);
  h.set([..."TEST"].map(c => c.charCodeAt(0)), 0x16);
  w(0x76, 1 << 2 | 1 << 4);                    // PAL, 6581
  if (extra.cia) h[0x15] = 1;
  if (extra.basic) h[0x77] |= 2;
  return Uint8Array.from([...h, ...code]);
}

function madeUp() {
  // init ($1000): song number to $1100, volume 15; play ($1003): a count to voice 1's frequency, then a gate
  const code = [
    0x4C, 0x06, 0x10,                           // $1000 JMP $1006
    0x4C, 0x10, 0x10,                           // $1003 JMP $1010
    0x8D, 0x00, 0x11,                           // $1006 STA $1100
    0xA9, 0x0F, 0x8D, 0x18, 0xD4,               // $1009 LDA #$0F; STA $D418
    0x60, 0xEA,                                 // $100E RTS; NOP
    0xEE, 0x01, 0x11,                           // $1010 INC $1101
    0xAD, 0x01, 0x11, 0x8D, 0x00, 0xD4,         // $1013 LDA $1101; STA $D400
    0xA9, 0x41, 0x8D, 0x04, 0xD4,               // $1019 LDA #$41; STA $D404
    0x60,                                       // $101E RTS
  ];
  const t = psid(tune(0x1000, 0x1000, 0x1003, code));
  check("a PSID's header read", t.name === "TEST" && t.load === 0x1000 && t.init === 0x1000 && t.play === 0x1003 && t.clock === 1 && t.model === 1);
  const r = sidRun(t, 1, 1);
  check("init's writes", JSON.stringify(r.init) === "[[0,24,15]]" && !r.error);
  check("50 play calls a second (PAL: 50.12), each with its writes", Math.abs(r.rate - 985248 / 19656) < 1e-9 && r.frames.length === 50 &&
    r.frames.every((f, k) => JSON.stringify(f) === JSON.stringify([[0, 0, k + 1 & 255], [0, 4, 0x41]])));

  // the same through an interrupt: init sets $0314 to the handler, which ends in JMP $EA31
  const irq = [
    0xA9, 0x0C, 0x8D, 0x14, 0x03,               // $1000 LDA #<$100C; STA $0314
    0xA9, 0x10, 0x8D, 0x15, 0x03,               // $1005 LDA #>$100C; STA $0315
    0x60, 0xEA,                                 // $100A RTS; NOP
    0xEE, 0x01, 0x11,                           // $100C INC $1101
    0xAD, 0x01, 0x11, 0x8D, 0x00, 0xD4,         // $100F LDA $1101; STA $D400
    0x4C, 0x31, 0xEA,                           // $1015 JMP $EA31
  ];
  const t2 = psid(tune(0x1000, 0x1000, 0, irq));
  const r2 = sidRun(t2, 1, 1);
  check("a tune with its own interrupt handler, through $0314 and out through $EA31",
    !r2.error && r2.frames.length === 50 && r2.frames.every((f, k) => JSON.stringify(f) === JSON.stringify([[0, 0, k + 1 & 255]])));

  // a CIA-timed tune: init sets timer A to 1/4 of a frame
  const cia = [0xA9, 4913 & 255, 0x8D, 0x04, 0xDC, 0xA9, 4913 >> 8, 0x8D, 0x05, 0xDC, 0x60, 0x60];
  const r3 = sidRun(psid(tune(0x1000, 0x1000, 0x100B, cia, { cia: 1 })), 1, 1);
  check(`a CIA-timed tune runs at its timer's rate (${r3.rate.toFixed(2)} Hz)`, Math.abs(r3.rate - 985248 / 4914) < 1e-9 && r3.frames.length === 200);

  let err = "";
  try { psid(tune(0x1000, 0, 0, [0x60], { rsid: 1, basic: 1 })); } catch (e) { err = e.message; }
  check("an RSID tune that starts BASIC is refused, saying why", err.startsWith("an RSID tune that starts a BASIC program"));
}

function cia() {                                // the 6526's timers, as its data sheet has them
  const c = new Cia();
  c.wr(4, 99); c.wr(5, 0); c.wr(0xE, 0x11);     // timer A: 100 cycles a period, continuous
  c.run(99);
  check("timer A counts down to 0 without an underflow", c.ca === 0 && !c.flags);
  c.run(1);
  check("... then reloads its latch: an underflow", c.ca === 99 && c.flags === 1);
  c.run(1000);
  check("10 periods later: the same count", c.ca === 99);
  c.wr(0xD, 0x81);
  check("its interrupt asked for: the flags read with bit 7, then cleared", c.rd(0xD) === 0x81 && c.rd(0xD) === 0 && !c.irq);
  c.wr(0xD, 0x01);
  c.run(100);
  check("... and not asked for any more", c.flags === 1 && !c.irq);
  c.wr(0xE, 0x19);                              // one shot
  c.run(250);
  check("one shot: stops after one underflow, at its latch", !(c.cra & 1) && c.ca === 99);
  c.wr(6, 2); c.wr(7, 0); c.wr(0xF, 0x51); c.wr(0xE, 0x11);   // timer B counts timer A's underflows
  c.flags = 0;
  c.run(299);
  check("timer B counting timer A: 3 underflows of A, B at 0", c.cb === 0 && !(c.flags & 2));
  c.run(1);
  check("... a 4th: B underflows", c.cb === 2 && c.flags & 2);
}

function rsid() {
  const many = (r, k) => r.frames.map(f => f.filter(w => w[1] === k).length);
  // a raster interrupt at line $40: INC $D019 acknowledges it, a count goes to voice 1's frequency, out via $EA81
  const raster = [0x78, 0xA9, 0x7F, 0x8D, 0x0D, 0xDC, 0xAD, 0x0D, 0xDC, 0xA9, 0x01, 0x8D, 0x1A, 0xD0, 0xA9, 0x40, 0x8D, 0x12, 0xD0,
    0xAD, 0x11, 0xD0, 0x29, 0x7F, 0x8D, 0x11, 0xD0, 0xA9, 0x2B, 0x8D, 0x14, 0x03, 0xA9, 0x10, 0x8D, 0x15, 0x03, 0x58, 0x60,
    0xEA, 0xEA, 0xEA, 0xEA,
    0xEE, 0x19, 0xD0, 0xEE, 0x00, 0x11, 0xAD, 0x00, 0x11, 0x8D, 0x00, 0xD4, 0x4C, 0x81, 0xEA];   // $102B: the handler
  const t = psid(tune(0x1000, 0x1000, 0, raster, { rsid: 1 }));
  const r = sidRun(t, 1, 2), w = r.frames.flat();
  check(`RSID, a raster interrupt: once a frame (${w.length} in ${r.frames.length} frames)` + (r.error ? `: ${r.error}` : ""),
    t.rsid && !r.error && r.frames.length === 100 && many(r, 0).slice(1).every(n => n === 1) && w.every((x, k) => x[2] === (k + 1 & 255)));
  // CIA 1 timer A at 4914 cycles (200.5 a second): its handler reads $DC0D, then the count to the frequency
  const timer = [0x78, 0xA9, 0x21, 0x8D, 0x14, 0x03, 0xA9, 0x10, 0x8D, 0x15, 0x03, 0xA9, 0x31, 0x8D, 0x04, 0xDC, 0xA9, 0x13,
    0x8D, 0x05, 0xDC, 0xA9, 0x11, 0x8D, 0x0E, 0xDC, 0xA9, 0x81, 0x8D, 0x0D, 0xDC, 0x58, 0x60,
    0xAD, 0x0D, 0xDC, 0xEE, 0x00, 0x11, 0xAD, 0x00, 0x11, 0x8D, 0x00, 0xD4, 0x4C, 0x81, 0xEA];   // $1021: the handler
  const r2 = sidRun(psid(tune(0x1000, 0x1000, 0, timer, { rsid: 1 })), 1, 2), n2 = r2.frames.flat().length;
  check(`RSID, CIA 1's timer: ${n2} interrupts in 100 frames (200.5 a second)`, !r2.error && Math.abs(n2 - r2.frames.length * 19656 / 4914) <= 1);
  // CIA 2 timer A at $2001 cycles, an NMI each: the count to the volume; init returns with interrupts off (NMI ignores that)
  const nmi = [0x78, 0xA9, 0x20, 0x8D, 0x18, 0x03, 0xA9, 0x10, 0x8D, 0x19, 0x03, 0xA9, 0x00, 0x8D, 0x04, 0xDD, 0xA9, 0x20,
    0x8D, 0x05, 0xDD, 0xA9, 0x11, 0x8D, 0x0E, 0xDD, 0xA9, 0x81, 0x8D, 0x0D, 0xDD, 0x60,
    0x48, 0xAD, 0x0D, 0xDD, 0xEE, 0x01, 0x11, 0xAD, 0x01, 0x11, 0x8D, 0x18, 0xD4, 0x68, 0x40];   // $1020: the handler
  const r3 = sidRun(psid(tune(0x1000, 0x1000, 0, nmi, { rsid: 1 })), 1, 2), n3 = r3.frames.flat().length;
  check(`RSID, CIA 2's timer as NMIs: ${n3} in 100 frames (120.3 a second)`, !r3.error && Math.abs(n3 - r3.frames.length * 19656 / 8193) <= 1);
}

function real(files) {
  for (const f of files) {
    let t;
    try { t = psid(new Uint8Array(fs.readFileSync(f))); } catch (e) { console.log(`        ${path.basename(f)}: ${e.message}`); continue; }
    const r = sidRun(t, t.start, 60), all = r.frames.flat();
    const gates = r.frames.reduce((n, fr, k) => n + fr.filter(([c, reg, v]) => reg % 7 === 4 && reg < 21 && v & 1).length, 0);
    const chips = new Set(all.map(([c]) => c)).size;
    check(`${path.basename(f)}: "${t.name}" by ${t.author}, song ${t.start}/${t.songs}: ${r.frames.length} calls at ` +
      `${r.rate.toFixed(1)} Hz, ${all.length} writes on ${chips} chip(s), ${gates} gate writes` + (r.error ? `: ${r.error}` : ""),
      !r.error && gates > 0);
  }
}

dormann();
harte();
madeUp();
cia();
rsid();
real(process.argv.slice(2));
process.exit(failed ? 1 : 0);
