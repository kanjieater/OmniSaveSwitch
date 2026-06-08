#!/bin/bash
set -euo pipefail

# Find modified or newly added server/src/*.py files (not test files).
files=$(git diff --name-only origin/main...HEAD \
  | grep -E '^server/src/[^/]+\.py$' \
  | grep -v '__' \
  || true)

if [ -z "$files" ]; then
  exit 0
fi

missing=0
for file in $files; do
  base=$(basename "$file" .py)
  # Accept any test file that references the module name.
  if ! grep -rl "$base" server/tests/test_*.py > /dev/null 2>&1; then
    echo "WARNING: no test file references $file (expected coverage in server/tests/)"
    missing=$((missing + 1))
  fi
done

if [ "$missing" -gt 0 ]; then
  echo ""
  echo "NOTE: diff-cover enforces 100% line coverage on changed code."
  echo "The above warnings mean no test file imports or names $base — verify via diff-cover output."
fi
# Non-blocking: diff-cover in CI is the hard gate.
exit 0
