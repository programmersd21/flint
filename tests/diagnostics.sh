#!/bin/sh
set -eu

FLINT="${1:-./flint}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

set +e
"$FLINT" --color=never --error-format=human -e 'print("x"' \
	>"$tmp/human" 2>&1
status=$?
set -e
[ "$status" -eq 65 ]
grep -q 'error\[E0102\]' "$tmp/human"
grep -q 'print("x"' "$tmp/human"
# The replacement belongs on the help line, where it can be acted on. It used
# to print as a bare "|" block followed by the delimiter and an internal
# "applicability: machine-applicable" line, which told the reader nothing and
# leaked metadata. Applicability is still in the JSON, where tooling wants it.
grep -q 'help: add the missing delimiter' "$tmp/human"
if grep -q 'applicability' "$tmp/human"; then
	echo "internal applicability leaked into human output"
	cat "$tmp/human"
	exit 1
fi

set +e
"$FLINT" --error-format=json -e 'let x =' >"$tmp/json" 2>&1
status=$?
set -e
[ "$status" -eq 65 ]
python3 - "$tmp/json" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    diagnostic = json.loads(stream.readline())
assert diagnostic["severity"] == "error"
assert diagnostic["code"] == "E0100"
assert diagnostic["spans"][0]["is_primary"] is True
assert diagnostic["spans"][0]["start"] == 7
PY

set +e
"$FLINT" --error-format=json -e 'print(1 + "s")' \
	>"$tmp/runtime" 2>&1
status=$?
set -e
[ "$status" -eq 70 ]
python3 - "$tmp/runtime" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    diagnostic = json.loads(stream.readline())
assert diagnostic["code"] == "E0301"
assert len(diagnostic["spans"]) == 1
PY

set +e
"$FLINT" --error-format=short -e 'print("a" + 1)' \
	>"$tmp/short" 2>&1
status=$?
set -e
[ "$status" -eq 70 ]
grep -q 'error\[E0301\]' "$tmp/short"

set +e
"$FLINT" --error-format=human --color=never -e 'clok()' \
	>"$tmp/name" 2>&1
status=$?
set -e
[ "$status" -eq 70 ]
grep -q 'error\[E0202\]' "$tmp/name"
grep -q 'did you mean `clock`?' "$tmp/name"

[ "$("$FLINT" -e 'print(2 + 2)')" = "4" ]
"$FLINT" -e 'let x =' >"$tmp/legacy" 2>&1 || true
grep -q '^\[line 1\] Error at end:' "$tmp/legacy"

python3 - "$tmp/fix.fl" <<'PY'
import sys

with open(sys.argv[1], "w", encoding="utf-8") as stream:
    stream.write('print("fixed"\n')
PY
	"$FLINT" --fix "$tmp/fix.fl" \
	>"$tmp/fix.out" 2>"$tmp/fix.err"
grep -q '^fixed: ' "$tmp/fix.out"
printf 'print("fixed")\n' >"$tmp/fix.expected"
cmp "$tmp/fix.expected" "$tmp/fix.fl"
"$FLINT" "$tmp/fix.fl" >"$tmp/fix.run"
[ "$(cat "$tmp/fix.run")" = "fixed" ]

python3 - "$tmp/layout.fl" <<'PY'
import sys

with open(sys.argv[1], "wb") as stream:
    stream.write('\tprint("λ" + 1)\r\n'.encode("utf-8"))
PY
set +e
"$FLINT" --error-format=human --color=auto "$tmp/layout.fl" \
	>"$tmp/layout.out" 2>&1
status=$?
set -e
[ "$status" -eq 70 ]
grep -q '|     print("λ" + 1)' "$tmp/layout.out"
python3 - "$tmp/layout.out" <<'PY'
import pathlib
import sys

assert b"\x1b[" not in pathlib.Path(sys.argv[1]).read_bytes()
PY

echo 'diagnostic tests passed'

# --- typo suggestions ---
#
# `print` is a keyword, not a global, so a search over globals alone can never
# suggest it. `pritn` is the commonest typo in any language and it used to be
# reported with no help at all, for exactly that reason. These lock it down.

assert_suggests() {
	# $1 source, $2 expected suggestion
	printf '%s\n' "$1" >"$tmp/t.fl"
	set +e
	"$FLINT" --color=never --error-format=human "$tmp/t.fl" \
		>"$tmp/sug" 2>&1
	set -e
	grep -q "did you mean \`$2\`?" "$tmp/sug" || {
		echo "expected a suggestion of '$2' for: $1"
		cat "$tmp/sug"
		return 1
	}
}

# a keyword, a builtin, and a two-edit typo
assert_suggests 'pritn("x")' print
assert_suggests 'lenn("x")' len
assert_suggests 'spilt("a,b", ",")' split
assert_suggests 'execc("ls")' exec

# too far to guess: a suggestion here would be a guess, and a wrong
# suggestion is worse than none
printf 'widow_file("a")\n' >"$tmp/t.fl"
set +e
"$FLINT" --color=never --error-format=human "$tmp/t.fl" >"$tmp/nosug" 2>&1
set -e
if grep -q 'did you mean' "$tmp/nosug"; then
	echo "suggested something for a name that is four edits away"
	cat "$tmp/nosug"
	exit 1
fi

# --- option order ---
#
# flint's own options used to be recognised only *before* the script path, so
# `flint bad.fl --error-format=human` quietly printed the old format while
# `flint --error-format=human bad.fl` printed the new one. Two spellings of
# one command, found the first time anybody typed it the natural way.

printf 'print("x"\n' >"$tmp/bad.fl"

for order in "before" "after"; do
	if [ "$order" = before ]; then
		out=$("$FLINT" --color=never --error-format=human "$tmp/bad.fl" 2>&1 || true)
	else
		out=$("$FLINT" "$tmp/bad.fl" --color=never --error-format=human 2>&1 || true)
	fi
	grep -q 'error\[E0102\]' <<EOF
$out
EOF
done

# short flags stay positional, because they are the ones a script wants for
# itself. `flint t.fl -v` is a script asking for "-v", not a version request.
printf 'print(args())\n' >"$tmp/args.fl"
got=$("$FLINT" "$tmp/args.fl" -v 2>&1)
[ "$got" = '["-v"]' ] || {
	echo "expected -v to reach the script, got: $got"
	exit 1
}

# `--` ends flint's scanning, for a script that really does want one of these
# as an argument
got=$("$FLINT" "$tmp/args.fl" -- --color=always 2>&1)
[ "$got" = '["--color=always"]' ] || {
	echo "expected -- to stop option scanning, got: $got"
	exit 1
}

# a bad value is refused, with the accepted values, rather than ignored
if "$FLINT" --error-format=bogus "$tmp/args.fl" >/dev/null 2>"$tmp/err"; then
	echo "accepted an unknown error format"
	exit 1
fi
grep -q 'human, short or json' "$tmp/err"

