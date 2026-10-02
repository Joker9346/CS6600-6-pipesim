#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
SRC=$(sed -n '/^SOURCES/,/^HEADERS/p' Makefile | grep -v '^HEADERS' | sed 's/SOURCES =//; s/\\//g' | tr '\n' ' ')
cp pipesim /tmp/pipesim_saved 2>/dev/null || true
rc=0
for cfg in "4 1" "8 2" "2 0"; do
  set -- $cfg
  g++ -std=c++17 -O2 -I. -DPIPESIM_BTB_ENTRIES=$1 -DPIPESIM_BTB_TAG_BITS=$2 $SRC -o pipesim
  echo "== BTB entries=$1 tag_bits=$2 =="
  python3 tests/pa3/verify.py | grep -E "FAIL|passed|^ +-" || true
  python3 tests/pa3/verify.py | grep -q "18/18 passed" || rc=1
  fb=0; tg=0
  for t in tests/pa3/*.pisa; do
    out=$(printf 'n 3000000\ns\nq\n' | ./pipesim "$t" default_config.json 0 2>/dev/null)
    fb=$((fb + $(echo "$out" | awk '/^branch.false_branch_redirects/{print $2}')))
    tg=$((tg + $(echo "$out" | awk '/^branch.target_mispredictions/{print $2}')))
  done
  echo "   false-branch redirects over all programs: $fb, target errors: $tg"
done
[ -f /tmp/pipesim_saved ] && cp /tmp/pipesim_saved pipesim
[ "$rc" -eq 0 ] && echo "ALIAS STRESS PASSED" || { echo "ALIAS STRESS FAILED"; exit 1; }
