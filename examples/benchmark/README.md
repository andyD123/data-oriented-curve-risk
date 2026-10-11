# benchmark — library kernel on a seasoned book

Bonds, IBOR swaps and compounded-OIS swaps on a single SOFR-style RFR curve (`rfr_curves.txt`, illustrative par OIS quotes; the forward-projection block carries the same curve, as in a one-curve RFR market).

**Using a real curve (private run).** See [docs/BOE_CURVE.md](../../docs/BOE_CURVE.md) for downloading the Bank of England SONIA OIS curve and running the benchmark and reconciliation on it locally. Outputs stay git-ignored.

Schedules are walked back from the last regular payment date with a random remaining life, so the book is seasoned;
the grouping key is (type, maturity day) and groups mix seasoned and new trades on the same grid.

```
g++ -O3 -std=c++20 -march=native -ffp-contract=fast -I../.. -Wno-unused-result bench.cpp -o bench
./bench 500000 1 3        # N, run bump-and-reprice baseline (needs ~1 GB more at 500k), repetitions
```
<<<<<<< HEAD
Results on an Intel i7-12700K (AVX2, no AVX-512), GCC 13.3, one thread, are in
[BENCHMARKS.md](../../BENCHMARKS.md) and `benchmarks/results/2026-10-11_i7-12700K/`: at 500k instruments the baseline takes
24,045 ms per curve update, the scalar scan 458.5 ms and the grouped kernel 31.5 ms (764x; 96.5% lane occupancy;
grouped vs scalar 3.6e-15).
=======
Recorded on one Sapphire Rapids core (2.1 GHz, VM):
- `recorded_500k_seasoned.txt`: 500k, 96.5% lane occupancy, streaming stores 51.1 ms (1.55 ns/sensitivity), normal 65.3 ms,
  grouped vs scalar 3.4e-15.
- `recorded_200k_seasoned_with_baseline.txt`: 200k with the bump-and-reprice baseline, 14,913 ms vs 21.7 ms streaming (688x).

The paper's §9 headline (unseasoned bonds + vanilla swaps, 50.9 ms) is `../benchmark_paper`; this example is the library
form on a different book and is reported separately.

## Portfolio-only grouped reduction

[bench_grouped_reduce.cpp](bench_grouped_reduce.cpp) builds the same seeded
bond/IBOR/OIS book and shared date table, then compares:

- the existing `scan_grouped` full AoSoA output;
- full AoSoA output followed by portfolio aggregation;
- `scan_grouped_reduce` in-register portfolio totals, **without** the full
  per-instrument output buffer.

Enable the standalone benchmark explicitly:

```sh
cmake -S . -B build-reduce -DCMAKE_BUILD_TYPE=Release \
  -DLADDER_LANES=AVX2 -DLADDER_BUILD_REDUCE_BENCHMARK=ON
cmake --build build-reduce --target bench_grouped_reduce
./build-reduce/examples/benchmark/bench_grouped_reduce 500000 3 \
  ./examples/benchmark/quantlib_example_curves.txt
```

A reproducible single-threaded test on an unpinned GitHub Actions Ubuntu
runner with GCC Release, **PORTABLE** lanes, 500,000 instruments, 64,750
eight-lane groups and 30 discount + 36 projection waves gave the following
median-of-three measurements:

| Method | Time |
|---|---:|
| Existing grouped kernel, write full matrix | 66.61 ms |
| Write full matrix, then aggregate | 80.54 ms |
| **Direct in-register portfolio reduction** | **65.44 ms** |

Direct reduction was **1.23x faster** than writing and aggregating,
but only **1.02x** the speed of the grouped write kernel alone. It
reduces risk output from **273.5 MB** of padded trade-level sensitivities
to **528 bytes** of portfolio totals. A 2,000-instrument smoke test
was slightly slower for direct reduction; the benefit is workload dependent.

The maximum scaled difference against the original grouped results was
approximately **2.39e-13**. The benchmark does not establish universal
speedup, per-trade gamma performance or end-to-end curve-update cost.
Book/layout setup and date-table refresh are measured separately from the
kernels. The one-off full-book run can be inspected in
[the Actions evidence](https://github.com/andyD123/data-oriented-curve-risk/actions/runs/38102989684);
routine CI uses a small correctness smoke test instead.
>>>>>>> origin/main
