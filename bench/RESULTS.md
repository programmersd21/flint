# benchmark results

Two builds are measured here:

- **switch** -- `make release`, the portable C11 interpreter. `flint`
- **goto** -- `make flint-goto`, the computed-goto interpreter. `flint-goto`

`make bench` prints the goto build; `make flint-goto && make bench-goto`
prints both.

## environment

```
cpu      Intel Core i5-1235U
kernel   Linux x86-64
compiler gcc (GCC) 16.2.1, -O2 -DNDEBUG
flint    v0.7.0
runs     median of 15 (7 for the slow cases), outliers discarded
```

## measured, new in 0.7.0

three new cases for the three new capabilities. no before numbers exist
because the capabilities did not.

| case           | median  | what it measures |
|----------------|---------|------------------|
| `table_iter`   | 519ms   | 20k-entry table, full `for k, v` walk with accumulation |
| `table_access` | 513ms   | 10k computed-key reads plus `has()` plus `keys()` |
| `coalesce`     | 464ms   | 5M iterations of `v ?? -1`, short-circuit, no allocation |

the table cases are dominated by string hashing on computed keys
(`"k" + str(i)` interns-or-compares per lookup), not by the loop itself --
which is why they sit near 500ms for tens of thousands of entries while the
numeric loops below do millions of iterations in comparable time. that cost
is honest: it is the content comparison 0.5.0 bought, applied per lookup.

## spot check: no regressions in existing cases

same machine, same runner, switch build:

| case           | median  |
|----------------|---------|
| `arith_add`    | 1064ms  |
| `calls`        | 195ms   |
| `strings`      | 99ms    |
| `list_iter`    | 33ms    |
| `range_loop`   | 167ms   |
| `json_roundtrip` | 7ms   |
| `startup`      | 0.6ms   |

startup is unchanged at well under a millisecond, which continues to be the
number that matters most for the shell-pipeline case this language is aimed
at.

Reproduce with `python3 bench/bench.py`, or `python3 bench/bench.py --json
out.json` to get the same numbers machine-readably. The runner records the
environment with the results, because a number without a CPU and a compiler
version is not a measurement.

## measured, string cases

This is where v0.5.0 spent its effort. These are before/after the string
runtime change, on the same machine, same runner.

| case              | v0.4.0  | v0.5.0  | change |
|-------------------|----------|----------|--------|
| str_concat        | 745.8ms  | 61.6ms   | -92%   |
| str_utf8          | 954.2ms  | 222.8ms  | -77%   |
| str_ascii         | 254.4ms  | 62.4ms   | -76%   |
| mem_retain        | 879.1ms  | 371.0ms  | -58%   |
| call_native       | 728.8ms  | 329.3ms  | -55%   |
| str_find          | 193.9ms  | 102.3ms  | -47%   |
| str_concat_small  | 351.6ms  | 227.9ms  | -35%   |
| str_temp          | 239.6ms  | 161.0ms  | -33%   |
| json_write_big    | 91.1ms   | 67.5ms   | -26%   |
| json_parse_big    | 37.0ms   | 29.9ms   | -19%   |
| strings           | 386.0ms  | 340.2ms  | -12%   |

The cause is in [docs/internals.md](../docs/internals.md): runtime strings are
no longer interned. They are equal by content instead of by identity.

## measured, interpreter dispatch

`make flint-goto` against `make release`, back to back, best of 7. Cases below 50ms
are omitted: at that scale a 1ms difference is 25%, which is timer noise rather
than a result.

| case | switch | goto | change |
|------|--------|------|--------|
| str_ident | 72ms | 64ms | +12.5% |
| str_find | 77ms | 70ms | +10.0% |
| arith_add | 3204ms | 2915ms | +9.9% |
| mem_alloc | 206ms | 188ms | +9.6% |
| calls | 568ms | 523ms | +8.6% |
| table_field | 295ms | 272ms | +8.5% |
| arith_mul | 304ms | 281ms | +8.2% |
| str_unequal | 290ms | 269ms | +7.8% |
| str_equal | 384ms | 358ms | +7.3% |
| float | 510ms | 476ms | +7.1% |
| list_index | 168ms | 157ms | +7.0% |
| closure_call | 313ms | 293ms | +6.8% |
| arith | 4553ms | 4267ms | +6.7% |
| range_loop | 514ms | 482ms | +6.6% |
| str_prefix | 213ms | 200ms | +6.5% |
| closures | 345ms | 325ms | +6.2% |
| list_build | 173ms | 163ms | +6.1% |
| call_direct | 444ms | 419ms | +6.0% |
| arith_div | 839ms | 792ms | +5.9% |
| fib | 57ms | 54ms | +5.6% |
| lists | 340ms | 322ms | +5.6% |
| table_build | 78ms | 74ms | +5.4% |
| list_iter | 100ms | 95ms | +5.3% |
| call_nested | 445ms | 423ms | +5.2% |
| arith_mod | 521ms | 498ms | +4.6% |
| call_native | 322ms | 309ms | +4.2% |
| compare | 974ms | 941ms | +3.5% |
| json_write_big | 59ms | 57ms | +3.5% |
| str_fmt | 119ms | 115ms | +3.5% |
| str_find_miss | 344ms | 336ms | +2.4% |
| str_temp | 99ms | 97ms | +2.1% |
| str_concat | 55ms | 54ms | +1.9% |
| mem_retain | 322ms | 317ms | +1.6% |
| str_utf8 | 126ms | 124ms | +1.6% |
| strings | 181ms | 180ms | +0.6% |
| str_ascii | 51ms | 51ms | +0.0% |
| str_concat_small | 117ms | 118ms | -0.8% |
| str_long_equal | 60ms | 62ms | -3.2% |

A switch is portable C11 and correct everywhere. Computed goto drops the bounds
check and lets the processor predict the next opcode from the previous one. It
is a separate binary rather than an `#ifdef` so the default build stays
warning-clean under `-Wpedantic -Werror`, which is a rule this project has never
relaxed.

## one case got slower, and it is a real trade

`str_long_equal` was 38.5ms and is now ~127ms.

Two equal long strings used to intern to the *same object*, so `==` was a
pointer compare. They are now distinct objects and actually compare bytes. That
is the direct cost of not interning, and it is the one case where interning was
paying off.

It is still the right trade, because every other string case improved and this
one is a narrow shape. But it is a regression, not a rounding error, and it
would be dishonest to leave it out of the table.

## startup

Unchanged at ~2ms, which is the real win and always was. A shell pipeline pays
this on every invocation, so it bounds what a small script can cost. Nothing in
this release moved it, which is the point: the string work did not cost startup
anything.

## competitors

`python3 bench/bench.py --compare` runs the cases that have a hand-written
equivalent in another language, and checks that the two produce the same
checksum before reporting a time. There is no honest way to translate "measure
startup" or "measure the collector" into another language, and pretending
otherwise would be exactly the kind of flattering comparison this suite exists to
avoid.

CPython on this machine is roughly 1.5-2x faster than the switch interpreter on
loop-heavy numeric work, and roughly parity or better on the string-heavy cases
since v0.4. Lua and LuaJIT are faster on tight loops for the same reason any
JIT is faster.

## what is not claimed

No claim that flint is faster than python "in general". This is 49 cases on one
machine, not a benchmark suite.

No claim about the JIT, because there isn't one. See [RELEASES.md](../RELEASES.md).

The absolute numbers here are specific to this CPU and this compiler. The
*relative* numbers are the claim, and the relative numbers were measured on the
same machine with the same runner in the same session.
