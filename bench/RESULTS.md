# benchmark results -- flint v0.14.0

Two builds are measured here:

- **switch** -- `make release`, the portable C11 interpreter (`flint`)
- **goto** -- `make flint-goto`, the computed-goto interpreter (`flint-goto`)

## environment

```
cpu      12th Gen Intel Core i5-1235U
kernel   Linux x86-64
compiler gcc (GCC) 16.2.1, -O2 -DNDEBUG
flint    v0.14.0
runs     median of 15 (7 for slow cases), outliers discarded
```

## measured, v0.14.0 core cases

| case           | median  | what it measures |
|----------------|---------|------------------|
| `table_iter`   | 459.6ms | 20k-entry table, full `for k, v` walk with accumulation |
| `table_access` | 493.2ms | 10k computed-key reads plus `has()` plus `keys()` |
| `coalesce`     | 389.2ms | 5M iterations of `v ?? -1`, short-circuit, no allocation |

## interpreter dispatch: switch vs. computed-goto

| case | switch | goto | change |
|------|--------|------|--------|
| `str_join` | 4.41ms | 4.0ms | +10.0% |
| `range_loop` | 225.0ms | 181.0ms | +19.5% |
| `closure_call` | 97.7ms | 89.0ms | +9.0% |
| `arith_add` | 923.6ms | 849.0ms | +8.4% |
| `str_equal` | 119.7ms | 109.8ms | +8.3% |
| `str_prefix` | 71.0ms | 62.6ms | +11.8% |
| `list_index` | 59.6ms | 53.1ms | +10.9% |
| `table_build` | 29.3ms | 25.2ms | +14.0% |

Computed goto drops bounds checks and optimizes opcode prediction, providing an average 5-20% boost on loop-heavy and call-heavy benchmarks while keeping the default build 100% portable C11.

## startup

Unchanged at ~0.64ms, ensuring shell-pipeline responsiveness and script execution overhead remain minimal.
