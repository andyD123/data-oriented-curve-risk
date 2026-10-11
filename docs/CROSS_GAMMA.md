# Cross-gamma (optional second-order extension)

A separate, opt-in layer: it never touches the first-order SIMD kernels or their benchmarks.

## What it computes

For fixed discounted cashflows `x_i` at times `t_i`, the box-wave first derivatives are the overlaps
`g_k = -sum_i x_i o_k(t_i)`. The Hessian is

    H_jk = sum_i x_i o_j(t_i) o_k(t_i)    (j != k, same-curve fixed flows)

Every off-diagonal entry is fixed by the first-order ladder together with the interval geometry, so the
full K x K matrix is materialised from the ladder in O(K^2) after the usual O(N+K) scan. The diagonal
needs one extra linear second-moment pass. A matrix-free Hessian-vector product avoids building the
matrix at all. For overnight-index coupons paid with a lag, the discount-ratio product rule adds an exact
rank-two correction per coupon (`DiscountCrossGammaView`, `ladder/cross_gamma.hpp`).

| Header | Role |
|---|---|
| [`ladder/cross_gamma.hpp`](../ladder/cross_gamma.hpp) | Reduced-form Hessian, diagonal pass, OIS lag correction, matrix-free HVP |
| [`ladder/scan_gamma_grouped.hpp`](../ladder/scan_gamma_grouped.hpp) | Eight-lane grouped diagonal gamma, portfolio reduction, zero-copy grouped view |

## Verification

- `ctest -R cross_gamma` and `ctest -R gamma_grouped`: direct O(NK^2) analytic Hessian oracle, scalar vs
  grouped agreement, lag correction vs the exact product-rule second derivative, aliasing and stale-grid
  rejection, compile-time lifetime guards.
- `examples/cross_gamma/cross_gamma_example`: a 16-bucket worked example (two OIS coupons, open final
  wave), including the simultaneous two-wave scenario; worst relative Hessian error 4.4e-16, matrix-free
  HVP 3.1e-15.

## Benchmarks (opt-in: `-DLADDER_BUILD_GAMMA_BENCHMARK=ON`)

`bench_cross_gamma` (100,000 fixed cashflows, 66 buckets, one aggregated 66 x 66 Hessian, all three
methods produce the same output; cashflow sorting excluded from every timing):

1. reduced form (ladder + diagonal pass + lazy Hessian);
2. direct analytic Hessian: signed outer product of each cashflow's overlap vector, O(NK^2);
3. central bump-and-reprice: 8,713 revaluations (1 + 2*66 diagonal + 4*2,145 pairwise), with all overlaps
   cached to favour the comparator; the price loop is amounts and exponentials only.

4. second-order AAD baseline: a generic scalar tape (scale, add, exp nodes) recorded once, then one
   forward-over-reverse pass per direction, 66 Hessian-vector products. Only non-zero overlaps are recorded,
   the tape is reused and nothing is allocated in the timed loop. A plain tape, not a tuned AAD library.

Measured here (Intel i7-12700K, GCC 13.3, native-architecture build, one thread pinned to one CPU, three runs;
raw output in `benchmarks/results/i7-12700K_pinned/bench_cross_gamma_3runs.txt`):

| Method | Time |
|---|---:|
| Reduced form | 0.48-0.52 ms |
| Direct analytic Hessian | 17-21 ms (36-43x slower) |
| Central bump-and-reprice | 2.5-2.6 s (5,040-5,380x slower, including 23-29 ms to precompute overlaps) |
| Taped second-order AAD (7.0M tape nodes) | 4.5-5.8 s (9,190-11,160x slower), agrees to 4.5e-15 |

The earlier figures (0.45 ms, 38 ms, 2.3 s; 81-89x for the direct analytic Hessian) came from an earlier run
and are superseded; the direct analytic loop is faster in this native-architecture build and was not
compared like for like with the earlier build.

Relative error: reduced form vs direct analytic 1.7e-14; finite difference vs reduced form 1.5e-6
(truncation and cancellation, as expected for second differences).

`bench_gamma_grouped` (500,000 mixed instruments, grouped eight-lane diagonal gamma and a K-value
portfolio gamma reduction, on the Bank of England SONIA curve; see BOE_CURVE.md): the extra diagonal-gamma kernel
costs about 0.77-0.83x the first-order kernel (`first_order_kernel_ms` about 61, `gamma_diagonal_kernel_ms` about 48,
portable backend, on this machine), with 59,820 oracle checks at 2.6e-12. It has no bump baseline, so it is a cost measurement.
IBOR amounts in that diagonal are frozen off the projection curve (`scan_gamma_grouped.hpp`). On this single
SONIA curve those forecasts move with the discount curve, and the diagonal does not include that movement.
The 500,000-instrument timing still includes the term-rate third of the book under that freeze.

## Caveats

- The speedup is algorithmic, not SIMD. It is a single aggregated portfolio Hessian. A per-instrument
  66 x 66 output for 500,000 instruments is 17.4 GB of doubles and would be traffic-bound whatever the
  formula.
- The direct analytic and bump baselines are deliberately simple. The taped AAD row is a plain tape and is
  slower than the bump baseline; a tuned implementation would be faster. No claim is made about a tuned
  AAD library, and the O(NK^2) direct analytic Hessian is the natural point of comparison for one.
- Scope: fixed flows on one curve plus the lagged-OIS correction. Term-rate forecast amounts stay frozen
  off the projection curve. General nonlinear products and stochastic convexity are not covered.
  Wide schedule groups are rejected by the grouped diagonal.
- These timings are not comparable with the 500,000-instrument first-order headline (different machine,
  workload and output).
