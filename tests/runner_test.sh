#!/bin/sh
#
# The test runner's own contract.
#
# A runner that cannot fail is worse than no runner: it reports a green suite
# over tests that were never checked. These four cases are the ones the status
# check exists for, each built from a throwaway fixture so the real suite is
# untouched.
#
# usage: sh tests/runner_test.sh [path-to-flint]

set -e

FLINT=$(cd "$(dirname "$1")" && pwd)/flint
[ -x "$FLINT" ] || FLINT="${1:-./flint}"
REPO=$(pwd)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The runner reads tests/language/ relative to its working directory, so it
# gets a working directory that is not the repository.
mkdir -p "$WORK/tests/language/case"
cd "$WORK"

failed=0
checks=0

expect_fails() {
    # expect_fails <label>  -- the runner must reject the suite
    checks=$((checks + 1))
    if sh "$REPO/tests/run_tests.sh" "$FLINT" >/dev/null 2>&1; then
        echo "FAIL: $1: the runner passed a suite it should have rejected"
        failed=$((failed + 1))
    fi
}

expect_passes() {
    checks=$((checks + 1))
    if ! sh "$REPO/tests/run_tests.sh" "$FLINT" >/dev/null 2>&1; then
        echo "FAIL: $1: the runner rejected a suite it should have accepted"
        failed=$((failed + 1))
    fi
}

case=tests/language/case/good.fl

# --- a clean test passes ------------------------------------------------
printf 'print("hello")\n' > "$case"
printf 'hello\n' > tests/language/case/good.expected
expect_passes "output matches, exits 0"

# --- correct output, wrong status: must fail -----------------------------
printf '70\n' > tests/language/case/good.status
expect_fails "output matches but the status is wrong"
rm tests/language/case/good.status

# --- correct output, correct nonzero status: must pass -------------------
# An uncaught throw, which exits 70 -- the status a runtime error actually
# produces, not a number picked to look plausible.
printf 'throw "boom"\n' > "$case"
printf 'boom\n[line 1] in script\n' > tests/language/case/good.expected
printf '70\n' > tests/language/case/good.status
expect_passes "an intentional nonzero status is declared and accepted"

# --- a clean test that unexpectedly exits nonzero: must fail ------------
printf 'print("hello")\n' > "$case"
printf 'hello\n' > tests/language/case/good.expected
printf '70\n' > tests/language/case/good.status
expect_fails "a successful script that exits nonzero"

# --- malformed status file: reported, not silently defaulted --------------
printf 'print("hello")\n' > "$case"
printf 'hello\n' > tests/language/case/good.expected
printf 'seventy\n' > tests/language/case/good.status
checks=$((checks + 1))
out=$(sh "$REPO/tests/run_tests.sh" "$FLINT" 2>&1) && code=0 || code=$?
if [ "$code" -eq 0 ]; then
    echo "FAIL: a malformed status file was accepted"
    failed=$((failed + 1))
fi
checks=$((checks + 1))
case "$out" in
*"Malformed status file"*) ;;
*)
    echo "FAIL: a malformed status file was not reported clearly"
    failed=$((failed + 1))
    ;;
esac
rm tests/language/case/good.status

# --- the runner itself propagates failure --------------------------------
printf 'print("hello")\n' > "$case"
printf 'goodbye\n' > tests/language/case/good.expected
checks=$((checks + 1))
sh "$REPO/tests/run_tests.sh" "$FLINT" >/dev/null 2>&1 && code=0 || code=$?
if [ "$code" -eq 0 ]; then
    echo "FAIL: the runner exited 0 with a failing test"
    failed=$((failed + 1))
fi

# --- a .fl with no .expected is a helper, not a test ---------------------
rm tests/language/case/good.expected
printf 'fn helper() { return 1 }\n' > tests/language/helper_mod.fl
expect_passes "a module without an .expected is not a test"

if [ "$failed" -ne 0 ]; then
    echo "runner tests: $checks checks, $failed failures"
    exit 1
fi
echo "runner tests: $checks checks passed"
