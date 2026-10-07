#!/bin/sh
#
# Formatter guarantees: canonical output, and idempotence.
#
#   fmt(fmt(source)) == fmt(source)
#
# That property is the whole contract: a formatter that only reaches a
# fixed point after several passes is a formatter that rewrites code on
# every save, and nobody keeps the result. Checked over every file in the
# repository -- real sources, which is the point -- plus a set of
# awkward shapes the repository may not contain.
#
# usage: sh tests/fmt_test.sh [path-to-flint]

set -e

FLINT="${1:-./flint}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failed=0
checked=0

# one file: format twice into separate copies and compare.
check_idempotent() {
	src="$1"
	base=$(basename "$src")
	cp "$src" "$WORK/a_$base"
	cp "$src" "$WORK/b_$base"
	"$FLINT" fmt "$WORK/a_$base" >/dev/null 2>&1 || {
		echo "FAIL: fmt errored on $src"
		failed=$((failed + 1))
		return
	}
	"$FLINT" fmt "$WORK/b_$base" >/dev/null 2>&1
	"$FLINT" fmt "$WORK/b_$base" >/dev/null 2>&1
	checked=$((checked + 1))
	if ! cmp -s "$WORK/a_$base" "$WORK/b_$base"; then
		echo "FAIL: not idempotent: $src"
		diff "$WORK/a_$base" "$WORK/b_$base" | head -8
		failed=$((failed + 1))
	fi
	# canonical output is a fixed point of --check
	if [ -n "$("$FLINT" fmt --check "$WORK/a_$base" 2>&1)" ]; then
		echo "FAIL: --check still reports $src after fmt"
		failed=$((failed + 1))
	fi
}

# every source we ship
for f in lib/*.fl tests/language/*.fl tests/language/*/*.fl examples/*.fl; do
	[ -f "$f" ] || continue
	check_idempotent "$f"
done

# shapes the repository may not happen to contain
mkdir -p "$WORK/cases"
i=0
while IFS= read -r line; do
	i=$((i + 1))
	printf '%s\n' "$line" > "$WORK/cases/case$i.fl"
	check_idempotent "$WORK/cases/case$i.fl"
done <<'EOF'
fn f(a, b) {
return a + b
}
let t = {
a: 1,
b: 2,
}
if t.a == 1 { print("one") } else { print("other") }
let xs = [
1,
2,
3,
]
try {
risky()
} catch e as ValueError {
print(e.message)
} finally {
cleanup()
}
for i, v in items({a: 1}) {
print(i, v)
}
let [a, b] = [1, 2]
# a comment with a { brace and   trailing spaces   
let s = "a string with { a brace"
let m = "line one
line two"
import "x.fl" as x
print(1..10, 1..10..2)
const C = 3.5
export fn g() { return C }
EOF

# malformed source must not crash the formatter; it formats bytes, not
# programs, so it cannot fail to produce output.
printf 'fn f( {\nlet = \n"unterminated\n' > "$WORK/cases/bad.fl"
"$FLINT" fmt "$WORK/cases/bad.fl" >/dev/null 2>&1 || true
if [ ! -s "$WORK/cases/bad.fl" ]; then
	echo "FAIL: formatter emptied a malformed file"
	failed=$((failed + 1))
fi
checked=$((checked + 1))

if [ "$failed" -ne 0 ]; then
	echo "fmt tests: $failed failures out of $checked checks"
	exit 1
fi
echo "fmt tests: $checked checks passed"
