// SPDX-License-Identifier: GPL-2.0-or-later
// Our SID (firmware/src/sid.c) against reSID 0.16 (tests/ref/resid-0.16), both models, on random register writes:
// cycle by cycle (reSID's clock()), sample by sample in SAMPLE_FAST at 44.1 kHz (clock(delta_t, buf, n)), and in
// steps of any length (clock(delta_t)) on edge cases: chains of synced voices at one frequency, which sync on the
// same cycle, and noise at frequencies that land a step exactly on its shift, read back through OSC3.
// Run tests/run_sid_compare.sh.
#include <cstdio>
#include <cstdint>

#include "sid.h"
extern "C" {
#include "../firmware/src/sid.h"
}

static uint32_t rng = 1;
static uint32_t rnd(uint32_t n) { rng = rng * 1664525u + 1013904223u; return (rng >> 8) % n; }

// a register write a player might make: gates, every waveform with sync, ring and test, the filter
static void random_write(uint32_t &reg, uint32_t &val)
{
    static const uint8_t WAVES[] = {0x10, 0x20, 0x40, 0x80, 0x30, 0x50, 0x60, 0x70, 0x00, 0x90, 0xF0};
    reg = rnd(25);
    val = rnd(256);
    if (reg % 7 == 4 && reg < 21) {
        val = WAVES[rnd(sizeof WAVES)] | (rnd(4) == 0 ? 0x08 : 0) | (rnd(3) == 0 ? 0x04 : 0) | (rnd(3) == 0 ? 0x02 : 0)
              | rnd(2);
    }
    if (reg == 0x18 && rnd(2)) val |= 0x0f;     // mostly loud
}

enum { CYCLES, SAMPLES, EDGES };

static int run(int seed, chip_model model, int mode)
{
    RESID ref;
    sid_t ours;
    ref.set_chip_model(model);
    ref.reset();
    sid_init(&ours, model == MOS8580 ? SID_8580 : SID_6581);
    rng = (uint32_t)seed * 7919u + (uint32_t)mode * 2u + 1u;
    uint32_t diffs = 0, first = 0;
    for (uint32_t r = 0; r < 0x19; r++) {      // a first sound so that there is something to compare
        uint32_t v = r == 0x18 ? 0x1f : r == 0x17 ? 0xf1 : r % 7 == 4 ? 0x41 : r % 7 == 6 ? 0xf8 : rnd(256);
        ref.write(r, v);
        sid_write(&ours, r, v);
    }
    if (mode == EDGES) {
        static const uint32_t FREQ[] = {0x8000, 0x4000, 0x1000, 0x0800, 0xffff, 0x0001, 0x5555, 0x2000};
        for (uint32_t t = 0; t < 6000; t++) {
            if (rnd(4) == 0) {
                uint32_t k = rnd(3), f = FREQ[rnd(sizeof FREQ / sizeof FREQ[0])], all = rnd(3) == 0;
                for (uint32_t v = 0; v < 3; v++) {
                    if (!all && v != k) continue;
                    uint32_t ctl = (rnd(2) ? 0x80 : 0x20) | 0x02 | (rnd(5) == 0 ? 0x08 : 0) | 0x01;
                    uint32_t regs[3][2] = {{v * 7, f & 0xff}, {v * 7 + 1, f >> 8}, {v * 7 + 4, ctl}};
                    for (auto &w : regs) { ref.write(w[0], w[1]); sid_write(&ours, w[0], w[1]); }
                }
            }
            int dt = rnd(3) ? 1 + (int)rnd(40) : 1 + (int)rnd(3000);
            ref.clock(dt);
            sid_clock_n(&ours, dt);
            uint32_t osc_a = ref.read(0x1b), osc_b = sid_read(&ours, 0x1b);

            if (ref.output() != sid_output(&ours) || osc_a != osc_b) { if (!diffs) first = t; diffs++; }
        }
    } else if (mode == CYCLES) {
        for (uint32_t t = 0; t < 400000; t++) {
            if (rnd(500) == 0) {
                uint32_t reg, val;
                random_write(reg, val);
                ref.write(reg, val);
                sid_write(&ours, reg, val);
            }
            ref.clock();
            sid_clock(&ours);
            int a = ref.output(), b = sid_output(&ours);
            uint32_t ra = 0, rb = 0;
            if (t % 97 == 0) {
                uint32_t reg = 0x1b + rnd(3) % 2;
                ra = ref.read(reg), rb = sid_read(&ours, reg);
            }
            if (a != b || ra != rb) { if (!diffs) first = t; diffs++; }
        }
    } else {
        short ra[2048];
        int16_t rb[2048];
        uint32_t samples = 0;
        for (int chunk = 0; chunk < 600; chunk++) {
            uint32_t reg, val;
            random_write(reg, val);
            ref.write(reg, val);
            sid_write(&ours, reg, val);
            if (rnd(2)) {
                uint32_t reg2 = 0x1b + rnd(2);
                if (ref.read(reg2) != sid_read(&ours, reg2)) { if (!diffs) first = samples; diffs++; }

            }
            int dt = 1 + (int)rnd(30000);
            int32_t dt2 = dt;
            int na = ref.clock(dt, ra, 2048, 1);
            int nb = sid_render(&ours, &dt2, SID_CPS_PAL, rb, 2048, 1);
            if (na != nb || dt != dt2) { if (!diffs) first = samples; diffs++; continue; }
            for (int i = 0; i < na; i++)
                if (ra[i] != rb[i]) { if (!diffs) first = samples + i; diffs++; }
            samples += na;
        }
    }
    if (diffs) printf("%s %s seed %2d: FAIL %u differ, first at %u\n", model == MOS8580 ? "8580" : "6581",
                      mode == CYCLES ? "cycles " : mode == SAMPLES ? "samples" : "edges  ", seed, diffs, first);
    return diffs ? 1 : 0;
}

int main()
{
    int failed = 0, runs = 0;
    for (int m = 0; m < 2; m++)
        for (int seed = 0; seed < 12; seed++)
            for (int mode = CYCLES; mode <= EDGES; mode++, runs++)
                failed += run(seed, m ? MOS8580 : MOS6581, mode);
    if (failed) printf("%d of %d runs differ from reSID 0.16\n", failed, runs);
    else printf("all %d runs identical to reSID 0.16 (6581 and 8580: cycle by cycle, SAMPLE_FAST, edge cases)\n", runs);
    return failed ? 1 : 0;
}
