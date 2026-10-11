# benchmark — library kernel on a seasoned book

Bonds, IBOR swaps and compounded-OIS swaps on a single SOFR-style RFR curve (`rfr_curves.txt`, illustrative par OIS quotes; the forward-projection block carries the same curve, as in a one-curve RFR market).

**Using a real curve.** `make_rfr_curves_boe.py` builds a curve file from the Bank of England's published SONIA OIS spot curve (same 30 + 36 node grid; log-linear in discount factors between the 0.5y-25y pillars, flat forward beyond). The data are the Bank's copyright and are **not** redistributed here (see bankofengland.co.uk/legal; re-use needs the Bank's permission). Download `https://www.bankofengland.co.uk/-/media/boe/files/statistics/yield-curves/oisddata.zip`, then:

```
python3 examples/benchmark/make_rfr_curves_boe.py oisddata.zip --date 2026-09-30   # writes rfr_curves_boe.txt (git-ignored)
bench_library 100000 1 3 path/to/rfr_curves_boe.txt
```

Cite as: Bank of England, UK OIS spot curve (Bloomberg Finance L.P. and Bank calculations), curve date and download date. Published timings in this repository use the illustrative curve; the BoE curve was only used for a 20,000-instrument correctness check.
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
