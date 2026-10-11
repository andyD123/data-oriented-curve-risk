# data-oriented-curve-risk: Ultra-Fast Data-Oriented Curve Risk

<<<<<<< HEAD
> **764× faster than bump-and-reprice** on a 500,000-instrument book (0.95 ns per sensitivity, single thread, AVX2, Intel i7-12700K; 52× for the scalar scan alone). See [BENCHMARKS.md](BENCHMARKS.md) for baseline, hardware and caveats. Pure standalone C++20 with **zero external dependencies** and no object-oriented curve or calendar layer.

Companion sources for **"Reaching the Memory Wall in Rates Risk: Data-Oriented Design and the Reverse Scan Algorithm"** (Andrew Drakeford & Lars Schouw, October 2026).
Related work: [DR3](https://github.com/andyD123/DR3), a C++ library for writing vectorised code with generic lambdas and filters, where the instruction set is switched by changing the enclosing namespace.
The library computes first-order sensitivity to interval-forward **waves** (Hagan box stencils). The contribution here is data-oriented organisation: shared dates, grouped instruments, deal-axis SIMD, streaming non-temporal stores, and reverse suffix scans.

## The idea

Instruments in a book share payment dates because market conventions make them fungible. Sorting and grouping
them by date turns per-trade risk into a few contiguous scans over shared data. Curve risk then needs no
repeated repricing: one reverse scan over unit cashflows returns the sensitivity to every forward bucket.

## Quick start

A C++20 compiler and CMake 3.16+ are required. There are no other dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/examples/benchmark/bench_library 20000 1 3
```

ISA selection (AVX2, AVX-512, portable), presets, Windows and CLion notes: [docs/BUILD.md](docs/BUILD.md).

## Optional: cross-gamma

An opt-in second-order layer (full Hessian, diagonal gamma, OIS lag correction, matrix-free
Hessian-vector product) built on the same ladder. On a 100,000-cashflow, 66-bucket book it is about 36-43x
faster than a direct analytic Hessian, about 5,000x faster than central bump-and-reprice and about 10,000x faster than a plain
taped second-order AAD prototype (one aggregated
66 x 66 Hessian, this machine; caveats in [docs/CROSS_GAMMA.md](docs/CROSS_GAMMA.md)).

## Scope and limitations

- Zero curves only: the caller supplies boundary times and discount factors (or zero rates). There are no
  calendars, day-count conventions or curve construction in the library.
- Results are analytic first derivatives per unit forward shift, not finite-shock P&L.
- Tested on Linux x86-64 with GCC and Clang. Windows/MSVC and Apple/ARM were not run.
- Benchmark figures are single-threaded; see [BENCHMARKS.md](BENCHMARKS.md).
- The reference reconciliation covers single-curve instruments; the projection pass is checked separately on term-rate swaps (`rfr_projection_check.py`, wave ladders only). The cross-curve calibration Jacobian block is not reconciled.
- Groups own separate small vectors rather than viewing one flat arena; the effect of an arena was not measured.
- Results on third-party data (for example the Bank of England curve) are kept local and are not published here; see [docs/BOE_CURVE.md](docs/BOE_CURVE.md) for how to run it yourself.
=======
C++20, header-only curve-risk algorithms accompanying the draft **Risk at the Write Speed: Ultra Fast Data-Oriented DV01**. The manuscript is a technical draft; this repository does not assert publication.

The library organises dated cashflows into shared schedules and applies Hagan instantaneous-forward **box-wave** sensitivities through scalar reverse scans and eight-lane grouped kernels. Wave definitions are due to Hagan; the data-oriented schedule reuse, contiguous curve tables and scan implementations are the subject of this project.
>>>>>>> origin/main

## Quick start

<<<<<<< HEAD
The [demonstrator reading guide](examples/READING_GUIDE.md) points to the scenario-cache,
adjoint and aggregation experiments. These now use short named operations and a small
`measure_best(repetitions, work)` timing helper; their reports are separate from pricing.
This is a readability pass on those three examples, not a rewrite of the scan library.

For paper claims and their exact commands/records, see [Paper claim -> code and evidence](docs/PAPER_EVIDENCE.md).

## Wide schedule-group experiment

An opt-in example tests whether instruments sharing one schedule should be grouped independently
of the hardware SIMD width. Instead of making one logical group equal to one eight-double vector,
one schedule group may contain many eight-lane blocks driven by the same date/bucket traversal.

On a controlled 65,536-instrument, 60-cashflow, 40-bucket workload, the prototype reduced the
streaming-store AVX-512 kernel from 5.724 ms at logical width 8 to 2.675 ms at width 2,048
(2.14x), with every wider grouping reconciled against the width-8 output. AVX2 runs showed the
same broad effect. This remains an architectural experiment rather than the production
discount+projection path.

Build it with `-DLADDER_BUILD_WIDE_GROUP_EXPERIMENT=ON`. See
[docs/WIDE_SCHEDULE_GROUPS.md](docs/WIDE_SCHEDULE_GROUPS.md) for the implementation,
measurements, interpretation, limitations and reproduction command.

## Library and mathematical contract
=======
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
>>>>>>> origin/main

[**BENCHMARKS.md**](BENCHMARKS.md) collects the distinct benchmark definitions, measured results, baseline comparisons, output sizes and reproduction commands. Keep the paper's bonds/vanilla-swaps workload separate from the seasoned mixed bond/IBOR/OIS library workload, and distinguish full per-instrument ladders from portfolio-only outputs.

For article readers, [**run the baseline-versus-optimised comparisons directly**](examples/benchmark_paper/README.md#reader-run-comparisons-from-the-article) using the examples' one-command driver; its numerical gates and saved run manifest are part of the evidence. The archived 50.9 ms result has its own historical risk-coordinate and machine provenance.

In particular, the recorded 500,000-instrument paper timing uses an earlier interval-integrated-forward (`theta`) sensitivity coordinate, while the current source reports sensitivities per unit instantaneous-forward-rate shift. The recorded result is evidence for its stated executable and workload, **not** a newly measured timing for the current implementation.

<<<<<<< HEAD
`scan_discount` requires time-sorted, finite records and an output buffer of K doubles.
`scan_projection` requires finite terms with `a <= b`. Stencils must have at least two
finite, strictly increasing boundaries. Entry points reject these invalid inputs;
low-level geometry helpers require a valid stencil and valid wave indices.

`build_layout` unions members' payment dates and keys coupons by `(pay,a,b)`; missing
lane entries are zero. Each layout uses **one discount/projection curve pair**, a common
time axis and its supplied day scale. A signature is not a separate curve handle.
Adapters for curves with different reference dates must convert times explicitly.
Call `refresh_table` before `scan_grouped`, and again after changing the risk grid or
curve values. Factors must be finite and positive. Output has
`groups * (K_discount + K_projection) * 8` doubles and must be 64-byte aligned.
`ladder/aligned_memory.hpp` provides matching aligned allocation/deallocation.
Public layout internals must not be manually corrupted; buffer lengths remain caller obligations.

Preparation is separate from kernel work. With discount records already time-sorted,
`scan_discount` advances one monotone bucket cursor in the forward pass and one boundary
cursor in the backward pass: **O(N+K)** for N records and K waves. Projection risk still
visits the wave intervals crossed by each fixing period, so its cost depends on support
width. Layout construction, sorting, coupon replication and curve/date-table refresh are
separate costs.

## Supported formulas and reference validation

The replication helpers implement the displayed fixed, forecast IBOR-ratio and fully
forecast compounded-OIS-ratio formulas. They do not inspect calendars, historical
fixings, coupon pricers, averaging/lookback/lockout conventions, optionality or gearing.
A caller must incorporate gearing into the scale, separate fixed coupons and supply
correct forecast periods. Arbitrary seasoned OIS coupons are not supported merely by
passing their full start/end dates to the simple ratio helper.

The test suite validates sensitivity ladders against both direct analytical oracles and
high-precision central finite differences. `scan_wave` and its CTest replay consume
recorded weights and recorded finite differences to verify exact machine precision across
all tenors and zero leakage beyond instrument maturities.
See `VALIDATION.md` for the checks actually performed and verification suites.

## Evidence

Validation: [VALIDATION.md](VALIDATION.md). Benchmarks: [BENCHMARKS.md](BENCHMARKS.md). Claim-to-code mapping: [docs/PAPER_EVIDENCE.md](docs/PAPER_EVIDENCE.md). Provenance and unit notes: [docs/HISTORY.md](docs/HISTORY.md).
=======
More detail: [validation and limitations](VALIDATION.md), [reduced-form cross-gamma measurement](docs/CROSS_GAMMA_BENCHMARK.md), [live OIS gamma checks](docs/QUANTLIB_LIVE_OIS_GAMMA.md) and [third-party notices](THIRD_PARTY_NOTICES.md).
>>>>>>> origin/main
