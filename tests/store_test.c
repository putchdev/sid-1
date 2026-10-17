/* SPDX-License-Identifier: GPL-3.0-only */
/* The patch library's flash store (firmware/src/sid1.c) on the host's fake flash: a save reads back whole; an
 * empty, damaged or half-written store gives the factory patches. Run tests/run_store_test.sh. */
#define main host_main                          /* (the host's main: this file has its own) */
#include "../firmware/src/sid1.c"
#undef main

static int fails;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); fails += !ok; }

static int is_factory(void)
{
    uint32_t i;
    for (i = 0; i < NSLOT; i++) {
        patch_t z;
        memset(&z, 0, sizeof z);
        if (memcmp(&lib[i], i < NFACTORY ? &FACTORY[i] : &z, sizeof z)) return 0;
    }
    return 1;
}

int main(void)
{
    patch_t edited_patch;
    check(sizeof(patch_t) == 50u, "a patch is 50 bytes, no padding");
    memset(host_flash, 0xFF, sizeof host_flash);
    store_load();
    check(is_factory(), "an empty store gives the factory patches");

    lib[0].cut = 0x55;
    lib[40] = lib[3];
    memcpy(lib[40].name, "MY PAD", 7);
    edited_patch = lib[40];
    check(store_save() == 0, "save");
    memset(lib, 0, sizeof lib);
    store_load();
    check(lib[0].cut == 0x55 && !memcmp(&lib[40], &edited_patch, sizeof edited_patch) && !is_factory(),
          "a save reads back");

    host_flash[sizeof(store_head_t) + 100u] ^= 0x10;   /* a bit flipped in slot 2 */
    store_load();
    check(is_factory(), "a damaged store gives the factory patches");

    store_save();
    memset(host_flash, 0xFF, sizeof(store_head_t));   /* a save cut before its header */
    store_load();
    check(is_factory(), "a save without its header gives the factory patches");

    printf(fails ? "%d check(s) failed\n" : "all store checks passed\n", fails);
    return fails ? 1 : 0;
}
