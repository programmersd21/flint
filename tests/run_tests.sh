#!/bin/sh
#
# Run every language test and diff its output against the expected file.
#
# usage: sh tests/run_tests.sh [path-to-flint]
#
# A test is a set of files sharing a basename under tests/language/:
#
#   name.fl        the script
#   name.expected  its stdout, compared byte for byte
#   name.stdin     optional, fed to the script on stdin
#
# stderr is folded into stdout, so a test that is supposed to error carries
# the message in its .expected file. That is also how the error tests work:
# the script fails, the first line of output is the diagnostic, and the rest
# is whatever the script managed to print before it stopped.
#
# A .fl with no .expected is a helper module for another test, not a test.

set -e

FLINT="${1:-./flint}"

passed=0
failed=0

report_fail() {
    # $1 test file, $2 expected, $3 got
    echo "FAIL: $1"
    echo "Expected:"
    echo "$2"
    echo "Got:"
    echo "$3"
}

for test_file in $(find tests/language -type f -name '*.fl' | sort); do
    [ -f "$test_file" ] || continue
    base="${test_file%.fl}"
    expected="$base.expected"
    stdin_file="$base.stdin"

    [ -f "$expected" ] || continue

    # `|| true` because a test may exit non-zero on purpose
    if [ -f "$stdin_file" ]; then
        out=$("$FLINT" "$test_file" <"$stdin_file" 2>&1 || true)
    else
        # </dev/null rather than inheriting, so a test that calls input()
        # without a .stdin file sees EOF instead of hanging the suite on
        # whatever the terminal is doing
        out=$("$FLINT" "$test_file" </dev/null 2>&1 || true)
    fi
    expected_out=$(cat "$expected")

    if [ "$out" = "$expected_out" ]; then
        passed=$((passed + 1))
    else
        report_fail "$test_file" "$expected_out" "$out"
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
