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
grep -q 'machine-applicable' "$tmp/human"

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
