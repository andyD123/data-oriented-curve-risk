# benchmark_paper — the §9 configuration, exactly

Reproduces the table in §9 of the paper: 500,000 instruments, half semi-annual fixed-rate bonds and half
annual-30/360-vs-6M-Act/360 swaps, maturities 1–29y, on the Eonia/Euribor6M curves of QuantLib's
MulticurveBootstrapping example (`quantlib_example_curves.txt`, 30 + 36 forward buckets), 66 sensitivities
per instrument, streaming output. This is the standalone kernel the paper's numbers were taken from
(bench2); the library kernel in `../benchmark` is the same algorithm in library form on a broader book that
includes compounded-OIS legs, and is reported separately (62.7 ms) — do not mix the two.

```
g++ -O3 -std=c++20 -march=native -mprefer-vector-width=512 -Wno-unused-result bench_paper.cpp -o bench_paper
./bench_paper 500000 1 3        # N, run baseline (1/0), repetitions
```
Recorded run (`recorded_500k.txt`, one Sapphire Rapids core, 2.1 GHz, VM): baseline 57,445 ms; scalar scan
1,272 ms; 8-lane normal stores 61.4 ms; 8-lane streaming stores 50.9 ms (1.54 ns per sensitivity);
64,034 groups, 97.6% lane occupancy; GRP vs scalar 3.4e-11; BASE vs scalar 2.7e-8.
Streaming-store floor for 270 MB on the same core: 15.7 ms (`../../session_artifacts` membw).
`recorded_sweep.txt` is the working-set staircase (10k–500k).

## Adjoint baselines (`adjoint_bench.cpp`)

Same book and curves. Scalar, one core, replication excluded. `g++ -O3 -std=c++20 -march=native -I../.. adjoint_bench.cpp -o adjoint_bench && ./adjoint_bench 500000 2`

| | 500k | ns / sensitivity |
|---|---|---|
| reverse scan, N+K | 431 ms | 13.1 |
| tape-free N·K overlap adjoint (geometry known) | 726 ms | 22.0 |
| reverse mode with a recorded tape (~2,500 nodes / instrument) | 8,784 ms | 266 |

All agree to 4e-11. The tape's cost is memory traffic; the geometry-aware N·K adjoint is within 1.7x of the scan at scalar level.

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

### Results

![LRU date cache, 100,000 trades: time per curve update against cache capacity, random order and sorted by schedule](lru_cache_100k.svg)

100,000 trades; misses are column evaluations, both curves together; time is per curve update, best of 3 runs.

| columns per curve | cache per curve | random: misses | random: hit rate | random: time | sorted: misses | sorted: hit rate | sorted: time |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 17 KB | 5,185,875 | 23.2% | 2,763 ms | 4,778,344 | 29.2% | 2,650 ms |
| 32 | 35 KB | 4,842,108 | 28.3% | 2,533 ms | 3,264,181 | 51.6% | 1,761 ms |
| **64** | **70 KB** | **4,469,511** | **33.8%** | **2,366 ms** | **10,500** | **99.8%** | **265 ms** |
| 128 | 139 KB | 4,366,199 | 35.3% | 2,621 ms | 10,500 | 99.8% | 261 ms |
| 256 | 279 KB | 4,154,015 | 38.5% | 2,395 ms | 10,500 | 99.8% | 256 ms |
| 1,024 | 1.1 MB | 2,889,082 | 57.2% | 1,620 ms | 10,500 | 99.8% | 238 ms |
| 4,096 | 4.5 MB | 7,020 | 99.9% | 329 ms | 7,020 | 99.9% | 255 ms |

500,000 trades, 64 columns per curve, best of 2 runs:

| order | misses | hit rate | time |
|---|---:|---:|---:|
| random | 22,348,094 | 33.9% | 13,116 ms |
| sorted by schedule | 10,500 | 99.97% | 1,305 ms |

At 64 columns, sorted order is 10x faster than random order at 500,000 trades and computes 2,128 times fewer
columns. Its misses do not grow with the book (10,500 at both sizes), because they depend only on the number of
distinct dates; random-order misses grow with the number of trades.

Agreement with the scalar scan: 5.1e-8 at 100,000 trades and 6.6e-8 at 500,000, which is the truncation error of
the central difference at eps = 1e-5.

Measured on one core of an Intel Xeon VM at 2.1 GHz (48 KB L1d, 2 MB L2), GCC 13.3, AVX-512. Miss counts and
hit rates are deterministic (fixed seeds); times vary between runs. The two recorded 500,000-trade runs at 64
columns, sorted (`recorded_lru_500k_cap64_both_orders.txt` and `recorded_lru_500k.txt`), differ by a factor of two.

### The sort key, and the 10,500 misses

The code sorts by instrument type, then start date, then tenor (`sig = type * 100000 + start * 100 + years`), so all
bonds are priced before all swaps. A swap pays on the same discount dates as a bond with the same start date, but
by the time the swaps are priced those columns have been evicted. At 64 columns the projection curve is at its
floor (3,540 misses) and the discount curve has 6,960, every discount column computed twice.

Grouping by schedule first (start date, then type, then tenor) puts bonds and swaps that share dates next to each
other and reaches the floor of 7,020 at 64 columns. This was measured by changing only the sort key in a copy of
the source; it is not yet in the code.

In a seasoned book, where schedules are generated backwards from the last regular payment date, the
corresponding key is the last regular payment date (its position in the payment cycle), then maturity.

### Reproducing

Built by the `release` preset with GCC or Clang (not with MSVC). From `build/release/examples/benchmark_paper`:

```
LRU_CAPS=16,32,64,128,256,1024,4096 ./scenario_bench 100000 0 3   # the 100,000-trade sweep above
LRU_ALL=1 LRU_CAPS=64 ./scenario_bench 500000 0 2                 # the 500,000-trade comparison at 64 columns
```

Arguments are trades, baseline (1 adds the per-scenario repricing baseline) and repetitions. Above 100,000 trades
the random-order runs below 1,024 columns are skipped unless `LRU_ALL=1` is set; the CLion configuration
`scenario_bench 500k` does not set it. Standalone:
`g++ -O3 -std=c++20 -march=native -ffp-contract=fast -I../.. -Wno-unused-result scenario_bench.cpp -o scenario_bench`.
