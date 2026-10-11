# Paper claim -> code and evidence

Every benchmark number in the paper comes from the committed raw output in
[`benchmarks/results/2026-10-11_i7-12700K/`](../benchmarks/results/2026-10-11_i7-12700K/) (one machine, one day,
no AVX-512). Do not combine rows from different machines or runs into one table.

| Claim | Command / source | Evidence |
|---|---|---|
| Box-wave derivative, capped/open terminal wave | `ctest --preset release` | Oracle-based tests. |
| Scalar discount scan is O(N+K) once records are sorted | `ladder/scan.hpp` | Monotone forward/backward boundary cursors. |
| 500k book: BASE, SCAN, GRP, write-only comparison | `bench_library 500000 1 3`, `write_floor` | `bench_library_avx2_500k.txt`, `write_floor.txt` |
| Working set and lane occupancy | `bench_library N 0 5` | `working_set_sweep_avx2.txt` |
| Adjoint comparison (scan, direct N·K, tape) | `adjoint_bench 500000 2` | `adjoint_bench_500k.txt`; CTest `adjoint_demo_smoke`. |
| Aggregation: contiguous vs maps | `aggregation 100000` | `aggregation_100k.txt` |
| Date-cache ordering effect | `scenario_bench` | `scenario_bench_100k.txt` |
| RFR wave and hedge reconciliation | `python3 examples/rfr_reconcile/rfr_reference.py check <dir>`; CTest `rfr_reconcile_replay` | 15 illustrative par-OIS hedges; see the example README. |
| Cross-gamma | `ctest -R gamma`, `bench_cross_gamma` | [CROSS_GAMMA.md](CROSS_GAMMA.md) |

## Publication rule

A timing in the paper should name the exact result file that supports it. If a source change alters the timed
calculation, regenerate the timing instead of relabelling an old one.
