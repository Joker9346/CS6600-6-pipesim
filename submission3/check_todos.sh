#!/bin/sh

set -eu

cd "$(dirname "$0")"

show_locations=0
if [ "${1:-}" = "--list" ] || [ "${1:-}" = "-l" ]; then
  show_locations=1
fi

source_files=$(find . -type f \( -name '*.cc' -o -name '*.h' \) ! -path './tests/unit_tests.cc' | sort)
markers=$(echo "$source_files" | xargs grep -hoE 'TODO\((A2|PA3)-[A-Z-]+[0-9A-Z-]*\)' 2>/dev/null | sort -u || true)
marker_count=$(echo "$source_files" | xargs grep -oE 'TODO\((A2|PA3)-[A-Z-]+[0-9A-Z-]*\)' 2>/dev/null | wc -l | tr -d ' ')
group_count=$(echo "$markers" | grep -v '^$' | wc -l | tr -d ' ')

echo "CS6600 PA3 TODO status"
echo "Remaining groups: $group_count"
echo "Remaining markers: $marker_count"

if [ "$group_count" -gt 0 ]; then
  echo "$markers"
fi

if [ "$show_locations" -eq 1 ] && [ "$marker_count" -gt 0 ]; then
  echo "$source_files" | xargs grep -nE 'TODO\((A2|PA3)-[A-Z-]+[0-9A-Z-]*\)' 2>/dev/null
fi

if [ "$marker_count" -eq 0 ]; then
  exit 0
fi

exit 1
