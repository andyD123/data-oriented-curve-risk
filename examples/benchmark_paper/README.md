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
