# The Bank of England SONIA curve

The benchmarks and reconciliation in the paper use this curve. The data are not in this repository, so this page shows how to obtain it and repeat the runs. No curve data is committed; the build writes a synthetic flat curve into each example's build directory so the tests run without third-party data.

`make_rfr_curves_boe.py` builds a curve file from the Bank of England's published SONIA OIS spot curve (same 30 + 36 node grid; log-linear in discount factors between the 0.5y-25y pillars, flat forward beyond). The curve is GBP / SONIA.

The data are the Bank's copyright and are **not** redistributed here (see bankofengland.co.uk/legal; the terms allow personal / internal non-commercial use and re-use needs the Bank's permission). The repository and the paper therefore contain no rates and no rate-derived values (quotes, present values, hedge notionals, rate sensitivities, the curve file itself). They contain timings, ratios and agreement between methods only.

1. Download the archive yourself (browser, `curl` or `wget`) from the Bank's yield-curve statistics page, `https://www.bankofengland.co.uk/statistics/yield-curves`; the direct link is `https://www.bankofengland.co.uk/-/media/boe/files/statistics/yield-curves/oisddata.zip`. Keep it outside the repository.
2. Build the curve file for a date that exists in the archive. The script reads the "spot curve" sheet of the newest workbook in the zip that contains the date and stops with an error if the date is absent:

```
python3 examples/benchmark/make_rfr_curves_boe.py /path/to/oisddata.zip --date YYYY-MM-DD   # writes rfr_curves_boe.txt (git-ignored)
```

3. Run the benchmark on it (all `bench_library` arguments are positional; the fourth is the curve file). The drivers used for the published results take an optional curve argument:

```
bench_library 500000 1 3 examples/benchmark/rfr_curves_boe.txt
benchmarks/run_headline_set.sh   <avx2 build> <portable build> <out dir> <curve file>   # unpinned headline set
benchmarks/run_pinned_campaign.sh <build> <out dir> 2 <curve file>                       # pinned ablation, threads, insert
benchmarks/run_cpu_sweep.sh      <build> <out dir> <curve file>                          # per-CPU variation
```

   The other benchmark programs read `rfr_curves.txt` from their working directory, so for them copy the BoE file to `rfr_curves.txt` in the program's build directory (`build/examples/benchmark`, `benchmark_paper`, `aggregation`). The build only writes its synthetic curve when that file is absent, so it never overwrites yours.
4. For the reconciliation (`examples/rfr_reconcile`), generate the fixtures from the curve file into a git-ignored folder, then scan and check there. The projection pass has the same option:

```
D=benchmarks/results/boe_reconcile        # matches the git-ignored *boe* pattern
mkdir -p $D
python3 examples/rfr_reconcile/rfr_reference.py generate $D --curve examples/benchmark/rfr_curves_boe.txt
# run scan_wave in $D (see examples/rfr_reconcile/README.md), then:
python3 examples/rfr_reconcile/rfr_reference.py check $D
# projection pass (term-rate swaps): same pattern with rfr_projection_check.py generate/check
```

   The check prints rate-derived values (a base present value, quote sensitivities, hedge notionals). Do not copy those anywhere public; the agreement figures (residuals, ratios, relative differences) are safe to report.

   `--curve` converts the curve to par OIS quotes, `S_T = (1 - D(T)) / sum_{j=1..T} D(j)` at the pillars. These quotes come from a fitted curve, so the check is a self-consistency check, not a market test.
5. Write reconciliation fixtures and logs under a git-ignored path. Benchmark outputs contain only timings and may be committed after you check them for rates. `.gitignore` already covers `examples/benchmark/rfr_curves_boe.txt`, `benchmarks/results/*boe*/` and `oisddata.zip`.

If you publish anything derived from the output, cite: Bank of England, UK OIS spot curve (Bloomberg Finance L.P. and Bank calculations), with the curve date and the download date.
