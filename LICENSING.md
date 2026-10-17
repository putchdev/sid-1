# Licensing

SID-1 is free software under the GNU General Public License, version 3 only (`GPL-3.0-only`, full text in
`LICENSE`). Its SID core is a port of reSID, which is GPL-2.0-or-later; a firmware built from both is distributed
under GPL-3.0. If you give someone a firmware built from it, give them its complete source under the same licence too.

## Who made what

| | |
| --- | --- |
| The platform: `firmware/hal/`, `firmware/loader/`, `crt0.S`, `app.ld`, `firmware/src/` (`usb.c`, `midi_uart.c`, `ota.c`, `recovery.c`, `lcd.c`, `gfx.c`, `libc.c`, `bootguard.h`, `uac_fir.h`), `tools/` (but `gen_font.py`) | Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments ([Felucca](https://github.com/hugelton/Felucca)), with the changes of [SLOOP](https://github.com/isod89/sloop-fm1) (isod89) |
| The wiring `firmware/src/platform.c`, the host mode `tests/host.c`, `tests/run_host.sh`, `docs/API.md`, `docs/RULES.md` | zvenson ([Dinghy](https://github.com/zvenson/dinghy)), with OPN-1's and SID-1's changes |
| The app `firmware/src/sid1.c`, `tools/gen_font.py`, the page `web/index.html`, the tests in `tests/` not listed above | The SID-1 authors |

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Press Start 2P by CodeMan38 (The Press Start 2P Project Authors), rendered into `firmware/gen/felucca_font.h` by `tools/gen_font.py`; the page loads it, and VT323 by Peter Hull, from Google Fonts | SIL OFL 1.1 | `assets/fonts/PressStart2P-OFL.txt` |
| `firmware/src/sid.c`, `sid.h`, `sid_tables.h`: the SID core, a C port of reSID 0.16 by Dag Lem; `sid_tables.h` is written by reSID's own code (`tests/sid_tables.cpp`) | GPL-2.0-or-later | the notice in `sid.c` |
| reSID 0.16 by Dag Lem, as kept in VICE's tree (libsidplayfp/resid, commit f872782), in `tests/ref/resid-0.16/` (tests only, not in the firmware). Unmodified, except `siddefs.h`, written from its `siddefs.h.in` | GPL-2.0-or-later | `tests/ref/resid-0.16/COPYING` |
| JieLi AC79 SDK: `uboot.boot`, `cfg_tool.bin`, `eq_cfg_hw.bin` are read from your SDK checkout at build time and placed in the package; no SDK files are in this tree | Apache-2.0 | <https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK> |

## No warranty

SID-1 comes with no warranty, as sections 15 and 16 of the GPL set out. Installing it on an FM-1 is done at the
owner's own risk, and its authors accept no responsibility for damaged or unusable devices.

## Trademarks

Commodore 64 is a trademark of its owners. "Felucca" and "Hügelton Instruments" are names of Hügelton Instruments;
"M-VAVE" and "FM-1" belong to their owners. SID-1 is not made or endorsed by any of them.
