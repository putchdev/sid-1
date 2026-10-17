/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * MOS 6581 / 8580 SID: a C port of reSID 0.16 (Copyright (C) 2004 Dag Lem <resid@nimrod.no>), as kept in VICE's
 * tree. Integer only: the tables reSID builds in doubles at start-up are in sid_tables.h, written by reSID itself
 * (tests/sid_tables.cpp). tests/sid_compare.cpp checks this file against reSID, sample for sample.
 *
 * This program is free software; you can redistribute it and/or modify it under the terms of the GNU General
 * Public License as published by the Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version. This program is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */
#include "sid.h"
#include "sid_tables.h"

#ifndef SID_HOT
#define SID_HOT
#endif
#define INL static inline __attribute__((always_inline))

enum { ATTACK, DECAY_SUSTAIN, RELEASE };

static const uint16_t RATE_PERIOD[16] = {9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251};
static const uint8_t SUSTAIN_LEVEL[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                          0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};

/* voice k's sync source is voice SYNC_SRC[k], and it is the sync source of voice SYNC_DEST[k] */
static const uint8_t SYNC_SRC[3] = {2, 0, 1};
static const uint8_t SYNC_DEST[3] = {1, 2, 0};

/* ---- the waveform generator */

INL void wave_shift(sid_wave_t *w)
{
    uint32_t bit0 = ((w->shift >> 22) ^ (w->shift >> 17)) & 1u;
    w->shift = ((w->shift << 1) & 0x7fffffu) | bit0;
}

INL void wave_clock(sid_wave_t *w)
{
    uint32_t prev;
    if (w->test) return;
    prev = w->acc;
    w->acc = (w->acc + w->freq) & 0xffffffu;
    w->msb_rising = !(prev & 0x800000u) && (w->acc & 0x800000u);
    if (!(prev & 0x080000u) && (w->acc & 0x080000u)) wave_shift(w);
}

INL void wave_clock_n(sid_wave_t *w, int32_t dt)
{
    uint32_t prev, delta, period = 0x100000u;
    if (w->test) return;
    prev = w->acc;
    delta = (uint32_t)dt * w->freq;
    w->acc = (w->acc + delta) & 0xffffffu;
    w->msb_rising = !(prev & 0x800000u) && (w->acc & 0x800000u);
    /* the noise shifts each time bit 19 rises: every 2^20 added */
    while (delta) {
        if (delta < period) {
            period = delta;
            if (period <= 0x080000u) {
                if (((w->acc - period) & 0x080000u) || !(w->acc & 0x080000u)) break;
            } else {
                if (((w->acc - period) & 0x080000u) && !(w->acc & 0x080000u)) break;
            }
        }
        wave_shift(w);
        delta -= period;
    }
}

INL void wave_sync(sid_wave_t *wv, uint32_t k)
{
    sid_wave_t *w = &wv[k];
    /* a source synced itself on the cycle its MSB rises does not sync its destination */
    if (w->msb_rising && wv[SYNC_DEST[k]].sync && !(w->sync && wv[SYNC_SRC[k]].msb_rising))
        wv[SYNC_DEST[k]].acc = 0;
}

INL uint32_t wave_tri(const sid_wave_t *wv, uint32_t k)
{
    const sid_wave_t *w = &wv[k];
    uint32_t msb = (w->ring_mod ? w->acc ^ wv[SYNC_SRC[k]].acc : w->acc) & 0x800000u;
    return ((msb ? ~w->acc : w->acc) >> 11) & 0xfffu;
}

INL uint32_t wave_pulse(const sid_wave_t *w) { return (w->test || (w->acc >> 12) >= w->pw) ? 0xfffu : 0u; }

INL uint32_t wave_noise(const sid_wave_t *w)  /* shift register bits 22 20 16 13 11 7 4 2 -> output bits 11..4 */
{
    uint32_t r = w->shift;
    return (r >> 22 & 1u) << 11 | (r >> 20 & 1u) << 10 | (r >> 16 & 1u) << 9 | (r >> 13 & 1u) << 8 |
           (r >> 11 & 1u) << 7 | (r >> 7 & 1u) << 6 | (r >> 4 & 1u) << 5 | (r >> 2 & 1u) << 4;
}

INL uint32_t wave_out(const sid_t *s, uint32_t k)
{
    const sid_wave_t *w = &s->wave[k];
    const uint8_t (*t)[4096] = SID_WAVE[s->model];
    switch (w->waveform) {
    case 0x1: return wave_tri(s->wave, k);
    case 0x2: return w->acc >> 12;
    case 0x3: return (uint32_t)t[0][w->acc >> 12] << 4;
    case 0x4: return wave_pulse(w);
    case 0x5: return ((uint32_t)t[1][wave_tri(s->wave, k) >> 1] << 4) & wave_pulse(w);
    case 0x6: return ((uint32_t)t[2][w->acc >> 12] << 4) & wave_pulse(w);
    case 0x7: return ((uint32_t)t[3][w->acc >> 12] << 4) & wave_pulse(w);
    case 0x8: return wave_noise(w);
    default: return 0;                          /* (noise with another waveform: 0, as reSID 0.16) */
    }
}

/* ---- the envelope generator */

INL void env_step(sid_env_t *e)
{
    /* the first step of an attack also resets the exponential counter */
    if (e->state == ATTACK || ++e->exp_counter == e->exp_period) {
        e->exp_counter = 0;
        if (e->hold_zero) return;
        switch (e->state) {
        case ATTACK:
            e->counter = (e->counter + 1u) & 0xffu;
            if (e->counter == 0xffu) {
                e->state = DECAY_SUSTAIN;
                e->rate_period = RATE_PERIOD[e->decay];
            }
            break;
        case DECAY_SUSTAIN:
            if (e->counter != SUSTAIN_LEVEL[e->sustain]) e->counter--;
            break;
        default:
            e->counter = (e->counter - 1u) & 0xffu;
            break;
        }
        switch (e->counter) {
        case 0xff: e->exp_period = 1; break;
        case 0x5d: e->exp_period = 2; break;
        case 0x36: e->exp_period = 4; break;
        case 0x1a: e->exp_period = 8; break;
        case 0x0e: e->exp_period = 16; break;
        case 0x06: e->exp_period = 30; break;
        case 0x00: e->exp_period = 1; e->hold_zero = 1; break;   /* frozen at zero */
        }
    }
}

INL void env_clock(sid_env_t *e)
{
    /* the ADSR delay bug: a period set below the counter makes it count on, wrapping at 2^15 */
    if (++e->rate_counter & 0x8000u) e->rate_counter = (e->rate_counter + 1u) & 0x7fffu;
    if (e->rate_counter != e->rate_period) return;
    e->rate_counter = 0;
    env_step(e);
}

INL void env_clock_n(sid_env_t *e, int32_t dt)
{
    int32_t step = (int32_t)e->rate_period - (int32_t)e->rate_counter;
    if (step <= 0) step += 0x7fff;
    while (dt) {
        if (dt < step) {
            e->rate_counter += (uint32_t)dt;
            if (e->rate_counter & 0x8000u) e->rate_counter = (e->rate_counter + 1u) & 0x7fffu;
            return;
        }
        e->rate_counter = 0;
        dt -= step;
        /* (env_step, but a frozen counter still waits a whole period for its next step) */
        env_step(e);
        step = (int32_t)e->rate_period;
    }
}

/* ---- the filter and the external filter */

INL void filter_route(sid_t *s, int32_t v1, int32_t v2, int32_t v3, int32_t *vi)
{
    int32_t ve = s->ext_in >> 7, in = 0, nf = 0;
    v1 >>= 7;                                   /* 20 bits to 13 */
    v2 >>= 7;
    /* voice 3 off does not silence voice 3 when it goes through the filter */
    v3 = (s->voice3off && !(s->filt & 4u)) ? 0 : v3 >> 7;
    if (s->filt & 1u) in += v1; else nf += v1;
    if (s->filt & 2u) in += v2; else nf += v2;
    if (s->filt & 4u) in += v3; else nf += v3;
    if (s->filt & 8u) in += ve; else nf += ve;
    s->vnf = nf;
    *vi = in;
}

INL void filter_clock(sid_t *s, int32_t v1, int32_t v2, int32_t v3)
{
    int32_t vi, dbp, dlp;
    filter_route(s, v1, v2, v3, &vi);
    dbp = s->w0_ceil_1 * s->vhp >> 20;
    dlp = s->w0_ceil_1 * s->vbp >> 20;
    s->vbp -= dbp;
    s->vlp -= dlp;
    s->vhp = (s->vbp * s->q1024 >> 10) - s->vlp - vi;
}

INL void filter_clock_n(sid_t *s, int32_t dt, int32_t v1, int32_t v2, int32_t v3)
{
    int32_t vi, step = 8;                       /* the most cycles a step for the filter to stay stable */
    filter_route(s, v1, v2, v3, &vi);
    while (dt) {
        int32_t w0dt, dbp, dlp;
        if (dt < step) step = dt;
        w0dt = s->w0_ceil_dt * step >> 6;
        dbp = w0dt * s->vhp >> 14;
        dlp = w0dt * s->vbp >> 14;
        s->vbp -= dbp;
        s->vlp -= dlp;
        s->vhp = (s->vbp * s->q1024 >> 10) - s->vlp - vi;
        dt -= step;
    }
}

INL int32_t filter_out(const sid_t *s)
{
    int32_t vf = 0;
    if (s->hp_bp_lp & 1u) vf += s->vlp;
    if (s->hp_bp_lp & 2u) vf += s->vbp;
    if (s->hp_bp_lp & 4u) vf += s->vhp;
    return (s->vnf + vf + s->mixer_dc) * (int32_t)s->vol;
}

/* R = 10 kOhm, C = 1000 pF lowpass; 1 kOhm, 10 uF highpass: w0 = 1 / RC, times 1.048576 */
#define EXT_W0LP 104858
#define EXT_W0HP 105

INL void ext_clock(sid_t *s, int32_t vi)
{
    int32_t dlp = (EXT_W0LP >> 8) * (vi - s->ext_lp) >> 12;
    int32_t dhp = EXT_W0HP * (s->ext_lp - s->ext_hp) >> 20;
    s->ext_vo = s->ext_lp - s->ext_hp;
    s->ext_lp += dlp;
    s->ext_hp += dhp;
}

INL void ext_clock_n(sid_t *s, int32_t dt, int32_t vi)
{
    int32_t step = 8;
    while (dt) {
        int32_t dlp, dhp;
        if (dt < step) step = dt;
        dlp = (EXT_W0LP * step >> 8) * (vi - s->ext_lp) >> 12;
        dhp = EXT_W0HP * step * (s->ext_lp - s->ext_hp) >> 20;
        s->ext_vo = s->ext_lp - s->ext_hp;
        s->ext_lp += dlp;
        s->ext_hp += dhp;
        dt -= step;
    }
}

/* ---- the chip */

INL int32_t voice_out(const sid_t *s, uint32_t k)
{
    return ((int32_t)wave_out(s, k) - s->wave_zero) * (int32_t)s->env[k].counter + s->voice_dc;
}

static void set_w0(sid_t *s)
{
    int32_t w0 = SID_W0[s->model][s->fc];
    s->w0_ceil_1 = w0 <= SID_W0_MAX_1 ? w0 : SID_W0_MAX_1;
    s->w0_ceil_dt = w0 <= SID_W0_MAX_DT ? w0 : SID_W0_MAX_DT;
}

void sid_set_model(sid_t *s, int model)
{
    s->model = model == SID_8580 ? SID_8580 : SID_6581;
    if (s->model == SID_6581) {
        s->wave_zero = 0x380;                   /* the waveform DAC's measured zero */
        s->voice_dc = 0x800 * 0xff;             /* the envelope DAC's DC offset */
        s->mixer_dc = -0xfff * 0xff / 18 >> 7;
    } else {                                    /* no DC offsets in the 8580 */
        s->wave_zero = 0x800;
        s->voice_dc = 0;
        s->mixer_dc = 0;
    }
    set_w0(s);
    s->q1024 = SID_Q1024[s->res];
}

void sid_reset(sid_t *s)
{
    uint32_t k;
    for (k = 0; k < 3u; k++) {
        sid_wave_t *w = &s->wave[k];
        sid_env_t *e = &s->env[k];
        w->acc = 0; w->shift = 0x7ffff8u; w->freq = 0; w->pw = 0;
        w->test = 0; w->ring_mod = 0; w->sync = 0; w->msb_rising = 0;
        e->counter = 0; e->attack = e->decay = e->sustain = e->release = 0; e->gate = 0;
        e->rate_counter = 0; e->exp_counter = 0; e->exp_period = 1;
        e->state = RELEASE; e->rate_period = RATE_PERIOD[0]; e->hold_zero = 1;
    }
    s->fc = 0; s->res = 0; s->filt = 0; s->voice3off = 0; s->hp_bp_lp = 0; s->vol = 0;
    s->vhp = s->vbp = s->vlp = s->vnf = 0;
    set_w0(s);
    s->q1024 = SID_Q1024[0];
    s->ext_lp = s->ext_hp = s->ext_vo = 0;
    s->bus_value = 0;
    s->bus_ttl = 0;
}

void sid_init(sid_t *s, int model)
{
    uint32_t k;
    s->model = SID_6581;
    for (k = 0; k < 3u; k++) s->wave[k].waveform = 0;
    s->ext_in = 0;
    s->sample_offset = 0;
    sid_reset(s);
    sid_set_model(s, model);
}

void sid_input(sid_t *s, int32_t sample) { s->ext_in = (sample << 4) * 3; }

static void write_control(sid_t *s, uint32_t k, uint32_t v)
{
    sid_wave_t *w = &s->wave[k];
    sid_env_t *e = &s->env[k];
    uint32_t test = v & 0x08u, gate = v & 0x01u;
    w->waveform = (v >> 4) & 0x0fu;
    w->ring_mod = v & 0x04u;
    w->sync = v & 0x02u;
    if (test) { w->acc = 0; w->shift = 0; }    /* (the register really fades over ~0x2000 cycles: not modelled) */
    else if (w->test) w->shift = 0x7ffff8u;
    w->test = test;
    /* the rate counter is never reset: an envelope starts after a delay */
    if (!e->gate && gate) {
        e->state = ATTACK;
        e->rate_period = RATE_PERIOD[e->attack];
        e->hold_zero = 0;
    } else if (e->gate && !gate) {
        e->state = RELEASE;
        e->rate_period = RATE_PERIOD[e->release];
    }
    e->gate = gate;
}

void sid_write(sid_t *s, uint32_t reg, uint32_t v)
{
    uint32_t k = reg / 7u;
    v &= 0xffu;
    s->bus_value = v;
    s->bus_ttl = 0x4000;
    if (reg < 21u) {
        sid_wave_t *w = &s->wave[k];
        sid_env_t *e = &s->env[k];
        switch (reg % 7u) {
        case 0: w->freq = (w->freq & 0xff00u) | v; break;
        case 1: w->freq = (v << 8) | (w->freq & 0xffu); break;
        case 2: w->pw = (w->pw & 0xf00u) | v; break;
        case 3: w->pw = ((v << 8) & 0xf00u) | (w->pw & 0xffu); break;
        case 4: write_control(s, k, v); break;
        case 5:
            e->attack = v >> 4;
            e->decay = v & 0x0fu;
            if (e->state == ATTACK) e->rate_period = RATE_PERIOD[e->attack];
            else if (e->state == DECAY_SUSTAIN) e->rate_period = RATE_PERIOD[e->decay];
            break;
        default:
            e->sustain = v >> 4;
            e->release = v & 0x0fu;
            if (e->state == RELEASE) e->rate_period = RATE_PERIOD[e->release];
            break;
        }
        return;
    }
    switch (reg) {
    case 0x15: s->fc = (s->fc & 0x7f8u) | (v & 7u); set_w0(s); break;
    case 0x16: s->fc = ((v << 3) & 0x7f8u) | (s->fc & 7u); set_w0(s); break;
    case 0x17: s->res = v >> 4; s->q1024 = SID_Q1024[s->res]; s->filt = v & 0x0fu; break;
    case 0x18: s->voice3off = v & 0x80u; s->hp_bp_lp = (v >> 4) & 7u; s->vol = v & 0x0fu; break;
    }
}

uint32_t sid_read(sid_t *s, uint32_t reg)
{
    switch (reg) {
    case 0x19: case 0x1a: return 0xff;         /* the paddles: nothing connected */
    case 0x1b: return wave_out(s, 2) >> 4;
    case 0x1c: return s->env[2].counter;
    default: return s->bus_value;
    }
}

INL int32_t output(const sid_t *s)
{
    int32_t v = s->ext_vo / ((4095 * 255 >> 7) * 3 * 15 * 2 / 65536);
    return v >= 32768 ? 32767 : v < -32768 ? -32768 : v;
}

int32_t sid_output(const sid_t *s) { return output(s); }

SID_HOT void sid_clock(sid_t *s)
{
    uint32_t k;
    if (--s->bus_ttl <= 0) { s->bus_value = 0; s->bus_ttl = 0; }
    for (k = 0; k < 3u; k++) env_clock(&s->env[k]);
    for (k = 0; k < 3u; k++) wave_clock(&s->wave[k]);
    for (k = 0; k < 3u; k++) wave_sync(s->wave, k);
    filter_clock(s, voice_out(s, 0), voice_out(s, 1), voice_out(s, 2));
    ext_clock(s, filter_out(s));
}

SID_HOT void sid_clock_n(sid_t *s, int32_t dt)
{
    uint32_t k;
    int32_t left = dt;
    if (dt <= 0) return;
    s->bus_ttl -= dt;
    if (s->bus_ttl <= 0) { s->bus_value = 0; s->bus_ttl = 0; }
    for (k = 0; k < 3u; k++) env_clock_n(&s->env[k], dt);
    /* the oscillators in steps that end where a sync source's MSB rises */
    while (left) {
        int32_t step = left;
        for (k = 0; k < 3u; k++) {
            const sid_wave_t *w = &s->wave[k];
            uint32_t d, next;
            if (!(s->wave[SYNC_DEST[k]].sync && w->freq)) continue;
            d = (w->acc & 0x800000u ? 0x1000000u : 0x800000u) - w->acc;
            next = d / w->freq;
            if (d % w->freq) next++;
            if ((int32_t)next < step) step = (int32_t)next;
        }
        for (k = 0; k < 3u; k++) wave_clock_n(&s->wave[k], step);
        for (k = 0; k < 3u; k++) wave_sync(s->wave, k);
        left -= step;
    }
    filter_clock_n(s, dt, voice_out(s, 0), voice_out(s, 1), voice_out(s, 2));
    ext_clock_n(s, dt, filter_out(s));
}

SID_HOT int sid_render(sid_t *s, int32_t *dt, int32_t cps, int16_t *out, int n, int stride)
{
    int made = 0;
    for (;;) {
        int32_t next = s->sample_offset + cps + (1 << 15);
        int32_t step = next >> 16;
        if (step > *dt) break;
        if (made >= n) return made;
        sid_clock_n(s, step);
        *dt -= step;
        s->sample_offset = (next & 0xffff) - (1 << 15);
        out[made++ * stride] = (int16_t)output(s);
    }
    sid_clock_n(s, *dt);
    s->sample_offset -= *dt << 16;
    *dt = 0;
    return made;
}
