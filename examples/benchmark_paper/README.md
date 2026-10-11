# benchmark_paper: adjoint comparison, write-only reference, LRU date cache

Supporting benchmarks for the paper. They read the RFR curve in `rfr_curves.txt` from the working directory (30 + 36 forward buckets; the committed file is an invented placeholder, and the published results used a copy built from the Bank of England SONIA curve, see docs/BOE_CURVE.md). All results quoted here are in
[`benchmarks/results/i7-12700K/`](../../benchmarks/results/i7-12700K/): one thread, Intel
i7-12700K (AVX2, no AVX-512), GCC 13.3, unpinned. The headline 500,000-instrument benchmark is
`../benchmark/bench.cpp` (`bench_library`); see [BENCHMARKS.md](../../BENCHMARKS.md).

## Adjoint baselines (`adjoint_bench.cpp`)

Scalar, one core, replication excluded. `./adjoint_bench 500000 2`.

| | 500k | ns / sensitivity |
|---|---|---|
| reverse scan, N+K | 269.5 ms | 8.2 |
| tape-free N·K overlap adjoint (geometry known) | 639.8 ms | 19.4 |
| reverse mode with a recorded tape (960 nodes in the last instrument) | 5,402.9 ms | 163.7 |

All agree to about 3e-11. The recorded-tape timing includes expression construction, exponentials, tape recording
and reverse traversal; this experiment does not isolate tape-memory traffic or establish the cost of other AAD
implementations. The geometry-aware N·K adjoint is within 2.4x of the scan at scalar level.

## Write-only reference (`write_floor.cpp`)

Streams zeros into a buffer of the size of the 500k book's padded output (273,504,000 bytes) with non-temporal
stores (AVX-512 or AVX2; plain `memset` on a portable build). Three runs: 8.3 to 8.6 ms (about 32 GB/s) on this
machine; further runs gave up to 10.2 ms. `./write_floor 273504000 15`.

## LRU date cache: grouping trades by shared schedule (`scenario_bench.cpp`)

This example shows why the order in which trades are priced matters when scenario curves are cached by date.
It computes the 132 wave scenarios (30 discount-curve and 36 projection-curve nodes, each bumped up and down)
for a book of bonds and vanilla swaps by repricing on a scenario-vector curve. Same book specification as the
§9 benchmark.

### How the pricing is vectorised

For each payment date the curve supplies one column of 136 doubles: the discount (or projection) factor at that
date for every scenario, padded from 132 to a multiple of 8. A fixed cashflow is priced with one broadcast of its
amount and a multiply-add down the column, which is 17 AVX-512 FMAs on contiguous memory. The pricing loop has no
per-scenario interpolation and no `exp`; those are done once per column, when the column is filled.

Columns are computed on demand and held in an LRU cache with a fixed number of columns per curve. A miss locates
the curve interval once and fills the column; a hit reuses it. A column is 1,088 bytes, so 64 columns are 70 KB
per curve.

### Why the order matters

Trades on the same schedule pay on the same dates. Priced one after another, ordered by tenor, they reuse the same
columns while those columns are in the cache; when the group ends its columns are evicted and the next group's are
loaded. The cache therefore needs only as many columns as the longest schedule has dates: 58 in this book
(29 years, semi-annual), so 64 columns are enough and 32 are not. A 15-year semi-annual book (30 dates) would need 32.

In random order, consecutive trades use unrelated dates. The cache keeps evicting columns that are needed again
soon, and it stops recomputing them only when it can hold every date in the book.

The floor is each column computed once per curve: 7,020 column evaluations for this book (3,480 dates used by the
discount curve plus 3,540 used by the projection curve).

### Reading the output

A run prints five blocks:

1. **Header**: the instruction set and compiler the binary was built with, the book, the 132 scenarios and the
   sensitivities they produce (66 per instrument, from central differences of the up/down pairs).
2. **terms**: definitions, including the miss floor for this book (7,020: 3,480 discount-curve dates plus 3,540
   projection-curve dates) and the three orders.
3. **result**: the demonstration. One row per cache size and order; time is for one curve update, starting from an
   empty cache. The last lines give grouped against random at each cache size, and say when grouped is at the floor.
   Above 100,000 trades the random rows below 1,024 columns are not run unless `LRU_ALL=1` is set.
4. **context**: the same book priced from a full table (every date's column computed up front, no cache), the cost
   of building that table per curve update, and the scalar reverse scan. With the baseline argument set to 1, a
   per-scenario repricing row (BASE) and speed-ups against it are added.
5. **accuracy and correctness gate**: every run is compared with the scalar scan, relative to max(|value|, 1e4).
   About 5e-8 is the truncation error of the central difference at eps = 1e-5. The gate fails if any run differs
   by more than 1e-6 or produces a non-finite value; the program then names the run and exits with status 1.

Malformed arguments, curve files and `LRU_CAPS` values are rejected with a message and exit status 1.

### Results (100,000 trades, `scenario_bench_100k.txt`)

Three orders are measured: **random** (shuffled); **sorted** (instrument type, then start date, then tenor); and
**grouped** (start date, then type, then tenor, so trades sharing a schedule are adjacent; a prototype ordering in the
code). Misses are column evaluations, both curves together; time is per curve update, best of 3 runs.

| columns per curve | cache per curve | random: misses | random: time | sorted: misses | sorted: time | grouped: misses | grouped: time |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 17 KB | 5,185,875 | 1,733 ms | 4,778,344 | 1,609 ms | 4,778,344 | 1,600 ms |
| 32 | 35 KB | 4,842,108 | 1,657 ms | 3,264,181 | 1,183 ms | 3,264,181 | 1,170 ms |
| **64** | **70 KB** | **4,469,511** | **1,575 ms** | **10,500** | **221 ms** | **7,020** | **217 ms** |
| 128 | 139 KB | 4,366,199 | 1,565 ms | 10,500 | 220 ms | 7,020 | 215 ms |

At 64 columns grouped is 7.3x faster than random and at the miss floor. For comparison on the same book, the scalar
reverse scan (sensitivities only, no scenarios) takes 52.8 ms.

### Sorted against grouped: the 10,500 misses

The sorted order prices all bonds before all swaps. A swap pays on the same discount dates as a bond with the same
start date, but by the time the swaps are priced those columns have been evicted. At 64 columns the projection
curve is at its floor (3,540 misses) and the discount curve has 6,960: every discount column is computed twice.

The grouped order puts bonds and swaps that share dates next to each other, and reaches the floor of 7,020 at 64
columns. Here the 3,480 extra fills in sorted order cost little next to the pricing itself, so the two run in about
the same time; the large difference is between random order and either schedule-based order.

In a seasoned book, where schedules are generated backwards from the last regular payment date, the
corresponding grouping key is the last regular payment date (its position in the payment cycle), then maturity.

### Reproducing

From the build directory (`build/examples/benchmark_paper`):

```
LRU_CAPS=16,32,64,128 ./scenario_bench 100000 0 3
LRU_ALL=1 LRU_CAPS=64 ./scenario_bench 500000 0 2
```

Arguments are trades, baseline (1 adds the per-scenario repricing baseline) and repetitions. Above 100,000 trades
the random-order runs below 1,024 columns are skipped unless `LRU_ALL=1` is set. The program changes into its own
data directory, so it can be run from anywhere.
