<<<<<<< HEAD
# Benchmarks

All figures are single-threaded first-order curve sensitivities (waves), computed for every instrument against
every curve node, on the illustrative SOFR-style RFR curve in `examples/benchmark/rfr_curves.txt`.
Raw output for every number below is in
[`benchmarks/results/2026-10-11_i7-12700K/`](benchmarks/results/2026-10-11_i7-12700K/).

## Baseline

**BASE** is central-difference bump-and-reprice: each of the Ko + Kp curve nodes is bumped up and down
(2·(Ko+Kp) bumps) and the whole book is repriced each time. The scan methods compute the same sensitivities
analytically; agreement with BASE is checked in each run (about 6e-8 relative, the finite-difference error).

## Headline: 500,000-instrument seasoned book

Machine: Intel i7-12700K (AVX2, no AVX-512), GCC 13.3, one thread. Book: bonds, IBOR-style swaps and
compounded-OIS swaps, 9.3 million cashflows (18.6 per instrument), 66 sensitivities per instrument
(33 million values, 264 MB), 3,638 unique payment dates, 64,750 groups of 8 (96.5% lane occupancy).
Command: `bench_library 500000 1 3`.

| Variant | Time per curve update | ns / sensitivity | Speedup vs BASE |
|---|---:|---:|---:|
| BASE bump-and-reprice | 24,045 ms | 728.6 | 1× |
| SCAN replication + scalar reverse scan | 458.5 ms | 13.9 | 52× |
| GRP AVX2, 8 lanes, date table + kernel | 31.5 ms | 0.954 | **764×** |

GRP is 14.6× faster than SCAN per curve update. The grouped kernel with normal stores takes 44.7 ms; with
streaming stores 31.4 ms. On the portable auto-vectorised backend (no intrinsics) GRP takes 49.9 ms (467× vs the BASE of that run, 23,263 ms; a separate program run).
GRP agrees with SCAN to 3.6e-15 relative.

**Write-only reference.** Streaming 273.5 MB of zeros (the padded output size) took 8.3-8.6 ms in the three
saved runs on this machine (31.6-33.0 GB/s; `write_floor`), so the 31.4 ms kernel is roughly 3-4× the
write-only time. The write-only figure ignores input read traffic (roughly 80 MB of cashflow amounts plus date
indices), so it is a lower bound on the memory floor.

**Copy reference.** A plain `memcpy` of a 273.5 MB buffer (read plus write, 15 runs, one-off program, not in the
repository) took 27.8 ms at best and about 29.3 ms typically, so the 31.5 ms risk calculation is about 1.1× the
best copy time. Untuned library `memcpy`; one measurement.

**Structural zeros.** Two thirds of the book (bonds and compounded-OIS swaps) have no projection leg, so about
12 of the 33 million outputs are zero by construction (derived from the book composition, not counted in the
run). Over the remaining ~21 million the streaming kernel costs about 1.5 ns per output instead of 0.95 ns.

**Scalar vs lanes.** The 52× row includes replication (288 ms); the lane rows exclude it. The scalar ladder alone
takes 170 ms, so the lane kernel is 3.8× (normal stores) or 5.4× (streaming stores) faster than the scalar ladder.

**Output layout.** Output stays grouped and interleaved (66 consecutive eight-lane vectors per group); the timed
kernel includes no de-interleave. `gather` produces a per-instrument view through the lane-to-instrument map
and is not part of the timing.

**Group storage.** Each `Group` owns several small `std::vector`s (no single flat arena); a few heap allocations
per group, 64,750 groups here. The effect of an arena was not measured.

## Adjoint baselines (500,000 instruments, scalar, replication excluded)

| Method | Time | ns / sensitivity |
|---|---:|---:|
| Reverse scan, N+K | 269.5 ms | 8.2 |
| Tape-free N·K overlap adjoint (geometry known) | 639.8 ms | 19.4 |
| Reverse mode with a recorded tape (960 nodes in the last instrument) | 5,402.9 ms | 163.7 |

All agree to about 3e-11. The tape timing includes expression construction, exponentials, tape recording and
reverse traversal; it does not isolate tape-memory traffic and says nothing about other AAD implementations.

## Working set and lane occupancy (GRP kernel, streaming stores, AVX2)

| Instruments | Groups | Lane occupancy | ns / sensitivity |
|---:|---:|---:|---:|
| 10,000 | 4,482 | 27.9% | 1.99 |
| 30,000 | 5,876 | 63.8% | 1.09 |
| 100,000 | 14,794 | 84.5% | 1.17 |
| 300,000 | 39,831 | 94.1% | 1.03 |
| 500,000 | 64,750 | 96.5% | 0.89 (separate best-of-5 run; the headline run above gives 0.95 with the table refresh) |

Small books pay more because groups are less full. No cost increase appears when the output outgrows the cache
on this machine: the streaming-store kernel is write-bound.

## Aggregation (100,000 instruments × 66 sensitivities)

| Representation | Time |
|---|---:|
| Contiguous ordered ladders, vector add | 3.58 ms |
| Two-level hierarchy (book, desk) | 3.60 ms |
| `std::unordered_map` per instrument | 166.8 ms (47× slower) |
| `std::map` per instrument | 223.8 ms (63× slower) |

Memory: 53 MB flat against an estimated 581 MB for the maps (node-size estimate, not measured).

## Notes

- Speedup depends on book size, cashflows per instrument, number of nodes, ISA and the baseline. Quote it together
  with those, not alone.
- The grouped method requires a shared date table and one-off layout construction (about 0.9 s at 500k instruments),
  which are not part of the per-update timing.
- Timings are wall clock, best of several runs, on an unpinned desktop. They are not statistical evidence.
- No AVX-512 machine was available. AVX-512 results, when measured, should be added as a separate result directory.
- See [docs/PAPER_EVIDENCE.md](docs/PAPER_EVIDENCE.md) for the claim-to-command mapping.

## Pinned re-measurement, ablation and extras (11 October 2026)

Raw output: `benchmarks/results/2026-10-11_i7-12700K_pinned/` (driver `benchmarks/run_pinned_campaign.sh`, summary
`summary.txt`). Same machine; single-thread runs pinned with `taskset` to logical CPU 2. These are a different run
from the table above and are not mixed with it.

| Layer (500,000 instruments, median of 3 runs, best of 3 inside each) | Time | ns / sensitivity | vs BASE |
|---|---:|---:|---:|
| BASE bump-and-reprice, object model with virtual `npv` | 23,570 ms | 714 | 1x |
| SOA same bumps, flat arrays, no virtual calls | 22,665 ms | 687 | 1.04x |
| TABLE as SOA plus shared date table | 2,086 ms | 63.2 | 11x |
| SCAN scalar reverse scan incl. replication | 454 ms | 13.8 | 52x |
| GRP eight AVX2 lanes, normal stores (kernel) | 52.0 ms | 1.58 | 453x |
| GRP streaming stores (table refresh + kernel) | 37.8 ms | 1.14 | 624x |

**Variation.** The 31.5 ms headline above is a favourable best-of-3 from an unpinned run. Pinned to CPU 2 the
best-of-3 inside three full runs was 34.1-39.4 ms. The best of seven repetitions across six logical CPUs ranged
from 30.3 ms (CPUs 6, 8) to 37.2 ms (CPU 0), so the speedup over BASE lies between roughly 620x and 780x. Fifteen
independent single-repetition processes also pay the first touch of the output pages and gave a median of 42.1 ms
(36.4-51.8); their normal-store figures (median 118 ms) are dominated by first touch and should not be used.

**Same-CPU reference ratios.** Kernel 30.3-37.2 ms against write-only 7.3-8.5 ms: 4.0-4.7x. Against a plain
`memcpy` of the same 273.5 MB (`copy_floor`, best of 15, 17.8-20.2 ms): 1.7-1.8x. Both references were measured
on the same CPU as the kernel.

**Threads** (groups split into contiguous ranges, unpinned, best of 15; 6 cores, 12 threads): 1: 31.8 ms,
2: 32.2, 4: 18.4, 6: 21.8, 8: 18.4, 12: 18.0. Scaling stops near 1.8x at about 15 GB/s written; not diagnosed.

**One instrument per group** (`LADDER_UNIQUE_SIG=1`, 500,000 groups, 12.5% occupancy, 2.1 GB padded output):
kernel 253 ms (7.7 ns/output), layout build 2.5 s.

**Incremental insert** (`LADDER_INSERT=20000`): 14,651 inserts into a free lane at 4.8 us, 5,349 new
single-instrument groups at 16.0 us, none refused; 156 ms in total against 896 ms to rebuild the 520,000-instrument
layout (about 115,000 inserts). All ladders agree with the scalar scan to 3.6e-15. A date outside the date table
needs a rebuild; new groups hold one instrument, so occupancy degrades; deletion is not implemented.
=======
# Benchmarks and performance contracts

This page is an index to the repository's *different* performance experiments. Each number is meaningful only with its workload, output contract, machine, compiler, baseline and timed region. The entries below are not interchangeable measurements of one universal risk engine.

**Units:** the library's current forward-wave derivatives are per unit additive instantaneous-forward-rate shift. The archived paper `recorded_500k.txt` executable instead emitted interval-integrated-forward (`theta`) sensitivities, related by wave widths. Its timing has **not** been reassigned to the current `bench_paper.cpp` source.

## Runnable reader comparisons

For each claim in the joint-author article, readers can execute the corresponding baseline and optimised method from `examples/`. The [paper comparison driver](examples/benchmark_paper/run_comparisons.py) coordinates existing programs and their numerical gates. On an AVX-512 GCC/Clang machine with C++20 and CMake, from the repository root:

```sh
python3 examples/benchmark_paper/run_comparisons.py --suite all            # 2,000 instruments; quick comparison
python3 examples/benchmark_paper/run_comparisons.py --suite all --full     # 500,000 instruments; expensive
python3 examples/benchmark_paper/run_comparisons.py --suite write-floor --full
```

`paper` directly compares repricing, scalar scan and SIMD on its generated book; `cache` compares scenario-cache orderings; `adjoint` compares direct overlaps against the scan (plus an explicitly nonrepresentative taped prototype); and `aggregation` checks contiguous versus associative data structures. In the full combined suite, aggregation defaults to 100,000 instruments to avoid excessive associative-map memory use; paper, cache and adjoint retain 500,000. Use `--aggregation-instruments N` to change only that case. Outputs and machine/source provenance are saved under `build/paper-comparisons/reader-results/`. The `write-floor` comparator measures only output streaming stores, not a full memory copy or an input-reading risk calculation. **Do not combine runtimes from different suites to manufacture an incremental speedup ladder.** The historical `theta`/current rate-derivative distinction described above still applies.

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
>>>>>>> origin/main
