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
# macOS resolves every symbol at link time, while Linux leaves host
# calls for the loader: -undefined dynamic_lookup is what makes a test
# module behave the Linux way, resolving fl_* against the host that
# dlopens it. dlopen itself takes the same .so name on both.
if [ "$(uname)" = "Darwin" ]; then
	CFLAGS="$CFLAGS -undefined dynamic_lookup"
fi

for name in hello_native bad_version no_symbol lifecycle; do
	# shellcheck disable=SC2086
	$CC $CFLAGS -o "$WORK/$name.so" "$REPO/tests/native/$name.c"
done

ok

# --- a module that works -------------------------------------------------
# plain `import`, no subcommand: the .so files sit beside the script, so
# the bare library name resolves to them the way Python finds extension
# modules on sys.path.
cat > "$WORK/uses.fl" <<'EOF'
import hello_native
print(hello_native.greet("flint"))
print(hello_native.add(2, 3))
print(hello_native.add("not a number", 1))
print(hello_native.build(4))
print(hello_native.table())
try {
    hello_native.boom()
} catch e {
    print("caught: " + str(e))
}
print(hello_native.remember(7))
EOF

out=$("$FLINT" "$WORK/uses.fl" 2>&1) \
	&& status=0 || status=$?
expect_eq "a working module runs" 0 "$status"
expect_contains "a native string concatenates across the boundary" "hello, flint" "$out"
expect_contains "native arithmetic" "5" "$out"
expect_contains "a non-number argument reads as nil, not a crash" "hello, flint" "$out"
expect_contains "a native list" "[0, 1, 2, 3]" "$out"
expect_contains "a native table" "kind: native" "$out"
expect_contains "a native error is catchable" "message: native module refused" "$out"

# --- source still wins over native --------------------------------------
# a .fl beside the same name shadows the .so: the probe above fell
# through to native only because no .fl existed. edited source must
# never lose to a stale shared object.
cat > "$WORK/hello_native.fl" <<'EOF'
export fn greet(name) {
    return "source wins: " + name
}
EOF
cat > "$WORK/shadow.fl" <<'EOF'
import hello_native
print(hello_native.greet("flint"))
EOF
out=$("$FLINT" "$WORK/shadow.fl" 2>&1) \
	&& status=0 || status=$?
expect_eq "source shadows the shared object" 0 "$status"
expect_contains "the .fl ran instead" "source wins: flint" "$out"
rm "$WORK/hello_native.fl" "$WORK/shadow.fl"

# --- the paths that must fail -------------------------------------------
cat > "$WORK/missing.fl" <<'EOF'
import nope
print(nope)
EOF
out=$("$FLINT" "$WORK/missing.fl" 2>&1) && status=0 || status=$?
expect_eq "a missing library fails" 1 "$([ $status -ne 0 ] && echo 1 || echo 0)"
expect_contains "and says so" "could not open module file" "$out"

cat > "$WORK/nosym.fl" <<'EOF'
import no_symbol
print(no_symbol)
EOF
out=$("$FLINT" "$WORK/nosym.fl" 2>&1) && status=0 || status=$?
expect_eq "a library with no entry point fails" 1 "$([ $status -ne 0 ] && echo 1 || echo 0)"
expect_contains "and names the symbol" "flint_module_init" "$out"

cat > "$WORK/badver.fl" <<'EOF'
import bad_version
print(bad_version)
EOF
out=$("$FLINT" "$WORK/badver.fl" 2>&1) && status=0 || status=$?
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
#
# platform notes, both found on the macOS probe: the cdylib calls fl_*
# host functions, which Linux leaves for the loader but macOS must be
# told to allow (like the C modules above), and the library suffix is
# .dylib there rather than .so. dlopen takes either name.
DYLIB_EXT="so"
if [ "$(uname)" = "Darwin" ]; then
	DYLIB_EXT="dylib"
	# one -C link-arg per word: link-args with a space splits into two
	# argv entries and rustc reads the second as an input file. append,
	# not assign: a caller-provided RUSTFLAGS stays in force.
	RUSTFLAGS="${RUSTFLAGS:+$RUSTFLAGS }-C link-arg=-undefined -C link-arg=dynamic_lookup"
	export RUSTFLAGS
fi
# a failed build prints its log: "did not build, skipping" with no reason
# is how a red platform hides for a month. $2 selects the network:
# "offline" for crates with no registry dependencies (hermetic), "online"
# for ones the lockfile pins but the machine must download (ratatui).
build_rust_module() {
	# $1: manifest path. $2: log label. $3: offline or online.
	net="--offline"
	if [ "$3" = "online" ]; then
		net=""
	fi
	if cargo build --release $net --quiet \
		--manifest-path "$1" >"$WORK/cargo-$2.log" 2>&1; then
		return 0
	fi
	echo "native abi tests: $2 module did not build, skipping:"
	tail -5 "$WORK/cargo-$2.log"
	return 1
}
if command -v cargo >/dev/null 2>&1; then
	if build_rust_module "$REPO/rust/examples/rust-native/Cargo.toml" rust offline; then
		ok
		# absolute path import: the binding derives from the filename,
		# so `as` says what the script means.
		cat > "$WORK/rust.fl" <<RFL
import "$REPO/rust/examples/rust-native/target/release/librust_native.$DYLIB_EXT" as rust
print(rust.sum(5))
print(rust.words("the quick brown fox"))
print(rust.describe("text"))
try {
    rust.shout("hello")
} catch e {
    print("caught: " + e.message)
}
try {
    rust.explode()
} catch e {
    print("contained")
}
RFL
		out=$("$FLINT" "$WORK/rust.fl" 2>/dev/null) || status=1
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
	if build_rust_module "$REPO/rust/examples/tui-native/Cargo.toml" tui online; then
		ok
		cat > "$WORK/tui.fl" <<TFL
import "$REPO/rust/examples/tui-native/target/release/libtui_native.$DYLIB_EXT" as tui
let rows = tui.render_frame("demo", ["one", "two"])
for row in rows { print(row) }
print(tui.size())
TFL
		out=$("$FLINT" "$WORK/tui.fl" 2>/dev/null) || status=1
		expect_contains "a ratatui frame comes back bordered" "│one" "$out"
		expect_contains "with its title block" "demo" "$out"
		expect_contains "and the size is reported" "[80, 24]" "$out"
	else
		echo "native abi tests: tui module did not build, skipping"
	fi
fi

# --- the unload lifecycle: request, quiesce, busy -----------------------
# a module that retains a handle at init cannot be unloaded: the request
# must name the retained handle and leave the library loaded. busy is the
# structured answer, so the exit status stays 0.
out=$("$FLINT" native-unload "$WORK/lifecycle.so" lifecycle 2>&1) \
	&& status=0 || status=$?
expect_eq "an unload request against a retained handle succeeds as a request" 0 "$status"
expect_contains "and the answer is busy" "busy:" "$out"
expect_contains "naming the retained handle" "retained handle" "$out"

# a module with no retained handles is still busy: its exported functions
# stay callable through the VM's globals for as long as the VM lives, so
# there is always a route to the library's code.
out=$("$FLINT" native-unload "$WORK/hello_native.so" hello_demo 2>&1) \
	&& status=0 || status=$?
expect_eq "an unload request against exported functions succeeds as a request" 0 "$status"
expect_contains "and the answer is busy" "busy:" "$out"
expect_contains "naming the exported functions" "exported function" "$out"

# the library is still loaded after a busy answer: its functions run.
# bare import again -- the .so beside the script resolves with no help.
cat > "$WORK/stillhere.fl" <<'EOF'
import lifecycle
print(lifecycle.ping())
EOF
out=$("$FLINT" "$WORK/stillhere.fl" 2>&1) \
	&& status=0 || status=$?
expect_eq "a busy module still runs" 0 "$status"
expect_contains "because nothing was force-closed" "true" "$out"

# a path that cannot load fails before any unload is requested.
out=$("$FLINT" native-unload "$WORK/nope.so" demo 2>&1) && status=0 || status=$?
expect_eq "an unload of a missing library fails" 1 "$([ $status -ne 0 ] && echo 1 || echo 0)"
expect_contains "and says it cannot load" "cannot load" "$out"

if [ "$failed" -ne 0 ]; then
	echo "native abi tests: $checks checks, $failed failures"
	exit 1
fi
echo "native abi tests: $checks checks passed"
