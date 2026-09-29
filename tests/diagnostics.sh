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

# --- span precision ---
#
# Both of these used to point somewhere unhelpful.
#
# A missing `)` was reported at end of file, so the caret sat on the line
# *after* the code that was actually wrong. The insertion point for a
# delimiter is the end of the previous token, which is where it belongs.
#
# A runtime index error underlined the whole line, so `print(xs[10])` got a
# caret across the whole call rather than across the subscript. Each code
# byte now records the source offset it came from, and the end of one
# instruction is the start of the next, so the failing expression falls out
# of two array reads.

printf 'print("x"\n' >"$tmp/nodelim.fl"
"$FLINT" --color=never --error-format=human "$tmp/nodelim.fl" \
	>"$tmp/nodelim.out" 2>&1 || true

grep -q 'nodelim.fl:1:' "$tmp/nodelim.out" || {
	echo "missing-delimiter error should point at the end of line 1"
	cat "$tmp/nodelim.out"
	exit 1
}
if grep -q 'nodelim.fl:2:' "$tmp/nodelim.out"; then
	echo "missing-delimiter error still points at end of file"
	cat "$tmp/nodelim.out"
	exit 1
fi

printf 'let xs = [1, 2, 3]\nprint(xs[10])\n' >"$tmp/idx.fl"
"$FLINT" --color=never --error-format=human "$tmp/idx.fl" \
	>"$tmp/idx.out" 2>&1 || true
grep -q 'xs\[10\]' "$tmp/idx.out" || {
	echo "index error should underline the subscript, not the line"
	cat "$tmp/idx.out"
	exit 1
}

printf 'let s = "abc"\nprint(s[99])\n' >"$tmp/stridx.fl"
"$FLINT" --color=never --error-format=human "$tmp/stridx.fl" \
	>"$tmp/stridx.out" 2>&1 || true
grep -q 's\[99\]' "$tmp/stridx.out" || {
	echo "string index error should underline the subscript"
	cat "$tmp/stridx.out"
	exit 1
}

echo "diagnostic tests passed"

# --- secondary labels ---
#
# A redeclaration used to say "already a variable with this name" and stop.
# The reader then had to go and find the other one. Both spans are now shown.

printf 'fn f() {\n    let x = 1\n    let x = 2\n}\n' >"$tmp/dup.fl"
out=$("$FLINT" --color=never --error-format=human "$tmp/dup.fl" 2>&1 || true)
grep -q 'already defined in this scope' <<EOF
$out
EOF
grep -q 'defined again here' <<EOF
$out
EOF
grep -q 'first defined here' <<EOF
$out
EOF
# the secondary span must carry its own line of source, not be printed as a
# bare sentence the reader has to verify
if ! grep -q '2 |     let x = 1' <<EOF
$out
EOF
then
	echo "secondary label has no source line of its own"
	cat "$tmp/dup.fl" "$out"
	exit 1
fi

# --- the summary line ---
#
# One line saying how many things were wrong, so that fixing the first error
# and re-running tells you whether that was the whole job.

printf 'fn f() {\n    let x = 1\n    let x = 2\n}\n' >"$tmp/one.fl"
out=$("$FLINT" --color=never --error-format=short "$tmp/one.fl" 2>&1 || true)
grep -q 'due to 1 error' <<EOF
$out
EOF
# and it must not say "1 errors"
if grep -q 'due to 1 errors' <<EOF
$out
EOF
then
	echo "summary is not pluralised"
	exit 1
fi

# --- option order and the new options ---
#
# flint's own options used to be recognised only *before* the script path, so
# `flint bad.fl --error-format=human` quietly printed the old format while
# `flint --error-format=human bad.fl` printed the new one.

printf 'print("x"\n' >"$tmp/bad.fl"
for order in before after; do
	if [ "$order" = before ]; then
		out=$("$FLINT" --color=never --error-format=human "$tmp/bad.fl" 2>&1 || true)
	else
		out=$("$FLINT" "$tmp/bad.fl" --color=never --error-format=human 2>&1 || true)
	fi
	grep -q 'error\[E0102\]' <<EOF
$out
EOF
done

printf 'print(args())\n' >"$tmp/args.fl"
got=$("$FLINT" "$tmp/args.fl" -v 2>&1)
[ "$got" = '["-v"]' ] || {
	echo "expected -v to reach the script, got: $got"
	exit 1
}
got=$("$FLINT" "$tmp/args.fl" -- --color=always 2>&1)
[ "$got" = '["--color=always"]' ] || {
	echo "expected -- to stop option scanning, got: $got"
	exit 1
}
got=$("$FLINT" --warnings=none "$tmp/args.fl" 2>&1)
[ "$got" = '[]' ] || {
	echo "--warnings= should be accepted before the path, got: $got"
	exit 1
}
if "$FLINT" --warnings=bogus "$tmp/args.fl" >/dev/null 2>&1; then
	echo "accepted an unknown warning mode"
	exit 1
fi

# --quiet drops the repl prompt and nothing else
got=$(printf 'print(7)\n' | "$FLINT" --quiet 2>&1 | tr -d '\n')
[ "$got" = "7" ] || {
	echo "expected a bare 7 from --quiet, got: $got"
	exit 1
}
got=$(printf 'print(7)\n' | "$FLINT" 2>&1 | tr -d '\n')
if [ "$got" = "7" ]; then
	echo "expected the repl prompt without --quiet, got: $got"
	exit 1
fi

# --- lowercase messages ---
#
# every diagnostic in the language is lowercase, matching the rest of the
# repository. the language tests cover the text; this catches a new one being
# added in the wrong case.

printf 'pritn("x")\n' >"$tmp/case.fl"
out=$("$FLINT" --color=never --error-format=short "$tmp/case.fl" 2>&1 || true)
if grep -q 'error\[E[0-9]*\]: [A-Z]' <<EOF
$out
EOF
then
	echo "diagnostic message starts with a capital"
	cat "$tmp/case.fl" "$out"
	exit 1
fi


# --- --fix ---
#
# --fix only ever applied a fix when the missing delimiter was at end of file.
# The common case is a missing brace in the middle of a function, and that got
# neither a fix nor the right span. Two bugs with the same cause: the offer was
# gated on the token being EOF, and the matcher compared "Expect '}'" against a
# message that had since been lowercased to "expect '}'", so only `)` ever
# matched at all.

printf 'fn f() {\n    let a = 1\n' >"$tmp/brace.fl"
cp "$tmp/brace.fl" "$tmp/brace.orig"
"$FLINT" --color=never "$tmp/brace.fl" --fix >"$tmp/fixout" 2>&1 || true
if cmp -s "$tmp/brace.fl" "$tmp/brace.orig"; then
	echo "--fix did not add a missing closing brace"
	cat "$tmp/fixout"
	exit 1
fi
# the fixed file must actually compile, which is the whole point of a fix
if ! "$FLINT" "$tmp/brace.fl" >/dev/null 2>&1; then
	echo "--fix produced a file that still does not compile"
	cat "$tmp/brace.fl"
	exit 1
fi

# the fix lands at the end of the previous token, not at the parser's position
out=$("$FLINT" --color=never --error-format=short "$tmp/brace.orig" 2>&1 || true)
grep -q "brace.orig:2:" <<EOF
$out
EOF

# --- error counts ---
#
# The legacy format set had_error without incrementing the counter, so the
# summary said "due to 0 errors" directly under three errors it had just
# printed. A summary that contradicts the lines above it is worse than none.

printf 'let x =\n' >"$tmp/one.fl"
for fmt in short human; do
	out=$("$FLINT" --color=never --error-format=$fmt "$tmp/one.fl" 2>&1 || true)
	grep -q 'due to 1 error' <<EOF
$out
EOF
done
# and the default format, which is the one people actually run
out=$("$FLINT" "$tmp/one.fl" 2>&1 || true)
grep -q 'due to 1 error' <<EOF
$out
EOF

# --- the repl ---
#
# It opened to a bare cursor, which tells a first-time user nothing. It now
# says what it is and what to type, and :help explains the one limitation
# that surprises people, which is that an unclosed block is an error.

out=$(printf ':help\n' | "$FLINT" 2>&1)
grep -q 'flint' <<EOF
$out
EOF
grep -q ':quit' <<EOF
$out
EOF
grep -q 'multi-line' <<EOF
$out
EOF

# --quiet must print neither banner nor prompt
out=$(printf 'print(1)\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "1" ] || {
	echo "expected just 1 from --quiet, got: $out"
	exit 1
}

# and the repl still keeps state across lines
out=$(printf 'let x = 40\nprint(x + 2)\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "42" ] || {
	echo "repl lost state between lines, got: $out"
	exit 1
}


# --- the repl echoes expressions ---
#
# `1 + 2` in a script is an expression statement: evaluated, then thrown away,
# which is right for a script. At a repl the person typing it is asking for the
# answer, and a repl that says nothing is a calculator with the screen off.

out=$(printf '1 + 2\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "3" ] || {
	echo "expected the repl to echo 3, got: $out"
	exit 1
}
out=$(printf '"hi"\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "hi" ] || {
	echo "expected a string to echo bare, got: $out"
	exit 1
}
out=$(printf '[1, 2, 3]\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "[1, 2, 3]" ] || {
	echo "expected a list to echo as itself, got: $out"
	exit 1
}

# a statement is not an expression and must not print anything
out=$(printf 'let x = 5\n' | "$FLINT" --quiet 2>&1)
[ -z "$out" ] || {
	echo "a let should print nothing, got: $out"
	exit 1
}
out=$(printf 'print(7)\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "7" ] || {
	echo "print should print exactly once, got: $out"
	exit 1
}

# state carries across lines, and an error does not end the session
out=$(printf 'let x = 5\nx * 2\n' | "$FLINT" --quiet 2>&1)
[ "$out" = "10" ] || {
	echo "repl lost state, got: $out"
	exit 1
}
out=$(set +e; printf 'nope\n1 + 1\n' | "$FLINT" --quiet 2>&1 | tail -1)
[ "$out" = "2" ] || {
	echo "an error should not end the repl, got: $out"
	exit 1
}

# and the exit code is still 70 when a line failed, so a shell can tell.
# set +e because the failure is the point and `set -e` would abort on it.
set +e
printf 'nope\n' | "$FLINT" --quiet >/dev/null 2>&1
repl_status=$?
set -e
[ "$repl_status" -eq 70 ] || {
	echo "expected exit 70 from a failing repl line, got $repl_status"
	exit 1
}

