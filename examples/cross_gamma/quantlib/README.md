# QuantLib cross-gamma equivalence and reconciliation

This reader-oriented subproject accompanies the
[16-bucket worked example](../README.md). It distinguishes the archived
independent QuantLib v1.33 evidence from *optional live* second-order
repricing. The exact scope is deterministic Hagan **instantaneous-forward
box-wave bumps**, not zero-rate bumps, quote rebootstrap or stochastic
convexity adjustment.

## 1. Already reproducible: five-year QuantLib OIS, two-business-day lag

Upstream QuantLib generator:
[ql_waves.cpp](../../hagan_waves/ql_waves.cpp). The existing original
reference files remain in
[examples/hagan_waves](../../hagan_waves):

- cashflows_wave.txt — dated fixed/OIS/IBOR payoff descriptors.
- unit_cashflows_wave.txt — the corresponding first-order signed weights.
- wave_buckets.txt — 30 discount and 36 projection wave boundaries.
- wave_risk_5e-05.txt — recorded QuantLib *first-order* finite differences.

The focused C++ test
[test_cross_gamma_quantlib_archive.cpp](../../../tests/test_cross_gamma_quantlib_archive.cpp)
uses the actual five-year, two-business-day-lag OIS fixture. It independently
checks first-order scan output against archived QuantLib and checks the
cross-gamma formula against analytic differentiation and step-halved
four-point *reconstructed-coupon* finite differences.

From the repository root:

```sh
cmake -S . -B build-gamma -DLADDER_LANES=PORTABLE \
  -DLADDER_BUILD_EXAMPLES=ON -DLADDER_BUILD_QUANTLIB=OFF
cmake --build build-gamma --target test_cross_gamma_quantlib_archive
ctest --test-dir build-gamma -R '^cross_gamma_quantlib_archive$' --output-on-failure
```

For waves **19 and 20**, the archived OIS produces a frozen-flow cross-gamma
of about **+37.922**, a payment-lag correction of **-5,418.334**, and the
correct analytic model gamma of **-5,380.412**. Ignoring the nonlinear lag
correction reverses the sign.

**Evidence boundary:** The recorded QuantLib bump data is FIRST-ORDER only.
The second-order finite differences in this archived regression use
a re-implementation of the exported OIS valuation equation, not a new
QuantLib engine run.

## 2. Optional live QuantLib second-order exercise

The independent
[live_ois_cross_gamma.py](live_ois_cross_gamma.py) constructs a real
QuantLib.OvernightIndexedCoupon and prices it with freshly relinked
QuantLib.DiscountCurve instances under each Hagan-wave bump.

It checks base PV, accrual first derivative, frozen-vs-corrected
cross-gamma, and four independently repriced QuantLib scenarios for each
step of the mixed second derivative.

The supplied script has two scenarios:

| Scenario | Accrual and payment lag | Purpose |
|---|---|---|
| standard | Short forecast coupon, 2 business days | Conventional payment-delay case |
| stress | Approximately 20-year forward start, 65 business days | Amplify the model's lag curvature |

Run the algebra-only offline check first:

```sh
python examples/cross_gamma/quantlib/live_ois_cross_gamma.py --scenario offline
```

Then install the optional package on a machine where it is supported:

```sh
python -m pip install 'QuantLib>=1.33'
python examples/cross_gamma/quantlib/live_ois_cross_gamma.py --scenario standard
python examples/cross_gamma/quantlib/live_ois_cross_gamma.py --scenario stress
# Both:
python examples/cross_gamma/quantlib/live_ois_cross_gamma.py --scenario all
```

QuantLib's log-linear DiscountCurve makes the shocked discount factors
consistent with the chosen piecewise-constant forward waves. Coupon
forecasting and discounting share the same relinkable curve; there is
no curve rebootstrap between bump scenarios.

The runner prints the QuantLib version, wave dates, real coupon price,
analytic expectations, residuals and mixed finite-difference convergence.
It exits nonzero on failure.

**Execution boundary:** This project's Python file passes its offline
algebra check and syntax check. The authoring environment did not have
QuantLib installed, so **a live QuantLib second-order pass has not been
claimed or recorded**.

## 3. Mathematics and limitations

For a fully forecast single-curve compounded OIS coupon with accrual
dates a and b, payment p >= b and signed notional scaling N:

\[
V=N D(p)\left(\frac{D(a)}{D(b)}-1\right).
\]

With A=ND(p)D(a)/D(b), r_j=h_j(b)-h_j(a),
s_j=h_j(p)-h_j(b), the correct cross-gamma is the fixed-weight
Hessian plus a compact rank-two update:

\[
H_{\mathrm{true}}=H_{\mathrm{frozen}}-A(rs^T+sr^T).
\]

The lag correction is zero when p=b.

A full stochastic HJM payment-delay convexity correction, partially known
overnight fixings, lookbacks, lockouts, observation shifts, caps/floors,
cross-curve IBOR discount/projection products and quote-level rebootstrap
are **outside** these tests. A long lag alone does not validate or model
stochastic convexity. No speedup or general QuantLib equivalence is
inferred from running the optional Python test.
