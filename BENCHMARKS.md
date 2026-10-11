# Benchmarks

All figures are single-threaded first-order curve sensitivities (waves), computed for every instrument against
every curve node, on the Bank of England SONIA OIS curve (see [docs/BOE_CURVE.md](docs/BOE_CURVE.md); the data
are the Bank's copyright and are not redistributed). In keeping with that licence this repository reports timings,
ratios and agreement between methods only: no rates and no rate-derived values (quotes, present values, hedge
notionals or rate sensitivities). No curve data is committed: the build writes a synthetic flat curve into each example's build directory so the
tests and smoke runs work without third-party data; the numbers below were not produced with it.
Raw output for every number below is in [`benchmarks/results/i7-12700K/`](benchmarks/results/i7-12700K/) and
[`benchmarks/results/i7-12700K_pinned/`](benchmarks/results/i7-12700K_pinned/).

## Baseline

**BASE** is central-difference bump-and-reprice: each of the Ko + Kp curve nodes is bumped up and down
(2·(Ko+Kp) bumps) and the whole book is repriced each time. The scan methods compute the same sensitivities
analytically; agreement with BASE is checked in each run (about 6e-8 relative, the finite-difference error).

## Headline: 500,000-instrument seasoned book

Machine: Intel i7-12700K (AVX2, no AVX-512), GCC 13.3. Book: bonds, IBOR-style swaps and
compounded-OIS swaps, 9.3 million cashflows (18.6 per instrument), 66 sensitivities per instrument
(33 million values, 264 MB logical, 273.5 MB padded), 3,638 unique payment dates, 64,750 groups of 8
(96.5% lane occupancy). The headline is the pinned ablation below (logical CPU 2, median of three).
The table in this section is an earlier unpinned best-of-three (`bench_library 500000 1 3`) whose core
was not controlled. Do not mix the two runs into one ratio.

### Uncontrolled run (unpinned, best of three)

| Variant | Time per curve update | ns / sensitivity | Speedup vs BASE |
|---|---:|---:|---:|
| BASE bump-and-reprice | 23,482 ms | 711.6 | 1× |
| SCAN replication + scalar reverse scan | 462.4 ms | 14.0 | 51× |
| GRP AVX2, 8 lanes, date table + kernel | 39.1 ms | 1.18 | 601× |

The grouped kernel with normal stores takes 51.1 ms; with streaming stores 39.0 ms. On the portable
auto-vectorised backend (no intrinsics) GRP takes 59.8 ms (393× vs the BASE of that run, 23,543 ms; a
separate program run). GRP agrees with SCAN to 3.5e-15 relative. This run landed on a slower core than
the pinned headline (about 600×-780× across runs and cores).

**Scalar vs lanes, this run only.** The 51× row includes replication (281 ms); the lane rows exclude it.
The scalar ladder alone takes 181 ms, so the lane kernel is 3.5× (normal stores) or 4.6× (streaming
stores) faster than that scalar ladder. Those multiples are not the headline ratios. The pinned ablation's
own ratios are 10.7× against its scalar row, 4.2× against its scalar ladder alone (172 ms), and a further
1.2× from streaming stores.

**Write-only reference.** Streaming 273.5 MB of zeros (the padded output size) took 7.5-7.8 ms in the three
saved unpinned runs (35.2-36.4 GB/s; `write_floor`), so the 39.0 ms kernel is about 5× the write-only time
of this slower-core run. On pinned cores the same comparison is 3.1-4.6× (kernel 32.3-37.3 ms, write-only
7.5-11.1 ms). The write-only figure ignores input reads (roughly 80 MB of cashflow amounts plus date
indices). It is not the remaining opportunity.

**Copy reference.** A plain `memcpy` of a 273.5 MB buffer (`copy_floor`, read plus write, best of 15, untuned
library routine) took 18.9-22.4 ms on the six pinned CPUs, so the pinned kernel is 1.6-1.9× the copy time.
That is the fairer floor.

**Structural zeros.** Two thirds of the book (bonds and compounded-OIS swaps) have no projection leg, so about
12 million of the 33 million outputs are zero by construction (derived from the book composition, not counted
in the run). Over the remaining ~21 million the streaming kernel costs about 1.6 ns per output instead of 1.0 ns
on the pinned headline (1.18 ns on the unpinned run above).

**Output layout.** Output stays grouped and interleaved (66 consecutive eight-lane vectors per group); the timed
kernel includes no de-interleave. `gather` produces a per-instrument view through the lane-to-instrument map
and is not part of the timing.

**Group storage.** Each `Group` owns several small `std::vector`s (no single flat arena); a few heap allocations
per group, 64,750 groups here. The effect of an arena was not measured.

## Adjoint baselines (500,000 instruments, scalar, replication excluded)

| Method | Time | ns / sensitivity |
|---|---:|---:|
| Reverse scan, N+K | 289.0 ms | 8.8 |
| Tape-free N·K overlap adjoint (geometry known) | 647.4 ms | 19.6 |
| Reverse mode with a recorded tape (960 nodes in the last instrument) | 5,774.6 ms | 175.0 |

All agree to about 6e-11. The tape timing includes expression construction, exponentials, tape recording and
reverse traversal; it does not isolate tape-memory traffic and says nothing about other AAD implementations.

## Working set and lane occupancy (GRP kernel, streaming stores, AVX2)

| Instruments | Groups | Lane occupancy | ns / sensitivity |
|---:|---:|---:|---:|
| 10,000 | 4,482 | 27.9% | 2.33 |
| 30,000 | 5,876 | 63.8% | 1.68 |
| 100,000 | 14,794 | 84.5% | 1.28 |
| 300,000 | 39,831 | 94.1% | 1.24 |
| 500,000 | 64,750 | 96.5% | 1.05 (separate best-of-5 run; the unpinned table gives 1.18 and the pinned headline gives 1.00, both with the table refresh) |

Small books pay more because groups are less full. No cost increase appears when the output outgrows the cache
on this machine: the streaming-store kernel is write-bound.

## Aggregation (100,000 instruments × 66 sensitivities)

| Representation | Time |
|---|---:|
| Contiguous ordered ladders, vector add | 3.32 ms |
| Two-level hierarchy (book, desk) | 3.74 ms |
| `std::unordered_map` per instrument | 185.2 ms (56× slower) |
| `std::map` per instrument | 262.4 ms (79× slower) |

Memory: 53 MB flat against an estimated 581 MB for the maps (node-size estimate, not measured).

## Notes

- Speedup depends on book size, cashflows per instrument, number of nodes, ISA and the baseline. Quote it together
  with those, not alone.
- The grouped method requires a shared date table and one-off layout construction (about 1 s at 500k instruments),
  which are not part of the per-update timing.
- Timings are wall clock, best of several runs, on one desktop. The headline ablation is pinned to logical CPU 2; the earlier table is unpinned. They are not statistical evidence.
- No AVX-512 machine was available. AVX-512 results, when measured, should be added as a separate result directory.
- See [docs/PAPER_EVIDENCE.md](docs/PAPER_EVIDENCE.md) for the claim-to-command mapping.

## Headline ablation (pinned) and extras

Raw output: `benchmarks/results/i7-12700K_pinned/` (drivers `benchmarks/run_pinned_campaign.sh` and
`benchmarks/run_cpu_sweep.sh`, summary `summary.txt`). Same machine; single-thread runs pinned with `taskset`
(logical CPU 2 unless stated). These are a different run from the table above and are not mixed with it.

| Layer (500,000 instruments, median of 3 runs, best of 3 inside each) | Time | ns / sensitivity | vs BASE |
|---|---:|---:|---:|
| BASE bump-and-reprice, object model with virtual `npv` | 22,929 ms | 695 | 1x |
| SOA same bumps, flat arrays, no virtual calls | 22,354 ms | 677 | 1.03x |
| TABLE as SOA plus shared date table | 1,720 ms | 52.1 | 13x |
| SCAN scalar reverse scan incl. replication | 439 ms | 13.3 | 52x |
| GRP eight AVX2 lanes, normal stores (kernel) | 41.1 ms | 1.24 | 558x |
| GRP streaming stores (table refresh + kernel) | 33.0 ms | 1.00 | 695x |

**Variation.** The headline is the 33.0 ms median in the table above (logical CPU 2). The 39.1 ms figure is an unpinned best-of-3 on a core that was not controlled. Pinned to
CPU 2 the best-of-3 inside three full runs was 29.6-34.5 ms. The best of seven repetitions across six logical CPUs
(0, 2, 4, 6, 8, 10; `per_cpu/`) ranged from 32.4 ms (CPU 4) to 37.4 ms (CPU 0), so the speedup over BASE lies
between roughly 600x and 780x. Fifteen independent single-repetition processes also pay the first touch of the
output pages and gave a median of 31.6 ms (29.8-48.1); their normal-store figures are dominated by first touch and
should not be used.

**Same-CPU reference ratios.** Kernel 32.3-37.3 ms against write-only 7.5-11.1 ms: 3.1-4.6x. Against a plain
`memcpy` of the same 273.5 MB (`copy_floor`, best of 15, 18.9-22.4 ms): 1.6-1.9x. Both references were measured
on the same CPU as the kernel.

**Threads** (groups split into contiguous ranges, unpinned, `taskset` on the whole chip, best of 15; 6 cores, 12 threads): 1: 29.1 ms,
2: 21.1, 4: 19.1, 6: 19.1, 8: 18.4, 12: 17.2. Scaling stops near 1.7x. The printed rate, about 16 GB/s, counts
bytes of sensitivity output; it is not a memory-controller measurement, and the plateau was not diagnosed.
`LADDER_THREAD_CPUS` can pin worker i, and was not used for this figure. `LADDER_PHASES=1` times amount
formation, the discount walk and the projection walk separately and is not in these tables. Discount and
projection phases each include their own amount formation, so they do not partition the fused kernel.

**Wider schedule groups.** Width 8 is the published kernel. `build_layout(..., group_lanes)` and
`LADDER_GROUP_LANES` accept a positive multiple of 8 and default to 8. The separate one-schedule experiment
in [docs/WIDE_SCHEDULE_GROUPS.md](docs/WIDE_SCHEDULE_GROUPS.md) is not a row of either table above and is not
the 500,000-instrument mixed book. Grouped diagonal gamma rejects wide groups. Phase timing, wider groups
and the realised overnight factor were off for every figure in this file. The published timings come from
commit `2a9ce9f`.

**One instrument per group** (`LADDER_UNIQUE_SIG=1`, 500,000 groups, 12.5% occupancy, 2.1 GB padded output):
kernel 178 ms (5.4 ns/output), layout build 1.8 s.

**Incremental insert** (`LADDER_INSERT=20000`): 14,651 inserts into a free lane at 4.6 us, 5,349 new
single-instrument groups at 14.8 us, none refused; 147 ms in total against 859 ms to rebuild the 520,000-instrument
layout (about 117,000 inserts). All ladders agree with the scalar scan to 3.5e-15. A date outside the date table
needs a rebuild; new groups hold one instrument, so occupancy degrades; deletion is not implemented.

**Grouped diagonal gamma** (`bench_gamma_grouped`, portable backend, `bench_gamma_grouped_3runs.txt`): the extra
diagonal-gamma kernel costs 0.77-0.83x the first-order kernel (about 48 ms against 61 ms); 59,820 oracle checks at 2.6e-12.
IBOR amounts in that diagonal are frozen off the projection curve. On this single SONIA curve those forecasts
move with discount, and the diagonal does not include that movement. The timing still includes the term-rate
third of the book under that freeze. Scope is fixed flows plus the lagged-overnight correction.
