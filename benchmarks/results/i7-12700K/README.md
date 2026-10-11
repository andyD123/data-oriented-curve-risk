# Results: Intel i7-12700K

Raw program output, unedited. One thread, wall clock, best of the stated repetitions, unpinned desktop.

- CPU: 12th Gen Intel Core i7-12700K (AVX2 + FMA, **no AVX-512**), 25 GB RAM, Linux, GCC 13.3.0, Release.
- Curves: `rfr_curves.txt` in each program's working directory (30 + 36 forward buckets; built from the Bank of England SONIA curve, see docs/BOE_CURVE.md; regenerate with `benchmarks/run_headline_set.sh`).
- "avx2" results use `-DLADDER_LANES=AVX2 -DLADDER_NATIVE_ARCH=ON`; "portable" uses the default auto-vectorised
  build. The sweep, adjoint, aggregation and write-floor runs were built with `-march=native`.

| File | Command (from the example's build directory) |
|---|---|
| `bench_library_avx2_500k.txt`, `bench_library_portable_500k.txt` | `bench_library 500000 1 3` |
| `working_set_sweep_avx2.txt` | `bench_library N 0 5` for N = 10k, 30k, 100k, 300k, 500k (selected lines) |
| `adjoint_bench_500k.txt` | `adjoint_bench 500000 2` |
| `aggregation_100k.txt` | `aggregation 100000` |
| `write_floor.txt` | `write_floor 273504000 15`, three consecutive runs |
| `scenario_bench_100k.txt` | `LRU_CAPS=16,32,64,128 scenario_bench 100000 0 3` |

These are not comparable with results from other machines. Do not mix rows from different files into one table.
