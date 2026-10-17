#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""web/index.html in headless Chrome, against a fake FM-1 that speaks docs/MIDI.md as firmware/src/sid1.c does and
loses some of what the page sends.
  tests/web_app_test.py [DROP_PERCENT]
The library is read; the Editor follows the patch playing, plays edits live and saves them; slots are copied, moved,
renamed and emptied; a bank goes out as JSON and back in; an FM-1 that is not SID-1 is refused. Passes when the fake
FM-1 ends up holding exactly what it should."""
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading


def patch(name, ad, sr, wave, flags, wt=()):
    """50 bytes as sid1.c's patch_t: an independent layout from the one in the page"""
    b = list(name.encode().ljust(12, b"\0"))
    b += [ad, sr, wave, flags, 0x08, 0x00, 0xF0, 0x10, 0xE0, 0x10, 9, 0x80, 0x85, 0x22, 0xF4, 5, 10, 200, 40]
    b += [len(wt), 1, 1] + [c for c, _ in wt] + [0] * (8 - len(wt)) + [n & 255 for _, n in wt] + [0] * (8 - len(wt))
    assert len(b) == 50
    return b


LIB = {0: patch("PWM LEAD", 0x08, 0xA9, 0x40, 2), 1: patch("SAW BASS", 0x08, 0x8A, 0x20, 3),
       2: patch("KICK", 0x08, 0x00, 0x10, 2, [(0x81, 84), (0x11, 52), (0x11, -4)])}

FAKE = r"""<script>
const DROP = %DROP% / 100;
let rng = 12345;
const rnd = () => (rng = (rng * 1103515245 + 12345) >>> 0) / 4294967296;
const dev = { lib: Array(128).fill(null), cur: null, slot: 1, edited: 0, rx: null, saved: 0, frames: [], notes: [], chnotes: [], pcs: [], allOff: 0, dropped: 0, other: false,
  parts: [0, 1, 2, 3, 4, 5].map(p => ({ chan: p ? 16 : 17, slot: 1 })), mode: 0, ed: 0, cfg: 0, kit: [], kx: null };
for (const [s, b] of Object.entries(%LIB%)) dev.lib[s] = b;
dev.cur = dev.lib[1].slice();
const fakeIn = { name: "FM-1 MIDI 1", onmidimessage: null };
function reply(a, b, c) { Promise.resolve().then(() => fakeIn.onmidimessage && fakeIn.onmidimessage({ data: Uint8Array.from([a, b, c]) })); }
function dump(begin, s, b) {
  reply(0xBF, begin, s);
  for (const x of b) { reply(0xBF, 0x32, x & 127); reply(0xBF, 0x33, x >> 7); }
  const sum = b.reduce((a, x) => a + x, 0);
  reply(0xBF, 0x34, sum & 127); reply(0xBF, 0x35, sum >> 7 & 127);
}
function devMsg(st, d1, d2) {
  if ((st & 0xF0) === 0x90) dev.notes.push(d1);
  if ((st & 0xF0) === 0x90 && d2) dev.chnotes.push([st & 15, d1]);
  if ((st & 0xF0) === 0xC0) dev.pcs.push([st & 15, d1]);
  if ((st & 0xF0) === 0xB0 && d1 === 123) dev.allOff++;
  if ((st & 0xF0) === 0xC0 && dev.lib[d1]) { dev.cur = dev.lib[d1].slice(); dev.slot = d1; dev.edited = 0; }
  if (st !== 0xBF) return;
  const rx = dev.rx;
  switch (d1) {
  case 0x1F: if (!dev.other) reply(0xBF, 0x1B, 2); reply(0xBF, 0x1C, 1); reply(0xBF, 0x1D, 0); reply(0xBF, 0x1E, 0); break;
  case 0x50: case 0x5B: dev.rx = { slot: d1 === 0x5B, t: d2, b: [], lo: -1, want: 0 }; break;
  case 0x52: if (rx) { if (rx.lo >= 0) rx.bad = true; rx.lo = d2; } break;
  case 0x53: if (rx && rx.lo >= 0) { rx.b.push(rx.lo | d2 << 7); rx.lo = -1; } break;
  case 0x54: if (rx) rx.want = d2; break;
  case 0x55: {
    if (!rx) break;
    const ok = !rx.bad && rx.lo < 0 && rx.b.length === 50 && (rx.want | d2 << 7) === (rx.b.reduce((a, x) => a + x, 0) & 0x3FFF);
    if (ok && rx.slot) { dev.lib[rx.t] = rx.b; if (rx.t === dev.slot && rx.b.join() === dev.cur.join()) dev.edited = 0; }
    if (ok && !rx.slot) { dev.cur = rx.b; dev.slot = rx.t; dev.edited = 1; dev.frames.push({ t: rx.t, b: rx.b }); }
    reply(0xBF, rx.slot ? (ok ? 0x3E : 0x3F) : (ok ? 0x56 : 0x57), rx.t);
    dev.rx = null;
  } break;
  case 0x5C: if (dev.lib[d2]) dump(0x30, d2, dev.lib[d2]); else reply(0xBF, 0x36, d2); break;
  case 0x5D: dump(0x37, dev.slot, dev.cur); reply(0xBF, 0x38, dev.edited); break;
  case 0x5E: dev.lib[d2] = null; reply(0xBF, 0x3D, d2); break;
  case 0x4E: reply(0xBF, 0x6E, 0x4E); dev.saved++; reply(0xBF, 0x4D, 0); break;
  case 0x20: if (d2 < 6) dev.cfg = d2; break;
  case 0x21: dev.parts[dev.cfg].chan = d2; reply(0xBF, 0x2F, dev.cfg); break;
  case 0x25: if (dev.lib[d2]) { dev.parts[dev.cfg].slot = d2; if (dev.cfg === dev.ed) { dev.cur = dev.lib[d2].slice(); dev.slot = d2; } } reply(0xBF, 0x2F, dev.cfg); break;
  case 0x26: dev.mode = d2; reply(0xBF, 0x2F, 0x7F); break;
  case 0x2E: dev.ed = d2; dev.cur = dev.lib[dev.parts[d2].slot].slice(); dev.slot = dev.parts[d2].slot; reply(0xBF, 0x2F, 0x7E); break;
  // the kit (docs/MIDI.md 2.6): filled in order, each sample's length first
  case 0x68: dev.kit = []; reply(0xBF, 0x6C, 0); break;
  case 0x60: case 0x67: dev.kx = { len: d1 === 0x67, k: d2, v: [], lo: -1, bad: false }; break;
  case 0x61: case 0x62: if (dev.kx) dev.kx.v.push(d2); break;
  case 0x63: if (dev.kx) { if (dev.kx.lo >= 0) dev.kx.bad = true; dev.kx.lo = d2; dev.kx.v.push(d2); } break;
  case 0x64: if (dev.kx) { if (dev.kx.lo < 0) dev.kx.bad = true; dev.kx.lo = -1; dev.kx.v.push(d2); } break;
  case 0x65: if (dev.kx) dev.kx.want = d2; break;
  case 0x66: {
    const x = dev.kx; dev.kx = null;
    if (!x) break;
    const v = x.v, at = v[0] | v[1] << 7, e = [];
    for (let i = 2; i + 1 < v.length; i += 2) e.push([v[i + 1] >> 3, (v[i] | v[i + 1] << 7) & 1023]);
    let ok = !x.bad && x.lo < 0 && v.length >= 2 && v.length % 2 === 0 && (x.want | d2 << 7) === (v.reduce((a, y) => a + y, 0) & 0x3FFF);
    if (ok && x.len) { ok = !e.length && x.k === dev.kit.length && at > 0; if (ok) dev.kit.push(Array(at).fill(null)); }
    else if (ok) { const k = dev.kit[x.k]; ok = !!k && at + e.length <= k.length; if (ok) e.forEach((w, i) => k[at + i] = w); }
    reply(0xBF, ok ? 0x6A : 0x6B, x.k);
  } break;
  case 0x27: { const p = dev.parts[d2]; reply(0xBF, 0x20, d2); reply(0xBF, 0x21, p.chan); reply(0xBF, 0x25, p.slot); reply(0xBF, 0x26, dev.mode); reply(0xBF, 0x2E, dev.ed); reply(0xBF, 0x2F, d2); } break;
  }
}
const fakeOut = { name: "FM-1 MIDI 1", id: "fm1", send(data) {
  for (let i = 0; i + 1 < data.length; ) {
    const n = (data[i] & 0xF0) === 0xC0 ? 2 : 3;
    if (rnd() < DROP) dev.dropped++; else devMsg(data[i], data[i + 1], data[i + 2]);
    i += n;
  }
} };
navigator.requestMIDIAccess = async () => ({ outputs: new Map([["fm1", fakeOut]]), inputs: new Map([["fm1in", fakeIn]]), onstatechange: null });
const errs = [];
addEventListener("error", e => errs.push(e.message + " @" + e.lineno));
addEventListener("unhandledrejection", e => errs.push("rejected: " + (e.reason && e.reason.message || e.reason)));
setInterval(() => { const r = document.getElementById("result"); if (r && !r.textContent.startsWith("RESULT")) r.textContent = "STEP " + (window.step || 0) + " " + errs.join(" | ") + " status: " + (document.getElementById("status") || {}).textContent; }, 200);
</script>"""

DRIVER = r"""<script>
(async () => {
  const sleep = ms => new Promise(r => setTimeout(r, ms));
  const idle = async () => { await sleep(50); while (busy || edPending()) await sleep(50); await sleep(50); };
  const checks = [];
  const check = (what, ok) => { checks.push((ok ? "Passed  " : "FAILED  ") + what); window.step = checks.join(" / "); };
  const eq = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  const lib = %LIB%;
  const btn = t => [...document.querySelectorAll("#cards button")].find(x => x.textContent === t);
  const range = l => document.querySelector(`#cards input[aria-label="${l}"]`);
  const pick = s => { state.bank = s >> 5; state.sel = s; renderLib(); };
  const menuPick = (sel, v) => {                // a dropdown as a person uses it: its button, then the entry (the page's own list)
    const b = sel.nextElementSibling.querySelector(".menubtn"), l = b.nextElementSibling;
    b.click();
    const open = !l.classList.contains("hide") && l.children.length === sel.options.length;
    l.children[[...sel.options].findIndex(o => o.value === v)].click();
    return open && l.classList.contains("hide");
  };
  await sleep(600); await idle();
  check("the version: SID-1 1.0.0", state.fw === "1.0.0" && $("pilltext").textContent === "FM-1 · SID-1 1.0.0");
  check("the library read as the FM-1 holds it", eq(state.lib[0], lib[0]) && eq(state.lib[2], lib[2]) && state.lib.filter(Boolean).length === 3);
  check("the Editor on the patch playing (slot 2)", state.ed.slot === 1 && $("edname").value === "SAW BASS" && $("edsave").disabled);
  check("the slot's card shows its waveform and filter", $("slots").children[1].textContent.includes("SAW F"));

  btn("NOISE").click(); await sleep(300); await idle();
  const last = () => dev.frames[dev.frames.length - 1] || { b: [] };
  check("a toggle plays live, as slot 2's", last()?.t === 1 && last().b[14] === 0xA0 && dev.edited === 1);
  const at = range("ATTACK"); at.value = 9; at.dispatchEvent(new Event("input")); await sleep(300); await idle();
  check("a slider plays live and keeps its place", last().b[12] === 0x98 && range("ATTACK") === at);
  check("the Editor says edited", $("edwhere").textContent.includes("EDITED") && !$("edsave").disabled);
  $("edsave").click(); await idle(); $("done").close();
  check("Save writes it to its slot and keeps it", eq(dev.lib[1], dev.cur) && dev.lib[1][12] === 0x98 && dev.edited === 0 && dev.saved === 1);

  editSlot(2); await idle();
  check("the patch menu's slot plays on the FM-1", dev.slot === 2 && $("edname").value === "KICK");
  const shut = menuPick(document.querySelector('#cards select[aria-label="Step 2 waveform"]'), "128"); await sleep(300); await idle();
  check("a wavetable step's waveform, from its list", shut && last().t === 2 && last().b[35] === 0x80);
  check("every dropdown is the page's own list", [...document.querySelectorAll("select")].every(s => s.classList.contains("hide") && s.nextElementSibling?.matches(".menu")));
  const n = document.querySelector('#cards input[aria-label="Step 3 note"]');
  check("a relative step shows signed", n.value === "-4");
  n.value = "7"; n.onchange(); await sleep(300); await idle();
  check("a wavetable step's note", last().b[44] === 7);
  $("edrevert").click(); await sleep(300); await idle();
  check("Revert plays the slot's patch again", eq(last().b, lib[2]));

  dev.cur = dev.lib[0].slice(); dev.slot = 0; dev.edited = 0;
  fakeIn.onmidimessage({ data: Uint8Array.from([0xBF, 0x3C, 0]) }); await sleep(300); await idle();
  check("the Editor follows PRESETS on the FM-1", state.ed.slot === 0 && $("edname").value === "PWM LEAD" && $("edslot").value === "0");

  pick(0); $("copyslot").click(); pick(40); $("pasteslot").click(); await idle(); $("done").close();
  check("copied to B2 09", eq(dev.lib[40], lib[0]) && eq(dev.lib[0], lib[0]));
  pick(40); $("cutslot").click(); pick(41); $("pasteslot").click(); await idle(); $("done").close();
  check("moved to B2 10", eq(dev.lib[41], lib[0]) && dev.lib[40] === null && state.lib[40] === null);
  pick(41); $("libname").value = "my lead"; $("rename").click(); await idle(); $("done").close();
  check("renamed", String.fromCharCode(...dev.lib[41].slice(0, 7)) === "MY LEAD" && dev.lib[41][7] === 0 && eq(dev.lib[41].slice(12), lib[0].slice(12)));
  pick(41); $("clearslot").click(); await sleep(50);
  check("emptying asks first", $("done").open && $("doneok").textContent === "Empty it");
  $("doneok").click(); await idle(); $("done").close();
  check("emptied", dev.lib[41] === null);

  pick(0);
  const json = JSON.stringify({ format: "sid1-bank", version: 1, patches: state.lib.slice(0, 32).map(p => p ? toJson(p) : null) });
  check("a patch as JSON names its fields", JSON.parse(json).patches[2].wt_note[2] === -4 && JSON.parse(json).patches[1].transpose === -12);
  state.bank = 3; renderLib();
  const dt = new DataTransfer(); dt.items.add(new File([json], "bank1.json")); $("importfile").files = dt.files;
  $("importfile").onchange({ target: $("importfile") });
  for (let t = 0; t < 100 && !$("done").open; t++) await sleep(20);   // (reading the file is not on Chrome's virtual clock)
  check("importing a bank asks first", $("done").open && $("doneok").textContent === "Import");
  $("doneok").click(); await idle(); $("done").close();
  check("bank 1 imported into bank 4, byte for byte", [0, 1, 2].every(i => eq(dev.lib[96 + i], dev.lib[i])) && dev.lib.slice(99, 128).every(x => x === null));

  noteOn(60); noteOff(60);
  check("the keyboard plays", dev.notes.includes(60));

  const card = t => [...$("setups").children].find(b => b.querySelector("b").textContent === t);
  check("the setup read: Single", state.partsKnown && card("Single").classList.contains("on"));
  showTab("setup"); card("Split").click(); await idle();
  check("Split: parts 1 and 2 on channels 1 and 2", dev.mode === 0 && dev.parts[0].chan === 0 && dev.parts[1].chan === 1 &&
    dev.parts.slice(2).every(p => p.chan === 16) && $("setupbody").querySelectorAll("tr").length === 3);
  menuPick($("setupbody").querySelector('select[aria-label="Part 2\'s patch"]'), "2"); await idle();
  check("a part's patch, from its list", dev.parts[1].slot === 2 &&
    $("setupbody").querySelector('.menubtn[aria-label="Part 2\'s patch"]').textContent.startsWith("B1 03"));
  [...$("setupbody").querySelectorAll("button")].find(b => b.textContent === "Edit").click(); await idle();
  check("Edit: part 2 becomes the edited one, the Editor on its patch", dev.ed === 1 && state.edPart === 1 && $("edname").value === "KICK" &&
    $("edwhere").textContent.startsWith("PART 2"));
  [...$("setupbody").querySelectorAll("button")].find(b => b.textContent === "Add a channel").click(); await idle();
  check("a channel added", dev.parts[2].chan === 2);
  card("Voices").click(); await idle();
  check("Voices: the mode, six rows", dev.mode === 1 && $("setupbody").querySelectorAll("tr").length === 7);
  card("Single").click(); await idle();
  check("Single: part 1 on All alone, edited", dev.mode === 0 && dev.parts[0].chan === 17 && dev.parts.slice(1).every(p => p.chan === 16) && dev.ed === 0);
  const saves = dev.saved; $("keepparts").click(); await idle(); $("done").close();
  check("Save keeps the setup", dev.saved === saves + 1);
  showTab("library");

  {                                             // a made-up tune: one pulse note (C4, AD 09 SR A9) every 16 frames, and with
                                                // it a drum: 32 volume writes 44 cycles apart
    const code = [0xA9, 0x0F, 0x8D, 0x18, 0xD4, 0xA9, 0x09, 0x8D, 0x05, 0xD4, 0xA9, 0xA9, 0x8D, 0x06, 0xD4,   // $1000 init
      0xA9, 0x00, 0x8D, 0x02, 0xD4, 0xA9, 0x08, 0x8D, 0x03, 0xD4, 0xA9, 0x67, 0x8D, 0x00, 0xD4, 0xA9, 0x11, 0x8D, 0x01, 0xD4, 0x60,
      0xEE, 0x00, 0x11, 0xAD, 0x00, 0x11, 0x29, 0x0F, 0xD0, 0x08, 0xA9, 0x41, 0x8D, 0x04, 0xD4, 0x4C, 0x40, 0x10,   // $1024 play: gate
      0xC9, 0x08, 0xD0, 0x05, 0xA9, 0x40, 0x8D, 0x04, 0xD4, 0x60,                                                  // on at 0, off at 8
      0xA2, 0x20, 0x8A, 0x29, 0x0F, 0x8D, 0x18, 0xD4, 0xA0, 0x06, 0x88, 0xD0, 0xFD, 0xCA, 0xD0, 0xF2, 0x60];       // $1040 the drum
    const h = new Uint8Array(0x7C), w = (o, v) => { h[o] = v >> 8; h[o + 1] = v & 255; };
    h.set([0x50, 0x53, 0x49, 0x44]); w(4, 2); w(6, 0x7C); w(8, 0x1000); w(0xA, 0x1000); w(0xC, 0x1024); w(0xE, 2); w(0x10, 1); w(0x76, 4);
    h.set([..."Testtune"].map(c => c.charCodeAt(0)), 0x16);
    const dt = new DataTransfer(); dt.items.add(new File([h, Uint8Array.from(code)], "test.sid")); $("sidfile").files = dt.files;
    $("sidfile").onchange({ target: $("sidfile") });
    let shown = false;
    for (let t = 0; t < 500 && !imp.smp; t++) { shown ||= $("done").open && $("donetitle").textContent === "Reading the tune..."; await sleep(5); }   // (reading the file is not on Chrome's virtual clock)
    check("a popup while the tune is read, gone after", shown && !$("done").open);
    check("a .sid tune's instrument found and named", imp.list.length === 1 && imp.list[0].name === "TESTTUNE 1" &&
      $("implist").textContent.includes("PULSE") && $("impsong").options.length === 2);
    const smp = imp.smp.samples;
    const mb = $("impsong").nextElementSibling.querySelector(".menubtn"), ml = mb.nextElementSibling;
    mb.click();
    const opened = !ml.classList.contains("hide") && ml.children.length === 2 && document.activeElement === ml.children[0];
    ml.children[1].click(); for (let i = 0; i < 100 && imp.song !== 2; i++) await sleep(20); await sleep(300);
    check("the song menu: opens on a click, a pick reads that song and closes it", opened && imp.song === 2 && ml.classList.contains("hide") && mb.textContent === "2 of 2");
    $("impsong").value = "1"; $("impsong").onchange(); for (let i = 0; i < 100 && imp.song !== 1; i++) await sleep(20); await sleep(300);
    check("and shows a song set by the page", mb.textContent === "1 of 2");
    check("its drum found as a sample: 32 writes, 44 cycles apart, the value down 1 each", smp.length === 1 && smp[0].writes.length === 32 &&
      smp[0].writes.every((w, i) => w[0] === (32 - i & 15) && w[1] === 44) && $("kitlist").children.length === 1 && !$("impkit").classList.contains("hide"));
    const before = dev.notes.length;
    [...$("implist").querySelectorAll("button")].find(b => b.textContent === "Play").click(); await sleep(1200);
    check("Play previews it on the FM-1, at its pitch", dev.frames.length && last().b[12] === 0x09 && last().b[13] === 0xA9 && dev.notes.slice(before).includes(60));
    state.bank = 2; renderLib(); $("slots").children[4].click();
    check("picking a slot sets where imports go, shown in the grid", $("tbank").value === "2" && $("tslot").value === "4" &&
      $("slots").children[4].textContent.includes("NEW: TESTTUNE 1"));
    $("sendimp").click(); await idle(); $("done").close();
    const b = dev.lib[68];
    check("sent to B3 05 as found", b && String.fromCharCode(...b.slice(0, 10)) === "TESTTUNE 1" && eq(b.slice(12, 18), [0x09, 0xA9, 0x40, 0, 8, 0]) && b[31] === 0 && b[30] === 0);
    check("its drum sent to the FM-1's kit", dev.kit.length === 1 && JSON.stringify(dev.kit[0]) === JSON.stringify(smp[0].writes));
    const ev = songEvents(), mid = midiFile(ev, "Testtune");
    check("the song as MIDI: a C4 every 16 frames on channel 1, from B3 05, the drum with it on 10; a .mid of it",
      ev.filter(e => e.m[0] === 0x90).length >= 100 && ev.filter(e => e.m[0] === 0x90).every(e => e.m[1] === 60) &&
      ev.some(e => e.m[0] === 0xC0 && e.m[1] === 68) && ev.filter(e => e.m[0] === 0x99 && e.m[1] === 36).length >= 100 &&
      ev.filter(e => e.m[0] === 0x99).every(d => ev.some(e => e.m[0] === 0x90 && Math.abs(e.t - d.t) < 0.02)) && String.fromCharCode(...mid.slice(0, 4)) === "MThd");
    const was = dev.chnotes.length;
    $("songplay").click(); await sleep(1500);
    const heard = dev.chnotes.slice(was);
    check("Play song: voices mode, the patch's program change, notes on channel 1, the drum on channel 10", dev.mode === 1 &&
      dev.pcs.some(p => p[0] === 0 && p[1] === 68) && heard.filter(n => n[0] === 0).length >= 3 && heard.every(n => n[0] === 0 && n[1] === 60 || n[0] === 9 && n[1] === 36) &&
      heard.some(n => n[0] === 9) &&
      $("songplay").textContent === "Stop");
    $("songplay").click(); await sleep(600); await idle();
    check("Stop: all notes off, back to parts mode", dev.allOff >= 6 && dev.mode === 0 && $("songplay").textContent === "Play song");
  }

  state.bank = 2; renderLib(); $("emptybank").click(); await sleep(50);
  check("emptying a bank asks first", $("done").open && $("doneok").textContent === "Empty it");
  $("doneok").click(); await idle(); $("done").close();
  check("Empty bank: bank 3 emptied on the FM-1, the others kept", dev.lib.slice(64, 96).every(x => x === null) && dev.lib[0] && dev.lib[1]);

  dev.other = true; state.fw = null; await readAll();
  check("an FM-1 that is not SID-1 is not used", state.fw === false && $("done").open && $("donetitle").textContent === "Needs SID-1");
  $("done").close(); const before = JSON.stringify(dev.lib); $("readlib").click(); await sleep(100);
  check("and nothing is sent to it", $("status").textContent === "This page needs SID-1 on the FM-1." && JSON.stringify(dev.lib) === before);
  document.getElementById("result").textContent = "RESULT\n" + checks.join("\n") + `\n(dropped ${dev.dropped} messages, status "${$("status").textContent}")`;
})();
</script><pre id="result"></pre>"""


def main():
    drop = float(sys.argv[1]) if len(sys.argv) > 1 else 2
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    page = open(os.path.join(root, "web", "index.html")).read()
    lib = json.dumps(LIB)
    page = page.replace("<head>", "<head>" + FAKE.replace("%DROP%", str(drop)).replace("%LIB%", lib), 1)
    page = page.replace("</body>", DRIVER.replace("%LIB%", lib) + "</body>", 1)
    d = tempfile.mkdtemp()
    open(os.path.join(d, "t.html"), "w").write(page)

    class Quiet(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a): super().__init__(*a, directory=d)
        def log_message(self, *a): pass
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    out = subprocess.run(["google-chrome", "--headless=new", "--disable-gpu", "--no-sandbox", f"--user-data-dir={d}/c",
                          "--virtual-time-budget=120000", "--dump-dom", f"http://127.0.0.1:{srv.server_address[1]}/t.html"],
                         capture_output=True, text=True, timeout=600).stdout
    srv.shutdown()
    shutil.rmtree(d, ignore_errors=True)
    m = re.search(r'<pre id="result">(RESULT[^<]*)</pre>', out)
    s = re.search(r'<pre id="result">([^<]*)</pre>', out)
    text = m.group(1).replace("&quot;", '"') if m else "RESULT none: the page did not finish: " + (s.group(1) if s else "")
    print(text)
    sys.exit(0 if m and "FAILED" not in text else 1)


main()
