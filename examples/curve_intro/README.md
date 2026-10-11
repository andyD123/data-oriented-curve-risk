# First-order curve-risk introduction

This is the shortest way into the public API. Start with
[the C++ program](curve_intro.cpp), then work through these four operations:

1. Build a log-linear discount curve with a Hagan-wave grid:
   `Curve::flat({0.0, 0.5, 1.0, 2.0, 3.0}, 0.03)`.
2. Convert dated fixed payments to signed discounted unit cashflows using
   `replicate_fixed`. This is *not* the amount-only cashflow record: it is
   the contribution of a payment to PV at the base curve.
3. Run `scan_discount` to obtain every derivative with respect to an
   additive instantaneous-forward **wave**. The cashflows must be sorted
   by time.
4. Independently reprice one bounded or open-final-wave shock with
   `Curve::df_bumped`, and check the central-difference gradient.

Run on any backend:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target curve_intro
./build/examples/curve_intro/curve_intro
```

`Curve` stores discount factors as log values between user-supplied
boundaries, with an implicit valuation node `D(0)=1` when the first
supplied boundary lies after zero. It extrapolates the last log-discount
slope for maturity beyond the grid. `open_last` governs whether the
*last wave bump* continues beyond the final boundary; it does not turn
base-curve extrapolation on or off.

The displayed derivatives are per **unit instantaneous-forward-rate
shift**. Multiplying by `1e-4` gives a linearised one-basis-point PV
change, not exact finite-shock P&L.

For grouped SIMD/portfolio risk, proceed to
[the library benchmark](../benchmark/README.md).
For nonlinear same-curve OIS and second-order risk, continue with
[the cross-gamma example](../cross_gamma/README.md).
