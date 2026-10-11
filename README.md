# data-oriented-curve-risk

C++20, header-only curve-risk algorithms accompanying the draft **Risk at the Write Speed: Ultra Fast Data-Oriented DV01**. The manuscript is a technical draft; this repository does not assert publication.

The library organises dated cashflows into shared schedules and applies Hagan instantaneous-forward **box-wave** sensitivities through scalar reverse scans and eight-lane grouped kernels. Wave definitions are due to Hagan; the data-oriented schedule reuse, contiguous curve tables and scan implementations are the subject of this project.

## Quick start

No QuantLib installation, network fetch or third-party runtime is needed for the standard build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/examples/benchmark/bench_library 20000 0 3
```

Requires a C++20 compiler and CMake 3.16+. The default build chooses a supported SIMD backend without silently enabling host-specific ISA flags. See [build and test instructions](docs/BUILD.md) for AVX2/AVX-512/portable configurations, presets, CLion, optional dependencies and test targets.

## Start with a curve

[`ladder::Curve`](ladder/curve.hpp) is a small, validated log-linear
discount curve with factories for zero rates, discount factors and a flat
rate. It has a consistent valuation origin, flat-forward extrapolation
and exact bounded/open Hagan-wave bumps. It is **not** a calendar,
market bootstrap or pricing-engine abstraction.

The [minimal first-order example](examples/curve_intro/README.md)
constructs a curve, discounts three cashflows, computes all wave
sensitivities in one reverse scan and checks them by repricing.
New readers should start there before the more involved grouped
and second-order demonstrations.

## Algorithms and contracts

The header-only API is in namespace `ladder` (CMake interface target `ladder::ladder`).

- **First-order discount risk:** for signed discounted unit cashflows `(t_i, x_i)`, `g_k = -sum_i x_i h_k(t_i)`, where `h_k` is the box-wave overlap. Sorted records use the O(N+K) reverse-scan implementation.
- **Projection risk:** `sum_i w_i [h_k(b_i)-h_k(a_i)]`. Its cost also depends on how many wave intervals the projection fixing periods cross.
- **Grouped risk:** `scan_grouped` uses shared dates and eight-lane vectors for trade-level discount and projection ladders. `scan_grouped_reduce` computes only portfolio totals in `Kd+Kp` doubles without first materialising every trade's output.
- **Second-order risk:** a separate optional diagonal-gamma pass and lazy cross-gamma/Hessian-vector operations cover fixed discounted cashflows and the specified fully forecast same-curve OIS payment-lag correction. These do **not** constitute arbitrary nonlinear coupon gamma, full multi-curve Hessians or quote-level rebootstrap gamma.

For bounded wave `k`, `h_k(t) = clamp(t - B[k-1], 0, B[k]-B[k-1])`. Setting `open_last=true` makes the final wave extend past its last boundary. Analytic derivatives are per unit additive instantaneous-forward-rate shift: multiply by `1e-4` for *linearised* one-basis-point risk, not exact finite-shock P&L.

`scan_discount` requires finite time-sorted cashflow records; projection intervals must be valid; stencil boundaries must be finite and strictly increasing. Grouped execution requires a refreshed date table and a consistent discount/projection curve snapshot. Its full per-trade output uses `groups * (Kd+Kp) * 8` doubles and 64-byte alignment. The portfolio-only reduction emits `Kd+Kp` scalar doubles. Curve replication, schedule construction, sorting and refresh are distinct from kernel timings.

The simple fixed, IBOR and overnight-ratio replication expressions assume the caller supplies correct cashflow conventions. Fixing histories, lockouts, convexity adjustments and arbitrary financial-engine conventions are **not** silently handled.

## Start with the demonstrations

| Topic | Where to begin |
|---|---|
| Grouped risk, shared schedule layout and buffer ownership | [Grouped data-flow guide](docs/GROUPED_DATA_FLOW.md) and [library benchmark](examples/benchmark/README.md) |
| Portfolio-only first-order risk, avoiding the full output matrix | [`scan_grouped_reduce.hpp`](ladder/scan_grouped_reduce.hpp) and [measured comparison](examples/benchmark/README.md#portfolio-only-grouped-reduction) |
| Fixed-flow and OIS cross-gamma, matrix-free Hessian-vector products | [16-bucket worked example](examples/cross_gamma/README.md) and [grouped gamma guide](docs/GROUPED_CROSS_GAMMA_STAGE2.md) |
| Independent single-curve overnight-rate bootstrap, quote risk and hedges | [RFR reconciliation](examples/rfr_reconcile/README.md), without QuantLib |
| Optional live independent financial-library checks | [QuantLib cross-gamma reconciliation](examples/cross_gamma/quantlib/README.md) |
| Scenario curve cache, direct overlaps, adjoint and aggregation | [Demonstrator reading guide](examples/READING_GUIDE.md) |
| Logical schedule groups wider than one SIMD vector | [Wide-group experiment](docs/WIDE_SCHEDULE_GROUPS.md) |

The RFR reference example supplements, rather than replaces, the original Hagan and QuantLib examples. Optional QuantLib C++ generators require a separately installed library; the normal C++ build does not.

## Performance and reproducibility

[**BENCHMARKS.md**](BENCHMARKS.md) collects the distinct benchmark definitions, measured results, baseline comparisons, output sizes and reproduction commands. Keep the paper's bonds/vanilla-swaps workload separate from the seasoned mixed bond/IBOR/OIS library workload, and distinguish full per-instrument ladders from portfolio-only outputs.

For article readers, [**run the baseline-versus-optimised comparisons directly**](examples/benchmark_paper/README.md#reader-run-comparisons-from-the-article) using the examples' one-command driver; its numerical gates and saved run manifest are part of the evidence. The archived 50.9 ms result has its own historical risk-coordinate and machine provenance.

In particular, the recorded 500,000-instrument paper timing uses an earlier interval-integrated-forward (`theta`) sensitivity coordinate, while the current source reports sensitivities per unit instantaneous-forward-rate shift. The recorded result is evidence for its stated executable and workload, **not** a newly measured timing for the current implementation.

More detail: [validation and limitations](VALIDATION.md), [reduced-form cross-gamma measurement](docs/CROSS_GAMMA_BENCHMARK.md), [live OIS gamma checks](docs/QUANTLIB_LIVE_OIS_GAMMA.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
