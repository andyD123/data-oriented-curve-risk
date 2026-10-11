# benchmark — library kernel on a seasoned book

Bonds, IBOR swaps and compounded-OIS swaps on a single SOFR-style RFR curve (`rfr_curves.txt`, illustrative par OIS quotes; the forward-projection block carries the same curve, as in a one-curve RFR market).

**Using a real curve (private run).** See [docs/BOE_CURVE.md](../../docs/BOE_CURVE.md) for downloading the Bank of England SONIA OIS curve and running the benchmark and reconciliation on it locally. Outputs stay git-ignored.

Schedules are walked back from the last regular payment date with a random remaining life, so the book is seasoned;
the grouping key is (type, maturity day) and groups mix seasoned and new trades on the same grid.

```
g++ -O3 -std=c++20 -march=native -ffp-contract=fast -I../.. -Wno-unused-result bench.cpp -o bench
./bench 500000 1 3        # N, run bump-and-reprice baseline (needs ~1 GB more at 500k), repetitions
```
Results on an Intel i7-12700K (AVX2, no AVX-512), GCC 13.3, one thread, are in
[BENCHMARKS.md](../../BENCHMARKS.md) and `benchmarks/results/2026-10-11_i7-12700K/`: at 500k instruments the baseline takes
24,045 ms per curve update, the scalar scan 458.5 ms and the grouped kernel 31.5 ms (764x; 96.5% lane occupancy;
grouped vs scalar 3.6e-15).
