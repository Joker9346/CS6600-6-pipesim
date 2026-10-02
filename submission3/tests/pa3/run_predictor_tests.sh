#!/bin/sh
# Exercises TAGE, the direct-indexed loop predictor, and the Statistical Corrector.
# Run from anywhere:  sh tests/pa3/run_predictor_tests.sh
set -eu
cd "$(dirname "$0")/../.."
CXX=${CXX:-g++}
FLAGS="-std=c++17 -O2 -Wall -Wextra -I."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "== build (SC enabled) =="
$CXX $FLAGS tests/pa3/predictor_tests.cc predictors/predictor_one.cc -o "$TMP/on"
echo "== build (SC disabled via -DPIPESIM_SC_THRESHOLD=99) =="
$CXX $FLAGS -DPIPESIM_SC_THRESHOLD=99 tests/pa3/predictor_tests.cc predictors/predictor_one.cc -o "$TMP/off"

echo; rc_on=0; "$TMP/on" > "$TMP/on.txt" || rc_on=$?; cat "$TMP/on.txt"
echo; echo "[SC off] same random-bias stream with the corrector disabled"
rc_off=0; "$TMP/off" 0 > "$TMP/off.txt" || rc_off=$?; cat "$TMP/off.txt"

on=$(grep SC_MISS_RATE "$TMP/on.txt" | awk '{print $2}')
off=$(grep SC_MISS_RATE "$TMP/off.txt" | awk '{print $2}')
echo; echo "== SC benefit: miss rate with SC = $on%, without SC = $off% =="
if [ "$rc_on" -ne 0 ] || [ "$rc_off" -ne 0 ]; then echo "PREDICTOR TESTS FAILED"; exit 1; fi
awk -v a="$on" -v b="$off" 'BEGIN{ if (a < b) {print "SC A/B: PASS (SC reduces mispredictions)"; exit 0} else {print "SC A/B: FAIL (SC does not help)"; exit 1} }'
echo "ALL PREDICTOR TESTS PASSED"
