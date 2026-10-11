# Benchmarks and performance contracts

This page is an index to the repository's *different* performance experiments. Each number is meaningful only with its workload, output contract, machine, compiler, baseline and timed region. The entries below are not interchangeable measurements of one universal risk engine.

**Units:** the library's current forward-wave derivatives are per unit additive instantaneous-forward-rate shift. The archived paper `recorded_500k.txt` executable instead emitted interval-integrated-forward (`theta`) sensitivities, related by wave widths. Its timing has **not** been reassigned to the current `bench_paper.cpp` source.

## First-order paper workload: 500,000 instruments

[Raw recorded result](examples/benchmark_paper/recorded_500k.txt) and [complete example/methodology](examples/benchmark_paper/README.md). One Sapphire Rapids virtualised core, AVX-512; half fixed bonds and half vanilla swaps, approximately 37.5 cashflows per instrument, 30 discount and 36 projection waves, 64,034 padded groups.

| Calculation | Time | Speedup vs the stated baseline |
|---|---:|---:|
| Central-difference bump/reprice of the full book, 132 curve shocks | 57,444.7 ms | 1× |
| Scalar reverse scan | 1,271.81 ms | 45× |
| Eight-lane grouped risk, normal stores | 61.39 ms | 936× |
| Eight-lane grouped risk, streaming stores | **50.91 ms** | **1,128×** |
| Shared curve-table refresh plus streaming kernel | 51.05 ms | 1,125× |

The measured kernel writes one sensitivity per instrument per wave: 500,000 × 66 logical values, with 270,479,616 bytes of padded group output. Layout construction and other one-off preprocessing were measured separately. This **recorded `theta`-coordinate timing is not a current-source benchmark claim**; to report an updated rate-coordinate timing, rerun the updated executable.

The recorded 270 MiB write-only comparator represents **283,115,520 bytes**, not the 270,479,616-byte output. Use `write_floor` for an exact-size traffic reference; see the [paper benchmark notes](examples/benchmark_paper/README.md).

## Seasoned library workload: bonds, IBOR and OIS

A different book, with approximately 18.6 cashflows per instrument, signed OIS/IBOR contributions and mixed seasoned schedules. The 500,000-instrument fixture contains 64,750 groups, 30+36 waves and 96.5% valid SIMD lane occupancy. [Committed measurements](examples/benchmark/recorded_500k_seasoned.txt) and [benchmark design](examples/benchmark/README.md):

| Stage | Time |
|---|---:|
| Scalar scan (pre-replicated inputs) | 254.69 ms |
| Grouped kernel, normal stores | 68.00 ms |
| Grouped kernel, streaming stores | **51.92 ms** |
| Shared-date table refresh | 0.14 ms |

The baseline comparison is separately recorded on a [200,000-instrument mixed book](examples/benchmark/recorded_200k_seasoned_with_baseline.txt): **15,041.5 ms** bump/reprice versus **21.34 ms** streaming grouped risk on that workload. Do **not** splice its speedup into the 500,000-instrument paper table.

## Portfolio-only grouped reduction

The optional [direct reduction implementation](ladder/scan_grouped_reduce.hpp) computes portfolio-level first-order risk without writing the full per-trade sensitivity matrix. The [same-book benchmark](examples/benchmark/README.md#portfolio-only-grouped-reduction) compares it with the original grouped kernel.

One single-threaded unpinned Ubuntu/GCC **PORTABLE** run, median of three on the seeded 500,000-instrument bond/IBOR/OIS generator, 64,750 groups, 30+36 waves:

| Work actually timed | Time |
|---|---:|
| Original grouped kernel, full output write | 66.613 ms |
| Full output write **plus portfolio aggregation** | 80.541 ms |
| **In-register portfolio reduction** | **65.439 ms** |

The reduction is **1.23× faster than write-and-aggregate** here, but only 1.02× the full-output *write kernel alone*. It returns **528 bytes** (66 portfolio doubles) instead of materialising **273.504 MB** of padded risk output. Maximum scaled numerical discrepancy against the full-output reduction: **2.39e-13**. At 2,000 instruments, direct reduction was marginally slower; this is a workload-dependent improvement.

[Measured Actions run](https://github.com/andyD123/data-oriented-curve-risk/actions/runs/38102989684). A small correctness smoke remains in regular CI; the full-book performance measurement is an opt-in executable.

## Optional grouped gamma on a mixed book

The [grouped second-order implementation](docs/GROUPED_CROSS_GAMMA_STAGE2.md) adds a **separate** discount-gamma diagonal pass, including the supported fully forecast same-curve OIS payment-lag correction. Its pure projection and mixed discount/projection Hessian blocks are not claimed.

The seeded 500,000-instrument mixed book, 64,750 groups, 30 discount waves and 36 projection waves, measured single-threaded with GCC Release on a virtualised AMD EPYC:

| Timed operation | AVX-512 | AVX2 |
|---|---:|---:|
| Existing first-order discount + projection scan | 46.207 ms | 46.545 ms |
| Additional per-instrument discount-gamma diagonal | 44.099 ms | 46.710 ms |
| Portfolio-only discount-gamma diagonal | 44.835 ms | 43.775 ms |
| First-order plus separate gamma pass | **90.145 ms** | **91.779 ms** |

Adding the full per-trade diagonal-gamma output roughly doubles kernel time. The first-order output is approximately 273.5 MB, the extra gamma output 124.3 MB; portfolio-only gamma is 30 doubles. The performance figures **must not** be mixed with the separate paper or portfolio-reduce host measurements. Live QuantLib correctness for the selected OIS derivative contract is [reported separately](docs/QUANTLIB_LIVE_OIS_GAMMA.md).

## Fixed-cashflow structured cross-gamma

[Reproduction commands and baseline definitions](docs/CROSS_GAMMA_BENCHMARK.md). This experiment forms **one aggregated 66×66 Hessian for 100,000 fixed discounted cashflows** on an AMD EPYC; it does **not** compute 100,000 individual dense Hessians.

| Whole-portfolio calculation | Time |
|---|---:|
| Structured reverse scan and complete Hessian | **0.962 ms** |
| Direct per-cashflow analytic Hessian | 29.424 ms |
| Central finite-difference bump/reprice, including overlap preparation | approximately 4,065 ms |

Approximately **30.6×** faster than the stated direct analytic algorithm and **4,227×** faster than its stated full bump/reprice comparator. These are algorithmic comparisons against those specific baselines, not claims against state-of-the-art AAD or nonlinear market instruments. Matrix-free Hessian-vector products avoid the full matrix when only a risk projection is requested.

## Wider logical schedule groups

[Wide-group report](docs/WIDE_SCHEDULE_GROUPS.md): on a separate synthetic 65,536-instrument, 60-cashflow, 40-bucket experiment, widening the *logical* schedule group from 8 to 2,048 members reduced the measured AVX-512 streaming kernel from **5.724 ms to 2.675 ms** (2.14×), while the SIMD primitive remained eight lanes. This prototype is not the production discount/projection grouped implementation.

For the LRU scenario-cache and adjoint experiments, use the [paper benchmark walkthrough](examples/benchmark_paper/README.md). Their timing contracts differ from the scan and gamma tests above.

## Reproducing the optional comparisons

For portfolio first-order reduction:

```sh
cmake -S . -B build-reduce -DCMAKE_BUILD_TYPE=Release \
  -DLADDER_LANES=AVX2 -DLADDER_BUILD_REDUCE_BENCHMARK=ON
cmake --build build-reduce --target bench_grouped_reduce
./build-reduce/examples/benchmark/bench_grouped_reduce 500000 3 \
  ./examples/benchmark/quantlib_example_curves.txt
```

For grouped gamma, substitute `-DLADDER_BUILD_GAMMA_BENCHMARK=ON` and target `bench_gamma_grouped`; see the [gamma guide](docs/GROUPED_CROSS_GAMMA_STAGE2.md). For the paper workload, the `release-paper` CMake preset enables the specialised x86 benchmark where supported; follow [`examples/benchmark_paper/README.md`](examples/benchmark_paper/README.md) for the exact invocation and input curves.

**Comparison rules:** retain the numerical unit/coordinate contract, input population, grouping, ISA, store policy, compilation flags, threading and timing region. Do not conflate a write-only reference, the read+write cost of a copy, a whole-book bump/reprice baseline, an aggregated Hessian or a per-instrument ladder. Do not call an earlier recorded measurement a new run of different source.
