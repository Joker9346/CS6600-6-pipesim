#!/bin/sh
# check_todos.sh -- report TODO(A2-...) completion status for the starter.
#
# Exit code:
#   0 if no TODO(A2-...) markers remain in any .cc / .h file
#   1 if at least one marker remains (i.e. work still to do)
#
# Usage:
#   ./check_todos.sh          # summary only
#   ./check_todos.sh --list   # also list every remaining marker (file:line)

set -eu

cd "$(dirname "$0")"

LIST=0
if [ "${1:-}" = "--list" ] || [ "${1:-}" = "-l" ]; then
  LIST=1
fi

# Only look at source files. Excluding this script and README/policy docs.
FILES=$(find . -type f \( -name '*.cc' -o -name '*.h' \) \
          ! -path './tests/unit_tests.cc' \
          | sort)

# All 24 TODO IDs shipped in the starter, in the order the PDF lists them.
ALL_IDS='A2-DESIGN-01 A2-DESIGN-02 A2-DESIGN-03 A2-DESIGN-05 A2-DESIGN-06
A2-EXEC-01 A2-EXEC-02 A2-EXEC-03 A2-EXEC-04 A2-EXEC-05
A2-CACHE-02 A2-CACHE-04 A2-CACHE-05 A2-CACHE-06 A2-CACHE-07 A2-CACHE-08
A2-CACHE-09 A2-CACHE-10 A2-CACHE-11 A2-CACHE-12 A2-CACHE-13
A2-REPL-01 A2-REPL-02 A2-REPL-03'
TOTAL_IDS=24

# Count remaining markers.
if command -v grep >/dev/null 2>&1; then
  MARKERS=$(echo "$FILES" | xargs grep -ho 'TODO(A2-[A-Z]*-[0-9]*)' 2>/dev/null \
              | sort -u || true)
else
  echo "check_todos.sh: grep not found" >&2
  exit 2
fi

REMAINING_COUNT=$(echo "$FILES" | xargs grep -oE 'TODO\(A2-[A-Z]+-[0-9]+\)' \
                    2>/dev/null | wc -l | tr -d ' ')
REMAINING_IDS=$(echo "$MARKERS" | grep -v '^$' | wc -l | tr -d ' ')

COMPLETED_IDS=$((TOTAL_IDS - REMAINING_IDS))

echo "CS6600 A2 -- TODO status"
echo "------------------------"
echo "Unique TODO IDs total     : $TOTAL_IDS"
echo "Unique TODO IDs remaining : $REMAINING_IDS"
echo "Unique TODO IDs completed : $COMPLETED_IDS"
echo "Total TODO markers left   : $REMAINING_COUNT"
echo

if [ "$REMAINING_IDS" -gt 0 ]; then
  echo "Remaining TODO groups:"
  echo "$MARKERS" | sed 's/^/  /'
  echo
fi

if [ "$LIST" = "1" ] && [ "$REMAINING_COUNT" -gt 0 ]; then
  echo "Remaining TODO markers (file:line):"
  echo "$FILES" | xargs grep -nE 'TODO\(A2-[A-Z]+-[0-9]+\)' 2>/dev/null \
    | sed 's/^/  /'
  echo
fi

if [ "$REMAINING_COUNT" -eq 0 ]; then
  echo "All TODOs cleared. Now run:  make && ./tests/run_tests.sh"
  exit 0
else
  echo "Still $REMAINING_COUNT marker(s) to clear across $REMAINING_IDS group(s)."
  exit 1
fi
