// SPDX-License-Identifier: GPL-2.0-or-later
// Writes firmware/src/sid_tables.h from reSID 0.16 itself (tests/ref/resid-0.16): the combined waveform samples and
// the filter's cutoff and resonance values, which reSID works out in doubles at start-up. Run tests/run_sid_compare.sh.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sid.h"

struct FilterTap : Filter {
    int w0_for(int fc) { writeFC_LO(fc & 7); writeFC_HI(fc >> 3); return w0; }
    int w0_ceil_1_now() const { return w0_ceil_1; }
    int w0_ceil_dt_now() const { return w0_ceil_dt; }
    int q_for(int res) { writeRES_FILT(res << 4); return _1024_div_Q; }
};

static void wave(FILE *f, const char *file)
{
    char path[256];
    unsigned char b[4096];
    snprintf(path, sizeof path, "tests/ref/resid-0.16/%s", file);
    FILE *in = fopen(path, "rb");
    if (!in || fread(b, 1, 4096, in) != 4096) { perror(path); exit(1); }
    fclose(in);
    fprintf(f, "    {   /* %s */\n", file);
    for (int i = 0; i < 4096; i += 32) {
        fprintf(f, "       ");
        for (int k = 0; k < 32; k++) fprintf(f, " %u,", b[i + k]);
        fprintf(f, "\n");
    }
    fprintf(f, "    },\n");
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "firmware/src/sid_tables.h";
    FILE *f = fopen(out, "w");
    if (!f) { perror(out); return 1; }
    fprintf(f, "/* SPDX-License-Identifier: GPL-2.0-or-later */\n"
               "/* reSID 0.16's tables (Dag Lem), written by tests/sid_tables.cpp: do not edit */\n\n");
    static const char *WAVES[2][4] = {{"wave6581__ST.dat", "wave6581_P_T.dat", "wave6581_PS_.dat", "wave6581_PST.dat"},
                                      {"wave8580__ST.dat", "wave8580_P_T.dat", "wave8580_PS_.dat", "wave8580_PST.dat"}};
    fprintf(f, "/* the combined waveforms, by model: __ST, P_T, PS_, PST */\n"
               "static const uint8_t SID_WAVE[2][4][4096] = {\n");
    for (int m = 0; m < 2; m++) {
        fprintf(f, "  {\n");
        for (int w = 0; w < 4; w++) wave(f, WAVES[m][w]);
        fprintf(f, "  },\n");
    }
    fprintf(f, "};\n\n");

    FilterTap flt;
    int max_1 = 0, max_dt = 0;
    fprintf(f, "/* the cutoff, w0 = 2 pi f0 1.048576, by model and FC */\nstatic const int32_t SID_W0[2][2048] = {\n");
    for (int m = 0; m < 2; m++) {
        flt.set_chip_model(m ? MOS8580 : MOS6581);
        fprintf(f, "  {");
        for (int fc = 0; fc < 2048; fc++) {
            int w0 = flt.w0_for(fc);
            if (flt.w0_ceil_1_now() < w0) max_1 = flt.w0_ceil_1_now();
            if (flt.w0_ceil_dt_now() < w0) max_dt = flt.w0_ceil_dt_now();
            fprintf(f, "%s%d,", fc % 16 ? " " : "\n    ", w0);
        }
        fprintf(f, "\n  },\n");
    }
    fprintf(f, "};\n");
    fprintf(f, "#define SID_W0_MAX_1 %d                    /* 16 kHz: the one cycle filter's limit */\n", max_1);
    fprintf(f, "#define SID_W0_MAX_DT %d                    /* 4 kHz: the delta_t filter's limit */\n\n", max_dt);
    fprintf(f, "/* 1024 / Q by resonance */\nstatic const int32_t SID_Q1024[16] = {");
    for (int r = 0; r < 16; r++) fprintf(f, "%s%d", r ? ", " : "", flt.q_for(r));
    fprintf(f, "};\n");
    fclose(f);

    int pal = (int)(985248.0 / 44100 * 65536 + 0.5), ntsc = (int)(1022727.0 / 44100 * 65536 + 0.5);
    if (pal != SID_CPS_PAL_CHECK || ntsc != SID_CPS_NTSC_CHECK) {
        fprintf(stderr, "sid.h: SID_CPS_PAL should be %d, SID_CPS_NTSC %d\n", pal, ntsc);
        return 1;
    }
    if (!max_1 || !max_dt) { fprintf(stderr, "no w0 limit seen\n"); return 1; }
    printf("tables -> %s\n", out);
    return 0;
}
