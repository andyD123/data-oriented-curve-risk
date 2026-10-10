# Cross-gamma integration gates (11 October 2026)

This checklist separates **safe additive research integration** from **production risk-path integration**. The existing first-order scalar and SIMD algorithms are not changed by PR #3.

## Current checks

| Gate | Evidence | Status |
|---|---|---|
| Branch mergeability | Draft PR #3 from `main`, no production kernel edits | Passed (as checked 11 Oct 2026) |
| C++20 portable build | GitHub Actions run 38096221423 | Passed |
| Fixed-flow diagonal and cross-gamma | `test_cross_gamma`, 100,000 seeded cashflow oracle, boundary cases | Passed |
| OIS lag rank-two correction | Synthetic analytic differentiation, in-place HVP | Passed |
| Historical QuantLib five-year lagged OIS | Recorded *first-order* QuantLib comparison, reconstructed-product gamma test | Passed within documented scope |
| Reader example | Sixteen irregular waves, open-final, zero/positive-lag OIS, monetary cross-PV | Passed |
| Offline Python OIS algebra | `--scenario offline` | Passed |
| Live QuantLib second derivatives | `--scenario standard` and `stress` | **Not run** |
| MSVC and macOS/Clang PR CI | Platform matrix | **Not run** |
| Production 500k mixed-book performance | Separate diagonal pass on current grouped layout, with no dense Hessian output | **Not measured** |
| PR #2 compatibility | Original QuantLib generator is deleted by standalone-cleanup PR #2 | Partial: source provenance link pinned |

## Safe merge boundary

1. Treat the new header as a **research/optional** C++20 API. Do not add it to a stable public umbrella header yet.
2. Preserve the user's architecture: first-order scans remain unchanged; gamma is a separate optional pass.
3. Do not promise fast per-instrument gamma until grouped/AoSoA integration and memory-traffic measurements are complete. The current scalar pass allocates two K-length vectors per call; `DiscountCrossGammaView` and coupon-product code also allocate scratch for HVPs.
4. The view borrows `Stencils`, first-order gradient and gamma diagonal through references/spans. Those buffers must outlive the view and belong to the **same curve/cashflow snapshot**.
5. The fixed-flow cross-gamma identity applies only to Hagan-wave derivatives of fixed discounted cashflows; do not apply it uncorrected to nonlinear coupon payoffs. For supported fully forecast same-curve OIS ratio coupons, sum one rank-two correction **per coupon**.
6. Never interpret the archived C++ test as live second-order QuantLib reconciliation. It checks historical QuantLib *first-order* data, analytic derivatives and reconstructed-coupon finite differences.
7. Never mix the 100k-cashflow/66-bucket AMD EPYC preliminary timings with the paper's separately recorded 500k-instrument first-order results.

## Cross-PR sequencing

- PR #3 can be merged into current `main` without replacing the first-order scan.
- PR #2 removes the original `examples/hagan_waves/ql_waves.cpp` generator while retaining exported fixtures. The new QuantLib README pins an immutable historical source URL, so readers can still see the generator after PR #2.
- If PR #2 merges before #3, rebase/reconcile #3 and rerun CTest. The two branches both edit `CMakeLists.txt` and `examples/READING_GUIDE.md`, and PR #2 removes the `LADDER_BUILD_QUANTLIB` CMake option. The optional Python QuantLib script is self-contained and requires no CMake generator.
- Keep the full first-order CI and regression benchmarks green before describing the work as production-safe.

## Acceptance commands

From the repository root:

```sh
cmake -S . -B build-gamma -DCMAKE_BUILD_TYPE=Release \
    -DLADDER_LANES=PORTABLE -DLADDER_BUILD_EXAMPLES=ON
cmake --build build-gamma --target test_cross_gamma \
    test_cross_gamma_quantlib_archive cross_gamma_example --parallel 2
ctest --test-dir build-gamma --output-on-failure -R '^cross_gamma'
```

On a checkout that still has the historical QuantLib generator, add `-DLADDER_BUILD_QUANTLIB=OFF` to avoid building its optional targets.

**Recommendation:** merging PR #3 as an optional experimental research facility is reasonable once the normal repository build is rechecked; production status needs the missing cross-platform tests, live QuantLib second-order reference (if claimed), and a measured grouped implementation without per-instrument heap churn.
