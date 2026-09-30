# benchmark results

Intel i5-1235U, Linux x86-64, gcc `-O2`, CPython 3.14.7. Reproduce with
`make bench` or `python3 bench/bench.py`.

Every case is a `.fl` and a `.py` doing the same work, and the runner compares
the two outputs before it reports a timing. If the checksums differ it says so
rather than printing a meaningless number.

Best of 7 runs, wall clock.

| case      | flint    | python   | python/flint |
|-----------|----------|----------|--------------|
| startup   | 0.59 ms  | 9.0 ms   | **15.31x**   |
| hello     | 0.50 ms  | 9.5 ms   | **18.93x**   |
| arith     | 1.49 s   | 2.46 s   | **1.65x**    |
| lists     | 105.5 ms | 193.9 ms | **1.84x**    |
| fib       | 15.8 ms  | 25.1 ms  | **1.59x**    |
| calls     | 169.5 ms | 252.9 ms | **1.49x**    |
| closures  | 106.9 ms | 164.1 ms | **1.54x**    |
| strings   | 109.6 ms | 59.4 ms  | **0.54x**    |

flint binary: 161 KB, linked against nothing but libc and libm.

## what is actually true

**Startup is the real win and it is not close.** ~15-20x on an empty program. A
shell pipeline pays this on every invocation, so it bounds what a small script
can cost. Nothing else in this table matters as much for the work flint is
aimed at.

**The language benchmarks are real but modest, 1.5x to 1.8x.** Loop-heavy
cases where flint's dispatch is a compiled `switch` and CPython re-dispatches
every bytecode op. A genuine advantage, not a landslide.

**This overrules a prediction made before any of it was measured.** The
expectation was that `arith` would be flint's *worst* case, on the theory that
every number is a nan-boxed double while CPython's small integers are tagged
and never boxed. Measured, flint is 1.65x *faster* on that loop. The
double-boxing cost is real but smaller than CPython's per-operation dispatch,
so it never surfaces as a deficit. The prediction was wrong and the
measurement overrules it.

**strings is an honest loss: CPython is ~1.8x faster.** This is the one to
be uncomfortable about, so it is here in full.

The `strings` case is mostly *not* measuring the string primitives. Timing
the phases separately, building the 200k-element list of parts is ~104 ms of
the total; `split`, `contains` and `replace` together add ~30 ms. So
the case is dominated by `str()` and `push()` in a loop, not by `split`.

That said, the primitives were made faster and the change is kept:
`split`, `contains` and `replace` now use `memchr` to find candidate
positions and `memcmp` only to confirm, instead of a libc call at every
byte, and `replace` copies the runs between matches with `memcpy` rather than
byte at a time. `str()` formats small integers with a digit loop instead of
`snprintf("%.15g")`, which is ~250ns per call and was measurable: 200k
`str(i)` calls went from 50.4 ms to 39.5 ms.

The remaining gap is the interpreter loop itself and CPython's whole-string
operations, which are heavily tuned C. Closing it means a real string
representation, not a better inner loop.

## what is not claimed

No claim that flint is faster than python "in general". Eight cases is not
pyperf. These workloads were chosen because they are what small scripts do,
and a workload flattering either language would be easy to add and dishonest
to include. The strings row is here for exactly that reason.
