# Hagan box waves on the reverse scan — QuantLib example

Risk defined as Hagan's forward-curve box shifts (Hagan & West 2006/2008; Hagan, Wilmott 2015),
computed by the reverse scan from unit cashflows, on QuantLib's **shipped** MulticurveBootstrapping
curves with their **cubic** interpolation left unchanged. Reconciled against QuantLib's own wave
bumps, then hedged against the example's par OIS and swaps.

The point: the scan needs the bump to leak nothing outside its bucket. Box waves satisfy that by
construction whatever the pricing curve, so the pricing interpolation is free to be whatever pricing
wants.

## Files

| file | what it does |
|---|---|
| `ql_waves.cpp` | QuantLib 1.33 C++: Eonia + Euribor6M curves from the shipped example (`PiecewiseYieldCurve<Discount, Cubic>`, quotes verbatim). The reconciliation population has eight instruments: the original seven used for the paper hedge plus `bond_35y_beyond_last_pillar`, which exercises Hagan's open final wave. Defines `BoxShifted`, writes wave risk by central difference, exports first-order dated weights and writes risk of 17 par hedge instruments. |
| `scan_wave.cpp` | Geometry-only reverse scan: unit cashflows + bucket boundaries in, wave ladder out. No curve inside. |
| `compare.py` | Scan vs QuantLib wave bumps per instrument; the residual must fall ~4× when ε halves. |
| `hagan_hedge.py` | Hagan maturity-aligned hedge. Defaults to the named seven-instrument `paper7` population and uses explicit back-substitution; `--book extended8` includes the 35-year boundary-test bond. |
| `hedge.py` | Older least-squares comparison on the full pillar-wave ladder; retained for comparison only. |
| `run.sh` | Build (CMake or make), generate, scan and compare recorded wave risk. |

Requires QuantLib C++ (`apt install libquantlib0-dev` on Ubuntu 24.04 gives 1.33), GCC ≥ 13, numpy.

## The wave in QuantLib

```cpp
Real overlap(Time t) const {
    const Real inside = std::max(t - t0_, 0.0);
    return open_last_ ? inside : std::min(inside, t1_ - t0_);
}

DiscountFactor discountImpl(Time t) const override {
    return base_->discount(t, true) * std::exp(-delta_ * overlap(t));
}
```
Interior waves use the bounded policy. The final wave uses `open_last_ = true`, matching Hagan (2015,
eq. 2.2b/2.3b). The actual implementation is in `ql_waves.cpp`; this excerpt only shows the terminal policy.
Relink the handle the instruments price off; at-par coupon fixings on the projection curve follow automatically.
Times are in the base curve's own basis (the Euribor curve's reference date is the settlement date).

## Recorded results (this directory's `*.txt`)

Scan vs QuantLib wave bumps on the shipped cubic curves:

| instrument | max rel (ε=1e-4) | max rel (ε=5e-5) | ε² ratio |
|---|---|---|---|
| swap 5y (example) | 4.1e-10 | 1.0e-10 | 3.64 |
| swap 1y5y forward (example) | 4.1e-10 | 1.0e-10 | 3.59 |
| bond 4.5% 2007–2017 (example) | 1.6e-09 | 4.1e-10 | 3.97 |
| zero-coupon Aug 2013 (example) | 4.9e-11 | 1.3e-11 | 1.80 (at rounding floor) |
| OIS 5y, 2-day lag | 1.7e-09 | 4.2e-10 | 3.94 |
| amortising 7y | 4.2e-10 | 1.0e-10 | 3.65 |
| 6y, front stub, 25 bp spread | 2.8e-10 | 7.0e-11 | 3.40 |

Book wave ladder per 1 bp (largest): Euribor6M 2–3y 370, 3–4y 356, 4–5y 339, 5–6y 223; Eonia 2–5y ≈ 94 each;
monthly FRA waves 0.5–2y 25–35 each; nothing past 7y.

Legacy least-squares comparison against the example's par OIS 2–10y and par swaps 3–10y (rank 17):
pay OIS 5y 1.00M, IRS 4y 0.14M, IRS 5y 1.14M, IRS 6y 2.14M, IRS 7y 0.14M.
Residual wave risk ≤ 29 per bp against a book maximum of 370 (FRA-region and stub risk the annual swaps cannot
represent). Parallel 1 bp: book 2,468, hedge 2,482.

For contrast, defining risk as bumps of the cubic curve's nodes on the same curves makes the same scan disagree
by up to 15% with risk reported past the swap's maturity — see the main paper, §7.

## Exactness: the finite difference converges to the scan

Residual |QuantLib wave bump − scan| summed over the 66 waves, as the bump size halves (`wave_risk_<eps>.txt`):

| instrument | ε=4e-4 | 2e-4 | 1e-4 | 5e-5 | 2.5e-5 | successive ratios |
|---|---|---|---|---|---|---|
| swap 5y | 2.40e-2 | 5.99e-3 | 1.50e-3 | 4.13e-4 | 1.77e-4 | 4.00 3.99 3.64 2.33 |
| swap 1y5y fwd | 2.83e-2 | 7.08e-3 | 1.77e-3 | 4.94e-4 | 2.08e-4 | 4.00 3.99 3.59 2.38 |
| bond 4.5% | 7.21e-6 | 1.80e-6 | 4.51e-7 | 1.14e-7 | 3.04e-8 | 4.00 4.00 3.97 3.74 |
| zero-coupon | 2.20e-8 | 5.54e-9 | 1.68e-9 | 9.38e-10 | 2.65e-9 | 3.97 3.29 1.80 0.35 |
| OIS 5y lag 2 | 9.16e-2 | 2.29e-2 | 5.73e-3 | 1.46e-3 | 3.80e-4 | 4.00 4.00 3.94 3.83 |
| amortising 7y | 1.80e-2 | 4.50e-3 | 1.13e-3 | 3.09e-4 | 1.16e-4 | 4.00 3.99 3.65 2.67 |
| stub + spread 6y | 1.99e-2 | 4.99e-3 | 1.25e-3 | 3.68e-4 | 1.90e-4 | 4.00 3.99 3.40 1.94 |

The residual is the central difference's own O(ε²) truncation: it falls by exactly 4 per halving until the
rounding floor of the bump (PV rounding / ε) takes over at the smallest steps. Richardson extrapolation
(4·FD(5e-5) − FD(1e-4))/3 removes the ε² term and agrees with the scan to 3e-12 … 1.6e-11 of each
instrument's largest wave — double precision. The scan's value is the limit the finite difference is
converging to.

## Exactness without a step size: adjoint reference (`aad_check.py`)

`aad_check.py` contracts the exported first-order dated weights with the explicit wave overlaps and compares
that N·K derivative with the scan at zero shift. For products/ratios the exported signed records are a first-order
representation; they are not asserted to reproduce the full finite-shock valuation. Against the scan the explicit
derivative agrees at 2e-18 … 1.6e-16 relative on the recorded population, with bitwise agreement on the zero-coupon
case. The scan evaluates the same zero-shift contraction with the overlap structure replaced by date order.

## Hedging as Hagan states it (`hagan_hedge.py`) — paper7 by default

Hagan (2015, eqs. 2.2, 2.7–2.10) puts one wave per hedge instrument at its maturity. With the hedge instruments
ordered by maturity, `H.T` is upper-triangular and `hagan_hedge.py` solves `H.T a = -g` by explicit back-substitution.
The default `--book paper7` excludes `bond_35y_beyond_last_pillar`, because that bond was added only to test the
terminal wave and is not part of the paper's hedge population. `--book extended8` includes it and produces a
different long-end hedge, as it should.

For `paper7`: nine Eonia waves (par OIS 2–10y), eight Euribor waves (par swaps 3–10y); the residual is of order
1e-14 per bp. Principal notionals are receive OIS 5y about 1.00M, IRS 5y 1.14M, IRS 6y 2.14M, IRS 4y and 7y
0.14M each, and IRS 3y about -0.05M. The script prints the selected instrument names and units before the solve.
`hedge.py` is the older least-squares comparison and is not the paper's maturity-aligned hedge.

## Last-wave convention and the instrument that tests it

Hagan's last wave (2015, eq. 2.2b) extends flat beyond the final maturity; the library's `Stencils::open_last` selects
that convention (false = box of length ℓ_K). An earlier version of the scalar scan double-counted cashflows beyond
B[K] (uncapped interior weight plus the ℓ_K tail) and no example reached that region; found in external review.
Fixed in `ladder/scan.hpp` and `scan_simd.hpp`, with `tests/test_ladder.cpp` now covering both conventions against the
direct overlap adjoint. Here `ql_waves` applies the open last wave in `BoxShifted` and the book includes
`bond_35y_beyond_last_pillar` (35y 4% annual, beyond the Eonia curve's 30y pillar): finite-difference ratio 4.00,
adjoint 1.6e-16 (`compare.py`, `aad_check.py`).

## Non-pillar reporting grid

Hagan's risk grid is independent of the calibration grid. Put boundary times (years from today, first entry 0) in
`risk_grid.txt` and `ql_waves` applies box waves on that grid to both curves, last wave open. With the annual grid in
`risk_grid_annual_example.txt` (0–10y): finite-difference ratios 3.97–4.01, adjoint agreement 1e-16 on all eight
instruments. Remove the file to return to the pillar grid.
