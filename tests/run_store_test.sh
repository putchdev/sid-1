#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# The patch library's flash store, on the host (tests/store_test.c).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
cc -O2 -Wall -Wno-unused-function -DSID1_HOST -DAPP_VER='"dev"' -Ifirmware/src -Ifirmware/gen \
    -o build/host/store_test tests/store_test.c firmware/src/sid.c
build/host/store_test
