# Stage 2 — optional grouped cross-gamma

## Implementation

The new [eight-lane gamma header](../ladder/scan_gamma_grouped.hpp) uses
the current GroupLayout, SIMD primitives and refreshed DateTable **without
changing either first-order scan**.

It adds:
- GroupGammaScratch, retained across groups and curve updates (one instance
  per concurrent worker, not shared across threads).
- scan_grouped_gamma_diagonal: one O(N+K) grouped second-moment pass
  (after grouping), writing Kd*8 doubles per group with normal or streaming
  stores. Fully forecast same-curve OIS payment-lag curvature is included.
- scan_grouped_gamma_portfolio_diagonal: just Kd aggregated values, avoiding
  the per-instrument gamma-output buffer; coupons are still traversed.
- GroupedDiscountCrossGammaView: zero-copy off-diagonal cross-gamma queries
  from existing first-order AoSoA plus corrected gamma diagonal, including
  exact OIS payment-lag corrections; no dense matrix materialisation.
- Matrix-free HVP with reusable scratch and in-place support; avoids
  adding the OIS **diagonal** correction twice.
- GammaDiagonalScratch: reusable scalar reference scratch; existing
  three-argument scan remains compatible.

For a fully forecast same-curve OIS coupon, payment p >= accrual end b:
the diagonal extra term relative to frozen cashflow weights is
-2*A*r[k]*s[k], A = N*D(p)*D(a)/D(b). With r representing [a,b]
and s representing [b,p], both overlap only in the one wave containing b.
Thus the diagonal OIS correction takes at most ONE SIMD update per coupon.

This implements the **discount curve Hessian**. For IBOR coupons the
projection curve is held fixed. Pure projection and mixed discount/projection
Hessian blocks, historical fixings, stochastic convexity, quote rebootstrap
and arbitrary overnight observation conventions are NOT included.

## Like-for-like measured performance

Same seeded (42) bonds, IBOR and OIS book constructor and same dual-curve
fixtures as the existing benchmark. N=500,000; 64,750 groups of eight;
30 discount waves and 36 projection waves. Single-threaded Release C++20,
Linux virtualised AMD EPYC 9V74, five visible virtual CPUs, GCC 14.2.
Median of five timed kernel repetitions, after warm-up.

| Operation | AVX-512 | AVX2 |
|---|---:|---:|
| Existing first-order discount + projection (66) | 46.207 ms | 46.545 ms |
| **Extra** grouped discount-gamma diagonal (30) | 44.099 ms | 46.710 ms |
| Portfolio-only gamma diagonal (30 values) | 44.835 ms | 43.775 ms |
| Sequential first-order + gamma | 90.145 ms | 91.779 ms |
| Extra gamma / first-order time | 0.954x | 1.004x |
| Combined / first-order time | 1.951x | 1.972x |

First-order output: 273.504 MB padded. Per-instrument extra gamma output:
124.320 MB padded. Portfolio-only output: 30 doubles (240 bytes).
Refreshing the existing date table was about 0.11 ms separately.

On the 500,000-instrument mixed book, 59,820 sampled independent
original-coupon second-derivative checks had maximum relative error
1.18e-12. Portfolio gamma aggregation agreed with the materialised gamma
sum to 8.79e-14 relative. Stage-2 smaller random tests also check
matrix-free HVP, OIS off-diagonals, capped/open tails and AVX store parity.

These results are **not** the paper's 500,000-instrument 63.3 ms run;
different host and grouping counts. Do not conflate the run sets or call
the extra gamma pass free. Nor is this a QuantLib/AAD second-order
performance baseline.

## Reproduce

~~~sh
cmake -S . -B build-gamma -DCMAKE_BUILD_TYPE=Release \
  -DLADDER_LANES=AVX2 \
  -DLADDER_BUILD_GAMMA_BENCHMARK=ON \
  -DLADDER_BUILD_QUANTLIB=OFF
cmake --build build-gamma --target test_gamma_grouped bench_gamma_grouped
ctest --test-dir build-gamma --output-on-failure -R '^gamma_grouped$'
./build-gamma/examples/benchmark/bench_gamma_grouped 500000 5 \
  ./examples/benchmark/quantlib_example_curves.txt
~~~

For AVX-512, select LADDER_LANES=AVX512 on a compatible host.
For a portable build, select LADDER_LANES=PORTABLE.

## Independent QuantLib 1.43 second-order gate

[Live QuantLib CI run 38100830063](https://github.com/andyD123/data-oriented-curve-risk/actions/runs/38100830063)
passed three fully forecast same-curve OIS cases, including an accrual-end
date lying inside a wave. The interior wave-3 gamma consists of
**-461.423463 frozen risk minus 833.219334 lag correction**,
giving **-1294.642797**. Live QuantLib finite differences and the
complete 5x5 Hessian agreed within the test tolerances. This verifies the
financially nontrivial diagonal lag correction absent from the earlier
boundary-aligned tests. The test is a curve-wave derivative, not
stochastic payment-delay convexity or quote-rebootstrap gamma.

## Safe usage

~~~cpp
GroupGammaScratch scratch;
double* delta = /* existing aligned grouped first-order output */;
double* gamma = /* groups * Kd * 8 doubles, 64-byte aligned */;
scan_grouped<Store::streaming>(layout, discount, projection, delta);
scan_grouped_gamma_diagonal<Store::streaming>(
    layout, discount, gamma, scratch);
GroupedDiscountCrossGammaView view(
    layout, discount, projection.K(), delta, gamma);
const double h_3_7 = view.at(group_index, lane, 2, 6);
view.multiply(group_index, lane, shocks, hvp_output, scratch);
~~~

The view borrows the layout and two output buffers. They must remain alive
and correspond to the same curve snapshot. Do not refresh/overwrite while
reading, or share the mutable scratch between concurrent callers.

No extra gamma work is performed by the original first-order scan.
This stage is an opt-in CPU implementation; full multi-curve second-order
risk and threaded production scheduling remain future work.
