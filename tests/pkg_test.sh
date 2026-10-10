#!/bin/sh
#
# The package manager, end to end, against real fixtures.
#
# These are integration tests: they copy a project into a temporary
# directory, run `flint pkg` there, and check what came out. No network:
# the git fixture is a local repository reached over file://, which is
# exactly what the manifest format accepts and needs no registry.
#
# HOME is redirected into the temp directory so the git mirror cache
# cannot reach, or be polluted by, the developer's real ~/.flint.
#
# usage: sh tests/pkg_test.sh [path-to-flint]

set -e

FLINT=$(cd "$(dirname "$1")" && pwd)/flint
[ -x "$FLINT" ] || FLINT="${1:-./flint}"

REPO=$(pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

failed=0
checks=0

# in-place sed that works on GNU and BSD: BSD sed needs an argument to
# -i and GNU forbids the same form, so a temp file plus mv is the one
# version that edits the same bytes everywhere. the macOS probe found
# this: `sed -i` without a suffix is "invalid command code" there.
sedi() {
	expr="$1"
	file="$2"
	tmp="$file.sedit"
	sed "$expr" "$file" >"$tmp" && mv "$tmp" "$file"
}

fail() {
	echo "FAIL: $1"
	failed=$((failed + 1))
}

expect() {
	# expect <label> <expected> <actual>
	checks=$((checks + 1))
	if [ "$2" != "$3" ]; then
		echo "FAIL: $1"
		echo "  expected: $2"
		echo "  got:      $3"
		failed=$((failed + 1))
	fi
}

expect_contains() {
	checks=$((checks + 1))
	case "$3" in
	*"$2"*) ;;
	*)
		echo "FAIL: $1"
		echo "  expected to contain: $2"
		echo "  got:                $3"
		failed=$((failed + 1))
		;;
	esac
}

export HOME="$WORK/home"
mkdir -p "$HOME"

# --- path dependencies -------------------------------------------------
cp -r "$REPO/tests/pkg" "$WORK/pathproj"
cd "$WORK/pathproj/app"

out=$("$FLINT" pkg install 2>&1)
expect_contains "path install reports installed" "installed greet 1.2.0" "$out"
expect "lock exists" "yes" "$([ -f flint.lock ] && echo yes || echo no)"
expect "module copied" "yes" \
	"$([ -f flint_modules/greet/main.fl ] && echo yes || echo no)"
expect "no .git in a path copy" "no" \
	"$([ -d flint_modules/greet/.git ] && echo yes || echo no)"
expect "list shows the dependency" "greet 1.2.0 (../greet)" \
	"$("$FLINT" pkg list 2>&1)"

out=$("$FLINT" main.fl 2>&1)
expect "package import works" "hi sam
HI SAM" "$out"

# reinstall is idempotent
"$FLINT" pkg install >/dev/null 2>&1
expect "reinstall keeps the module" "yes" \
	"$([ -f flint_modules/greet/main.fl ] && echo yes || echo no)"

# the lock records a content hash of what was copied
content=$(sed -n '/^content = /s/content = "\(.*\)"/\1/p' flint.lock)
expect "lock records a 64-char content hash" "64" "${#content}"

# a modified tree heals from the live source on the next install
echo "tampered" >> flint_modules/greet/main.fl
out=$("$FLINT" pkg install 2>&1)
expect_contains "modified tree reinstalls" "installed greet 1.2.0" "$out"
expect "modified tree is healed" "no" \
	"$(grep -q tampered flint_modules/greet/main.fl && echo yes || echo no)"

# a version requirement that cannot hold fails loudly
sedi 's/version = "\^1.0.0"/version = "^9.0.0"/' flint.toml
out=$("$FLINT" pkg install 2>&1) && code=0 || code=$?
expect "bad requirement exits non-zero" "1" "$([ $code -ne 0 ] && echo 1 || echo 0)"
expect_contains "bad requirement explains" "does not satisfy" "$out"

cd "$WORK" || exit 1

# --- git dependencies ---------------------------------------------------
if ! command -v git >/dev/null 2>&1; then
	echo "git fixtures: skipped (no git)"
	echo "pkg tests: $checks checks, $failed failures"
	[ "$failed" -eq 0 ] || exit 1
	exit 0
fi

mkdir -p "$WORK/gitsrc"
cat > "$WORK/gitsrc/flint.toml" <<'EOF'
[package]
name = "shout"
version = "0.3.0"
EOF
cat > "$WORK/gitsrc/main.fl" <<'EOF'
export fn loud(who) {
    return "HEY " + who
}
EOF
git init -q "$WORK/gitsrc"
git -C "$WORK/gitsrc" add -A
git -C "$WORK/gitsrc" \
	-c user.email=test@example.com -c user.name=test \
	commit -qm "first"
git -C "$WORK/gitsrc" tag v0.3.0

mkdir -p "$WORK/gitproj"
cat > "$WORK/gitproj/flint.toml" <<'EOF'
[package]
name = "gitproj"
version = "0.1.0"
EOF
cat > "$WORK/gitproj/use.fl" <<'EOF'
import "shout"
print(shout.loud("zed"))
EOF
cd "$WORK/gitproj" || exit 1

out=$("$FLINT" pkg add "file://$WORK/gitsrc" 2>&1)
expect_contains "git add installs" "installed shout 0.3.0" "$out"
expect_contains "git add records the URL" "git = " "$(cat flint.toml)"
expect "git module has no .git" "no" \
	"$([ -d flint_modules/shout/.git ] && echo yes || echo no)"
expect "git import works" "HEY zed" "$("$FLINT" use.fl 2>&1)"

# the lock pins a commit, and a second install reinstalls that pin
pin=$(sed -n '/^commit = /s/commit = "\(.*\)"/\1/p' flint.lock)
expect "lock pins a 40-char commit" "40" "${#pin}"

# A damaged mirror is replaced in one install, without following symlinks.
mirror=
for candidate in "$HOME"/.flint/git/gitsrc-*; do
	[ -d "$candidate" ] && { mirror=$candidate; break; }
done
expect "git mirror cache exists" "yes" "$([ -n "$mirror" ] && echo yes || echo no)"
rm -rf flint_modules/shout
rm -f "$mirror/HEAD" "$mirror/config"
out=$("$FLINT" pkg install 2>&1) && code=0 || code=$?
expect "damaged mirror recovers in one install" "0" "$code"
expect "recovered mirror still imports" "HEY zed" "$("$FLINT" use.fl 2>&1)"

# A symlink at the cache path must be unlinked, never recursively followed.
target="$WORK/mirror-target"
mkdir -p "$target"
echo "keep me" > "$target/marker"
rm -rf flint_modules/shout "$mirror"
ln -s "$target" "$mirror"
out=$("$FLINT" pkg install 2>&1) && code=0 || code=$?
expect "symlink mirror is safely replaced" "0" "$code"
expect "symlink target data is preserved" "keep me" "$(cat "$target/marker")"
expect "replacement mirror still imports" "HEY zed" "$("$FLINT" use.fl 2>&1)"

cat > "$WORK/gitsrc/main.fl" <<'EOF'
export fn loud(who) {
    return "LOUDER " + who
}
EOF
git -C "$WORK/gitsrc" add -A
git -C "$WORK/gitsrc" \
	-c user.email=test@example.com -c user.name=test \
	commit -qm "second"

rm -rf flint_modules
"$FLINT" pkg install >/dev/null 2>&1
expect "install keeps the pin" "HEY zed" "$("$FLINT" use.fl 2>&1)"

"$FLINT" pkg update shout >/dev/null 2>&1
expect "update moves the pin" "LOUDER zed" "$("$FLINT" use.fl 2>&1)"
newpin=$(sed -n '/^commit = /s/commit = "\(.*\)"/\1/p' flint.lock)
checks=$((checks + 1))
if [ "$newpin" = "$pin" ]; then
	echo "FAIL: update did not change the pin"
	failed=$((failed + 1))
fi

out=$("$FLINT" pkg update nosuch 2>&1) && code=0 || code=$?
expect_contains "update of an unknown name explains" "no dependency" "$out"

# content hashes: recorded beside the pin, verified on reinstall,
# enforced against tampering
content=$(sed -n '/^content = /s/content = "\(.*\)"/\1/p' flint.lock)
expect "git lock records a 64-char content hash" "64" "${#content}"

out=$("$FLINT" pkg install 2>&1)
expect_contains "unchanged reinstall verifies in place" "verified shout" "$out"

cp flint.lock "$WORK/gitlock.good"
first=$(sed -n '/^content = /s/content = "\(.\).*/\1/p' flint.lock)
if [ "$first" = "0" ]; then rep=1; else rep=0; fi
sedi "s/^content = \"$first/content = \"$rep/" flint.lock
rm -rf flint_modules
out=$("$FLINT" pkg install 2>&1) && code=0 || code=$?
expect "content mismatch exits non-zero" "1" "$([ $code -ne 0 ] && echo 1 || echo 0)"
expect_contains "content mismatch names the package" "does not match flint.lock" "$out"
expect "content mismatch keeps the lock" "yes" \
	"$([ -f flint.lock ] && echo yes || echo no)"
expect "content mismatch removes the suspect tree" "no" \
	"$([ -d flint_modules/shout ] && echo yes || echo no)"
cp "$WORK/gitlock.good" flint.lock
"$FLINT" pkg install >/dev/null 2>&1
expect "restored lock reinstalls the pin" "LOUDER zed" "$("$FLINT" use.fl 2>&1)"

# a rev pins to that tag regardless of what the branch has since done
cat > flint.toml <<'EOF'
[package]
name = "gitproj"
version = "0.1.0"

[dependencies]
shout = { git = "file:///PLACEHOLDER", rev = "v0.3.0" }
EOF
sedi "s|file:///PLACEHOLDER|file://$WORK/gitsrc|" flint.toml
"$FLINT" pkg install >/dev/null 2>&1
expect "rev pins to the tag" "HEY zed" "$("$FLINT" use.fl 2>&1)"

# a dead URL fails at add time and leaves no manifest entry behind
out=$("$FLINT" pkg add "file://$WORK/nope" 2>&1) && code=0 || code=$?
expect "dead url exits non-zero" "1" "$([ $code -ne 0 ] && echo 1 || echo 0)"
expect "dead url writes nothing" "no" \
	"$(grep -q "$WORK/nope" flint.toml && echo yes || echo no)"

# --- uninstall-by-edit: a dropped dependency takes its directory -------
cat > flint.toml <<'EOF'
[package]
name = "gitproj"
version = "0.1.0"
EOF
"$FLINT" pkg install >/dev/null 2>&1
expect "dropped dependency removed" "no packages installed." "$("$FLINT" pkg list 2>&1)"
expect "its directory is gone" "no" \
	"$([ -d flint_modules/shout ] && echo yes || echo no)"

cd "$WORK" || exit 1
# --- transitive dependencies --------------------------------------------
# a package that depends on another, with the nested path written relative
# to the package that declares it. this is the shape that used to install
# `mid` and leave it unable to import `base`, because the dependency's own
# flint.toml was mistaken for the project root.
mkdir -p "$WORK/chain/base" "$WORK/chain/mid" "$WORK/chain/app"
cat > "$WORK/chain/base/flint.toml" <<'EOF2'
[package]
name = "base"
version = "1.0.0"
EOF2
cat > "$WORK/chain/base/main.fl" <<'EOF2'
export fn ping() {
    return "pong"
}
EOF2
cat > "$WORK/chain/mid/flint.toml" <<'EOF2'
[package]
name = "mid"
version = "1.0.0"

[dependencies]
base = { path = "../base" }
EOF2
cat > "$WORK/chain/mid/main.fl" <<'EOF2'
import "base"

export fn twice() {
    return base.ping() + base.ping()
}
EOF2
cat > "$WORK/chain/app/flint.toml" <<'EOF2'
[package]
name = "app"
version = "0.1.0"

[dependencies]
mid = { path = "../mid" }
EOF2
printf 'import "mid"\nimport "base"\nprint(mid.twice())\nprint(base.ping())\n' \
	> "$WORK/chain/app/run.fl"
cd "$WORK/chain/app" || exit 1
out=$("$FLINT" pkg install 2>&1)
expect_contains "a transitive dependency installs" "installed base 1.0.0" "$out"
expect "both packages are present" "base mid" \
	"$(ls flint_modules | sort | tr '\n' ' ' | sed 's/ $//')"
out=$("$FLINT" run.fl 2>&1) && status=0 || status=$?
expect "a package can import what it depends on" 0 "$status"
expect_contains "and the call works" "pongpong" "$out"
expect_contains "the transitive package is importable directly" "pong" "$out"
# the lockfile records the nested dependency with the path relative to the
# project, not to whichever package mentioned it
expect_contains "the lock records the nested source" "../base" \
	"$(cat flint.lock)"
# reinstalling is still idempotent
"$FLINT" pkg install >/dev/null 2>&1
expect "reinstalling the chain succeeds" 0 "$?"

cd "$WORK" || exit 1
if [ "$failed" -ne 0 ]; then
	echo "pkg tests: $checks checks, $failed failures"
	exit 1
fi
echo "pkg tests: $checks checks passed"
