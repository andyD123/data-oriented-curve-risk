# Lars cross-gamma research branch — 10 October 2026

Source: the uploaded Lars draft archive `data-oriented-curve-risk-lars(1).zip`, SHA-256 `8d3e1dea3b4adcb5571a275ce414c6998646108e1362e16a7c8a13b415a46b02`. Its local Git HEAD is `3a7fcd3f4d1fcab43960ff762e0c91fc1186435e` (not present on this GitHub remote).

This **working branch** is based on the corrected GitHub `main` (`eb02cfdd8664a2d0ea92c59df928a045c30a5a5d`) and ports selected cross-gamma work from Lars's draft. It deliberately does **not** replace corrected scalar/grouped scans with older versions or publish unrelated manuscripts and scratch files from the archive.

Scope: separate fixed-cashflow gamma second pass, lazy off-diagonal identity `H[j,k]=-width[min(j,k)]*gradient[max(j,k)]`, matrix-free Hessian-vector product, and a rank-two OIS payment-lag correction. First-order kernels remain untouched.

Caveats: Hessian factorisation by itself applies to fixed discounted cashflows. Fully forecast single-curve OIS discount-ratio coupons require the stated lag correction; other nonlinear coupon conventions, stochastic convexity and full QuantLib second-order repricing require separate validation. The archived implementation had a pre-first-boundary bug and an overbroad nonlinear Hessian claim; neither is imported as-is.

Status: experimental, not publication-benchmarked, no claims of unchanged bitwise rounding or improved mixed-book speed. Run the focused local C++20 regression before integrating with the core CI. For the full independent QuantLib investigation, consult the handoff ZIP prepared on 10 October 2026.
