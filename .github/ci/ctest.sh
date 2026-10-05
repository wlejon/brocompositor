#!/usr/bin/env bash
# Runs ctest with the given arguments, then prints why each skipped test
# skipped (its own "SKIP..." line), so a job's log says what it did not cover
# and why. Exits with ctest's status.
#
#   ctest.sh --test-dir build -C Release
set -uo pipefail

ctest --output-on-failure "$@"
rc=$?

dir=build
prev=""
for a in "$@"; do
    [ "$prev" = "--test-dir" ] && dir="$a"
    prev="$a"
done
# Whole tests that skipped and checks inside passing tests that skipped (a
# client that is not installed, an opt-in sub-check), each under its test.
reasons="$(awk '/^[0-9]+\/[0-9]+ Testing: / { t = $3 } /SKIP/ { print t ": " $0 }' \
    "$dir"/Testing/Temporary/LastTest*.log 2>/dev/null | sort -u)"
if [ -n "$reasons" ]; then
    echo
    echo "Skipped (whole tests and checks inside tests):"
    echo "$reasons"
fi
exit "$rc"
