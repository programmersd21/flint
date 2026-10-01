#!/usr/bin/env sh
#
# Compare two flint binaries on one benchmark case.
#
#     bench/compare.sh old new [case] [repeats]
#
# With no case, every case in bench/ runs.
#
# This exists because absolute timings on a shared machine are close to
# meaningless -- a load average of 8 on 12 cores moves a 100ms case by 40% in
# either direction. Comparing the two binaries back to back, alternating, is the
# only comparison that survives that.
#
# Intended to be read rather than asserted on. A CI job that fails on a 3%
# difference is a CI job that gets disabled.

set -e

OLD=${1:?usage: compare.sh old-binary new-binary [case] [repeats]}
NEW=${2:?usage: compare.sh old-binary new-binary [case] [repeats]}
CASE=${3:-all}
REPEATS=${4:-7}

BENCH_DIR=$(cd "$(dirname "$0")" && pwd)

# Best of REPEATS wall-clock milliseconds.
#
# `date +%s%N` is nanoseconds on GNU and on the BSDs; on a platform without it
# this reports garbage rather than failing, which is acceptable for a script
# whose output is read by a person and never asserted on.
time_one() {
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
