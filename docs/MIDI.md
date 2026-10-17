# SID-1 MIDI

SID-1 takes USB MIDI and MIDI on the TRS jack. SysEx belongs to the updater and never reaches the app, so the page
(`web/index.html`) talks to the FM-1 in control changes on channel 16 (status `BF`), and the FM-1 answers the
same way.

## 1. Playing

SID-1 plays up to six **parts**. A part is a library slot on a MIDI channel; all parts share the six voices
(three on each chip). The FM-1's keys, knobs and SAVE, and the page's current patch (2.2, 2.3), are the edited
part's, picked with PART on the GLO page or `2E` (2.5). Channel 16 is the page's: its notes, program changes and
pitch bend go to the edited part, as the FM-1's keys do, whatever the mode. All means channels 1 to 9 and 11 to 15.

| Mode | What a channel plays |
| --- | --- |
| Parts (0) | Each part on its channel (or All, or off), as many notes as there are voices. Parts on one channel sound together (a layer). A new note takes a free voice released longest ago, else the part's own oldest note, else anyone's |
| Voices (1) | Channel n (1 to 6) is part n on voice n, one note at a time: chip 1 has voices 1 to 3, chip 2 voices 4 to 6, as a C64 tune's own voices |

The setups by what they do, as the GLO page names them: **Single** (part 1 alone on All, the default), **Layer**
(parts stacked on All), **Split** (each part on a channel of its own), **Mixed** (anything else).

Each chip has one filter for its three voices. In Parts mode a filtered part's note goes to a chip whose filter no
other filtered part is using, where it can; else the two share it, and the newest note's part sets the cutoff,
resonance and mode.

On a part's channel:

- **Note on and off.**
- **Program change n**: library slot n onto the part (0 to 127: bank 1 slot 1 is 0, bank 4 slot 32 is 127). An empty
  slot is ignored.
- **Pitch bend**: 2 semitones each way, until **RPN 0** (CC 101 0, CC 100 0, then CC 6 n) sets n, up to 24.
- **CC 123**: all the part's notes off.

**Channel 10** is the sample kit: notes 36 to 51 play samples through chip 1's volume register, as C64 tunes play
their sampled drums. The 6581's DC offset makes that register a 4-bit DAC, so the samples are heard on the 6581 and
barely at all on the 8580, as on a real C64; they also move the volume of chip 1's voices while they play. The page
sends a tune's drums as the kit (2.6), kept in flash; until one is sent, it is a test kit: 36 a kick, 38 a snare,
42 a hat (8 kHz).

## 2. The page

### 2.1 The firmware

| Page sends | FM-1 replies |
| --- | --- |
| `1F 0` | `1B 2` (the firmware: 2 is SID-1; OPN-1 sends no 1B), then `1C` major, `1D` minor, `1E` patch. `1C 127` is a development build |

### 2.2 A patch

A patch travels as its 50 bytes (section 3), each as **CC 52** with its low 7 bits and **CC 53** with its top bit,
between a begin and a checksum:

| CC | Meaning |
| --- | --- |
| `5B s` | Begin: for library slot s. Reply `3E s` (taken) or `3F s` (refused) |
| `50 s` | Begin: play it now as the current patch, as slot s's (not stored; the FM-1 shows it as edited, and its SAVE keeps it in slot s). Reply `56 s` (taken) or `57 s` (refused) |
| `52 lo`, `53 hi` | One byte. 50 pairs, in order |
| `54 lo`, `55 hi` | The checksum: the sum of the 50 bytes mod 2^14, low 7 bits then high 7. `55` ends the frame |

A frame with the wrong number of bytes or a wrong checksum is refused. On no reply, send it again. Whatever the
FM-1 takes, it makes safe to play: the name ends at 11 characters, the wavetable at 8 steps, and unused bits are
cleared.

Writing a slot changes RAM only. To keep the library, send `4E` (2.4).

### 2.3 Reading

| Page sends | FM-1 replies |
| --- | --- |
| `5C s` | Slot s: `30 s`, its 50 bytes as `32 lo` `33 hi` pairs, then `34 lo` `35 hi`, the checksum. An empty slot replies `36 s` alone |
| `5D 0` | The current patch: `37 s` (s: its slot), the bytes and the checksum as above, then `38 e` (1: edited since it was loaded or saved) |

The FM-1 sends **`3C s`** when the current patch or its slot changes on the FM-1 itself (PRESETS, a knob, a program
change), at most every 200 ms. The page can then read it with `5D`. Changes the page makes do not echo back.

### 2.4 The library

| Page sends | FM-1 replies |
| --- | --- |
| `5E s` | Empty slot s. Reply `3D s` |
| `4E 0` | Keep the library and the parts (2.5) in flash. Reply `6E 4E` at once, then `4D r` when done: 0 kept, 4 erase failed, 5 write failed. The sound drops out for a moment while the flash is erased |

A slot is empty when its name's first byte is 0.

### 2.5 The parts

| Page sends | FM-1 replies |
| --- | --- |
| `20 p` | Pick part p (0 to 5) for the CCs below. No reply |
| `21 c` | Its MIDI channel: 0 to 15, 16 off, 17 All. Notes playing stop. Reply `2F p` |
| `25 s` | Its library slot (an empty one is ignored). Reply `2F p` |
| `26 m` | The mode: 0 parts, 1 voices. Notes playing stop. Reply `2F 7F` |
| `2E p` | Edit part p: the FM-1's keys and knobs, channel 16 and the current patch (2.2, 2.3) become its. Reply `2F 7E` |
| `27 p` | Read part p: `20 p`, `21 c`, `25 s`, `26 m`, `2E e` (the edited part), then `2F p` |

The FM-1 sends **`28+p s`** when part p's slot changes (PRESETS on the edited part, a program change). `22` to
`24` are taken and ignored. Changes are RAM only until `4E` or SAVE keeps them, in the library's store.

### 2.6 The sample kit

A sample is a list of writes to the volume register: each a value (0 to 15) and the cycles until the next write
(up to 1023, at the PAL clock, 985248 a second). Sample k is channel 10's note 36+k. The kit holds 16 samples and
8192 writes between them.

| Page sends | FM-1 replies |
| --- | --- |
| `68 0` | Empty the kit. Reply `6C 0` |
| `67 k`, a frame | Sample k's length (the frame's number, no writes). Reply `6A k`, or `6B k` if refused |
| `60 k`, a frame | Sample k's writes from index n (the frame's number). Reply `6A k`, or `6B k` if refused |

A frame after `67 k` or `60 k`: `61 a`, `62 b` (the number, a + b * 128); up to 128 writes, each `63 lo`, `64 hi`
(e = lo + hi * 128: the value is e >> 10, the cycles e & 1023); then `65 lo`, `66 hi`, the sum of the 7-bit values
of `61` to `64`, mod 16384. The kit fills in order: after `68`, sample 0's length, then 1's, and so on; a length for
a sample that has one, or past the room left, is refused. Data frames can be sent again. `4E` keeps the kit in flash.

## 3. The patch

50 bytes, in this order. Signed values are two's complement.

| Byte | Name | Meaning |
| --- | --- | --- |
| 0-11 | name | ASCII, 0 after the last character (11 at most) |
| 12 | ad | The SID's attack (high 4 bits) and decay (low 4) |
| 13 | sr | Sustain (high 4) and release (low 4) |
| 14 | wave | The control register without the gate: waveform bits 7-4 (noise, pulse, saw, triangle), test 3, ring 2, sync 1 |
| 15 | flags | Bit 0: the voice goes through its chip's filter. Bit 1: hard restart |
| 16, 17 | pw_hi, pw_lo | Pulse width at note on, 12 bits (pw_hi's low 4 bits are the top) |
| 18 | pw_speed | Signed: added to the pulse width each frame (50 a second), turning back at the limits |
| 19, 20 | pw_min, pw_max | The sweep's limits, times 16 |
| 21 | mode | The filter's mode: register 0x18 bits 6-4 (high-pass, band-pass, low-pass) |
| 22 | res | Resonance, 0 to 15 |
| 23 | cut | Cutoff at note on: the top 8 of the 11 bits (register 0x16) |
| 24 | cut_speed | Signed: added to the 11-bit cutoff each frame until it reaches cut_end |
| 25 | cut_end | Where the cutoff sweep stops, as cut |
| 26 | transpose | Signed semitones |
| 27 | fine | Signed cents |
| 28 | vib_delay | Frames before the vibrato starts |
| 29 | vib_speed | Phase step a frame: a cycle is 256 / speed frames |
| 30 | vib_depth | Cents either way |
| 31 | wt_len | Wavetable steps, 0 to 8 (0: no wavetable) |
| 32 | wt_loop | The step after the last (0-based); wt_len or more holds the last |
| 33 | wt_abs | Bit k: step k's note is a MIDI note, not an offset from the key |
| 34-41 | wt_ctl | Each step's waveform (bits 7-4; 0: the patch's). Test, ring and sync come from the patch |
| 42-49 | wt_note | Each step's note: signed semitones, or a MIDI note (wt_abs) |

Each chip's filter is shared by its three voices, as on a C64: a note of a patch with bit 0 of flags set restarts
its chip's cutoff sweep, for every note on that chip.

Reference: `tests/library_test.py` exercises every message above on the host build.
