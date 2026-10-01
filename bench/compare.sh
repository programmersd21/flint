#!/usr/bin/env sh
#
# Compare two flint binaries on one benchmark case, best of N.
#
#     bench/compare.sh /path/to/old /path/to/new [case] [repeats]
#
# Exits 0 always; the numbers are the output. Intended to be read, not
# asserted on, because a microbenchmark on a shared machine is a noisy
# measurement and a CI job that fails on 3% is a CI job that gets disabled.

set -e

OLD=${1:?old binary}
NEW=${2:?new binary}
CASE=${3:-all}
REPEATS=${4:-9}

BENCH_DIR=$(dirname "$0")

time_one() {
	# Best of REPEATS wall-clock milliseconds. Best-of rather than mean,
	# because the scheduler is the noise source and the fastest honest run
	# is the one that was interrupted least.
	bin=$1
	file=$2
	best=999999999
	i=0
	while [ "$i" -lt "$REPEATS" ]; do
		start=$(date +%s%N)
		"$bin" "$file" >/dev/null 2>&1 || true
		end=$(date +%s%N)
		ms=$(( (end - start) / 1000000 ))
		if [ "$ms" -lt "$best" ]; then
			best=$ms
		fi
		i=$((i + 1))
	done
	echo "$best"
}

run_case() {
	case_file=$1
	name=$(basename "$case_file" .fl)
	old_ms=$(time_one "$OLD" "$case_file")
	new_ms=$(time_one "$NEW" "$case_file")
	printf "%-14s old %7s ms   new %7s ms   " "$name" "$old_ms" "$new_ms"
	awk -v a="$old_ms" -v b="$new_ms" 'BEGIN {
		if (a == 0 || b == 0) { print "n/a"; exit }
		r = a / b
		printf "%+6.1f%%  (%.2fx)\n", (r - 1) * 100, r
	}'
}

cd "$BENCH_DIR/.." || exit 1

printf "%-14s %-21s %-21s %s\n" case old new change
printf -- "--------------------------------------------------------------\n"

if [ "$CASE" = "all" ]; then
	for f in "$BENCH_DIR"/*.fl; do
		run_case "$f"
	done
else
	run_case "$BENCH_DIR/$CASE.fl"
fi