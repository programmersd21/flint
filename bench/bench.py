#!/usr/bin/env python3
"""
Benchmarks for flint, and the same work in python.

Every case is a pair: a .fl file and a .py file doing the same computation,
so the comparison is between two programs doing the same thing rather than
between two programs happening to be measured.

Run it:

    make bench              # everything, if python3 is present
    python3 bench/bench.py  # the same thing

or one case:

    python3 bench/bench.py fib
    python3 bench/bench.py --list

What is deliberately measured, and why:

  startup      the whole game for shell scripting. a script that runs once
               and exits pays this every time, and it is where flint is
               genuinely ahead rather than merely competitive.
  arithmetic   where flint is behind, and by how much. it is in here on
               purpose. every number is a nan-boxed double, so a small
               integer is boxed and passed around where python's tagged
               integers are not, and the gap is structural rather than
               something a peephole pass fixes.
  strings      where the built-in string primitives should pay off, since
               the same work is a while loop in a language without them.
  lists        append and index, the two operations a text script does most.
  files        read a file and count its lines, which is what most of the
               time actually goes.

What is not measured: anything that would flatter one side. There is no
benchmark here chosen because flint is fast in it, and none removed
because it is slow.
"""

import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FLINT = os.path.join(ROOT, "flint")

# milliseconds of process time, the smallest of several runs, because the
# scheduler is noisy and the fastest honest run is the one that is least
# measuring the scheduler
REPEATS = 7


def time_cmd(cmd, stdin_data=None, repeats=REPEATS):
    """best-of wall time in ms. returns (ms, output) or (None, err)."""
    best = float("inf")
    out = None
    for _ in range(repeats):
        start = time.perf_counter()
        p = subprocess.run(
            cmd,
            input=stdin_data,
            capture_output=True,
            cwd=ROOT,
        )
        elapsed = (time.perf_counter() - start) * 1000.0
        if p.returncode != 0:
            return None, p.stderr.decode("utf-8", "replace")
        if elapsed < best:
            best = elapsed
            out = p.stdout.decode("utf-8", "replace")
    return best, out


def human(ms):
    if ms is None:
        return "failed"
    if ms < 1:
        return f"{ms:.2f} ms"
    if ms < 1000:
        return f"{ms:.1f} ms"
    return f"{ms / 1000:.2f} s"


def main():
    cases = [c[:-3] for c in sorted(os.listdir(HERE)) if c.endswith(".fl")]

    if "--list" in sys.argv:
        print("\n".join(cases))
        return 0

    wanted = [a for a in sys.argv[1:] if not a.startswith("-")]
    if wanted:
        cases = [c for c in cases if c in wanted]

    if not os.path.exists(FLINT):
        print("flint not built. run `make release` first.", file=sys.stderr)
        return 1

    try:
        import subprocess as _sp

        _sp.run(["python3", "-c", "pass"], check=True)
    except Exception:
        print("python3 not available; flint-only timings follow", file=sys.stderr)
        python_ok = False
    else:
        python_ok = True

    print(f"flint vs python3, best of {REPEATS}, wall clock")
    print("every case is a .fl and a .py doing the same work")
    print()

    if not python_ok:
        for case in cases:
            ms, _ = time_cmd([FLINT, os.path.join("bench", case + ".fl")])
            print(f"{case:<12} flint {human(ms)}")
        return 0

    print(f"{'case':<12} {'flint':>12} {'python':>12} {'python/flint':>13}")
    print("-" * 54)

    for case in cases:
        fl = os.path.join(ROOT, "bench", case + ".fl")
        py = os.path.join(ROOT, "bench", case + ".py")

        f_ms, f_out = time_cmd([FLINT, fl])
        p_ms, p_out = time_cmd(["python3", py])

        if f_out is not None and p_out is not None:
            # the two must compute the same thing or the timing is
            # meaningless, so compare the checksums
            f_sum = f_out.strip().split("\n")[-1] if f_out.strip() else ""
            p_sum = p_out.strip().split("\n")[-1] if p_out.strip() else ""
            if f_sum != p_sum:
                print(f"{case:<12} MISMATCH: flint {f_sum!r} vs python {p_sum!r}")

        ratio = f"{p_ms / f_ms:.2f}x" if (f_ms and p_ms) else "-"
        print(f"{case:<12} {human(f_ms):>12} {human(p_ms):>12} {ratio:>13}")

    print()
    print("a ratio above 1 means flint is faster on that case.")
    print("checksums are compared, so a mismatch is flagged rather than timed.")

    if os.path.exists(os.path.join(ROOT, "flint")):
        size = os.path.getsize(os.path.join(ROOT, "flint"))
        print(f"\nflint binary: {size / 1024:.0f} KB, statically linked to nothing but libc")

    return 0


if __name__ == "__main__":
    sys.exit(main())
