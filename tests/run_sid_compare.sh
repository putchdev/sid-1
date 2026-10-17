#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Rewrite firmware/src/sid_tables.h from reSID 0.16, then check firmware/src/sid.c against reSID, sample for sample.
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host/cmp
B=build/host/cmp
R=tests/ref/resid-0.16
for f in envelope extfilt filter pot sid voice wave wave6581__ST wave6581_P_T wave6581_PS_ wave6581_PST \
         wave8580__ST wave8580_P_T wave8580_PS_ wave8580_PST; do
    [ $B/resid_$f.o -nt $R/$f.cc ] || c++ -std=c++14 -O2 -w -I$R -c $R/$f.cc -o $B/resid_$f.o
done
CPS="-DSID_CPS_PAL_CHECK=$(sed -n 's/^#define SID_CPS_PAL \([0-9]*\).*/\1/p' firmware/src/sid.h)"
CPS="$CPS -DSID_CPS_NTSC_CHECK=$(sed -n 's/^#define SID_CPS_NTSC \([0-9]*\).*/\1/p' firmware/src/sid.h)"
c++ -std=c++14 -O2 -Wall $CPS -I$R tests/sid_tables.cpp $B/resid_*.o -o $B/sid_tables
$B/sid_tables firmware/src/sid_tables.h
cc -std=c99 -O2 -Wall -Ifirmware/src -c firmware/src/sid.c -o $B/sid.o
c++ -std=c++14 -O2 -Wall -I$R tests/sid_compare.cpp $B/sid.o $B/resid_*.o -o $B/sid_compare
$B/sid_compare
