/* SPDX-License-Identifier: GPL-2.0-or-later */
/* MOS 6581 / 8580 SID: a C port of reSID 0.16 by Dag Lem, integer only. See sid.c. */
#ifndef SID_H
#define SID_H
#include <stdint.h>

enum { SID_6581, SID_8580 };
#define SID_CLOCK_PAL 985248u
#define SID_CLOCK_NTSC 1022727u
#define SID_CPS_PAL 1464154                    /* cycles per 44.1 kHz sample, Q16: (int)(985248 / 44100 * 65536 + 0.5) */
#define SID_CPS_NTSC 1519851                   /* (int)(1022727 / 44100 * 65536 + 0.5) */

typedef struct {
    uint32_t acc, shift, freq, pw, waveform, test, ring_mod, sync, msb_rising;
} sid_wave_t;

typedef struct {
    uint32_t rate_counter, rate_period, exp_counter, exp_period, counter, hold_zero;
    uint32_t attack, decay, sustain, release, gate, state;
} sid_env_t;

typedef struct {
    int model;
    sid_wave_t wave[3];
    sid_env_t env[3];
    int32_t wave_zero, voice_dc;
    /* filter */
    uint32_t fc, res, filt, voice3off, hp_bp_lp, vol;
    int32_t mixer_dc, vhp, vbp, vlp, vnf, w0_ceil_1, w0_ceil_dt, q1024;
    /* external filter */
    int32_t ext_lp, ext_hp, ext_vo;
    int32_t ext_in;
    uint32_t bus_value;
    int32_t bus_ttl;
    int32_t sample_offset;                      /* sid_render: the fraction of a cycle owed, Q16 */
} sid_t;

void sid_init(sid_t *s, int model);             /* reset, then the model */
void sid_set_model(sid_t *s, int model);
void sid_reset(sid_t *s);
void sid_write(sid_t *s, uint32_t reg, uint32_t val);
uint32_t sid_read(sid_t *s, uint32_t reg);
void sid_input(sid_t *s, int32_t sample);      /* EXT IN, 16 bits */
void sid_clock(sid_t *s);                       /* one cycle */
void sid_clock_n(sid_t *s, int32_t dt);         /* dt cycles at once, as reSID's clock(delta_t) */
int32_t sid_output(const sid_t *s);             /* 16 bits, as reSID's output() */
/* reSID's SAMPLE_FAST: up to n samples (one every cps / 65536 cycles) into out at a stride, while *dt cycles last.
 * Returns the samples made; *dt is what is left */
int sid_render(sid_t *s, int32_t *dt, int32_t cps, int16_t *out, int n, int stride);

#endif
