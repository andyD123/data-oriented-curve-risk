# data-oriented-curve-risk: Ultra-Fast Data-Oriented Curve Risk

> **~1,100× faster than bump-and-reprice** on a 500,000-instrument book (1.5 ns per sensitivity, single thread, AVX-512; historical run, current-source rerun pending, see [BENCHMARKS.md](BENCHMARKS.md) for baseline, hardware and caveats). Pure standalone C++20 with **zero external dependencies** and no object-oriented curve or calendar layer.

Companion sources for **"Reworking the Inner Loop: Data-Oriented Curve Risk"** (Andrew Drakeford & Lars Schouw, October 2026).
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

## Scope and limitations

- Zero curves only: the caller supplies boundary times and discount factors (or zero rates). There are no
  calendars, day-count conventions or curve construction in the library.
- Results are analytic first derivatives per unit forward shift, not finite-shock P&L.
- Tested on Linux x86-64 with GCC and Clang. Windows/MSVC and Apple/ARM were not run.
- Benchmark figures are single-threaded; see [BENCHMARKS.md](BENCHMARKS.md).

## Reading the examples

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

`ladder/` is header-only. The CMake interface target is `ladder` (alias `ladder::ladder`).
`Stencils`, `UnitCashflow`, `ProjectionTerm`, scalar scans, replication helpers, grouped
layout and grouped scans remain in namespace `ladder`.

For bounded wave k, `overlap = clamp(t - B[k-1], 0, B[k] - B[k-1])`.
For `open_last=true`, the final overlap is `max(t - B[K-1], 0)`.
Discount risk is `-sum(x * overlap)`; projection risk is `sum(w * (overlap(b)-overlap(a)))`.
A projection coupon wholly beyond the start of an open final wave still contributes
`w*(b-a)`. It does not have the zero downstream tail of a bounded wave.

Results are analytic first derivatives per unit forward shift. Multiplying by `1e-4`
gives linearised 1 bp risk, not exact finite 1 bp scenario P&L. For products/ratios,
generalised unit cashflows are first-order signed weights `dV/dlog D(t)`; they do not
by themselves determine higher derivatives or an exact finite-shock decomposition.
Floating-point summation and contraction can introduce rounding differences.

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

Validation: [VALIDATION.md](VALIDATION.md). Benchmarks: [BENCHMARKS.md](BENCHMARKS.md). Claim-to-code mapping: [docs/PAPER_EVIDENCE.md](docs/PAPER_EVIDENCE.md). Historical records and unit notes: [docs/HISTORY.md](docs/HISTORY.md).
