# benchmark — library kernel on a seasoned book

Bonds, IBOR swaps and compounded-OIS swaps on a single RFR curve. No curve data is committed: the build writes a synthetic flat `rfr_curves.txt` into the build directory for tests and smoke runs, and the published results use the Bank of England SONIA curve; the forward-projection block carries the same curve, as in a one-curve RFR market.

**Using the Bank of England SONIA curve.** The published results use it. See [docs/BOE_CURVE.md](../../docs/BOE_CURVE.md) for downloading it and running the benchmark and reconciliation. The curve file stays git-ignored.

Schedules are walked back from the last regular payment date with a random remaining life, so the book is seasoned;
the grouping key is (type, maturity day) and groups mix seasoned and new trades on the same grid.

```
g++ -O3 -std=c++20 -march=native -ffp-contract=fast -I../.. -Wno-unused-result bench.cpp -o bench
./bench 500000 1 3        # N, run bump-and-reprice baseline (needs ~1 GB more at 500k), repetitions
```
Results on an Intel i7-12700K (AVX2, no AVX-512), GCC 13.3, are in
[BENCHMARKS.md](../../BENCHMARKS.md). The headline is the pinned ablation
(`benchmarks/results/i7-12700K_pinned/`): at 500k instruments, logical CPU 2, median of three,
the baseline takes 22,929 ms and the streaming grouped kernel 33.0 ms (695x, 1.00 ns).
The earlier unpinned best-of-three in `benchmarks/results/i7-12700K/` is 23,482 ms, 462.4 ms and
39.1 ms (601x); that run's core was not controlled. Grouped vs scalar agreement on that book is 3.5e-15.
