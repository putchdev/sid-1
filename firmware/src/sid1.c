/* SPDX-License-Identifier: GPL-3.0-only */
/* SID-1: two Commodore 64 SID chips (sid.c, reSID 0.16) as an instrument on the FM-1. Six voices (three a chip)
 * play up to six parts, each a patch on a MIDI channel, as a C64 music player would: 50 frames a second.
 *
 *   keys         the edited part (PART on GLO); MIDI by part, or in VOICES mode channel n is voice n (docs/MIDI.md)
 *   PRESETS      the edited part's library slot (4 banks of 32, empty ones skipped); SAVE keeps its patch as edited
 *                in its slot, and the parts
 *   pages        HOME sound, ENV envelope, EDIT setup, FX pulse (again: filter), LFO vibrato, ARP wavetable (again:
 *                its loop), GLO global (chip model, 2x, the edited part, the mode; the parts). Four knobs each;
 *                ALGORITHM is the waveform, or the filter mode on EDIT and FILTER, or the step's waveform on the
 *                wavetable's (SEL: the next step)
 *   OCT- OCT+    the octave
 *
 * Each chip has one filter for its three voices, as on a C64: a note of a filtered patch starts its chip's cutoff
 * sweep again, for every note on that chip, and makes the filter its part's. Filtered parts get a chip each where
 * they can.
 *
 * The main loop never touches the chips: it queues register writes (wq_*), which the audio interrupt applies
 * before it renders. The chips run at the PAL clock, sampled as reSID's SAMPLE_FAST at 44.1 kHz, or at 88.2 kHz and
 * filtered down (2x, on GLO). */
#define APP_NAME "SID-1"                      /* the rescue and the crash screen say this */
#include "platform.c"
#include "sid.h"

#ifdef SID1_HOST
#define FM1_TICKS_PER_US 24u
static uint32_t fm1_ticks(void) { return fm1_ms * 24000u; }   /* host: the same 24 MHz units, simulated time */
#endif

/* ---- the register write queue: the main loop writes, the audio interrupt applies (one writer each) */
#define WQ 512u
enum { WQ_SID0, WQ_SID1, WQ_MODEL, WQ_DIG0, WQ_DIG1 };   /* WQ_DIGn: chip n plays kit sample reg */
static uint32_t wq[WQ];                         /* dev << 16 | reg << 8 | value */
static volatile uint32_t wq_w, wq_r;

static void q(uint32_t dev, uint32_t reg, uint32_t val)
{
    while (wq_w + 1u - wq_r > WQ) platform_poll();   /* full: the interrupt empties it every 5.8 ms */
    wq[wq_w % WQ] = dev << 16 | (reg & 0xFFu) << 8 | (val & 0xFFu);
    wq_w++;
#ifdef SID1_HOST
    if (host_sid_log && dev < 2u) printf("%6u ms  SID %u %u %u\n", fm1_ms, dev, reg & 0xFFu, val & 0xFFu);
#endif
}

/* ---- the sound (audio interrupt) */
static sid_t chip[2];
static int32_t owed[2];                         /* cycles given to a chip but not yet run (under 1024 between buffers) */
static int16_t sbuf[2][2u * HALF_FRAMES + 4u];
static int32_t hist[2];                         /* 2x: the mix's last two half-step samples, from the buffer before */
static volatile uint32_t oversample = 1;        /* main loop -> interrupt: 1 = 2x */
static volatile uint32_t load_ticks;            /* the interrupt's time, the worst since the main loop read it */
/* buffers to keep silent: the 6581's DC offset jumps with the volume register and the model, and the output's
 * highpass (reSID's external filter, ~10 ms) takes a while to take it away: a thump, unless nobody hears it */
#define SETTLE_BUFFERS 24u                      /* 140 ms */
static uint32_t settle = SETTLE_BUFFERS;
/* sid_render lives in RAM, out of a direct call's reach from flash */
static int (*volatile render)(sid_t *, int32_t *, int32_t, int16_t *, int, int) = sid_render;

/* samples, as a C64 tune's drums: values written to a chip's volume register, each at its own time. The 6581's
 * DC offset makes that register a 4-bit DAC (next to silent on the 8580, as on a real one), and it moves the chip's
 * voices too. A sample is its writes, as the tune made them: a value and the cycles until the next (an 8 kHz sample
 * has 123 between each; Arkanoid's drums change the gap as they go). The interrupt renders such a chip up to each
 * write's cycle */
#define NSMP 8192u                              /* writes, all samples together: 16 KiB */
#define NKIT 16u                                /* MIDI notes 36 to 51 on channel 10 */
typedef struct { uint16_t off, len; } kit_t;    /* in smp */
static uint16_t smp[NSMP];                      /* value << 12 | cycles until the next write (the last's: until the end) */
static kit_t kit[NKIT];
static struct { uint32_t on, i, end, v; int32_t left; } dig[2];   /* the interrupt's own; left: cycles to the next write */
#ifdef SID1_HOST
static uint32_t host_made[2];                   /* (samples made, for the log) */
#endif

static void vol_set(uint32_t c, uint32_t v)
{
    sid_t *s = &chip[c];
    sid_write(s, 0x18, s->voice3off | s->hp_bp_lp << 4 | v);
}

static void chip_render(uint32_t c, int16_t *out, uint32_t m, int32_t cps)
{
    uint32_t made = 0;
    if (dig[c].on) vol_set(c, dig[c].v);        /* (a queued write to 0x18 set the volume back) */
    while (made < m) {
        int32_t *dt = &owed[c];
        if (dig[c].on) {
            if (dig[c].left <= 0) {             /* a write is due: the next value, or the end (back to 15, as the voices use) */
                uint32_t e;
                if (dig[c].i >= dig[c].end) { dig[c].on = 0; vol_set(c, 15); continue; }
                e = smp[dig[c].i++];
                dig[c].v = e >> 12;
                dig[c].left = (int32_t)(e & 0xFFFu);
                vol_set(c, dig[c].v);
#ifdef SID1_HOST
                if (host_sid_log) printf("DIG %u %u %u\n", c, dig[c].v, host_made[c] + made);
#endif
                continue;
            }
            dt = &dig[c].left;
        }
        made += (uint32_t)render(&chip[c], dt, cps, out + made, (int)(m - made), 1);
        if (!dig[c].on && made < m) owed[c] += 1024;
    }
#ifdef SID1_HOST
    host_made[c] += m;
#endif
}

static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, c, t0 = fm1_ticks(), dt, os = oversample ? 2u : 1u, m = n * os;
    int32_t cps = os == 2u ? SID_CPS_PAL / 2 : SID_CPS_PAL;   /* (SID_CPS_PAL is even: 2x is exact) */
    while (wq_r != wq_w) {
        uint32_t e = wq[wq_r % WQ], dev = e >> 16;
        if (dev == WQ_MODEL) {
            sid_set_model(&chip[0], (int)(e & 1u));
            sid_set_model(&chip[1], (int)(e & 1u));
            settle = SETTLE_BUFFERS;
        } else if (dev >= WQ_DIG0) {
            const kit_t *k = &kit[(e >> 8 & 0xFFu) % NKIT];
            dig[dev - WQ_DIG0].on = k->len != 0 && k->off + k->len <= NSMP;
            dig[dev - WQ_DIG0].i = k->off;
            dig[dev - WQ_DIG0].end = (uint32_t)k->off + k->len;
            dig[dev - WQ_DIG0].left = 0;
        } else sid_write(&chip[dev & 1u], e >> 8 & 0xFFu, e & 0xFFu);
        wq_r++;
    }
    for (c = 0; c < 2u; c++) chip_render(c, sbuf[c], m, cps);   /* (about 22.3 cycles a sample) */
    for (i = 0; i < n; i++) {                   /* mono, as the chips are; -3 dB each (set by ear, not measured) */
        int32_t v;
        if (os == 2u) {                         /* [1 3 3 1] / 8 over the half steps, then every other one */
            int32_t a = hist[0], b = hist[1];
            hist[0] = sbuf[0][2u * i] + sbuf[1][2u * i];
            hist[1] = sbuf[0][2u * i + 1u] + sbuf[1][2u * i + 1u];
            v = (a + 3 * b + 3 * hist[0] + hist[1]) >> 3;
        } else v = sbuf[0][i] + sbuf[1][i];
        v = v * 181 >> 8;
        if (!settle) {
            out[2u * i] += v;
            out[2u * i + 1u] += v;
        }
    }
    if (settle) settle--;
    dt = fm1_ticks() - t0;
    if (dt > load_ticks) load_ticks = dt;
}

/* ---- patches: bytes only, so the same on the FM-1, the host and (later) the page. A C64 music player's instrument,
 * run 50 times a second (a PAL frame): the SID's ADSR and waveform, a wavetable (a waveform and a note a frame),
 * a pulse width sweep, a filter sweep and vibrato */
#define WT_MAX 8u
enum { PF_FILTER = 1, PF_HARD = 2 };            /* through the filter; hard restart */
typedef struct {
    char name[12];
    uint8_t ad, sr;                             /* the SID's attack/decay and sustain/release registers */
    uint8_t wave;                               /* the control register without the gate: waveform, test, ring, sync */
    uint8_t flags;
    uint8_t pw_hi, pw_lo;                       /* pulse width at the note's start, 12 bits */
    int8_t pw_speed;                            /* added each frame, turning back at pw_min and pw_max (x 16) */
    uint8_t pw_min, pw_max;
    uint8_t mode;                               /* filter: register 0x18's bits 6-4 (HP BP LP) */
    uint8_t res;                                /* 0..15 */
    uint8_t cut;                                /* cutoff at the note's start: the top 8 of 11 bits (register 0x16) */
    int8_t cut_speed;                           /* added to the 11-bit cutoff each frame, until cut_end (x 8) */
    uint8_t cut_end;
    int8_t transpose, fine;                     /* semitones, cents */
    uint8_t vib_delay, vib_speed, vib_depth;    /* frames; a cycle is 256 / speed frames; cents */
    uint8_t wt_len, wt_loop;                    /* steps (0: none); the step after the last (>= wt_len: hold the last) */
    uint8_t wt_abs;                             /* bit k: step k's note is a MIDI note, not an offset from the key's */
    uint8_t wt_ctl[WT_MAX];                     /* a step's waveform (0: the patch's) */
    uint8_t wt_note[WT_MAX];                    /* semitones (signed), or a MIDI note */
} patch_t;

#define NSLOT 128u                              /* 4 banks of 32 */
#define BANK_SLOTS 32u
static patch_t lib[NSLOT];

/* parts: up to six sounds, each a library slot on a MIDI channel, sharing the six voices. PARTS: a part plays as many
 * notes as there are voices, and parts on one channel sound together (a layer). VOICES: part k is voice k on MIDI
 * channel k, one note at a time, as a tune's own voices. The keys, the knobs and the page play and edit one part */
enum { MODE_PARTS, MODE_VOICES, NMODE };
#define NPART 6u
#define CHAN_OFF 16u
#define CHAN_ALL 17u                            /* channels 1 to 9 and 11 to 15 (10 is the sample kit's, 16 the page's) */
typedef struct {
    patch_t p;                                  /* its sound: its slot's, as the knobs and the page left it */
    uint8_t slot, chan, changed, bend_range;    /* changed: since its slot; bend_range: semitones (RPN 0) */
    int16_t bend;                               /* -8192..8191 */
} part_t;
static part_t pt[NPART];
static uint32_t ed_part, midi_mode;
#define cur (pt[ed_part].p)                     /* the edited part's patch, slot and edited mark */
#define cur_slot (pt[ed_part].slot)
#define edited (pt[ed_part].changed)

/* the factory patches, in bank 1 while the library is empty. Drums take absolute notes, so any key plays them */
#define WT(...) {__VA_ARGS__}
static const patch_t FACTORY[] = {
    {"PWM LEAD", 0x08, 0xA9, 0x40, PF_HARD, 0x08, 0x00, 24, 0x10, 0xE0, 0x10, 0, 0, 0, 0, 0, 0, 12, 20, 25,
     0, 0, 0, WT(0), WT(0)},
    {"SAW BASS", 0x08, 0x8A, 0x20, PF_FILTER | PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 10, 0x30, -6, 0x10, -12, 0,
     0, 0, 0, 0, 0, 0, WT(0), WT(0)},
    {"BRASS", 0x28, 0xA8, 0x20, PF_FILTER | PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 4, 0x10, 12, 0x80, 0, 0,
     18, 18, 20, 0, 0, 0, WT(0), WT(0)},
    {"SWEEP PAD", 0x8A, 0xCA, 0x40, PF_FILTER, 0x04, 0x00, 6, 0x20, 0xC0, 0x10, 12, 0x08, 2, 0xA0, 0, 0,
     0, 0, 0, 0, 0, 0, WT(0), WT(0)},
    {"FLUTE", 0x48, 0xA8, 0x10, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 20, 16, 30,
     0, 0, 0, WT(0), WT(0)},
    {"ORGAN", 0x08, 0xF6, 0x40, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     0, 0, 0, WT(0), WT(0)},
    {"BELL", 0x09, 0x09, 0x50, 0, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 12, 0, 0, 0, 0,
     0, 0, 0, WT(0), WT(0)},
    {"THIN BASS", 0x09, 0x6A, 0x40, PF_HARD, 0x02, 0x00, 4, 0x02, 0x30, 0x10, 0, 0, 0, 0, -12, 0, 0, 0, 0,
     0, 0, 0, WT(0), WT(0)},
    {"NOISE LEAD", 0x08, 0xA9, 0x40, PF_HARD, 0x06, 0x00, 16, 0x20, 0xD0, 0x10, 0, 0, 0, 0, 0, 0, 10, 24, 20,
     2, 1, 0, WT(0x81, 0), WT(24, 0)},          /* a frame of noise two octaves up, then the pulse: Hubbard's way */
    {"ARP MAJOR", 0x08, 0xA9, 0x40, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     3, 0, 0, WT(0, 0, 0), WT(0, 4, 7)},        /* a chord from one voice, a note a frame */
    {"ARP MINOR", 0x08, 0xA9, 0x40, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     3, 0, 0, WT(0, 0, 0), WT(0, 3, 7)},
    {"KICK", 0x08, 0x00, 0x11, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     6, 6, 0x3F, WT(0x81, 0x11, 0x11, 0x11, 0x11, 0x11), WT(84, 52, 45, 40, 36, 33)},
    {"SNARE", 0x09, 0x00, 0x81, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     4, 4, 0x0F, WT(0x81, 0x41, 0x41, 0x81), WT(96, 50, 47, 90)},
    {"HIHAT", 0x05, 0x00, 0x81, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     1, 1, 0x01, WT(0x81), WT(106)},
    {"TOM", 0x0A, 0x00, 0x11, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     6, 6, 0, WT(0x81, 0x11, 0x11, 0x11, 0x11, 0x11), WT(24, 0, (uint8_t)-2, (uint8_t)-4, (uint8_t)-6, (uint8_t)-8)},
    {"ZAP", 0x09, 0x00, 0x40, PF_HARD, 0x08, 0x00, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0,
     8, 8, 0, WT(0, 0, 0, 0, 0, 0, 0, 0), WT(36, 30, 24, 18, 12, 6, 0, (uint8_t)-6)},
};
#define NFACTORY (sizeof FACTORY / sizeof FACTORY[0])

static uint32_t slot_used(uint32_t i) { return i < NSLOT && lib[i].name[0] != 0; }

static int32_t slot_step(uint32_t from, int32_t turn)   /* the next used slot that way (-1: none) */
{
    uint32_t i = from, k;
    int32_t step = turn > 0 ? 1 : -1;
    for (; turn; turn -= step) {
        for (k = 0; k < NSLOT; k++) {
            i = (uint32_t)((int32_t)i + step + (int32_t)NSLOT) % NSLOT;
            if (slot_used(i)) break;
        }
        if (k == NSLOT) return -1;
    }
    return (int32_t)i;
}

/* ---- the library in flash, read back at boot: [header][lib], the header written last, so a save cut short reads
 * as none. ver changes with the layout: an old store is then ignored, not misread */
#ifndef STORE_AT                                /* (platform.c has them; the host build does not) */
#define STORE_AT 0x000A7000u
#define STORE_LEN (16u * 1024u)
#define KIT_AT 0x000AB000u
#define KIT_LEN (20u * 1024u)
#endif
#define STORE_MAGIC 0x31444953u                 /* "SID1" */
#define STORE_VER ((uint32_t)sizeof(patch_t) | 1u << 24)
typedef struct { uint32_t magic, ver, len, crc; } store_head_t;

#ifdef SID1_HOST                              /* host: the flash as a RAM array (one run) */
static uint8_t host_flash[KIT_AT + KIT_LEN - STORE_AT];
static int store_erase(uint32_t off) { uint32_t i; for (i = 0; i < 0x1000u; i++) host_flash[off - STORE_AT + i] = 0xFF; return 0; }
static int store_write(uint32_t off, const void *p, uint32_t n) { const uint8_t *b = p; while (n--) host_flash[off++ - STORE_AT] &= *b++; return 0; }
static int store_read(uint32_t off, void *p, uint32_t n) { uint8_t *b = p; while (n--) *b++ = host_flash[off++ - STORE_AT]; return 0; }
#else
static int store_erase(uint32_t off) { uint32_t took; return fl_erase4k(off, &took); }
static int store_write(uint32_t off, const void *p, uint32_t n) { return fl_write(off, p, n); }
static int store_read(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
#endif

static uint32_t crc(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t c = 0xFFFFFFFFu, k;
    while (n--) {
        c ^= *b++;
        for (k = 0; k < 8u; k++) c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static uint32_t store_save(void)                /* 0 kept; 4 erase failed; 5 write failed */
{
    store_head_t h = {STORE_MAGIC, STORE_VER, sizeof lib, 0};
    uint32_t off;
    h.crc = crc(lib, sizeof lib);
    for (off = 0; off < sizeof h + sizeof lib; off += 0x1000u) {
        platform_poll();                        /* (USB and the watchdog between the erases) */
        if (store_erase(STORE_AT + off)) return 4;
    }
    if (store_write(STORE_AT + sizeof h, lib, sizeof lib)) return 5;
    return store_write(STORE_AT, &h, sizeof h) ? 5u : 0u;
}

static void store_load(void)                    /* at boot: what was kept if it is whole, else the factory patches */
{
    store_head_t h;
    uint32_t i;
    if (!store_read(STORE_AT, &h, sizeof h) && h.magic == STORE_MAGIC && h.ver == STORE_VER && h.len == sizeof lib &&
        !store_read(STORE_AT + sizeof h, lib, sizeof lib) && crc(lib, sizeof lib) == h.crc) {
        for (i = 0; i < NSLOT; i++) lib[i].name[11] = 0;
        return;
    }
    memset(lib, 0, sizeof lib);
    for (i = 0; i < NFACTORY; i++) lib[i] = FACTORY[i];
}

/* ---- the player (main loop): six voices, three a chip, each a key's note under the current patch */
static const uint16_t FREQ[108] = {             /* MIDI note -> frequency register at the PAL clock: f * 2^24 / 985248 */
    139, 147, 156, 166, 175, 186, 197, 209, 221, 234, 248, 263, 278, 295, 313, 331,
    351, 372, 394, 417, 442, 468, 496, 526, 557, 590, 625, 662, 702, 743, 788, 834,
    884, 937, 992, 1051, 1114, 1180, 1250, 1325, 1403, 1487, 1575, 1669, 1768, 1873, 1985, 2103,
    2228, 2360, 2500, 2649, 2807, 2973, 3150, 3338, 3536, 3746, 3969, 4205, 4455, 4720, 5001, 5298,
    5613, 5947, 6300, 6675, 7072, 7493, 7938, 8410, 8910, 9440, 10001, 10596, 11226, 11894, 12601, 13350,
    14144, 14985, 15876, 16820, 17820, 18880, 20003, 21192, 22452, 23787, 25202, 26700, 28288, 29970, 31752, 33640,
    35641, 37760, 40005, 42384, 44904, 47574, 50403, 53401, 56576, 59940, 63504, 65535,
};

static const struct { const char *name; uint8_t ctl; } WAVES[] = {
    {"TRIANGLE", 0x10}, {"SAW", 0x20}, {"PULSE", 0x40}, {"NOISE", 0x80},
    {"SAW+TRI", 0x30}, {"PULSE+TRI", 0x50}, {"PULSE+SAW", 0x60}, {"P+S+T", 0x70},
};
#define NWAVES (sizeof WAVES / sizeof WAVES[0])

static uint32_t snd_model = SID_6581;          /* both chips */

#define FRAME_MS 20u                            /* 50 Hz, as a PAL C64's player */

typedef struct {
    uint8_t note, on, active, hr, part;         /* active: gated or releasing; hr: starts at the next frame */
    uint8_t pos, vib_phase;                     /* wavetable step; vibrato phase (256 a cycle) */
    uint16_t frames, pw;                        /* since the note's start; pulse width now */
    int16_t pw_step;
    uint32_t t;                                 /* fm1_ms at its last note on or off */
    int32_t ctl, freq, pwr;                     /* what the chip has (-1: unknown) */
} voice_t;
static voice_t vc[6];

static struct {
    uint32_t part;                              /* whose filter it is: the part of its last filtered note (NPART: none) */
    int32_t cut, cutr;                          /* the 11-bit cutoff now, and as the chip has it (-1: unknown) */
    int32_t reg17, reg18;                       /* as the chip has them (-1: unknown) */
} flt[2];

static void voice_reg(uint32_t v, uint32_t reg, uint32_t val) { q(v / 3u, (v % 3u) * 7u + reg, val); }

static uint32_t pitch(const voice_t *v)         /* the frequency register for the voice's step */
{
    const part_t *t = &pt[v->part];
    const patch_t *p = &t->p;
    int32_t c, n, frac;
    if (p->wt_len && (p->wt_abs >> v->pos & 1u)) c = (int32_t)p->wt_note[v->pos] * 100 + p->fine;
    else {
        c = ((int32_t)v->note + p->transpose + (p->wt_len ? (int8_t)p->wt_note[v->pos] : 0)) * 100 + p->fine;
        if (p->vib_depth && v->frames >= p->vib_delay) {   /* a triangle, -depth..depth */
            int32_t ph = v->vib_phase, tri = ph < 128 ? ph - 64 : 191 - ph;
            c += tri * p->vib_depth / 64;
        }
        c += t->bend * t->bend_range * 100 / 8192;
    }
    if (c < 0) c = 0;
    if (c > 106 * 100 + 99) c = 106 * 100 + 99;
    n = c / 100;
    frac = c % 100;
    return FREQ[n] + (uint32_t)(((int32_t)FREQ[n + 1] - FREQ[n]) * frac / 100);
}

static void voice_update(uint32_t k)            /* the voice's registers as its state says, the changed ones only */
{
    voice_t *v = &vc[k];
    const patch_t *p = &pt[v->part].p;
    /* a step's waveform, with the patch's test, ring and sync bits */
    int32_t ctl = ((p->wt_len && p->wt_ctl[v->pos] ? p->wt_ctl[v->pos] : p->wave) & 0xF0) | (p->wave & 0x0E);
    int32_t f = (int32_t)pitch(v);
    ctl |= v->on;
    if (f != v->freq) { voice_reg(k, 0, (uint32_t)f & 0xFFu); voice_reg(k, 1, (uint32_t)f >> 8); v->freq = f; }
    if (v->pw != v->pwr) { voice_reg(k, 2, v->pw & 0xFFu); voice_reg(k, 3, v->pw >> 8); v->pwr = v->pw; }
    if (ctl != v->ctl) { voice_reg(k, 4, (uint32_t)ctl); v->ctl = ctl; }   /* (last: a new waveform at its pitch) */
}

static void filter_update(uint32_t c)           /* its part's resonance and mode; a voice through it if its own patch is */
{
    const patch_t *p = &pt[flt[c].part < NPART ? flt[c].part : 0u].p;
    uint32_t k, route = 0;
    int32_t r17, r18 = (p->mode & 0x70) | 15;
    for (k = 0; k < 3u; k++) if (pt[vc[c * 3u + k].part].p.flags & PF_FILTER) route |= 1u << k;
    r17 = (int32_t)((uint32_t)p->res << 4 | route);
    if (flt[c].cut != flt[c].cutr) {
        q(c, 0x15, (uint32_t)flt[c].cut & 7u);
        q(c, 0x16, (uint32_t)flt[c].cut >> 3);
        flt[c].cutr = flt[c].cut;
    }
    if (r17 != flt[c].reg17) { q(c, 0x17, (uint32_t)r17); flt[c].reg17 = r17; }
    if (r18 != flt[c].reg18) { q(c, 0x18, (uint32_t)r18); flt[c].reg18 = r18; }
}

static void voice_start(uint32_t k)
{
    voice_reg(k, 5, pt[vc[k].part].p.ad);
    voice_reg(k, 6, pt[vc[k].part].p.sr);
    vc[k].ctl = -1;                             /* (the gate again, even if the waveform is the same) */
    voice_update(k);
}

static void voice_on(uint32_t k, uint32_t part, uint32_t note)   /* voice k plays note in part's sound */
{
    voice_t *v = &vc[k];
    const patch_t *p = &pt[part].p;
    v->part = (uint8_t)part;
    v->note = (uint8_t)note;
    v->pos = 0;
    v->frames = 0;
    v->vib_phase = 64;                          /* (the triangle's zero, rising) */
    v->pw = (uint16_t)((p->pw_hi & 15u) << 8 | p->pw_lo);
    v->pw_step = p->pw_speed;
    v->t = fm1_ms;
    v->on = 1;
    if (p->flags & PF_FILTER) {                 /* the chip's filter becomes the part's, its sweep started again */
        flt[k / 3u].part = part;
        flt[k / 3u].cut = p->cut << 3;
    }
    filter_update(k / 3u);
    if ((p->flags & PF_HARD) && v->active) {    /* hard restart: a frame silent, the envelope at 0 and the oscillator
                                                   held, so the next attack is not lost to the ADSR delay bug */
        voice_reg(k, 5, 0);
        voice_reg(k, 6, 0);
        voice_reg(k, 4, 0x08);
        v->ctl = 0x08;
        v->hr = 1;
    } else {
        v->hr = 0;
        voice_start(k);
    }
    v->active = 1;
}

static void voice_off(uint32_t k)
{
    vc[k].on = 0;
    vc[k].t = fm1_ms;
    if (!vc[k].hr) voice_update(k);
}

static void frame(void)                         /* a player frame: wavetables, sweeps, vibrato */
{
    uint32_t k, c;
#ifdef SID1_HOST
    if (host_sid_log) printf("%6u ms  FRAME\n", fm1_ms);
#endif
    for (k = 0; k < 6u; k++) {
        voice_t *v = &vc[k];
        const patch_t *p = &pt[v->part].p;
        int32_t lo = p->pw_min * 16, hi = p->pw_max * 16;
        if (!v->active) continue;
        if (v->hr) { v->hr = 0; voice_start(k); continue; }
        v->frames++;
        if (p->wt_len) {
            if (++v->pos >= p->wt_len) v->pos = (uint8_t)(p->wt_loop < p->wt_len ? p->wt_loop : p->wt_len - 1u);
        } else v->pos = 0;
        if (v->pw_step) {                       /* turn back at the limits */
            int32_t w = (int32_t)v->pw + v->pw_step;
            if (w >= hi || w > 4095) { w = hi < 4095 ? hi : 4095; v->pw_step = (int16_t)-(v->pw_step < 0 ? -v->pw_step : v->pw_step); }
            if (w <= lo || w < 0) { w = lo > 0 ? lo : 0; v->pw_step = (int16_t)(v->pw_step < 0 ? -v->pw_step : v->pw_step); }
            v->pw = (uint16_t)w;
        }
        if (p->vib_depth && v->frames >= p->vib_delay) v->vib_phase = (uint8_t)(v->vib_phase + p->vib_speed);
        voice_update(k);
        if (!v->on && v->frames > 2000u) v->active = 0;   /* (40 s after its start and released: let it be) */
    }
    for (c = 0; c < 2u; c++) {                  /* the filter sweeps to its part's cut_end and stays there */
        const patch_t *p;
        int32_t end, s;
        if (flt[c].part >= NPART) continue;
        p = &pt[flt[c].part].p;
        end = p->cut_end << 3;
        s = p->cut_speed;
        if (s > 0 && flt[c].cut < end) flt[c].cut = flt[c].cut + s < end ? flt[c].cut + s : end;
        if (s < 0 && flt[c].cut > end) flt[c].cut = flt[c].cut + s > end ? flt[c].cut + s : end;
        filter_update(c);
    }
}

/* ---- the parts: notes to voices. A filtered part wants a chip whose filter no other filtered part is using */
static void out16(uint32_t cc, uint32_t v);

static uint32_t voice_pick(uint32_t part, uint32_t note)   /* the voice for a new note: the cheapest, then the oldest */
{
    uint32_t k, j, best = 0, cost, best_cost = 99, filt = pt[part].p.flags & PF_FILTER;
    for (k = 0; k < 6u; k++) {
        const voice_t *v = &vc[k];
        uint32_t c = k / 3u, clash = 0, other = 0;
        if (v->on && v->part == part && v->note == note) return k;   /* (the same note again: its voice) */
        if (filt) {
            for (j = c * 3u; j < c * 3u + 3u; j++)
                if (vc[j].on && vc[j].part != part && pt[vc[j].part].p.flags & PF_FILTER) clash = 1;
            other = flt[c].part < NPART && flt[c].part != part && pt[flt[c].part].p.flags & PF_FILTER;
        }
        /* free; free where another part's filter was; this part's note; sharing a filter; another part's note */
        cost = other + (v->on ? (v->part == part ? 4u : 8u) : 0u) + clash * 5u;
        if (cost < best_cost || (cost == best_cost && (int32_t)(v->t - vc[best].t) < 0)) { best = k; best_cost = cost; }
    }
    return best;
}

static void part_note_on(uint32_t p, uint32_t note)
{
    if (note > 127u) return;
    voice_on(midi_mode == MODE_VOICES ? p : voice_pick(p, note), p, note);
}

static void part_note_off(uint32_t p, uint32_t note)
{
    uint32_t k;
    for (k = 0; k < 6u; k++) if (vc[k].on && vc[k].part == p && vc[k].note == note) voice_off(k);
}

static void part_quiet(uint32_t p)              /* (p NPART: every part) */
{
    uint32_t k;
    for (k = 0; k < 6u; k++) if (vc[k].on && (p == NPART || vc[k].part == p)) voice_off(k);
}

static void part_bend(uint32_t p, int32_t bend)
{
    uint32_t k;
    pt[p].bend = (int16_t)bend;
    for (k = 0; k < 6u; k++) if (vc[k].active && vc[k].part == p && !vc[k].hr) voice_update(k);
}

static void part_filter(uint32_t p, int restart)   /* the chips sounding part p through a filter take its filter */
{
    uint32_t c, k;
    for (c = 0; c < 2u; c++) {
        int here = flt[c].part == p;
        for (k = c * 3u; k < c * 3u + 3u; k++)
            if (vc[k].active && vc[k].part == p && pt[p].p.flags & PF_FILTER) here = 1;
        if (!here) continue;
        flt[c].part = p;
        if (restart) flt[c].cut = pt[p].p.cut << 3;
        filter_update(c);
    }
}

static void part_use(uint32_t n, const patch_t *p, uint32_t slot)   /* part n plays p, as slot's */
{
    uint32_t k;
    if (pt[n].slot != slot) out16(0x28 + n, slot);
    pt[n].p = *p;
    pt[n].slot = (uint8_t)slot;
    pt[n].changed = 0;
    part_filter(n, 1);
    for (k = 0; k < 6u; k++)
        if (vc[k].active && vc[k].part == n && !vc[k].hr) { voice_reg(k, 5, p->ad); voice_reg(k, 6, p->sr); }
}

static void part_load(uint32_t n, uint32_t slot) { if (slot_used(slot)) part_use(n, &lib[slot], slot); }

/* the sample kit: channel 10's notes 36 to 51, on chip 1 (a tune's own voices are there, as the samples moved
 * them on a C64). From the page (docs/MIDI.md 2.6), kept at KIT_AT; until then a test kit, made at boot */
#define KIT_CHAN 9u
#define KIT_CHIP 0u
static void kit_play(uint32_t note) { if (note >= 36u && note - 36u < NKIT && kit[note - 36u].len) q(WQ_DIG0 + KIT_CHIP, note - 36u, 0); }

static void kit_test(void)                      /* 36 a kick, 38 a snare, 42 a hat: 8 kHz, about the middle value (8) */
{
    static const int8_t SIN[64] = {0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126, 127, 126, 125, 122,
        117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12, 0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112,
        -117, -122, -125, -126, -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12};
    /* per drum: note, values, decay a value (Q15), the tone's start and end (Hz) and its fall (Q15), noise 0..1 (Q8) */
    static const struct { uint8_t note; uint16_t len, decay, f0, f1, fall, noise; } D[] = {
        {36, 2400, 32734, 160, 50, 32630, 0}, {38, 1600, 32620, 190, 180, 32760, 200}, {42, 800, 32620, 0, 0, 32000, 256}};
    uint32_t d, i, at = 0, rnd = 1;
    memset(kit, 0, sizeof kit);
    for (d = 0; d < sizeof D / sizeof D[0]; d++) {
        kit_t *k = &kit[D[d].note - 36u];
        int32_t env = 32767, sweep = 32767;
        uint32_t ph = 0;
        k->off = (uint16_t)at; k->len = D[d].len;
        for (i = 0; i < D[d].len; i++, at++) {
            int32_t f = (int32_t)D[d].f1 + (((int32_t)D[d].f0 - D[d].f1) * sweep >> 15), tone, x, v;
            ph += (uint32_t)f * 65536u * 64u / 8000u;
            rnd = rnd * 1103515245u + 12345u;
            tone = SIN[ph >> 16 & 63u];
            x = (tone * (256 - (int32_t)D[d].noise) + ((int32_t)(rnd >> 24) - 128) * (int32_t)D[d].noise) >> 8;
            v = 8 + ((x * env >> 15) * 7 + 63) / 127;
            if (i < 40u) v = 15 + (v - 15) * (int32_t)i / 40;   /* from and back to the volume the voices use: no click */
            if (D[d].len - i <= 40u) v = 15 + (v - 15) * (int32_t)(D[d].len - i) / 40;
            v = v < 0 ? 0 : v > 15 ? 15 : v;
            smp[at] = (uint16_t)((uint32_t)v << 12 | 123u);   /* (985248 / 8000) */
            env = env * D[d].decay >> 15;
            sweep = sweep * D[d].fall >> 15;
        }
    }
}

#define KIT_MAGIC 0x4B444953u                   /* "SIDK" */
typedef struct { uint32_t magic, used, crc; kit_t k[NKIT]; } kit_head_t;
static uint32_t kit_used, kit_dirty;            /* smp's writes taken; changed since kept */

static uint32_t kit_save(void)                  /* 0 kept (or nothing to keep); 4 erase failed; 5 write failed */
{
    kit_head_t h;
    uint32_t off;
    if (!kit_dirty) return 0;
    h.magic = KIT_MAGIC; h.used = kit_used; h.crc = crc(smp, kit_used * 2u);
    memcpy(h.k, kit, sizeof kit);
    for (off = 0; off < sizeof h + kit_used * 2u; off += 0x1000u) {
        platform_poll();
        if (store_erase(KIT_AT + off)) return 4;
    }
    if (kit_used && store_write(KIT_AT + sizeof h, smp, kit_used * 2u)) return 5;
    if (store_write(KIT_AT, &h, sizeof h)) return 5;
    kit_dirty = 0;
    return 0;
}

static void kit_load(void)                      /* what was kept if it is whole, else the test kit */
{
    kit_head_t h;
    uint32_t k;
    if (!store_read(KIT_AT, &h, sizeof h) && h.magic == KIT_MAGIC && h.used <= NSMP &&
        !store_read(KIT_AT + sizeof h, smp, h.used * 2u) && crc(smp, h.used * 2u) == h.crc) {
        for (k = 0; k < NKIT && (uint32_t)h.k[k].off + h.k[k].len <= h.used; k++) ;
        if (k == NKIT) { memcpy(kit, h.k, sizeof kit); kit_used = h.used; kit_dirty = 0; return; }
    }
    kit_test();
    kit_used = 0;
    for (k = 0; k < NKIT; k++) if ((uint32_t)kit[k].off + kit[k].len > kit_used) kit_used = (uint32_t)kit[k].off + kit[k].len;
    kit_dirty = 0;
}

static uint32_t rpn[16];                        /* a channel's RPN (CC 101, 100); 0 is the bend range */

static void parts_midi(uint32_t ch, uint32_t type, uint32_t d1, uint32_t d2)   /* a channel message, to its parts */
{
    uint32_t p;
    if (type == 0xB0u && d1 == 101u) rpn[ch] = (rpn[ch] & 0x7Fu) | d2 << 7;
    if (type == 0xB0u && d1 == 100u) rpn[ch] = (rpn[ch] & ~0x7Fu) | d2;
    if (ch == KIT_CHAN && type == 0x90u && d2) kit_play(d1);
    for (p = 0; p < NPART; p++) {
        if (ch == 15u ? p != ed_part :          /* (channel 16 is the page's: the edited part, as the keys) */
            midi_mode == MODE_VOICES ? p != ch : pt[p].chan != ch && (pt[p].chan != CHAN_ALL || ch == KIT_CHAN)) continue;
        if (type == 0x90u && d2) part_note_on(p, d1);
        else if (type == 0x80u || type == 0x90u) part_note_off(p, d1);
        else if (type == 0xC0u) part_load(p, d1);
        else if (type == 0xE0u) part_bend(p, (int32_t)(d1 | d2 << 7) - 8192);
        else if (type == 0xB0u && d1 == 6u && rpn[ch] == 0u) pt[p].bend_range = (uint8_t)(d2 > 24u ? 24u : d2);
        else if (type == 0xB0u && d1 == 123u) part_quiet(p);
    }
}

/* the setup by what it does, as the page names it: SINGLE part 1 alone on All; LAYER parts stacked on All; SPLIT
 * parts each on a channel of its own; MIXED anything else */
enum { SET_SINGLE, SET_LAYER, SET_SPLIT, SET_MIXED };
static uint32_t setup_kind(void)
{
    uint32_t p, all = 0, ch = 0;
    for (p = 0; p < NPART; p++) {
        if (pt[p].chan == CHAN_ALL) all++;
        else if (pt[p].chan != CHAN_OFF) ch++;
    }
    if (!ch && all == 1u && pt[0].chan == CHAN_ALL) return SET_SINGLE;
    if (!ch && all) return SET_LAYER;
    if (!all && ch) return SET_SPLIT;
    return SET_MIXED;
}

/* kept in the store's last sector, apart from the library (OPN-1 has the settings sector) */
#define PARTS_AT (STORE_AT + STORE_LEN - 0x1000u)
#define PARTS_MAGIC 0x50444953u                 /* "SIDP" */
typedef struct { uint8_t mode, ed, pad[2]; struct { uint8_t slot, chan, bend_range, pad; } p[NPART]; } parts_keep_t;

static uint32_t parts_save(void)                /* 0 kept; 4 erase failed; 5 write failed */
{
    parts_keep_t s;
    uint32_t h[2], p;
    memset(&s, 0, sizeof s);
    s.mode = (uint8_t)midi_mode;
    s.ed = (uint8_t)ed_part;
    for (p = 0; p < NPART; p++) { s.p[p].slot = pt[p].slot; s.p[p].chan = pt[p].chan; s.p[p].bend_range = pt[p].bend_range; }
    h[0] = PARTS_MAGIC;
    h[1] = crc(&s, sizeof s);
    if (store_erase(PARTS_AT)) return 4;
    if (store_write(PARTS_AT + sizeof h, &s, sizeof s)) return 5;
    return store_write(PARTS_AT, h, sizeof h) ? 5u : 0u;
}

static void parts_init(void)                    /* what was kept if it is whole, else part 1 on All; sounds from slots */
{
    parts_keep_t s;
    uint32_t h[2], p, first = slot_used(0) ? 0u : (uint32_t)(slot_step(0, 1) < 0 ? 0 : slot_step(0, 1));
    int kept = !store_read(PARTS_AT, h, sizeof h) && h[0] == PARTS_MAGIC && !store_read(PARTS_AT + sizeof h, &s, sizeof s) &&
               crc(&s, sizeof s) == h[1] && s.mode < NMODE && s.ed < NPART;
    midi_mode = kept ? s.mode : MODE_PARTS;
    ed_part = kept ? s.ed : 0u;
    for (p = 0; p < NPART; p++) {
        part_t *t = &pt[p];
        t->chan = (uint8_t)(p ? CHAN_OFF : CHAN_ALL);
        t->bend_range = 2;
        t->bend = 0;
        t->slot = (uint8_t)first;
        if (kept && s.p[p].chan <= CHAN_ALL && s.p[p].bend_range <= 24u && slot_used(s.p[p].slot)) {
            t->slot = s.p[p].slot; t->chan = s.p[p].chan; t->bend_range = s.p[p].bend_range;
        }
        part_use(p, &lib[t->slot], t->slot);
    }
}

/* ---- the pages: a button each (again: its second page), four knobs each, ALGORITHM as the page says */
enum { PG_SOUND, PG_ENV, PG_EDIT, PG_PULSE, PG_FILTER, PG_VIB, PG_WT, PG_WT2, PG_GLO, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"SOUND", "ENVELOPE", "SETUP", "PULSE", "FILTER", "VIBRATO",
                                             "WAVETABLE", "WAVE LOOP", "GLOBAL"};
typedef struct { const char *name; int16_t lo, hi, step; } knob_t;   /* step: a detent's worth */
static const knob_t KNOB[NPAGE][4] = {
    {{"CUTOFF", 0, 255, 2}, {"RES", 0, 15, 1}, {"PW", 0, 4095, 32}, {"RELEASE", 0, 15, 1}},
    {{"ATTACK", 0, 15, 1}, {"DECAY", 0, 15, 1}, {"SUSTAIN", 0, 15, 1}, {"RELEASE", 0, 15, 1}},
    {{"TRANSP", -36, 36, 1}, {"FINE", -50, 50, 1}, {"FILTER", 0, 1, 1}, {"EXTRA", 0, 7, 1}},
    {{"SPEED", -127, 127, 1}, {"MIN", 0, 255, 2}, {"MAX", 0, 255, 2}, {"START", 0, 4095, 32}},
    {{"CUTOFF", 0, 255, 2}, {"SPEED", -127, 127, 1}, {"END", 0, 255, 2}, {"RES", 0, 15, 1}},
    {{"DELAY", 0, 255, 1}, {"SPEED", 0, 255, 1}, {"DEPTH", 0, 255, 2}, {"", 0, 0, 0}},
    {{"STEP", 1, WT_MAX, 1}, {"NOTE", -48, 107, 1}, {"ABS", 0, 1, 1}, {"LENGTH", 0, WT_MAX, 1}},
    {{"STEP", 1, WT_MAX, 1}, {"NOTE", -48, 107, 1}, {"ABS", 0, 1, 1}, {"LOOP", 0, WT_MAX, 1}},
    {{"MODEL", 0, 1, 1}, {"2X", 0, 1, 1}, {"PART", 1, NPART, 1}, {"MODE", 0, NMODE - 1, 1}},
};
static uint32_t page, wt_sel;                   /* wt_sel: the wavetable step the knobs edit */
static const char *const FILT_NAME[8] = {"NONE", "LP", "BP", "LP+BP", "HP", "NOTCH", "BP+HP", "ALL"};
static const char *const EXTRA_NAME[8] = {"-", "SYNC", "RING", "SY+RI", "HARD", "HD+SY", "HD+RI", "ALL"};

static int32_t knob_get(uint32_t pg, uint32_t k)
{
    const patch_t *p = &cur;
    uint32_t s = wt_sel;
    switch (pg * 4u + k) {
    case PG_SOUND * 4 + 0: case PG_FILTER * 4 + 0: return p->cut;
    case PG_SOUND * 4 + 1: case PG_FILTER * 4 + 3: return p->res;
    case PG_SOUND * 4 + 2: case PG_PULSE * 4 + 3: return (p->pw_hi & 15) << 8 | p->pw_lo;
    case PG_SOUND * 4 + 3: case PG_ENV * 4 + 3: return p->sr & 15;
    case PG_ENV * 4 + 0: return p->ad >> 4;
    case PG_ENV * 4 + 1: return p->ad & 15;
    case PG_ENV * 4 + 2: return p->sr >> 4;
    case PG_EDIT * 4 + 0: return p->transpose;
    case PG_EDIT * 4 + 1: return p->fine;
    case PG_EDIT * 4 + 2: return p->flags & PF_FILTER ? 1 : 0;
    case PG_EDIT * 4 + 3: return (p->wave >> 1 & 3) | (p->flags & PF_HARD ? 4 : 0);   /* bits: sync, ring, hard */
    case PG_PULSE * 4 + 0: return p->pw_speed;
    case PG_PULSE * 4 + 1: return p->pw_min;
    case PG_PULSE * 4 + 2: return p->pw_max;
    case PG_FILTER * 4 + 1: return p->cut_speed;
    case PG_FILTER * 4 + 2: return p->cut_end;
    case PG_VIB * 4 + 0: return p->vib_delay;
    case PG_VIB * 4 + 1: return p->vib_speed;
    case PG_VIB * 4 + 2: return p->vib_depth;
    case PG_WT * 4 + 0: case PG_WT2 * 4 + 0: return (int32_t)s + 1;
    case PG_WT * 4 + 1: case PG_WT2 * 4 + 1: return p->wt_abs >> s & 1u ? p->wt_note[s] : (int8_t)p->wt_note[s];
    case PG_WT * 4 + 2: case PG_WT2 * 4 + 2: return p->wt_abs >> s & 1u;
    case PG_WT * 4 + 3: return p->wt_len;
    case PG_WT2 * 4 + 3: return p->wt_loop < p->wt_len ? p->wt_loop : WT_MAX;
    case PG_GLO * 4 + 0: return snd_model == SID_8580;
    case PG_GLO * 4 + 1: return (int32_t)oversample;
    case PG_GLO * 4 + 2: return (int32_t)ed_part + 1;
    case PG_GLO * 4 + 3: return (int32_t)midi_mode;
    }
    return 0;
}

static void adsr_live(void)                     /* the part's voices sounding take a new ADSR at once */
{
    uint32_t k;
    for (k = 0; k < 6u; k++)
        if (vc[k].active && vc[k].part == ed_part && !vc[k].hr) { voice_reg(k, 5, cur.ad); voice_reg(k, 6, cur.sr); }
}

static void knob_set(uint32_t pg, uint32_t k, int32_t v)   /* to the patch, and what sounds now if it shows at once */
{
    patch_t *p = &cur;
    uint32_t s = wt_sel, i;
    switch (pg * 4u + k) {
    case PG_SOUND * 4 + 0: case PG_FILTER * 4 + 0:
        p->cut = (uint8_t)v;
        part_filter(ed_part, 1);
        break;
    case PG_SOUND * 4 + 1: case PG_FILTER * 4 + 3: p->res = (uint8_t)v; part_filter(ed_part, 0); break;
    case PG_SOUND * 4 + 2: case PG_PULSE * 4 + 3:
        p->pw_hi = (uint8_t)(v >> 8); p->pw_lo = (uint8_t)v;
        for (i = 0; i < 6u; i++) if (vc[i].part == ed_part) vc[i].pw = (uint16_t)v;
        break;
    case PG_SOUND * 4 + 3: case PG_ENV * 4 + 3: p->sr = (uint8_t)((p->sr & 0xF0) | v); adsr_live(); break;
    case PG_ENV * 4 + 0: p->ad = (uint8_t)((p->ad & 0x0F) | v << 4); adsr_live(); break;
    case PG_ENV * 4 + 1: p->ad = (uint8_t)((p->ad & 0xF0) | v); adsr_live(); break;
    case PG_ENV * 4 + 2: p->sr = (uint8_t)((p->sr & 0x0F) | v << 4); adsr_live(); break;
    case PG_EDIT * 4 + 0: p->transpose = (int8_t)v; break;
    case PG_EDIT * 4 + 1: p->fine = (int8_t)v; break;
    case PG_EDIT * 4 + 2:
        p->flags = (uint8_t)((p->flags & ~PF_FILTER) | (v ? PF_FILTER : 0));
        part_filter(ed_part, 0);
        for (i = 0; i < 2u; i++) filter_update(i);   /* (which voices go through it) */
        break;
    case PG_EDIT * 4 + 3:
        p->wave = (uint8_t)((p->wave & ~6u) | (v & 3) << 1);
        p->flags = (uint8_t)((p->flags & ~PF_HARD) | (v & 4 ? PF_HARD : 0));
        break;
    case PG_PULSE * 4 + 0:
        p->pw_speed = (int8_t)v;
        for (i = 0; i < 6u; i++) if (vc[i].part == ed_part) vc[i].pw_step = (int16_t)v;
        break;
    case PG_PULSE * 4 + 1: p->pw_min = (uint8_t)v; break;
    case PG_PULSE * 4 + 2: p->pw_max = (uint8_t)v; break;
    case PG_FILTER * 4 + 1: p->cut_speed = (int8_t)v; break;
    case PG_FILTER * 4 + 2: p->cut_end = (uint8_t)v; break;
    case PG_VIB * 4 + 0: p->vib_delay = (uint8_t)v; break;
    case PG_VIB * 4 + 1: p->vib_speed = (uint8_t)v; break;
    case PG_VIB * 4 + 2: p->vib_depth = (uint8_t)v; break;
    case PG_WT * 4 + 0: case PG_WT2 * 4 + 0: wt_sel = (uint32_t)v - 1u; break;
    case PG_WT * 4 + 1: case PG_WT2 * 4 + 1:
        if (p->wt_abs >> s & 1u) { if (v < 0) v = 0; }
        else if (v > 48) v = 48;
        p->wt_note[s] = (uint8_t)v;
        break;
    case PG_WT * 4 + 2: case PG_WT2 * 4 + 2:   /* the same note either way: middle C (60) is relative 0 */
        if (v) {
            int32_t n = 60 + (int8_t)p->wt_note[s];
            p->wt_abs |= (uint8_t)(1u << s);
            p->wt_note[s] = (uint8_t)(n > 107 ? 107 : n);
        } else {
            int32_t n = (int32_t)p->wt_note[s] - 60;
            p->wt_abs &= (uint8_t)~(1u << s);
            p->wt_note[s] = (uint8_t)(n < -48 ? -48 : n);
        }
        break;
    case PG_WT * 4 + 3: p->wt_len = (uint8_t)v; break;
    case PG_WT2 * 4 + 3: p->wt_loop = (uint8_t)v; break;
    case PG_GLO * 4 + 0: snd_model = v ? SID_8580 : SID_6581; q(WQ_MODEL, 0, snd_model); break;
    case PG_GLO * 4 + 1: oversample = (uint32_t)v; break;
    case PG_GLO * 4 + 2: ed_part = (uint32_t)v - 1u; break;
    case PG_GLO * 4 + 3: part_quiet(NPART); midi_mode = (uint32_t)v; break;
    }
}

static int knob_edits_patch(uint32_t pg, uint32_t k) { return pg != PG_GLO && !((pg == PG_WT || pg == PG_WT2) && k == 0); }

/* ---- the screen: five bands, each drawn whole into the canvas and sent, one a loop, so MIDI never waits for a
 * whole screen. In the C64's own colours: blue, a light blue border */
#define C_BLUE RGB(124, 112, 218)               /* light blue: the border, labels */
#define C_BG RGB(53, 40, 121)                   /* blue: the screen */
#define C_SEL RGB(82, 68, 164)
#define C_BOX RGB(30, 22, 78)
#define C_LBL C_BLUE
#define C_CYAN RGB(112, 164, 178)
#define C_GREEN RGB(154, 210, 132)
#define C_YELLOW RGB(201, 212, 135)
#define C_ORANGE RGB(196, 108, 113)
#define C_MUTE RGB(100, 88, 186)
static const uint16_t KNOB_C[4] = {C_BLUE, C_CYAN, C_GREEN, C_YELLOW};
static const uint8_t BAND_Y[6] = {0, 22, 86, 150, 214, 240};
#define BANDS_ALL 0x1Fu
#define BAND_FOOT 0x10u
static uint32_t dirty = BANDS_ALL;
static int32_t ui_oct;
static uint32_t load_us;

static const char *msg;                         /* a word for a moment in the header (SAVED) */
static uint32_t msg_until;

static char *put_str(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *put_int(char *d, int32_t v, int plus)
{
    char t[12];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *d++ = '-';
    else if (plus && v > 0) *d++ = '+';
    do { t[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}
static char *put_note(char *d, uint32_t n)      /* MIDI 60 = C4 */
{
    static const char *const N[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    d = put_str(d, N[n % 12u]);
    return put_int(d, (int32_t)n / 12 - 1, 0);
}
static void text_r(int32_t x1, int32_t y, const char *s, uint16_t c) { cv_text(x1 - text_w(&FONT_S, s), y, &FONT_S, s, c); }
static void text_c(int32_t x, int32_t w, int32_t y, const char *s, uint16_t c) { cv_text(x + (w - text_w(&FONT_S, s)) / 2, y, &FONT_S, s, c); }

static const char *wave_name(uint32_t ctl)
{
    uint32_t i;
    for (i = 0; i < NWAVES; i++) if (WAVES[i].ctl == (ctl & 0xF0u)) return WAVES[i].name;
    return "NONE";
}

static const uint16_t ATTACK_MS[16] = {2, 8, 16, 24, 38, 56, 68, 80, 100, 250, 500, 800, 1000, 3000, 5000, 8000};

static char *put_ms(char *d, uint32_t ms)      /* "250", "1.5S" */
{
    if (ms < 1000u) return put_int(d, (int32_t)ms, 0);
    d = put_int(d, (int32_t)(ms / 1000u), 0);
    if (ms < 10000u) { *d++ = '.'; *d++ = (char)('0' + ms / 100u % 10u); }
    *d++ = 'S'; *d = 0;
    return d;
}

static void knob_text(uint32_t pg, uint32_t k, int32_t v, char *s)
{
    switch (pg * 4u + k) {
    case PG_ENV * 4 + 0: put_ms(s, ATTACK_MS[v]); return;
    case PG_ENV * 4 + 1: case PG_ENV * 4 + 3: case PG_SOUND * 4 + 3: put_ms(s, ATTACK_MS[v] * 3u); return;   /* (decay and release: 3x) */
    case PG_EDIT * 4 + 0: case PG_EDIT * 4 + 1: case PG_PULSE * 4 + 0: case PG_FILTER * 4 + 1: put_int(s, v, 1); return;
    case PG_EDIT * 4 + 2: put_str(s, v ? "ON" : "OFF"); return;
    case PG_EDIT * 4 + 3: put_str(s, EXTRA_NAME[v & 7]); return;
    case PG_VIB * 4 + 0: put_ms(s, (uint32_t)v * FRAME_MS); return;
    case PG_VIB * 4 + 1: {                      /* Hz, a decimal: 50 frames a second, 256 / speed frames a cycle */
        uint32_t c = (uint32_t)v * 500u / 256u;
        char *e = put_int(s, (int32_t)(c / 10u), 0);
        *e++ = '.'; *e++ = (char)('0' + c % 10u); *e = 0;
        return;
    }
    case PG_WT * 4 + 1: case PG_WT2 * 4 + 1:
        if (cur.wt_abs >> wt_sel & 1u) put_note(s, (uint32_t)v); else put_int(s, v, 1);
        return;
    case PG_WT * 4 + 2: case PG_WT2 * 4 + 2: put_str(s, v ? "ABS" : "REL"); return;
    case PG_WT2 * 4 + 3: if (v >= cur.wt_len) { put_str(s, "HOLD"); return; } put_int(s, v + 1, 0); return;
    case PG_GLO * 4 + 0: put_str(s, v ? "8580" : "6581"); return;
    case PG_GLO * 4 + 1: put_str(s, v ? "ON" : "OFF"); return;
    case PG_GLO * 4 + 3: put_str(s, v ? "VOICES" : "PARTS"); return;
    }
    put_int(s, v, 0);
}

static void draw_head(void)
{
    char s[24], *p = s;
    if (msg && (int32_t)(msg_until - fm1_ms) > 0) { cv_text(6, 3, &FONT_S, msg, C_ORANGE); }
    else {
        uint32_t k;
        msg = 0;
        *p++ = 'B'; *p++ = (char)('1' + cur_slot / BANK_SLOTS); *p++ = ' ';
        *p++ = (char)('0' + (cur_slot % BANK_SLOTS + 1u) / 10u); *p++ = (char)('0' + (cur_slot % BANK_SLOTS + 1u) % 10u);
        *p++ = ' ';
        for (k = 0; k < 11u && cur.name[k]; k++) *p++ = cur.name[k];
        if (edited) *p++ = '*';
        *p = 0;
        cv_text(6, 3, &FONT_S, s, C_WHITE);
    }
    text_r(234, 3, PAGE_NAME[page], C_BLUE);
    cv_rect(0, 19, 240, 2, C_BLUE);
}

static int32_t wave_at(uint32_t ctl, uint32_t ph, uint32_t pw, uint32_t *rnd)   /* 0..4095 at phase ph (12 bits) */
{
    uint32_t v = 4095u, any = 0;
    if (ctl & 0x80u) { *rnd = *rnd * 1103515245u + 12345u; return (int32_t)(*rnd >> 20); }
    if (ctl & 0x10u) { v &= ph < 2048u ? ph * 2u : (4095u - ph) * 2u; any = 1; }   /* (combined: a sketch, ANDed) */
    if (ctl & 0x20u) { v &= ph; any = 1; }
    if (ctl & 0x40u) { v &= ph >= pw ? 4095u : 0u; any = 1; }
    return any ? (int32_t)v : 2048;
}

static void draw_wave(uint32_t ctl, int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c)   /* two cycles */
{
    uint32_t rnd = 1, pw = (uint32_t)knob_get(PG_SOUND, 2);
    int32_t i, py = 0;
    for (i = 0; i < w; i++) {
        int32_t v = wave_at(ctl, (uint32_t)(i * 8192 / w) & 4095u, pw, &rnd), yy = y + h - 1 - v * (h - 1) / 4095;
        if (ctl & 0x80u && i % 3) yy = py;      /* (noise: a step every few pixels) */
        if (i) cv_line(x + i - 1, py, x + i, yy, c);
        py = yy;
    }
}

static void draw_sound(void)                    /* the waveform, and what the patch does */
{
    char s[24], *p;
    draw_wave(cur.wave, 12, 40, 116, 64, C_GREEN);
    cv_text(10, 118, &FONT_S, wave_name(cur.wave), C_WHITE);
    p = put_str(s, cur.flags & PF_FILTER ? FILT_NAME[cur.mode >> 4 & 7u] : "UNFILTERED");
    cv_text(142, 34, &FONT_S, s, cur.flags & PF_FILTER ? C_CYAN : C_LBL);
    p = put_str(s, "RES "); put_int(p, cur.res, 0);
    cv_text(142, 52, &FONT_S, s, C_LBL);
    if (cur.wt_len) cv_text(142, 70, &FONT_S, "WAVETBL", C_YELLOW);
    if (cur.pw_speed) cv_text(142, 88, &FONT_S, "PWM", C_YELLOW);
    if (cur.vib_depth) cv_text(190, 88, &FONT_S, "VIB", C_YELLOW);
    p = put_str(s, EXTRA_NAME[knob_get(PG_EDIT, 3)]);
    if (s[0] != '-') cv_text(142, 106, &FONT_S, s, C_ORANGE);
}

static void draw_env_page(void)                 /* attack up, decay to sustain, held, release: time as the index */
{
    int32_t a = cur.ad >> 4, d = cur.ad & 15, su = cur.sr >> 4, r = cur.sr & 15;
    int32_t x0 = 12, bot = 140, top = 34, x1, x2, x3, x4, sy = bot - su * (bot - top) / 15;
    x1 = x0 + 4 + a * 3;
    x2 = x1 + 4 + d * 4;
    x3 = x2 + 50;
    x4 = x3 + 4 + r * 4;
    cv_line(x0, bot, x1, top, C_BLUE); cv_line(x0, bot - 1, x1, top - 1, C_BLUE);
    cv_line(x1, top, x2, sy, C_CYAN); cv_line(x1, top - 1, x2, sy - 1, C_CYAN);
    cv_line(x2, sy, x3, sy, C_GREEN); cv_line(x2, sy - 1, x3, sy - 1, C_GREEN);
    cv_line(x3, sy, x4, bot, C_YELLOW); cv_line(x3, sy - 1, x4, bot - 1, C_YELLOW);
    {
        int32_t i;
        for (i = top; i <= bot; i += 3) cv_pset(x3, i, C_LBL);
    }
}

static void draw_setup(void)
{
    char s[24], *p;
    p = put_str(s, "WAVE  "); put_str(p, wave_name(cur.wave));
    cv_text(12, 32, &FONT_S, s, C_LBL);
    p = put_str(s, "FILTER  "); put_str(p, FILT_NAME[cur.mode >> 4 & 7u]);
    cv_text(12, 52, &FONT_S, s, cur.flags & PF_FILTER ? C_CYAN : C_LBL);
    cv_text(12, 72, &FONT_S, cur.flags & PF_FILTER ? "THIS PATCH: THROUGH IT" : "THIS PATCH: AROUND IT", C_LBL);
    cv_text(12, 100, &FONT_S, "ALGORITHM: FILTER MODE", C_MUTE);
    cv_text(12, 118, &FONT_S, "EXTRA: SYNC RING HARD", C_MUTE);
}

static void draw_curve(uint32_t what, int32_t x, int32_t y, int32_t w, int32_t h)   /* 2 s of a sweep, as the player runs it */
{
    int32_t f, v = 0, step = 0, lo = 0, hi = 0, py = 0, top = 0;
    uint8_t ph = 64;
    if (what == PG_PULSE) { v = knob_get(PG_SOUND, 2); step = cur.pw_speed; lo = cur.pw_min * 16; hi = cur.pw_max * 16; top = 4095; }
    else if (what == PG_FILTER) { v = cur.cut << 3; step = cur.cut_speed; lo = hi = cur.cut_end << 3; top = 2047; }
    else top = 256;
    cv_rect(x, y, w, 1, C_BOX);
    cv_rect(x, y + h - 1, w, 1, C_BOX);
    for (f = 0; f < 100; f++) {
        int32_t val, xx = x + f * w / 100, yy;
        if (what == PG_VIB) {
            int32_t tri = ph < 128 ? ph - 64 : 191 - ph;
            val = f >= cur.vib_delay ? 128 + tri * 2 * cur.vib_depth / 255 : 128;
            if (f >= cur.vib_delay) ph = (uint8_t)(ph + cur.vib_speed);
        } else {
            val = v;
            if (what == PG_PULSE && step) {     /* (as frame() turns it at the limits) */
                v += step;
                if (v >= hi || v > 4095) { v = hi < 4095 ? hi : 4095; step = -(step < 0 ? -step : step); }
                if (v <= lo || v < 0) { v = lo > 0 ? lo : 0; step = step < 0 ? -step : step; }
            } else if (what == PG_FILTER) {
                if (step > 0 && v < hi) v = v + step < hi ? v + step : hi;
                if (step < 0 && v > hi) v = v + step > hi ? v + step : hi;
            }
        }
        yy = y + h - 1 - val * (h - 1) / top;
        if (f) cv_line(x + (f - 1) * w / 100, py, xx, yy, what == PG_FILTER ? C_CYAN : what == PG_VIB ? C_GREEN : C_YELLOW);
        py = yy;
    }
}

static void draw_wt(void)                       /* the steps in a row: waveform, note; the knobs' outlined */
{
    static const char *const W[16] = {"PAT", "TRI", "SAW", "S+T", "PUL", "P+T", "P+S", "PST",
                                      "NOI", "N+T", "N+S", "NST", "N+P", "NPT", "NPS", "ALL"};
    uint32_t i;
    for (i = 0; i < WT_MAX; i++) {
        int32_t x = 4 + (int32_t)i * 29, on = i < cur.wt_len;
        uint16_t c = on ? C_WHITE : C_MUTE;
        char s[8];
        if (i == wt_sel) cv_rect(x, 30, 27, 72, C_SEL);
        if (i == wt_sel) { cv_rect(x, 30, 27, 1, C_BLUE); cv_rect(x, 101, 27, 1, C_BLUE); }
        put_int(s, (int32_t)i + 1, 0);
        text_c(x, 27, 32, s, C_LBL);
        text_c(x, 27, 54, W[cur.wt_ctl[i] >> 4], c);
        if (cur.wt_abs >> i & 1u) put_note(s, cur.wt_note[i]);
        else put_int(s, (int8_t)cur.wt_note[i], 1);
        text_c(x - 2, 31, 76, s, cur.wt_abs >> i & 1u ? (on ? C_YELLOW : C_MUTE) : c);
    }
    if (cur.wt_len) {                           /* where it goes after the last step */
        char s[24], *p = put_str(s, "THEN ");
        if (cur.wt_loop < cur.wt_len) { p = put_str(p, "BACK TO "); put_int(p, cur.wt_loop + 1, 0); }
        else put_str(p, "HOLD THE LAST");
        cv_text(6, 112, &FONT_S, s, C_LBL);
    } else cv_text(6, 112, &FONT_S, "OFF: THE PATCH'S WAVE", C_LBL);
    cv_text(6, 130, &FONT_S, "SEL: NEXT STEP", C_MUTE);
}

static void draw_glo(void)                      /* the setup, a part a line (the edited one white); the load */
{
    static const char *const SETUP_NAME[4] = {"SINGLE", "LAYER", "SPLIT", "MIXED"};
    char s[24], *p;
    uint32_t n, k;
    cv_text(12, 26, &FONT_S, midi_mode == MODE_VOICES ? "VOICES" : SETUP_NAME[setup_kind()], C_BLUE);
    p = put_str(s, "LOAD "); p = put_int(p, (int32_t)load_us, 0); p = put_str(p, "US ");
    p = put_int(p, (int32_t)(load_us * 100u / 5805u), 0);
    put_str(p, "%");
    text_r(228, 26, s, C_MUTE);
    for (n = 0; n < NPART; n++) {
        const part_t *t = &pt[n];
        uint32_t ch = midi_mode == MODE_VOICES ? n : t->chan;
        p = s;
        *p++ = (char)('1' + n);
        p = put_str(p, "  ");
        if (ch == CHAN_ALL) p = put_str(p, "ALL");
        else if (ch == CHAN_OFF) p = put_str(p, "OFF");
        else { p = put_str(p, "CH "); p = put_int(p, (int32_t)ch + 1, 0); }
        cv_text(12, 46 + 16 * (int32_t)n, &FONT_S, s, n == ed_part ? C_WHITE : ch == CHAN_OFF ? C_MUTE : C_LBL);
        p = s;
        for (k = 0; k < 11u && t->p.name[k]; k++) *p++ = t->p.name[k];
        if (t->changed) *p++ = '*';
        *p = 0;
        cv_text(96, 46 + 16 * (int32_t)n, &FONT_S, s, n == ed_part ? C_WHITE : ch == CHAN_OFF ? C_MUTE : C_LBL);
    }
}

static const int8_t ARC[46][2] = {
    {-8, 8}, {-9, 8}, {-10, 7}, {-11, 5}, {-11, 4}, {-12, 3}, {-12, 2}, {-12, 1}, {-12, -1}, {-12, -2}, {-12, -3},
    {-11, -4}, {-11, -5}, {-10, -7}, {-9, -8}, {-8, -8}, {-8, -9}, {-7, -10}, {-5, -11}, {-4, -11}, {-3, -12},
    {-2, -12}, {-1, -12}, {1, -12}, {2, -12}, {3, -12}, {4, -11}, {5, -11}, {7, -10}, {8, -9}, {8, -8}, {9, -8},
    {10, -7}, {11, -5}, {11, -4}, {12, -3}, {12, -2}, {12, -1}, {12, 1}, {12, 2}, {12, 3}, {11, 4}, {11, 5},
    {10, 7}, {9, 8}, {8, 8}};

static void draw_knobs(void)                    /* the page's four: an arc, the value, the name */
{
    uint32_t k, i;
    char s[12];
    for (k = 0; k < 4u; k++) {
        const knob_t *kn = &KNOB[page][k];
        int32_t v, cx = 30 + 60 * (int32_t)k, cy = 166;
        uint32_t lit;
        if (!kn->name[0]) continue;
        v = knob_get(page, k);
        lit = (uint32_t)((v - kn->lo) * 45 / (kn->hi - kn->lo) + 1);
        for (i = 0; i < 46u; i++) cv_rect(cx + ARC[i][0] - 1, cy + ARC[i][1] - 1, 3, 3, i < lit ? KNOB_C[k] : C_BOX);
        knob_text(page, k, v, s);
        text_c(cx - 30, 60, 178, s, C_WHITE);
        text_c(cx - 30, 60, 195, kn->name, C_LBL);
    }
}

static void draw_foot(void)
{
    char s[16], *p = s;
    uint32_t v;
    cv_rect(0, 214, 240, 1, C_BLUE);
    p = put_str(p, "OCT "); *p++ = ui_oct < 0 ? '-' : '+'; *p++ = (char)('0' + (ui_oct < 0 ? -ui_oct : ui_oct)); *p = 0;
    cv_text(6, 220, &FONT_S, s, C_LBL);
    for (v = 0; v < 6u; v++)                    /* the voices: gated, releasing, free */
        cv_rect(92 + 9 * (int32_t)v + (v >= 3u ? 4 : 0), 224, 7, 7, vc[v].on ? C_GREEN : vc[v].active ? C_SEL : C_BOX);
    p = put_str(s, snd_model == SID_8580 ? "8580" : "6581");
    if (oversample) put_str(p, " 2X");
    text_r(234, 220, s, C_LBL);
}

static void draw_scene(void)                    /* everything; the canvas keeps only its band */
{
    cv_rect(0, 0, 240, 3, C_BLUE);              /* the border, as the C64's */
    cv_rect(0, 237, 240, 3, C_BLUE);
    cv_rect(0, 0, 3, 240, C_BLUE);
    cv_rect(237, 0, 3, 240, C_BLUE);
    draw_head();
    switch (page) {
    case PG_SOUND: draw_sound(); break;
    case PG_ENV: draw_env_page(); break;
    case PG_EDIT: draw_setup(); break;
    case PG_PULSE: case PG_FILTER: case PG_VIB: draw_curve(page, 12, 34, 216, 104); break;
    case PG_WT: case PG_WT2: draw_wt(); break;
    default: draw_glo(); break;
    }
    draw_knobs();
    draw_foot();
}

static void draw_band(void)                     /* the first dirty band, if any */
{
    uint32_t b;
    for (b = 0; b < 5u && !(dirty >> b & 1u); b++) ;
    if (b == 5u) return;
    dirty &= ~(1u << b);
    cv_begin(240, BAND_Y[b + 1u] - BAND_Y[b], C_BG);
    cv_oy = -(int32_t)BAND_Y[b];
    draw_scene();
    cv_oy = 0;
    cv_blit(0, BAND_Y[b]);
}

/* ---- the page (web/index.html): CCs on channel 16, as docs/MIDI.md has them. A patch travels as its 50 bytes,
 * each as CC 52 (low 7 bits) and 53 (the top bit), between a begin and a checksum (54, 55: the bytes' sum mod 2^14) */
#define APP_ID 2u                               /* CC 1B: which firmware (OPN-1 sends none: 1) */
static struct { uint8_t on, kind, target, hi_due, sum_lo; uint32_t n, sum; uint8_t b[sizeof(patch_t)]; } rx;
enum { RX_SLOT = 1, RX_PLAY };
static patch_t told;                            /* the patch playing as the page last knew it (from it, or 3C) */
static uint32_t told_slot = NSLOT;

static void out16(uint32_t cc, uint32_t v)      /* a CC to the page, paced: USB's queue holds 64 and drops the rest */
{
    static uint32_t n;
    if (++n % 32u == 0u) { uint32_t t = fm1_ms; while (fm1_ms - t < 2u) platform_poll(); }
    midi_send(0xBF, cc, v & 0x7Fu);
}

static void patch_send(uint32_t begin, uint32_t s, const patch_t *p)   /* a dump: begin s, the bytes, the sum */
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t k, sum = 0;
    out16(begin, s);
    for (k = 0; k < sizeof(patch_t); k++) { out16(0x32, b[k]); out16(0x33, b[k] >> 7); sum += b[k]; }
    out16(0x34, sum);
    out16(0x35, sum >> 7);
}

static void version_send(void)                  /* 1B the firmware, then APP_VER as 1C major, 1D minor, 1E patch
                                                   (1C 127: a DEV build) */
{
    const char *s = APP_VER;
    uint32_t n[3] = {127u, 0, 0}, k = 0;
    if (*s >= '0' && *s <= '9')
        for (n[0] = 0; *s && k < 3u; s++) {
            if (*s == '.') k++;
            else if (*s >= '0' && *s <= '9') n[k] = n[k] * 10u + (uint32_t)(*s - '0');
            else break;
        }
    out16(0x1B, APP_ID);
    out16(0x1C, n[0]);
    out16(0x1D, n[1]);
    out16(0x1E, n[2]);
}

static void patch_sane(patch_t *p)              /* whatever came in, nothing the player can trip on */
{
    p->name[11] = 0;
    p->wave &= 0xFEu;
    p->flags &= PF_FILTER | PF_HARD;
    p->pw_hi &= 15u;
    p->mode &= 0x70u;
    p->res &= 15u;
    if (p->wt_len > WT_MAX) p->wt_len = WT_MAX;
}

static void rx_done(void)                       /* a frame's checksum is in: take it, or refuse it */
{
    patch_t p;
    uint32_t ok = rx.n == sizeof(patch_t) && !rx.hi_due;
    rx.on = 0;
    if (ok) {
        memcpy(&p, rx.b, sizeof p);
        patch_sane(&p);
    }
    if (rx.kind == RX_SLOT) {
        if (ok) {
            lib[rx.target] = p;
            if (rx.target == cur_slot && !memcmp(&cur, &p, sizeof p)) edited = 0;   /* (the page saved what plays) */
        }
        out16(ok ? 0x3E : 0x3F, rx.target);
    } else {
        if (ok) {
            part_use(ed_part, &p, rx.target);
            edited = 1;                         /* (the page's edit, not yet in the library) */
            told = cur;
            told_slot = cur_slot;
        }
        out16(ok ? 0x56 : 0x57, rx.target);
    }
    dirty = BANDS_ALL;
}

static uint32_t cfg_part;                       /* the part CCs 21-26 set */

/* a sample frame (docs/MIDI.md 2.6): 67 k its length, or 60 k a run of its writes from an index; 61 62 the number,
 * 63 64 a write each (value << 10 | cycles to the next, 14 bits), 65 66 the sum of the 7-bit values 61 to 64 */
static struct { uint8_t on, len, k, lo, lo_due, sum_lo; uint32_t at, n, sum; uint16_t e[128]; } kx;

static void kx_done(uint32_t sum)
{
    uint32_t ok = kx.on && !kx.lo_due && (sum & 0x3FFFu) == (kx.sum & 0x3FFFu) && kx.k < NKIT, i;
    kit_t *k = &kit[kx.k % NKIT];
    kx.on = 0;
    if (ok && kx.len) {                         /* (the kit fills in order: after a clear, each sample's length first) */
        ok = !kx.n && kx.at && kit_used + kx.at <= NSMP && !k->len;
        if (ok) { k->off = (uint16_t)kit_used; k->len = (uint16_t)kx.at; kit_used += kx.at; }
    } else if (ok) {
        ok = kx.at + kx.n <= k->len;
        for (i = 0; ok && i < kx.n; i++) smp[k->off + kx.at + i] = (uint16_t)((kx.e[i] >> 10) << 12 | (kx.e[i] & 1023u));
    }
    if (ok) kit_dirty = 1;
    out16(ok ? 0x6A : 0x6B, kx.k);
}

static void part_send(uint32_t p)               /* 20 p, 21 its channel, 25 its slot, 26 the mode, 2E the edited part, 2F p */
{
    out16(0x20, p);
    out16(0x21, pt[p].chan);
    out16(0x25, pt[p].slot);
    out16(0x26, midi_mode);
    out16(0x2E, ed_part);
    out16(0x2F, p);
}

static int page_cc(uint32_t cc, uint32_t v)     /* 1: a channel 16 CC that was the page's */
{
    switch (cc) {
    case 0x20: if (v < NPART) cfg_part = v; return 1;
    case 0x21:
        if (v <= CHAN_ALL && v != pt[cfg_part].chan) { part_quiet(NPART); pt[cfg_part].chan = (uint8_t)v; }
        out16(0x2F, cfg_part);
        dirty = BANDS_ALL;
        return 1;
    case 0x22: case 0x23: case 0x24: return 1;  /* (OPN-1's level, trigger and time: not SID-1's) */
    case 0x25: part_load(cfg_part, v); out16(0x2F, cfg_part); dirty = BANDS_ALL; return 1;
    case 0x26:
        if (v < NMODE && v != midi_mode) { part_quiet(NPART); midi_mode = v; }
        out16(0x2F, 0x7F);
        dirty = BANDS_ALL;
        return 1;
    case 0x27: if (v < NPART) part_send(v); return 1;
    case 0x68:                                  /* the kit emptied */
        memset(kit, 0, sizeof kit);
        kit_used = 0;
        kit_dirty = 1;
        out16(0x6C, 0);
        return 1;
    case 0x60: case 0x67:
        kx.on = 1; kx.len = cc == 0x67; kx.k = (uint8_t)v; kx.at = kx.n = kx.sum = 0; kx.lo_due = 0;
        return 1;
    case 0x61: kx.at = (kx.at & ~0x7Fu) | v; kx.sum += v; return 1;
    case 0x62: kx.at = (kx.at & 0x7Fu) | v << 7; kx.sum += v; return 1;
    case 0x63: if (kx.lo_due) kx.on = 0; kx.lo = (uint8_t)v; kx.lo_due = 1; kx.sum += v; return 1;
    case 0x64:
        if (!kx.lo_due || kx.n >= 128u) kx.on = 0;
        else kx.e[kx.n++] = (uint16_t)(kx.lo | v << 7);
        kx.lo_due = 0; kx.sum += v;
        return 1;
    case 0x65: kx.sum_lo = (uint8_t)v; return 1;
    case 0x66: kx_done(kx.sum_lo | v << 7); return 1;
    case 0x2E:                                  /* edit part v: the keys, knobs and current patch become its */
        if (v < NPART) ed_part = v;
        out16(0x2F, 0x7E);
        dirty = BANDS_ALL;
        return 1;
    case 0x1F: version_send(); return 1;
    case 0x5B: case 0x50:                       /* a patch begins: for slot v, or to play now as slot v's */
        rx.on = 1; rx.kind = cc == 0x5B ? RX_SLOT : RX_PLAY; rx.target = (uint8_t)v;
        rx.n = rx.sum = 0; rx.hi_due = 0;
        return 1;
    case 0x52:
        if (rx.on && !rx.hi_due && rx.n < sizeof rx.b) { rx.b[rx.n] = (uint8_t)v; rx.hi_due = 1; }
        else rx.n = sizeof rx.b + 1u;           /* (out of order: refused at the end) */
        return 1;
    case 0x53:
        if (rx.on && rx.hi_due) { rx.b[rx.n] |= (uint8_t)(v << 7); rx.sum += rx.b[rx.n]; rx.n++; rx.hi_due = 0; }
        return 1;
    case 0x54: rx.sum_lo = (uint8_t)v; return 1;
    case 0x55:
        if (rx.on) {
            if ((rx.sum_lo | v << 7) != (rx.sum & 0x3FFFu)) rx.n = sizeof rx.b + 1u;
            rx_done();
        }
        return 1;
    case 0x5C:                                  /* read slot v */
        if (v < NSLOT && slot_used(v)) patch_send(0x30, v, &lib[v]);
        else out16(0x36, v);
        return 1;
    case 0x5D:                                  /* read the patch playing (3C: changed on the FM-1) */
        patch_send(0x37, cur_slot, &cur);
        out16(0x38, edited);
        told = cur;
        told_slot = cur_slot;
        return 1;
    case 0x5E:                                  /* empty slot v */
        if (v < NSLOT) memset(&lib[v], 0, sizeof lib[v]);
        out16(0x3D, v);
        dirty = BANDS_ALL;
        return 1;
    case 0x4E: {                                /* the library and the parts to flash: 6E 4E at once, then 4D the outcome */
        uint32_t r;
        out16(0x6E, 0x4E);
        r = store_save();
        if (!r) r = parts_save();
        out16(0x4D, r ? r : kit_save());
        return 1;
    }
#ifdef SID1_HOST
    case 0x4C:                                  /* host tests only: as a reboot, the library and parts from the store alone */
        memset(lib, 0xAA, sizeof lib);
        memset(pt, 0xAA, sizeof pt);
        memset(smp, 0xAA, sizeof smp);
        store_load();
        parts_init();
        kit_load();
        out16(0x4C, 0);
        return 1;
#endif
    }
    return 0;
}

static void page_tell(void)                     /* the patch playing changed on the FM-1: CC 3C its slot, once */
{
    if (cur_slot == told_slot && !memcmp(&cur, &told, sizeof cur)) return;
    told = cur;
    told_slot = cur_slot;
    out16(0x3C, cur_slot);
}

static void app_main(void)
{
    static const uint8_t PAGE_BTN[][3] = {      /* a button, its page, its second page (again) */
        {BTN_HOME, PG_SOUND, PG_SOUND}, {BTN_ENV, PG_ENV, PG_ENV}, {BTN_EDIT, PG_EDIT, PG_EDIT},
        {BTN_FX, PG_PULSE, PG_FILTER}, {BTN_LFO, PG_VIB, PG_VIB}, {BTN_ARP, PG_WT, PG_WT2}, {BTN_GLO, PG_GLO, PG_GLO}};
    uint32_t prev = 0, k, t_load = 0, t_frame, t_tell = 0, shown_v = 0;
    int32_t oct = 0;
    uint8_t held[27] = {0};

    sid_init(&chip[0], SID_6581);
    sid_init(&chip[1], SID_6581);
    for (k = 0; k < 6u; k++) { vc[k].ctl = vc[k].freq = vc[k].pwr = -1; }
    for (k = 0; k < 2u; k++) { flt[k].part = NPART; flt[k].cutr = flt[k].reg17 = flt[k].reg18 = -1; }
    store_load();
    parts_init();
    kit_load();
    told = cur;
    told_slot = cur_slot;
    palette_set(0);
    msg = "SID-1 " APP_VER;                     /* (the version, a moment at power-on) */
    msg_until = fm1_ms + 2000u;
    t_frame = fm1_ms;

    for (;;) {
        uint32_t keys, down, up, b, rel, base, m;
        uint8_t st, d1, d2;
        int32_t w;
        platform_poll();

        base = (uint32_t)(53 + 12 * oct);       /* key 0 = F3 */
        keys = fm1_in.notes;
        down = keys & ~prev;
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { held[k] = (uint8_t)(base + k); part_note_on(ed_part, held[k]); }
            if (up >> k & 1u) part_note_off(ed_part, held[k]);
            fm1_led_key(KEY_LED(k), keys >> k & 1u);
        }

        b = fm1_input_edges(&rel);
        if (b & (1u << BTN_OCT_DOWN) && oct > -3) oct--;
        if (b & (1u << BTN_OCT_UP) && oct < 3) oct++;
        for (k = 0; k < sizeof PAGE_BTN / sizeof PAGE_BTN[0]; k++)
            if (b & (1u << PAGE_BTN[k][0])) page = page == PAGE_BTN[k][1] ? PAGE_BTN[k][2] : PAGE_BTN[k][1];
        if (b & (1u << BTN_SEL) && (page == PG_WT || page == PG_WT2)) wt_sel = (wt_sel + 1u) % WT_MAX;
        if (b & (1u << BTN_SAVE)) {             /* the patch as the knobs left it, over its slot, and the parts, kept */
            lib[cur_slot] = cur;
            msg = store_save() || parts_save() ? "SAVE FAILED" : "SAVED";
            msg_until = fm1_ms + 1500u;
            edited = 0;
        }
        if (b & ~(1u << BTN_OCT_DOWN | 1u << BTN_OCT_UP)) dirty = BANDS_ALL;
        if (oct != ui_oct) { ui_oct = oct; dirty |= BAND_FOOT; }

        w = fm1_enc_take(ENC_PRESETS);
        if (w) {
            int32_t s = slot_step(cur_slot, w);
            if (s >= 0) part_load(ed_part, (uint32_t)s);
            dirty = BANDS_ALL;
        }
        w = fm1_enc_take(ENC_ALGORITHM);        /* the filter mode on its pages, a step's waveform on the wavetable's,
                                                   else the patch's */
        if (w) {
            int32_t d = w > 0 ? 1 : -1;
            if (page == PG_EDIT || page == PG_FILTER) {
                cur.mode = (uint8_t)((((cur.mode >> 4) + (uint32_t)(d + 8)) & 7u) << 4);
                part_filter(ed_part, 0);
            } else if (page == PG_WT || page == PG_WT2) {
                cur.wt_ctl[wt_sel] = (uint8_t)((((cur.wt_ctl[wt_sel] >> 4) + (uint32_t)(d + 16)) & 15u) << 4);
            } else {
                uint32_t i;
                for (i = 0; i < NWAVES && WAVES[i].ctl != (cur.wave & 0xF0u); i++) ;
                i = (uint32_t)((int32_t)(i < NWAVES ? i : 0u) + d + (int32_t)NWAVES) % NWAVES;
                cur.wave = (uint8_t)((cur.wave & 0x0Eu) | WAVES[i].ctl);
            }
            edited = 1;
            dirty = BANDS_ALL;
        }
        for (k = 0; k < 4u; k++) {              /* the page's knobs */
            const knob_t *kn = &KNOB[page][k];
            int32_t t = fm1_enc_take(ENC_KNOB1 + k), v, nv;
            if (!t || !kn->name[0]) continue;
            v = knob_get(page, k);
            nv = v + t * kn->step;
            if (page == PG_WT || page == PG_WT2) {   /* (a note's range: by ABS; LOOP: up to HOLD) */
                if (k == 1u && !(cur.wt_abs >> wt_sel & 1u) && nv < -48) nv = -48;
            }
            nv = nv < kn->lo ? kn->lo : nv > kn->hi ? kn->hi : nv;
            if (nv == v) continue;
            knob_set(page, k, nv);
            if (knob_edits_patch(page, k)) edited = 1;
            dirty = BANDS_ALL;
        }

        while (midi_get(&st, &d1, &d2)) {
            uint32_t type = st & 0xF0u;
            if (st == 0xBFu && page_cc(d1, d2)) continue;
            parts_midi(st & 15u, type, d1, d2);
            if (type == 0xC0u) dirty = BANDS_ALL;
        }

        if ((int32_t)(fm1_ms - t_frame) >= (int32_t)FRAME_MS) {   /* (late: catch up, a frame at a time) */
            t_frame += FRAME_MS;
            frame();
        }

        fm1_led_key(BTN_OCT_DOWN, oct < 0);
        fm1_led_key(BTN_OCT_UP, oct > 0);
        fm1_led_key(BTN_HOME, page == PG_SOUND);
        fm1_led_key(BTN_ENV, page == PG_ENV);
        fm1_led_key(BTN_EDIT, page == PG_EDIT);
        fm1_led_key(BTN_FX, page == PG_PULSE || page == PG_FILTER);
        fm1_led_key(BTN_LFO, page == PG_VIB);
        fm1_led_key(BTN_ARP, page == PG_WT || page == PG_WT2);
        fm1_led_key(BTN_GLO, page == PG_GLO);
        fm1_led_key(BTN_SAVE, edited);
        for (m = 0, k = 0; k < 6u; k++) m |= (uint32_t)vc[k].on << k | (uint32_t)vc[k].active << (k + 8u);
        if (m != shown_v) { shown_v = m; dirty |= BAND_FOOT; }
        if (fm1_ms - t_tell >= 200u) { t_tell = fm1_ms; page_tell(); }
        if (fm1_ms - t_load >= 500u) {          /* the worst interrupt of the last half second; a message ending */
            t_load = fm1_ms;
            load_us = load_ticks / FM1_TICKS_PER_US;
            load_ticks = 0;
            if (page == PG_GLO) dirty |= 0x0Eu;
            if (msg && (int32_t)(msg_until - fm1_ms) <= 0) dirty |= 1u;
        }
        draw_band();
    }
}
