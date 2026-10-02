#!/bin/sh
cd "$(dirname "$0")/.."
pass=0; total=0
for prog in tests/release_tests/*.pisa; do
  n=$(basename "$prog" .pisa)
  total=$((total+1))
  got=$(printf 'n 5000000\np\nq\n' | timeout 120 ./pipesim "$prog" "tests/release_tests/$n.json" 0 2>&1 \
        | sed -n '/^BEGIN STATE/,/^END STATE/p')
  if [ "$got" = "$(cat tests/release_tests/$n.expected.txt)" ]; then
    echo "PASS $n"; pass=$((pass+1))
  else
    echo "FAIL $n"
  fi
done
echo "$pass/$total release tests passed"
[ "$pass" -eq "$total" ]
