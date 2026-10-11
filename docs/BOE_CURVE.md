# Running on the Bank of England SONIA curve (private)

This is an optional, local-only recipe. Results are not published in this repository (data licence).

`make_rfr_curves_boe.py` builds a curve file from the Bank of England's published SONIA OIS spot curve (same 30 + 36 node grid; log-linear in discount factors between the 0.5y-25y pillars, flat forward beyond). The curve is GBP / SONIA.

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
