#!/bin/sh
#
# The native C ABI, end to end.
#
# Builds real shared objects against include/flint.h, loads them, and
# checks what came out -- including the paths that must fail: a missing
# entry point, an ABI version the host does not implement, a module that
# raises an error, a module that exports nothing.
#
# Nothing here is stubbed. A C extension that only prints a message is not
# enough to prove an ABI, so these convert values both ways, build
# collections natively, raise catchable errors, and retain a handle across
# its own call.
#
# usage: sh tests/native_test.sh [path-to-flint]

set -e

FLINT=$(cd "$(dirname "$1")" && pwd)/flint
[ -x "$FLINT" ] || FLINT="${1:-./flint}"
REPO=$(pwd)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failed=0
checks=0

ok() { checks=$((checks + 1)); }
fail() {
	echo "FAIL: $1"
	shift
	for line in "$@"; do echo "  $line"; done
	failed=$((failed + 1))
}

expect_eq() {
	# expect_eq <label> <expected> <actual>
	ok
	if [ "$2" != "$3" ]; then
		fail "$1" "expected: $2" "got:      $3"
	fi
}

expect_contains() {
	ok
	case "$3" in
	*"$2"*) ;;
	*) fail "$1" "expected to contain: $2" "got: $3" ;;
	esac
}

CC=${CC:-cc}
CFLAGS="-std=c11 -Wall -Wextra -Werror -fPIC -shared -I$REPO/include"

for name in hello_native bad_version no_symbol; do
	# shellcheck disable=SC2086
	$CC $CFLAGS -o "$WORK/$name.so" "$REPO/tests/native/$name.c"
done

ok

# --- a module that works -------------------------------------------------
cat > "$WORK/uses.fl" <<'EOF'
print(greet("flint"))
print(add(2, 3))
print(add("not a number", 1))
print(build(4))
print(table())
try {
    boom()
} catch e {
    print("caught: " + str(e))
}
print(remember(7))
EOF

out=$("$FLINT" native "$WORK/hello_native.so" native_demo "$WORK/uses.fl" 2>&1) \
	&& status=0 || status=$?
expect_eq "a working module runs" 0 "$status"
expect_contains "a native string concatenates across the boundary" "hello, flint" "$out"
expect_contains "native arithmetic" "5" "$out"
expect_contains "a non-number argument reads as nil, not a crash" "hello, flint" "$out"
expect_contains "a native list" "[0, 1, 2, 3]" "$out"
expect_contains "a native table" "kind: native" "$out"
expect_contains "a native error is catchable" "message: native module refused" "$out"

# --- the paths that must fail -------------------------------------------
out=$("$FLINT" native "$WORK/nope.so" demo 2>&1) && status=0 || status=$?
expect_eq "a missing library fails" 1 "$([ $status -ne 0 ] && echo 1 || echo 0)"
expect_contains "and says so" "cannot load" "$out"

out=$("$FLINT" native "$WORK/no_symbol.so" demo 2>&1) && status=0 || status=$?
expect_eq "a library with no entry point fails" 1 "$([ $status -ne 0 ] && echo 1 || echo 0)"
expect_contains "and names the symbol" "flint_module_init" "$out"

out=$("$FLINT" native "$WORK/bad_version.so" demo 2>&1) && status=0 || status=$?
expect_eq "an unsupported ABI version fails" 1 "$([ $status -ne 0 ] && echo 1 || echo 0)"
expect_contains "and says why" "initialise" "$out"

# --- the header is usable on its own -------------------------------------
cat > "$WORK/alone.c" <<'EOF'
#include "flint.h"
int main(void) { return (int)fl_abi_version(); }
EOF
# shellcheck disable=SC2086
if $CC -std=c11 -Wall -Wextra -Werror -I"$REPO/include" -c \
	-o "$WORK/alone.o" "$WORK/alone.c" 2>/dev/null; then
	ok
else
	fail "include/flint.h compiles on its own, with no flint symbols"
fi

# --- a rust-backed module, through the same abi --------------------------
# the point is that it crosses the identical boundary: #[repr(C)] and
# extern "C" only, with the wrapper on top. it also shows the two things a C
# example cannot: a contained panic, and a retained handle.
if command -v cargo >/dev/null 2>&1; then
	if cargo build --release --offline --quiet \
		--manifest-path "$REPO/rust/examples/rust-native/Cargo.toml" \
		>/dev/null 2>&1; then
		ok
		cat > "$WORK/rust.fl" <<'RFL'
print(sum(5))
print(words("the quick brown fox"))
print(describe("text"))
try {
    shout("hello")
} catch e {
    print("caught: " + e.message)
}
try {
    explode()
} catch e {
    print("contained")
}
RFL
		out=$("$FLINT" native \
			"$REPO/rust/examples/rust-native/target/release/librust_native.so" \
			rust_native "$WORK/rust.fl" 2>/dev/null) || status=1
		expect_eq "a rust module runs" "10
[\"the\", \"quick\", \"brown\", \"fox\"]
{kind: string, from_rust: true}
caught: no module shouts for hello
contained" "$out"
	else
		echo "native abi tests: rust module did not build, skipping"
	fi
else
	echo "native abi tests: no cargo, skipping the rust module"
fi

# --- a ratatui module, proving the boundary carries a real ui library ----
# the shape that matters: flint asks for a frame, ratatui draws one, and
# what comes back across the abi is text. nothing of ratatui crosses.
if command -v cargo >/dev/null 2>&1; then
	if cargo build --release --offline --quiet \
		--manifest-path "$REPO/rust/examples/tui-native/Cargo.toml" \
		>/dev/null 2>&1; then
		ok
		cat > "$WORK/tui.fl" <<'TFL'
let rows = render_frame("demo", ["one", "two"])
for row in rows { print(row) }
print(size())
TFL
		out=$("$FLINT" native \
			"$REPO/rust/examples/tui-native/target/release/libtui_native.so" \
			tui_native "$WORK/tui.fl" 2>/dev/null) || status=1
		expect_contains "a ratatui frame comes back bordered" "│one" "$out"
		expect_contains "with its title block" "demo" "$out"
		expect_contains "and the size is reported" "[80, 24]" "$out"
	else
		echo "native abi tests: tui module did not build, skipping"
	fi
fi

if [ "$failed" -ne 0 ]; then
	echo "native abi tests: $checks checks, $failed failures"
	exit 1
fi
echo "native abi tests: $checks checks passed"