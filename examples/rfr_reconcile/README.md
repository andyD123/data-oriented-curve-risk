# RFR reconciliation (single overnight-indexed curve)

An independent, dependency-free reference for the reconciliation in the paper's Section 11, on a single
SOFR-style discount curve. There is no projection curve: floating legs are daily-compounded overnight legs, so a
coupon is `N (D(a)/D(b) - 1)` paid at `p`.

The par OIS quotes in `rfr_reference.py` are **illustrative, not a market snapshot**. Times are years from the
valuation date, and no calendar logic is involved.

| file | role |
|---|---|
| `rfr_reference.py generate` | bootstrap (log-linear discount factors), value the book, export unit cashflows, write finite-difference wave risk at five bump sizes |
| `scan_wave <dir>` | the C++ reverse scan on `unit_cashflows_wave.txt` and `wave_buckets.txt` |
| `rfr_reference.py check` | direct-overlap oracle, explicit adjoint, finite-difference convergence, quote risk (wave risk x bootstrap Jacobian vs re-bootstrap) and the Hagan hedge solve |

Run `ctest -R rfr_reconcile_replay`, or by hand:

```sh
python3 examples/rfr_reconcile/rfr_reference.py generate /tmp/rfr
build/examples/rfr_reconcile/scan_wave /tmp/rfr
python3 examples/rfr_reconcile/rfr_reference.py check /tmp/rfr
```

Book: 5y spot OIS, 5y 1y-forward OIS, a seasoned 4.5% bond, a zero-coupon bond, a 5y OIS with 2-day payment lag, a
7y linearly amortising swap, a swap with a 2-month front stub and 25 bp spread (5y2m), and a 35y bond whose
cashflows run beyond the last 30y pillar (exercising the open-ended last wave). Fifteen par OIS hedge instruments
(1y to 30y) follow in the same fixture.
