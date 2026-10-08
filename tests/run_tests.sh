#!/bin/sh
#
# Run every language test and check its output *and* its exit status.
#
# usage: sh tests/run_tests.sh [path-to-flint]
#
# A test is a set of files sharing a basename under tests/language/:
#
#   name.fl        the script
#   name.expected  its stdout, compared byte for byte
#   name.stdin     optional, fed to the script on stdin
#   name.status    optional, the expected exit status. absent means 0
#
# stderr is folded into stdout, so a test that is supposed to error carries
# the message in its .expected file. That is also how the error tests work:
# the script fails, the first line of output is the diagnostic, and the rest
# is whatever the script managed to print before it stopped.
#
# The status file exists because output alone is not enough. A test whose
# script prints exactly the right lines and then exits non-zero would pass on
# output alone, and so would one that exits 70 for the wrong reason -- a
# runtime error where the test meant a clean run, or a crash dressed as an
# expected diagnostic. The status is part of the contract, and checking it is
# how a test says "this one is supposed to fail" instead of merely tolerating
# it.
#
# A .fl with no .expected is a helper module for another test, not a test.

set -e

FLINT="${1:-./flint}"

passed=0
failed=0

report_fail() {
    # $1 test file, $2 expected, $3 got, $4 expected status, $5 got status
    echo "FAIL: $1"
    echo "Expected:"
    echo "$2"
    echo "Got:"
    echo "$3"
    if [ "$4" != "$5" ]; then
        echo "Expected exit status $4, got $5."
    fi
}

for test_file in $(find tests/language -type f -name '*.fl' | sort); do
    [ -f "$test_file" ] || continue
    base="${test_file%.fl}"
    expected="$base.expected"
    stdin_file="$base.stdin"
    status_file="$base.status"

    [ -f "$expected" ] || continue

    # absent means 0. a malformed status file is a broken test rather than
    # an implicit default, and says so instead of quietly passing.
    want_status=0
    if [ -f "$status_file" ]; then
        want_status=$(cat "$status_file")
        case "$want_status" in
        '' | *[!0-9]*)
            echo "FAIL: $test_file"
            echo "Malformed status file $status_file: '$want_status' is not an exit status."
            failed=$((failed + 1))
            continue
            ;;
        esac
    fi

    # `|| true` because a test may exit non-zero on purpose; the status is
    # compared below instead of being discarded.
    if [ -f "$stdin_file" ]; then
        out=$("$FLINT" "$test_file" <"$stdin_file" 2>&1) && got_status=0 || got_status=$?
    else
        # </dev/null rather than inheriting, so a test that calls input()
        # without a .stdin file sees EOF instead of hanging the suite on
        # whatever the terminal is doing
        out=$("$FLINT" "$test_file" </dev/null 2>&1) && got_status=0 || got_status=$?
    fi
    expected_out=$(cat "$expected")

    if [ "$out" = "$expected_out" ] && [ "$got_status" = "$want_status" ]; then
        passed=$((passed + 1))
    else
        report_fail "$test_file" "$expected_out" "$out" "$want_status" \
            "$got_status"
        failed=$((failed + 1))
    fi
done

echo "Tests: $((passed + failed)), Passed: $passed, Failed: $failed"

# a final test failure means a non-zero exit, which is what CI reads.
# written as an if because `test && exit` under `set -e` exits even when the
# test fails, which is the opposite of what it looks like.
if [ "$failed" -gt 0 ]; then
    exit 1
fi
exit 0
