# QuantLib reconciliation — node bumps on LogLinear curves

`ql_examples.cpp` builds QuantLib 1.33's MulticurveBootstrapping curves (quotes verbatim) with
`PiecewiseYieldCurve<Discount, LogLinear>`, freezes the nodes, prices the seven instruments, and writes
central-difference bucket risk at two step sizes plus the replication terms (`cashflows2.txt`).
`scan_reconcile.cpp` replicates the terms into unit cashflows with `ladder/replicate.hpp` and runs the
library scan; `compare2.py` reports per-instrument agreement, the eps^2 ratio and the exact zeros.

```
g++ -O2 -std=c++17 ql_examples.cpp -o ql_examples -lQuantLib && ./ql_examples
g++ -O2 -std=c++20 -I../.. scan_reconcile.cpp -o scan_reconcile && ./scan_reconcile
python3 compare2.py
```
Recorded: OIS and projection risk at 4e-10 relative (eps = 5e-5) with ratios 3.8-4.0; PVs exact; all
scan zeros confirmed at noise level in QuantLib. The same harness with `Cubic` in place of `LogLinear`
(`sed`) gives the negative control: up to 15% disagreement, ratios ~1, phantom risk past maturity.

## Quote-risk composition

`ql_examples` now writes the direct full-rebootstrap quote risk and the bootstrap Jacobian at two quote-bump
steps, `1e-6` and `5e-7`. After `scan_reconcile`, run:

```sh
python3 quote_risk_check.py --directory .
```

The checker Richardson-extrapolates both direct quote risk and the Jacobian, multiplies the seven-instrument
book's interval-forward ladder by that Jacobian, reports the Eonia-to-Euribor cross block, and exits non-zero if
the relative error exceeds `1e-10` or the expected cross-block count is not 289. With CMake/QuantLib/Python
available, CTest runs the same generation-and-check sequence automatically.
