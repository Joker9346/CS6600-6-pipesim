#!/bin/sh
set -eu

CXX=${CXX:-g++}
CXXFLAGS=${CXXFLAGS:--std=c++17 -Wall -Wextra -Wpedantic -O2}

# added execution_unit/impl.cc and execution_unit/helpers.cc
$CXX $CXXFLAGS -I. tests/unit_tests.cc execution_unit.cc execution_unit/impl.cc execution_unit/helpers.cc \
  framework_api.cc cache/cache_config.cc cache/replacement.cc cache/cache_controller.cc \
  framework/simple_memory.cc \
  -o tests/unit_tests
./tests/unit_tests

test_tmp=$(mktemp -d)
trap 'rm -rf "$test_tmp" tests/unit_tests' EXIT HUP INT TERM

printf 'n 1000\np\nq\n' | ./pipesim example.pisa default_config.json \
  > "$test_tmp/example.out"
sed -n '/BEGIN STATE/,/END STATE/p' "$test_tmp/example.out" \
  > "$test_tmp/example.state"
cmp tests/expected_example_state.txt "$test_tmp/example.state"
grep -Fx 'CYCLES 71' "$test_tmp/example.out" >/dev/null

printf 'n 2000\np\ns\nq\n' | ./pipesim tests/eviction.pisa default_config.json \
  > "$test_tmp/eviction.out"
grep -Fx 'STATUS HALTED' "$test_tmp/eviction.out" >/dev/null
grep -Fx 'mem 0x00000000 11' "$test_tmp/eviction.out" >/dev/null
grep -Fx 'mem 0x00000040 22' "$test_tmp/eviction.out" >/dev/null
grep -Fx 'mem 0x00000080 33' "$test_tmp/eviction.out" >/dev/null
grep -Fx 'mem 0x000000c0 44' "$test_tmp/eviction.out" >/dev/null
grep -E '^L1D.dirty_evictions [1-9][0-9]*$' "$test_tmp/eviction.out" >/dev/null

printf 'n 1000\np\nq\n' | ./pipesim example.pisa tests/unequal_config.json \
  > "$test_tmp/unequal.out"
sed -n '/BEGIN STATE/,/END STATE/p' "$test_tmp/unequal.out" \
  > "$test_tmp/unequal.state"
cmp tests/expected_example_state.txt "$test_tmp/unequal.state"

printf 'n 1000\np\ns\nq\n' | \
  ./pipesim tests/execution_hazards.pisa tests/execution_config.json \
  > "$test_tmp/execution.out"
grep -Fx 'STATUS HALTED' "$test_tmp/execution.out" >/dev/null
grep -Fx 'x1 7' "$test_tmp/execution.out" >/dev/null
grep -Fx 'x5 0' "$test_tmp/execution.out" >/dev/null
grep -E '^outstanding_waw_stalls [1-9][0-9]*$' \
  "$test_tmp/execution.out" >/dev/null
grep -E '^execution_unit_busy_stalls [1-9][0-9]*$' \
  "$test_tmp/execution.out" >/dev/null
grep -E '^execution_completion_slot_stalls [1-9][0-9]*$' \
  "$test_tmp/execution.out" >/dev/null

printf 'n 10000\np\ns\nq\n' | \
  ./pipesim tests/one_hundred.pisa multicycle_config.json \
  > "$test_tmp/hundred.out"
grep -Fx 'STATUS HALTED' "$test_tmp/hundred.out" >/dev/null
grep -Fx 'x1 33' "$test_tmp/hundred.out" >/dev/null
grep -Fx 'x2 0' "$test_tmp/hundred.out" >/dev/null
grep -Fx 'retired 100' "$test_tmp/hundred.out" >/dev/null

printf 'n 1000\ns\nreset\nn 1000\ns\nq\n' | \
  ./pipesim example.pisa multicycle_config.json > "$test_tmp/reset.out"
sed -n '/BEGIN STATISTICS/,/END STATISTICS/p' "$test_tmp/reset.out" \
  > "$test_tmp/reset.stats"
test "$(grep -c '^BEGIN STATISTICS$' "$test_tmp/reset.stats")" -eq 2
half_lines=$(($(wc -l < "$test_tmp/reset.stats") / 2))
head -n "$half_lines" "$test_tmp/reset.stats" > "$test_tmp/reset.first"
tail -n "$half_lines" "$test_tmp/reset.stats" > "$test_tmp/reset.second"
cmp "$test_tmp/reset.first" "$test_tmp/reset.second"

if ./pipesim example.pisa tests/invalid_unknown_key.json \
    > "$test_tmp/invalid.out" 2>&1; then
  echo 'invalid JSON configuration was accepted' >&2
  exit 1
fi
grep -F "root contains unknown key 'unexpected'" "$test_tmp/invalid.out" \
  >/dev/null

echo 'integration tests: PASS'
