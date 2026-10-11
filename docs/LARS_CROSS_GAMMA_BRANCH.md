# Lars cross-gamma research branch

Source: the uploaded Lars draft archive `data-oriented-curve-risk-lars(1).zip`, SHA-256 `8d3e1dea3b4adcb5571a275ce414c6998646108e1362e16a7c8a13b415a46b02`. Its local Git HEAD is `3a7fcd3f4d1fcab43960ff762e0c91fc1186435e` (not present on this GitHub remote).

This **working branch** is based on the corrected GitHub `main` (`eb02cfdd8664a2d0ea92c59df928a045c30a5a5d`) and ports selected cross-gamma work from Lars's draft. It deliberately does **not** replace corrected scalar/grouped scans with older versions or publish unrelated manuscripts and scratch files from the archive.

Scope: separate fixed-cashflow gamma second pass, lazy off-diagonal identity `H[j,k]=-width[min(j,k)]*gradient[max(j,k)]`, matrix-free Hessian-vector product, and a rank-two OIS payment-lag correction. First-order kernels remain untouched.

Caveats: Hessian factorisation by itself applies to fixed discounted cashflows. Fully forecast single-curve OIS discount-ratio coupons require the stated lag correction; other nonlinear coupon conventions, stochastic convexity and full QuantLib second-order repricing require separate validation. The archived implementation had a pre-first-boundary bug and an overbroad nonlinear Hessian claim; neither is imported as-is.

Status: experimental, not publication-benchmarked, no claims of unchanged bitwise rounding or improved mixed-book speed. Run the focused local C++20 regression before integrating with the core CI. For the full independent QuantLib investigation, consult the handoff ZIP prepared on 10 October 2026.


## Correctness follow-up 

The optional gamma calculation remains separate from the existing first-order grouped path.

Corrections to Lars's original draft:
- Use the production stencil validation, including rejecting nonfinite wave widths.
- Exclude cashflows before the first wave boundary from diagonal second moments.
- Preserve bounded versus open-final-wave curvature.
- Avoid treating frozen first-order unit cashflows as a general OIS/IBOR Hessian.
- Apply the exact rank-two correction for fully forecast, same-curve OIS payment lag.
- Make the multiple-coupon Hessian-vector correction safe when input and output overlap.

Two focused native CTest targets have been added:
1. cross_gamma: scalar first-order/oracle comparisons, shifted/boundary cases, 100,000 seeded cashflows against a direct long-double diagonal/off-diagonal oracle, bad inputs, in-place Hessian-vector products and synthetic OIS correction.
2. cross_gamma_quantlib_archive: historical QuantLib v1.33 five-year OIS with actual two-business-day payment lag, 30 Hagan discount-wave buckets, first-order delta reconciliation, exact reconstructed coupon gamma and step-halving finite-difference checks.

Local tested matrix: GCC C++20 Release PORTABLE (2/2), GCC Release AVX2 (2/2), Clang PORTABLE ASan/UBSan (2/2). A path-filtered workflow at .github/workflows/cross-gamma.yml runs these tiny tests on PRs.

To repeat only the gamma checks:

    cmake -S . -B build-gamma -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=PORTABLE -DLADDER_BUILD_EXAMPLES=OFF
    cmake --build build-gamma --target test_cross_gamma test_cross_gamma_quantlib_archive --parallel 2
    ctest --test-dir build-gamma -R '^cross_gamma' --output-on-failure

The archived QuantLib result is FIRST-ORDER evidence. The mixed-gamma regression independently reprices the exact coupon formula constructed from QuantLib-exported values; the subsequent **external** QuantLib Python 1.43 run reported passing second-order reconciliation for two selected mixed-wave coupon cases; see [the dated evidence](QUANTLIB_LIVE_OIS_GAMMA_2026-10-11.md). No stochastic convexity approximation is being certified.

Historical note: this text originally described an experimental branch;
Stage 1 was subsequently merged as PR #3 (commit
8ab6f4beeb60519f33cc507a754188962e627bb3), without touching the
first-order kernel. Stage 2 and later independent QuantLib validation
are documented separately. No new production speedup claim is made here.
