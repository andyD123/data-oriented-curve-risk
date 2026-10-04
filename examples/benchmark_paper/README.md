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

## Vectorised LRU date cache: the layout optimisation without the scan (`scenario_bench.cpp`)

Repeated wave valuation with an on-demand cache in front of the curve: each entry is one aligned column of discount
(or projection) factors for all 132 wave scenarios at one date; a miss locates the interval once and fills the
column; a hit is one broadcast and a contiguous multiply-add. Sorting instruments by schedule is the cache policy.
Same book specification as the §9 benchmark.

```
g++ -O3 -std=c++20 -march=native -ffp-contract=fast -I../.. -Wno-unused-result scenario_bench.cpp -o scenario_bench
./scenario_bench 100000 1 2                         # with the per-scenario baseline and the capacity sweep
LRU_ALL=1 LRU_CAPS=64 ./scenario_bench 500000 0 2   # like-for-like random vs sorted at 64 columns per curve
```

500k, 64 columns per curve (70 KB): sorted 1,310 ms with 10,500 column evaluations; random 10,921 ms with 22.3 million.
100k capacity sweep (`recorded_lru_100k_sweep.txt`): sorted reaches the cold-start floor at 64 columns; random needs
the whole date set (4,096) to stop re-evaluating. A prebuilt full table (the batch special case) is also timed.
Agreement with the scan: 6.6e-8 (central-difference floor).
