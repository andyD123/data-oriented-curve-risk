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

| Record | Bounded final wave | Open final wave |
|---|---:|---:|
| PV 100 at t=2 | -100 | -100 |
| PV 100 at t=2.5 | -100 | -150 |

Malformed stencils, non-finite or unsorted scalar records, reversed projection endpoints, invalid day scales and
invalid curve factors are rejected. Grouped execution detects missing refreshes, stale risk-grid bucket caches and
null or misaligned output. The raw buffer API still requires callers to provide enough storage; arbitrary
floating-point overflow is not a guaranteed domain.

## Scope

- The published benchmarks and reconciliation use the Bank of England SONIA curve (docs/BOE_CURVE.md); no curve data is committed (the build writes a synthetic flat curve for tests and smoke runs). The reconciliation quotes are implied by a fitted curve, so it is a self-consistency check, not a market test. Fixed-base wave
  bumps must not be confused with quote bumps plus a full re-bootstrap, and curve reference dates must be converted
  explicitly.
- Timings are in [BENCHMARKS.md](BENCHMARKS.md) with raw output in `benchmarks/results/`. They are single-machine
  wall-clock measurements, not statistical evidence. The headline ablation is pinned to logical CPU 2. The
  earlier table in `benchmarks/results/i7-12700K/` is unpinned and its core was not controlled.
- There is no external reference engine: validation rests on analytic oracles, the custom Python reference and
  finite differences.
- No release tag. The code and documents are Apache License 2.0 (see [LICENSE](LICENSE)); Bank of England data
  is not included and stays under the Bank's own terms.

## Projection-pass reconciliation

`examples/rfr_reconcile/rfr_projection_check.py` values four term-rate swaps (5y semi-annual, 7y starting in 1y, 3y
quarterly with spread, 32y beyond the last pillar) in an independent Python reference, with floating coupons forecast
off a second log-linear curve (the discount curve plus a smooth synthetic basis). Box waves are applied to the discount and projection curves
independently and central differences through the full valuation are compared with `scan_wave`'s discount and
projection ladders: residual ratio 4.00 per step halving, Richardson agreement 1.8e-13 (discount) and 7.6e-12
(projection), exact zeros beyond maturity. CTest `rfr_projection_replay` replays the recorded fixtures. This checks the
wave ladders only; the cross-curve calibration Jacobian (projection curve bootstrapped from term quotes) is not
reconciled. The 1.8e-13 (discount) and 7.6e-12 (projection) figures are this placeholder-quote replay. They are
not the paper's Bank of England projection check (1.4e-12 discount, 1.1e-11 projection). The cubic percentages
in the paper require `generate --curve` on the 28 August 2026 SONIA curve; `rfr_reconcile_replay` uses the
invented placeholder quotes in `examples/rfr_reconcile/rfr_reference.py`.
