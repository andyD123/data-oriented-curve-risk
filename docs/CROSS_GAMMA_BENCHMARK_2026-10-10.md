# Reduced-form cross-gamma benchmark — 10 October 2026

Research evidence only: this is NOT the paper's 500,000-instrument
benchmark, and it is NOT a QuantLib benchmark.

Build and run:

    g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
        examples/benchmark/bench_cross_gamma_research.cpp -o /tmp/bench_gamma
    /tmp/bench_gamma

Host: AMD EPYC 9V74; GCC 14.2. One CPU thread.
Fixture: 100,000 fixed discounted cashflows with signed random weights,
seed 20261010, random times in [-1,68], 66 Hagan wave buckets from 0
to 66, last bucket open. Cashflow sorting excluded from all timings.

The benchmark calculates **one aggregated 66x66 Hessian**, not 100,000
per-instrument dense Hessians. All three methods produce the same output.

## Three baselines, explicitly defined

1. **Reduced form**: first-order ladder, separate diagonal second-moment
   scan, and full 66x66 Hessian materialisation from the lazy fixed-flow
   cross-gamma identity. Complexity O(N+K+K^2).
2. **Direct analytic Hessian**: accumulate each cashflow's signed outer
   product of the Hagan-wave overlap vector, using symmetry and skipping
   zero rows. Roughly O(NK^2) work.
3. **Central bump/reprice**: cache all 6.6 million overlaps to favour the
   comparator. Revalue the complete cashflow portfolio 8,713 times:
   1 baseline + 2*66 diagonal bumps + 4*2145 pairwise cross bumps.
   This baseline's price loop evaluates cashflow amounts and exponentials
   only; it omits real-world QuantLib/curve-rebootstrap overhead.

## Reproducible run of the committed benchmark source

| Calculation | Measured time |
|---|---:|
| Reduced-form complete Hessian (median) | 0.961516 ms |
| Direct analytic complete Hessian (median) | 29.423832 ms |
| Precompute overlaps for bump/reprice | 16.451669 ms |
| Full central finite-difference repricing | 4048.086764 ms |

Speedup: **30.60x** against direct analytic Hessian and **4,227.22x**
against central bump/reprice including its overlap precomputation.

Maximum relative differences:
- Reduced-form versus direct analytic Hessian: 1.57398883e-14.
- Finite difference versus reduced-form Hessian: 1.54918211e-06
  (finite-difference truncation and floating-point cancellation).

A separate independent run of equivalent kernels found 0.906966 ms
versus 29.230882 ms for the structured and direct analytic full Hessians
(32.23x), and ~4115 ms for the full bump/reprice (approximately 4500x).
The optional diagonal pass alone was 0.459317 ms versus 0.442762 ms
for the existing first-order scalar scan.

## Interpretation

The speedup is algorithmic, not a small SIMD micro-optimisation. For
fixed discounted cashflows, all off-diagonal Hagan-wave cross-gamma
entries are determined by the first-order ladder. Diagonal gamma
requires another linear-time second-moment pass. Hessian-vector products
can avoid constructing the dense Hessian.

This is a single aggregated portfolio result. If an API instead requests
66x66 gamma matrices for 500,000 instruments, it must output
500000*66*66*8 = 17.424 GB of doubles, which will be traffic-bound even
with a very fast formula. General nonlinear IBOR/OIS products and
stochastic convexity are separate questions. For the supported
fully-forecast same-curve OIS discount-ratio model, the lag correction
is exact and rank two per coupon.

**Publication caution:** do not compare these AMD EPYC microbenchmark
measurements with the recorded Cascade Lake 500,000-instrument headline.
The direct analytic baseline is intentionally simple; a well-optimised
AAD implementation can be closer than conventional bump-and-reprice.
Replicate under the paper's reference hardware and clarify output
contracts before promoting the figures as a publication benchmark.
