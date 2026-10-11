<<<<<<< HEAD
# Validation

What is tested, how, and what is not covered. All results below were produced from the source in this repository
(Linux x86-64, Intel i7-12700K, GCC 13.3, CMake; no AVX-512 on this machine).

## Executed checks

| Configuration | CTest result |
|---|---|
| GCC Release, PORTABLE (auto-vectorised) | 12/12 passed |
| GCC Release, AVX2 + FMA | 12/12 passed |
| GCC Debug, PORTABLE, AddressSanitizer + UndefinedBehaviorSanitizer | 12/12 passed |

The AVX-512 backend was not executed here (no AVX-512 hardware); it compiles in the dual-ISA configuration but is
untested on this machine. Windows/MSVC and Apple/ARM were not executed either, so no portability claim is made.

The suite covers:

- **Boundary and oracle checks (15,640).** An independent direct overlap calculation (not `Stencils::overlap`)
  covers every boundary and neighbouring representable values, times before the first and beyond the last boundary,
  one-wave stencils, empty books and instruments, signed and cancelling records, open projection tails,
  unrelated and partially shared schedules, partial groups, output canaries and both store policies.
- **First-order identities.** Parallel-shift and coarsening identities; scalar scan against grouped scan.
- **RFR reconciliation** (`examples/rfr_reconcile`): oracle, explicit adjoint, finite-difference convergence,
  market-quote risk, hedge solve and a cubic-spline negative control, against a custom pure-Python reference.
- **Cross-gamma** (`test_cross_gamma`, `test_gamma_grouped`): direct analytic Hessian oracle, scalar against grouped,
  lag correction, aliasing, stale-grid rejection, compile-time lifetime guards.
- **Input handling.** Malformed, truncated and non-finite inputs, invalid arguments and corrupted comparison data
  are rejected, and replay outputs go to temporary directories so committed fixtures stay immutable.
- **Benchmark gates.** Each benchmark checks grouped against scalar and baseline results and exits non-zero on a
  failed comparison. In the 500,000-instrument run, grouped against scalar agreed to 3.6e-15 relative and the
  bump-and-reprice baseline to 5.9e-8 (finite-difference error).

These tests verify the algebra and the replay of supplied risk weights. They do not certify that every financial
instrument adapter supplies the right weights.

## Behaviour worth knowing

A cashflow before the first boundary contributes nothing to any wave (earlier versions wrongly gave a negative
interior overlap: for boundaries `{0,1,2}` and PV 100 at `t=-0.5` the first wave returned +50 instead of 0).
The final wave can be bounded or open:
=======
# Validation and supported scope

This document describes what the **current source and test suite** check.
It is not a diary of past branches or reviews. See the [build guide](docs/BUILD.md),
[benchmark definitions](BENCHMARKS.md), and
[grouped data-flow guide](docs/GROUPED_DATA_FLOW.md) for usage.

## Executable checks

| Area | Test or executable | Contract exercised |
|---|---|---|
| Curve and first-order introduction | `curve`, `curve_intro_smoke` | Discount/zero curve construction, shifted origin, log-linear interpolation, forward rates and exact bounded/open box bumps |
| Scalar discount and projection risk | `ladder`, `boundaries` | Finite, sorted cashflow records; independent overlap oracle; shifted/boundary/tail cases; projection support; coarsening and parallel-shift identities |
| Grouped eight-lane scan | `ladder`, `boundaries` | Signed fixed, IBOR and forecast OIS flows; scalar vs grouped, normal/streaming store parity within a backend, padded lanes, stale tables |
| Portfolio reduction | `grouped_reduce` | Grouped materialisation and independent scalar oracle vs direct portfolio totals; empty books, large wave grids, scratch reuse |
| Checked grouped API | `grouped_api` | Exact-size spans, 64-byte output alignment, live lane bounds, checked gamma/Hessian input sizes, no output modification on invalid shapes |
| Scalar cross-gamma | `cross_gamma` | Second moments, fixed-flow off-diagonal identity, matrix-free Hessian-vector products, overlapping inputs and rank-two OIS lag correction |
| Grouped discount gamma | `gamma_grouped` | Fully forecast same-curve OIS diagonal correction, all coupon-product Hessian entries, scalar oracle, AVX store parity and HVP |
| Recorded QuantLib comparison | `cross_gamma_quantlib_archive`, `wave_replay_and_input_checks` | Historical first-order wave fixtures; the archived second-order test independently differentiates a reconstructed formula, not live QuantLib |
| Independent RFR example | `rfr_reconcile_replay` | Recorded and freshly generated illustrative OIS curves, bootstrap Jacobian, first-order scan, hedging, convergence and negative spline control |
| Demonstrations | `benchmark_smoke`, `aggregation_demo_smoke` and scenario/quote-risk tests | Numerical comparison gates, input validation, separate output contracts |

The grouped first-order materialisation and portfolio-reduction paths now
share one coupon evaluator and the same discount/projection traversal.
The checked `std::span` overloads call the original allocation-free
kernels after verifying buffer extent; they do not own memory.

## Live QuantLib OIS second-order reconciliation
>>>>>>> origin/main

The optional Python
[QuantLib example](examples/cross_gamma/quantlib/README.md) constructs
actual forecast overnight coupons and applies Hagan forward-wave shocks.
Its focused CI executes ordinary and stressed payment-delay cases and
an interior-wave case with a **nonzero diagonal lag correction**.
All 15 unique entries of the five-wave interior Hessian are checked
against independently repriced QuantLib coupons, with expected
step-halving convergence. The full figures and scope are in
[the live gamma evidence](docs/QUANTLIB_LIVE_OIS_GAMMA.md).

<<<<<<< HEAD
Malformed stencils, non-finite or unsorted scalar records, reversed projection endpoints, invalid day scales and
invalid curve factors are rejected. Grouped execution detects missing refreshes, stale risk-grid bucket caches and
null or misaligned output. The raw buffer API still requires callers to provide enough storage; arbitrary
floating-point overflow is not a guaranteed domain.

## Scope

- Benchmarks use an illustrative SOFR-style RFR curve (`rfr_curves.txt`), not a market snapshot. Fixed-base wave
  bumps must not be confused with quote bumps plus a full re-bootstrap, and curve reference dates must be converted
  explicitly.
- Timings are in [BENCHMARKS.md](BENCHMARKS.md) with raw output in `benchmarks/results/`. They are single-machine,
  unpinned wall-clock measurements, not statistical evidence.
- There is no external reference engine: validation rests on analytic oracles, the custom Python reference and
  finite differences.
- No release tag and no explicit project licence yet.

## Projection-pass reconciliation

`examples/rfr_reconcile/rfr_projection_check.py` values four term-rate swaps (5y semi-annual, 7y starting in 1y, 3y
quarterly with spread, 32y beyond the last pillar) in an independent Python reference, with floating coupons forecast
off a second illustrative log-linear curve. Box waves are applied to the discount and projection curves
independently and central differences through the full valuation are compared with `scan_wave`'s discount and
projection ladders: residual ratio 4.00 per step halving, Richardson agreement 1.8e-13 (discount) and 7.6e-12
(projection), exact zeros beyond maturity. CTest `rfr_projection_replay` replays the recorded fixtures. This checks the
wave ladders only; the cross-curve calibration Jacobian (projection curve bootstrapped from term quotes) is not
reconciled.
=======
The other, archived QuantLib test only establishes its stated
first-order fixture reconciliation. The independent
[RFR example](examples/rfr_reconcile/README.md) has no QuantLib dependency
and uses illustrative par quotes rather than a market snapshot.

## Run the tests

```sh
cmake -S . -B build-test -DCMAKE_BUILD_TYPE=Release \
  -DLADDER_LANES=PORTABLE -DLADDER_BUILD_QUANTLIB=OFF
cmake --build build-test --parallel 2
ctest --test-dir build-test --output-on-failure

# Optional focused selection:
ctest --test-dir build-test --output-on-failure \
  -R '^(curve|grouped_api|grouped_reduce|gamma_grouped|cross_gamma|rfr_reconcile)'
```

The pull-request workflow runs a full PORTABLE and AVX2 matrix and the
optional live QuantLib Python reconciliation. Linux CI does **not**
establish MSVC, Apple/ARM, or cross-ISA bitwise identity.
The optional AVX-512 microbenchmarks require suitable hardware and
are not a replacement for source-level correctness checks.

## Limits that must remain explicit

- Forward-wave derivatives are **fixed-base rate-wave risks**, not
  market-quote risks involving curve rebootstrap.
- Supported fixed, forecast IBOR and compounded-overnight expressions
  assume supplied cashflow conventions. The library does not infer
  historical fixings, observation shifts, lockouts, coupon-pricer
  conventions, calendars, stochastic convexity or option exercise.
- Grouped gamma is the **discount-curve Hessian** (IBOR projection held
  fixed). Pure projection gamma, discount/projection mixed blocks and
  general quote-rebootstrap gamma are not implemented.
- Both `GroupLayout` and the first-order/gamma buffers are owned by the
  caller. `std::span` checks extents but never extends lifetime;
  `GroupedDiscountCrossGammaView` borrows stencils, layout and arrays
  from a single consistent curve snapshot. Mutable scratch is
  **one instance per concurrent worker**.
- Raw-pointer APIs retain their caller-managed size and alignment
  contracts. Floating-point summation and FMA association can vary
  with ISA/compiler and do not promise bitwise equality.
- Benchmark figures have different trade populations, machines,
  output units, baseline comparisons and timed regions.
  [BENCHMARKS.md](BENCHMARKS.md) preserves those distinctions.

The adapted QuantLib demonstration material retains the applicable
[third-party notices](THIRD_PARTY_NOTICES.md).
>>>>>>> origin/main
