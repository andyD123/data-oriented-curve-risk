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
streaming stores 31.4 ms. On the portable auto-vectorised backend (no intrinsics) GRP takes 49.9 ms (467× vs BASE).
GRP agrees with SCAN to 3.6e-15 relative.

**Write-only reference.** Streaming 273.5 MB of zeros (the padded output size) took 8.3-10.2 ms across
repeated runs on this machine (27-33 GB/s; `write_floor`, three runs saved), so the 31.4 ms kernel is roughly
3-4× the write-only time.

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
| 500,000 | 64,750 | 96.5% | 0.89 |

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
