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
| `ql_waves.cpp` | QuantLib 1.33 C++: Eonia + Euribor6M curves from the shipped example (`PiecewiseYieldCurve<Discount, Cubic>`, quotes verbatim), seven instruments (the example's 5y and 1y5y swaps, the Bonds example's 4.5% 2007–2017 and zero-coupon bonds, an OIS with 2-day lag, an amortising 7y, a 6y with front stub and spread). Defines `BoxShifted`, a `YieldTermStructure` that multiplies discount factors by exp(−δ·overlap). Writes wave risk by central difference at two step sizes, unit cashflows priced on the cubic curves, and wave risk of 17 par hedge instruments. |
| `scan_wave.cpp` | Geometry-only reverse scan: unit cashflows + bucket boundaries in, wave ladder out. No curve inside. |
| `compare.py` | Scan vs QuantLib wave bumps per instrument; the residual must fall ~4× when ε halves. |
| `hedge.py` | Book ladder and the least-squares hedge `H·q = r` against the par instruments. |
| `run.sh` | Build (CMake or make), generate, scan, compare, hedge. |

Requires QuantLib C++ (`apt install libquantlib0-dev` on Ubuntu 24.04 gives 1.33), GCC ≥ 13, numpy.

## The wave in QuantLib

```cpp
class BoxShifted : public YieldTermStructure {
    Handle<YieldTermStructure> base_; Time t0_, t1_; Real delta_;
public:
    BoxShifted(Handle<YieldTermStructure> b, Time t0, Time t1, Real d)
    : YieldTermStructure(b->dayCounter()), base_(std::move(b)), t0_(t0), t1_(t1), delta_(d) { registerWith(base_); enableExtrapolation(); }
    Date maxDate() const override { return base_->maxDate(); }
    const Date& referenceDate() const override { return base_->referenceDate(); }
    Calendar calendar() const override { return base_->calendar(); }
    Natural settlementDays() const override { return base_->settlementDays(); }
protected:
    DiscountFactor discountImpl(Time t) const override {
        Real overlap = std::min(std::max(t - t0_, 0.0), t1_ - t0_);
        return base_->discount(t, true) * std::exp(-delta_ * overlap);
    }
};
```
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

Hedge against the example's par OIS 2–10y and par swaps 3–10y (least squares, rank 17):
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

Under `BoxShifted` QuantLib's PV is `sum_i x_i * prod_k exp(-delta_k * ov_ki)` with `x_i` its own discounted
unit cashflows, so taping that expression and running the reverse pass is the adjoint of the real valuation
with respect to the wave sizes. Against the scan: 2e-18 … 1.3e-16 relative on the six multi-cashflow
instruments, bitwise on the zero-coupon bond. Tape length N·K (1,645 nodes for the 5y swap) against N+K
operations for the scan (156): the scan is the same adjoint with the tape replaced by the date order.

## Hedging as Hagan states it (`hagan_hedge.py`) — supersedes the least-squares `hedge.py`

Hagan (2015, eqs 2.2, 2.7–2.10) puts one wave per hedge instrument at its maturity, so the sensitivity matrix is square
and lower-triangular and the solve is a back-substitution. Built here from the pillar-wave ladders (adjacent box shifts
add). Nine Eonia waves (par OIS 2–10y), eight Euribor waves (par swaps 3–10y); residual 1.2e-14 per bp.
Hedge: receive OIS 5y 1.00M, IRS 5y 1.14M, IRS 6y 2.14M, IRS 4y and 7y 0.14M, IRS 3y −0.05M; zero beyond the
book's last paydate (his 3.1). `hedge.py` (least squares on all 66 pillar waves) is kept for comparison only.

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
