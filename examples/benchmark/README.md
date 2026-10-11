# benchmark — library kernel on a seasoned book

Bonds, IBOR swaps and compounded-OIS swaps on a single SOFR-style RFR curve (`rfr_curves.txt`, illustrative par OIS quotes; the forward-projection block carries the same curve, as in a one-curve RFR market).

**Using a real curve (private run).** `make_rfr_curves_boe.py` builds a curve file from the Bank of England's published SONIA OIS spot curve (same 30 + 36 node grid; log-linear in discount factors between the 0.5y-25y pillars, flat forward beyond). The curve is GBP / SONIA.

The data are the Bank's copyright and are **not** redistributed here (see bankofengland.co.uk/legal; the terms allow personal / internal non-commercial use and re-use needs the Bank's permission). Nothing derived from the Bank's data, including benchmark or reconciliation results, is committed to this repository or published in the paper.

1. Download the archive yourself (browser, `curl` or `wget`) from the Bank's yield-curve statistics page, `https://www.bankofengland.co.uk/statistics/yield-curves`; the direct link is `https://www.bankofengland.co.uk/-/media/boe/files/statistics/yield-curves/oisddata.zip`. Keep it outside the repository.
2. Build the curve file for a date that exists in the archive. The script reads the "spot curve" sheet of the newest workbook in the zip that contains the date and stops with an error if the date is absent:

```
python3 examples/benchmark/make_rfr_curves_boe.py /path/to/oisddata.zip --date 2026-08-28   # writes rfr_curves_boe.txt (git-ignored)
```

3. Run the benchmark on it (all `bench_library` arguments are positional; the fourth is the curve file):

```
bench_library 500000 1 3 examples/benchmark/rfr_curves_boe.txt
```

   The other benchmark programs read `rfr_curves.txt` from their working directory, so for them copy the BoE file over a *copy* of `rfr_curves.txt` in a build directory, never over the committed file.
4. For the reconciliation (`examples/rfr_reconcile`), copy `rfr_reference.py` into a git-ignored folder, replace `QUOTES` with the par OIS rates implied by the BoE curve, `S_T = (1 - D(T)) / sum_{j=1..T} D(j)` at the pillars in `PILLARS`, and run `rfr_reference.py generate`, `scan_wave` and `rfr_reference.py check` there. These par quotes are generated from a fitted curve, so the check is a self-consistency check, not a market test.
5. Write all outputs under a git-ignored path. `.gitignore` already covers `examples/benchmark/rfr_curves_boe.txt`, `benchmarks/results/*boe*/` and `oisddata.zip`.

If you publish anything derived from the output, cite: Bank of England, UK OIS spot curve (Bloomberg Finance L.P. and Bank calculations), with the curve date and the download date. Published timings in this repository use the illustrative curve.

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
