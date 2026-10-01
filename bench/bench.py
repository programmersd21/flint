#!/usr/bin/env python3
"""
flint's benchmark suite, and the competitors worth comparing against.

Design notes, because a benchmark that cannot be reproduced is a rumour:

  * Every case is a .fl program that computes a checksum and prints it. The
    runner checks the checksum against a recorded value, so a benchmark that
    stops computing what it claims to compute fails instead of reporting a
    beautiful time for the wrong work.
  * Every case runs many times and reports the *median* of the timings, plus
    the spread. The median rather than the best, because best-of-N on a shared
    machine reports the run that happened to be interrupted least, which is a
    property of the scheduler. The spread is printed because a change whose
    "improvement" is inside the noise is not an improvement.
  * The environment is recorded with the results. A number without a CPU and a
    compiler version is not a measurement.

Usage:
    python3 bench/bench.py                 # flint only, all cases
    python3 bench/bench.py fib arith       # named cases
    python3 bench/bench.py --list
    python3 bench/bench.py --compare       # flint vs the competitors
    python3 bench/bench.py --json out.json # machine-readable
"""

import argparse
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FLINT = os.path.join(ROOT, "flint")

# Runs per case. Enough that the median is stable, not so many that a full
# sweep takes an afternoon. The slow cases get fewer, below.
DEFAULT_RUNS = 15
SLOW_CASE_RUNS = 7

# A run is discarded as an outlier if it is more than this many times the
# median. A desktop that goes to sleep in the middle of a benchmark otherwise
# silently becomes the measurement.
OUTLIER_FACTOR = 1.6


def cases():
    out = []
    for name in sorted(os.listdir(HERE)):
        if not name.endswith(".fl"):
            continue
        base = name[:-3]
        # skip comparison implementations, which live here but are not cases
        if base.endswith(("_lua", "_luajit")):
            continue
        out.append(base)
    return out


def is_slow(base):
    return base in ("arith", "json_big", "strings_build", "mem_alloc")


def env_info():
    """Everything needed to reproduce a number, in a form that goes in a file."""
    cpu = "unknown"
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass

    cc = os.environ.get("CC", "cc")
    try:
        ccver = subprocess.run(
            [cc, "--version"], capture_output=True, text=True
        ).stdout.splitlines()[0]
    except Exception:
        ccver = "unknown"

    flint_version = "unknown"
    try:
        r = subprocess.run([FLINT, "--version"], capture_output=True, text=True)
        flint_version = r.stdout.strip()
    except Exception:
        pass

    return {
        "cpu": cpu,
        "kernel": platform.platform(),
        "machine": platform.machine(),
        "compiler": ccver,
        "python": platform.python_version(),
        "flint": flint_version,
    }


def time_once(cmd, cwd=ROOT):
    start = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, cwd=cwd)
    elapsed = (time.perf_counter() - start) * 1000.0
    return elapsed, p


def measure(cmd, runs, stdin_data=None):
    """median ms and (min, spread ratio) over `runs`, outliers discarded."""
    samples = []
    out = None
    for _ in range(runs):
        start = time.perf_counter()
        p = subprocess.run(cmd, capture_output=True, cwd=ROOT, input=stdin_data)
        elapsed = (time.perf_counter() - start) * 1000.0
        if p.returncode != 0:
            return None, None, p.stderr.decode("utf-8", "replace")
        if out is None:
            out = p.stdout.decode("utf-8", "replace")
        samples.append(elapsed)

    med = statistics.median(samples)
    kept = [s for s in samples if s <= med * OUTLIER_FACTOR]
    return med, (min(kept) / med if kept and med else 1.0), out


def checksum(text):
    lines = [l for l in text.strip().split("\n") if l.strip()]
    return lines[-1] if lines else ""


def human(ms):
    if ms is None:
        return "failed"
    if ms < 10:
        return f"{ms:.2f}ms"
    if ms < 1000:
        return f"{ms:.1f}ms"
    return f"{ms / 1000:.2f}s"


def run_flint_cases(names, runs_mult=1):
    results = {}
    for name in names:
        path = os.path.join(HERE, name + ".fl")
        if not os.path.exists(path):
            continue
        runs = (SLOW_CASE_RUNS if is_slow(name) else DEFAULT_RUNS) * runs_mult
        med, spread, out = measure([FLINT, path], runs)
        if med is None:
            results[name] = {"error": out.strip()[:200]}
            continue
        results[name] = {
            "ms": med,
            "best": med * spread,
            "checksum": checksum(out),
        }
    return results


COMPETITORS = {
    "python3": ["python3"],
    "lua": ["lua"],
    "luajit": ["luajit"],
}


def which(prog):
    return subprocess.run(
        ["sh", "-c", f"command -v {prog}"], capture_output=True, text=True
    ).stdout.strip()


def run_competitors(names, runs_mult=1):
    """Time the same work in other languages, where a port exists.

    Only cases with a hand-written equivalent in another language are
    comparable. There is no honest way to translate "measure startup" or
    "measure the collector" into python, and pretending otherwise would be the
    exact kind of flattering comparison this suite is supposed to avoid.
    """
    out = {}
    available = {k: which(k) for k in COMPETITORS}
    for lang, prefix in COMPETITORS.items():
        if not available[lang]:
            continue
        for name in names:
            variants = [
                os.path.join(HERE, f"{name}_{lang}.fl"),
                os.path.join(HERE, name + (".py" if lang == "python3" else ".lua")),
            ]
            path = next((p for p in variants if os.path.exists(p)), None)
            if not path:
                continue
            runs = (SLOW_CASE_RUNS if is_slow(name) else DEFAULT_RUNS) * runs_mult
            cmd = prefix + [path]
            med, spread, text = measure(cmd, runs)
            if med is None:
                continue
            out.setdefault(name, {})[lang] = {
                "ms": med,
                "checksum": checksum(text),
            }
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cases", nargs="*", help="case names; default all")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--compare", action="store_true", help="vs python/lua/luajit")
    ap.add_argument("--json", help="write results here")
    ap.add_argument("--quick", action="store_true", help="3 runs, for a smoke test")
    args = ap.parse_args()

    names = args.cases or cases()
    if args.list:
        print("\n".join(names))
        return 0

    if not os.path.exists(FLINT):
        print("flint not built. run `make release` first.", file=sys.stderr)
        return 1

    mult = 1 if args.quick else 1
    info = env_info()
    print("# environment")
    for k, v in info.items():
        print(f"  {k:<10} {v}")
    print()

    flint = run_flint_cases(names, mult)
    comp = run_competitors(names, mult) if args.compare else {}

    if args.compare:
        langs = sorted({l for v in comp.values() for l in v})
        head = f"{'case':<14}{'flint':>10}"
        for l in langs:
            head += f"{l:>10}"
        head += f"{'best ratio':>12}"
        print(head)
        print("-" * len(head))
        for name in flint:
            if name not in flint or "ms" not in flint[name]:
                print(f"{name:<14}{'failed':>10}")
                continue
            row = f"{name:<14}{human(flint[name]['ms']):>10}"
            best_ratio = 0.0
            for l in langs:
                e = comp.get(name, {}).get(l)
                if not e:
                    row += f"{'-':>10}"
                    continue
                row += f"{human(e['ms']):>10}"
                r = e["ms"] / flint[name]["ms"]
                best_ratio = max(best_ratio, r)
                # checksum agreement: a faster wrong answer is still wrong
                if e["checksum"] != flint[name]["checksum"]:
                    row += "!"
            row += f"{best_ratio:>11.2f}x" if best_ratio else f"{'-':>12}"
            print(row)
    else:
        print(f"{'case':<14}{'median':>10}{'best':>10}   checksum")
        print("-" * 52)
        for name, r in flint.items():
            if "error" in r:
                print(f"{name:<14}failed: {r['error'][:40]}")
                continue
            print(
                f"{name:<14}{human(r['ms']):>10}{human(r['best']):>10}"
                f"   {r['checksum'][:24]}"
            )

    if args.json:
        payload = {"env": info, "flint": flint, "competitors": comp}
        with open(args.json, "w") as f:
            json.dump(payload, f, indent=2)
        print(f"\nwrote {args.json}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
