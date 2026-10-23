#!/bin/sh
# Run every lockstep scenario, reference (Musashi) vs recompiled, for each
# game whose ROM is present in rom/.
# usage: tools/lockstep_suite.sh <build dir> [game ...]   (default: all)
B=${1:-build}; shift
GAMES=${*:-"desert jungle urban"}
T=${TMPDIR:-/tmp}/ds_lockstep
mkdir -p $T
fail=0
for s in 11 22 33; do python3 tools/make_inputs.py $s $T/seed$s.txt; done
for g in $GAMES; do
    ROM=rom/${g}_strike.bin
    [ -f $ROM ] || { echo "$g: no ROM, skipped"; continue; }
    run() {   # name frames inputs
        mkdir -p $T/$g/$1/ref $T/$g/$1/rc
        $B/ds_ref  $ROM $2 $3 $T/$g/$1/ref > /dev/null 2>&1 &
        $B/ds_test $ROM $2 $3 $T/$g/$1/rc  > /dev/null 2>&1 &
    }
    run menu 1500 harness/inputs_menu.txt
    run gameplay 15000 harness/inputs_gameplay.txt
    for s in 11 22 33; do run seed$s 20000 $T/seed$s.txt; done
done
wait
for g in $GAMES; do
    [ -f rom/${g}_strike.bin ] || continue
    for n in menu gameplay seed11 seed22 seed33; do
        printf "%-7s %-9s " $g $n
        python3 tools/lockstep.py $T/$g/$n/ref/frames.log $T/$g/$n/rc/frames.log || fail=1
    done
done
exit $fail
