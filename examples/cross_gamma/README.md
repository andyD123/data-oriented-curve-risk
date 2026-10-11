# Hagan-wave cross-gamma: a worked 16-bucket example

This example makes the second-order extension of the data-oriented curve-risk
library readable without understanding the SIMD kernels or benchmark harness.

**Main result:** For fixed discounted cashflows, every off-diagonal Hagan-wave
cross-gamma is determined by the *first-order risk ladder*. A separate optional
scan supplies just K diagonal second moments. Nonlinear payment-lag OIS coupons
need an additional compact rank-two correction.

Source: [cross_gamma_example.cpp](cross_gamma_example.cpp).
The companion [QuantLib reconciliation project](quantlib/README.md) explains the
independent recorded and optional live comparisons.

## Build and run

From the repository root, using any C++20 compiler:

```sh
g++ -std=c++20 -O2 -Wall -Wextra -I. \
  examples/cross_gamma/cross_gamma_example.cpp -o /tmp/cross_gamma_example
/tmp/cross_gamma_example
```

The example also has its own CMake target and small CTest smoke test:

```sh
cmake -S . -B build-gamma -DLADDER_LANES=PORTABLE \
  -DLADDER_BUILD_EXAMPLES=ON -DLADDER_BUILD_QUANTLIB=OFF
cmake --build build-gamma --target cross_gamma_example \
  test_cross_gamma test_cross_gamma_quantlib_archive
ctest --test-dir build-gamma --output-on-failure -R '^cross_gamma'
./build-gamma/examples/cross_gamma/cross_gamma_example
```

No QuantLib package is required for this C++ example.

## Portfolio and the wave grid

Sixteen irregular waves have boundaries (in years):

```text
0, 0.25, 0.5, 1, 2, 3, 4, 5, 7, 10, 12, 15, 20, 25, 30, 35, 40
```

Wave 11 covers [12,15); wave 12 covers [15,20). The final wave begins at
35 years and is **open-ended beyond 40 years**, exercising Hagan's last-wave
convention. A 43.5-year principal makes the tail test observable.

The example holds ten fixed discounted cashflows and two fully forecast
single-curve OIS coupons on a deterministic continuously compounded 3.25%
base curve:

| Coupon | Accrual period | Payment | Purpose |
|---|---|---|---|
| OIS A, notional 1,000,000 | 12.25–14.90 years | 15.25 years | Lag crosses the 15-year wave boundary |
| OIS B, notional 700,000 | 6.40–7.60 years | 7.60 years | Zero-lag control |

The curve only constructs the base weights. Hagan-wave shifts are independent
perturbations of the instantaneous forward rate, not changes in deposit/swap
quotes or a curve rebootstrap.

## Step 1: the existing first-order ladder

Write h_k(t) for the integrated overlap of wave k up to time t.
For a bounded wave [B_{k-1},B_k),

\[
h_k(t)=\max(0,\min(t,B_k)-B_{k-1}).
\]

For fixed signed discounted cashflows x_i, the existing scan gives

\[
g_k=\frac{\partial PV}{\partial\delta_k}
   =-\sum_i x_i h_k(t_i).
\]

For first-order risk only, OIS coupon weights are replicated into dated signed
cashflows, as in the pre-existing library.

## Step 2: gamma diagonal — a separate optional linear scan

The diagonal needs the *squared* overlap,

\[
H_{kk}=\sum_i x_i h_k(t_i)^2.
\]

For each bounded wave the fast calculation combines a local second moment
with a cashflow suffix sum:

\[
H_{kk}=
\sum_{B_{k-1}<t_i<B_k}x_i(t_i-B_{k-1})^2+
(B_k-B_{k-1})^2\sum_{t_i\ge B_k}x_i.
\]

The open-ended last wave instead uses its uncapped squared overlap.

The call to **scan_discount_gamma_diagonal** takes sorted dated weights and
writes *only 16 extra diagonal values*; it is O(N+K) after sorting.
The first-order scan remains unchanged.

For different bounded waves j<k,

\[
\boxed{H_{jk}=-(B_j-B_{j-1})g_k}.
\]

The **DiscountCrossGammaView** exposes any matrix element in O(1). An
Hessian-vector product is O(K) using prefix/suffix accumulations. A dense
K-by-K Hessian only needs to be materialised when explicitly requested.

## Step 3: account for nonlinear OIS payment lag

For a fully forecast, same-curve compounded OIS coupon (a<=b<=p),

\[
PV=N D(p)\left(D(a)/D(b)-1\right)=A-C,
\]

with A=ND(p)D(a)/D(b) and C=ND(p), including any pay/receive sign.

Define accrual exposures r_j=h_j(b)-h_j(a) and lag exposures
s_j=h_j(p)-h_j(b). Then

\[
\boxed{H_{\rm true}=H_{\rm frozen}-A(rs^T+sr^T)}.
\]

These are **two outer-product terms per coupon**, not a dense second-order
pricing tape. The correction vanishes when p=b. The example uses
**ois_lag_gamma_correction** for individual entries and
**add_ois_lag_gamma_product** for the matrix-free product.

## Step 4: observe the cross-risk in money

The working C++ example produces these values (currency per squared unit
instantaneous-forward shift):

| Wave pair | Frozen-cross-gamma | OIS lag correction | Correct cross-gamma |
|---|---:|---:|---:|
| 2, 8 | 160,890.733 | 0 | 160,890.733 |
| 7, 11 | -495,401.370 | 0 | -495,401.370 |
| **11, 12** | **4,981,867.906** | **-439,888.180** | **4,541,979.726** |
| 11, 16 | 7,276,667.132 | 0 | 7,276,667.132 |

Simultaneously shift wave 11 and wave 12 **up 50 bp** each, subtracting the
separate first-order scenario effects. The *pure cross-PV effect* is

\[
\Delta_{\rm cross}PV=PV(.005,.005)-PV(.005,0)-PV(0,.005)+PV(0,0).
\]

| Method | Monetary cross-PV |
|---|---:|
| Full nonlinear coupon repricing | **+111.2273** |
| Corrected local gamma times (0.005)^2 | **+113.5495** |
| Frozen-weight gamma without lag correction | **+124.5467** |

The structured gamma is the **exact second derivative at the base curve**.
It is not an exact finite-50-bp repricer: the difference of 2.32 versus
repricing is higher-order shock curvature. Omitting the OIS correction adds
a distinct second-order error.

The implementation checks every one of the 16x16 Hessian elements against
*independent direct product-rule differentiation* of the original coupon
expressions. In the locally executed portable example:

- Maximum relative full-Hessian error: **4.4e-16**.
- Maximum relative matrix-free Hessian-vector error: **3.1e-15**.
- The example exits with a nonzero code when checks fail.

## What QuantLib establishes

See [quantlib/README.md](quantlib/README.md) for two distinct methods.

1. The repo's existing real QuantLib v1.33 five-year, two-business-day-lag OIS
   export is checked by
   [test_cross_gamma_quantlib_archive.cpp](../../tests/test_cross_gamma_quantlib_archive.cpp).
   It validates the first-order scan against *recorded QuantLib finite
   differences*. The second derivative is compared to direct analytic
   differentiation and repricing of the original exported coupon formula.
2. The optional
   [live_ois_cross_gamma.py](quantlib/live_ois_cross_gamma.py)
   creates an actual QuantLib overnight coupon, bumps log-discount curves,
   and prepares a **live** second-order finite-difference reconciliation.
   The authoring environment lacked QuantLib, but an independent
   cloud Linux run on **11 October 2026** used **QuantLib Python 1.43**.
   Both standard and stressed payment-lag scenarios passed live four-price
   mixed-gamma finite differences. The externally supplied numerical
   evidence is transcribed in
   [the validation record](../../docs/QUANTLIB_LIVE_OIS_GAMMA_2026-10-11.md).
   This validates the specific fully forecast, same-curve model and selected
   cross-wave pair, not every OIS convention or stochastic convexity.

This does not claim that stochastic payment-delay convexity corrections,
historical fixings, lockouts, observation shifts, cross-curve IBOR products,
optionality or quote-rebootstrap Hessians are all covered by the simple OIS
formula.

**Output contract:** The example forms one portfolio-level Hessian and verifies
all 256 entries. That is not the same memory workload as writing a separate
16x16 matrix for each of 500,000 trades. The reduced form allows lazy
queries or matrix-free products without dense output when the caller wants
those operations.

**No new performance claim** is inferred from the explanatory example.
For the separately measured fixed-cashflow research benchmark, see
[the performance evidence note](../../docs/CROSS_GAMMA_BENCHMARK_2026-10-10.md).
