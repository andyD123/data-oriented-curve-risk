# Benchmarks

All figures are single-threaded first-order curve sensitivities (waves), computed for every instrument
against every curve node.

## Baseline

**BASE** is central-difference bump-and-reprice: each of the Ko + Kp curve nodes is bumped up and down
(2·(Ko+Kp) bumps) and the whole book is repriced each time. The scan methods compute the same sensitivities
analytically; agreement with BASE is checked in each run (about 2e-8 relative, the finite-difference error).

## Headline: 500,000-instrument book

Source: [`examples/benchmark_paper/recorded_500k.txt`](examples/benchmark_paper/recorded_500k.txt)
(`bench_paper`, AVX-512, 8-lane groups, 30 + 36 nodes, 37.5 cashflows per instrument).

| Variant | Time (ms) | ns / sensitivity | Speedup vs BASE |
|---|---:|---:|---:|
| BASE bump-and-reprice | 57,444.7 | 1740.75 | 1× |
| SCAN scalar reverse scan | 1,271.8 | 38.54 | 45× |
| GRP AVX-512, normal stores | 61.4 | 1.860 | 936× |
| GRP AVX-512, streaming stores | 50.9 | 1.543 | **1,128×** |

Caveat: this is a historical run. It predates the October 2026 rate-unit alignment and emitted
interval-integrated-forward (theta) sensitivities; current `bench_paper.cpp` emits per-unit instantaneous-forward
shifts. The timing is retained as recorded and has not been re-measured with current source.

## Current-source check: 20,000-instrument seasoned book

Command: `./build/examples/benchmark/bench_library 20000 1 3`
(portable auto-vectorised backend, GCC 13.3, 18.6 cashflows per instrument, 66 sensitivities each).

| Variant | Time per curve update | ns / sensitivity | Speedup vs BASE |
|---|---:|---:|---:|
| BASE | 939.7 ms | 711.9 | 1× |
| SCAN | 17.6 ms | 13.3 | 53× |
| GRP | 3.9 ms | 2.9 | 242× |

This is a smaller book, a different workload and a non-AVX-512 build, so its speedup is lower than the headline
and the two tables must not be combined.

## Notes

- Speedup depends on book size, cashflows per instrument, number of nodes, ISA and the baseline. Quote it together
  with those, not alone.
- The grouped method requires a shared date table and one-off layout construction (hundreds of ms at 500k
  instruments), which are not part of the per-update timing.
- Timings are wall clock, best of several runs, on unpinned machines. They are not statistical evidence.
- See [docs/PAPER_EVIDENCE.md](docs/PAPER_EVIDENCE.md) for the claim-to-command mapping.
