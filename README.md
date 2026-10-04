# data-oriented-curve-risk

C++20 companion sources for Andrew Drakeford's draft **Reworking the Inner Loop**.
The SSRN manuscript is the technical draft; the Wilmott article is the shorter draft.
This repository does not assert that either has been published.

The library computes first-order sensitivity to interval-forward **waves**. The wave
definition and its separation from curve stripping are due to Hagan. The contribution
here is the data-oriented organisation: shared dates, grouped instruments and reverse scans.

## Build and open in CLion

Open the repository root: `CMakeLists.txt` is here, not inside an archive or wrapper directory.
A C++20 compiler and CMake 3.16 or newer are required. No QuantLib installation or network
fetch is needed for the default build.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

The default `AUTO` backend uses the compiler target's AVX-512/AVX2 support, otherwise
portable loops. It does not silently enable host-native instructions. Select an ISA
only when the machine running the binary supports it:

```sh
cmake -S . -B build-avx2 -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=AVX2
cmake -S . -B build-avx512 -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=AVX512
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=PORTABLE
cmake -S . -B build-stdx -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=STDX -DLADDER_NATIVE_ARCH=ON
```

`STDX` requires `<experimental/simd>`. `LADDER_NATIVE_ARCH=ON` is opt-in and produces a
host-specific binary. Explicit AVX backends receive the appropriate compiler flags;
unknown backend names are configuration errors. Normal/streaming store equivalence is
tested within each configuration, not claimed as cross-backend bitwise reproducibility.
STDX can use x86 streaming stores when its target supports them; portable loops cannot.

Default targets (single-ISA build) are `test_ladder`, `test_boundaries`, `bench_library`, `aggregation` and `scan_wave`; the `release` preset builds the vectorised ones as `_avx2` and `_avx512` pairs (below).
CTest runs the original suite, independent boundary/oracle tests and a small benchmark
correctness gate. With Python 3 installed it also runs an isolated wave-fixture replay
and malformed-input checks (standard library only; four CTest tests in total).
`BUILD_TESTING=OFF` omits tests; `LADDER_BUILD_EXAMPLES=OFF` omits examples.

For example, on a single-configuration generator:

```sh
./build/examples/benchmark/bench_library 20000 0 7
```

Arguments are instrument count, whether to run the repeated-bump baseline, repetitions,
and an optional curve-data path. Examples enter their own build directories, where CMake
copies the bundled data, so CLion runs need no working-directory setting. Invalid arguments,
missing/malformed data and failed numerical comparisons return a non-zero exit code.

All shipped `CMakePresets.json` profiles **build Release binaries**: `release`,
`release-avx512`, `release-avx2`, `release-portable` and `release-paper`.
The configure presets set both `CMAKE_BUILD_TYPE=Release` (single-configuration generators)
and `CMAKE_CONFIGURATION_TYPES=Release` (multi-configuration generators). Every build
preset explicitly selects `configuration: Release`, equivalent to `--config Release`;
the test presets select Release too. The former `debug` preset has been removed.
With CMake 3.21 or newer:

```sh
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

The `release` preset builds both instruction sets. Each vectorised target exists twice,
built against `ladder_avx2` (`-mavx2 -mfma`) and `ladder_avx512`
(`-mavx512f -mavx512dq -mfma -mprefer-vector-width=512`):
`test_ladder_avx2` / `test_ladder_avx512`, `test_boundaries_avx2` / `test_boundaries_avx512`,
`bench_library_avx2` / `bench_library_avx512`. The scalar examples (`aggregation`,
`scan_wave`, the QuantLib generators) are built once. With GCC/Clang and
`<experimental/simd>`, `release` also builds the AVX-512 paper benchmarks
(`bench_paper`, `scenario_bench`, `adjoint_bench`); with other compilers they are skipped
with a message. Configure checks whether the build host can execute AVX-512: both variants
are always built, but on a host without AVX-512 CTest runs only the `_avx2` tests and the
`_avx512` binaries must not be run there. The equivalent cache settings are
`-DLADDER_LANES=AVX2 -DLADDER_DUAL_ISA=ON`.

The single-ISA presets are unchanged: `release-avx512`, `release-avx2`, `release-portable`
(no intrinsics; use it when neither AVX2 nor AVX-512 is wanted) and `release-paper`. No extra
`-DCMAKE_BUILD_TYPE=Release` or `--config Release` is needed: the presets supply them.
In CLion, reload CMake after pulling and enable the `release` profile; the shared run
configurations in `.run/` name the `release` targets, including both ISA variants.
WSL or MinGW with GCC 13+/libstdc++ is the route for the paper benchmarks on Windows.
Explicit Debug builds can still be configured separately for diagnosis; they are not part
of the shipped presets.

The paper benchmarks retain their historical input-handling/benchmark assumptions, not the
strict conformance contract of `bench_library`.

`scan_wave [data-and-output-directory]` optionally selects a separate replay directory;
its no-argument CLion run uses build-directory copies. The CTest replay always selects
a temporary directory. No generated result needs to overwrite a source-tree fixture.

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

Preparation is separate from kernel work. The current scalar discount implementation
uses a binary-search bucket lookup per record: **O(N log K + K)** once sorted, not an
unqualified O(N+K) algorithm. Grouped discount column walks are linear in columns plus
waves per group; coupon preparation and projection coupon/wave overlaps add their own
work. Layout construction, sorting and curve/date-table refresh are not free.

## Supported formulas and QuantLib examples

The replication helpers implement the displayed fixed, forecast IBOR-ratio and fully
forecast compounded-OIS-ratio formulas. They do not inspect calendars, historical
fixings, coupon pricers, averaging/lookback/lockout conventions, optionality or gearing.
A caller must incorporate gearing into the scale, separate fixed coupons and supply
correct forecast periods. Arbitrary seasoned OIS coupons are not supported merely by
passing their full start/end dates to the simple ratio helper.

Optional reference generators target the existing QuantLib 1.33 examples:

```sh
cmake -S . -B build-ql -DCMAKE_BUILD_TYPE=Release -DLADDER_BUILD_QUANTLIB=ON
cmake --build build-ql --config Release --parallel 2
```

This provides `ql_waves`, `ql_examples` and `scan_reconcile`. `AUTO` (the default)
enables them when QuantLib is found; `ON` requires it and fails clearly if absent;
`OFF` disables them. It never downloads dependencies automatically. Generators write
into their example build directories, not the source tree; keep fresh outputs separate
from immutable recorded evidence.
The historical example scripts additionally use NumPy. Their printed reports alone
are not substitutes for executable conformance gates.

`scan_wave` and its CTest replay consume recorded weights and recorded finite differences.
**Replay is not a freshly bootstrapped QuantLib reconciliation.** The current optional
harness is a research example, not a general production adapter: per-coupon fixing/pricer
semantics, today's/historical fixings, seasoned OIS treatment and engine-level valuations
still require a fresh full audit. The zero-coupon example is intentionally retained at
redemption 100, unlike the original QuantLib Bonds example's 116.92; it is an adaptation,
not an unchanged copy. The two curve reference dates are also not interchangeable.
See `VALIDATION.md` for the checks actually performed and outstanding cases.

## Evidence and benchmark generations

The fresh review evidence is in `VALIDATION.md`. Linux GCC/Clang configurations were
executed; Windows/MSVC and Apple/ARM were not executed in that review. The default build
no longer imposes GNU/x86 flags, and its core tests/benchmark use C++ aligned new/delete,
but that is not a claim of tested portability on those unexecuted systems.

Existing `recorded_*` files remain historical evidence, not results of this review.
Keep the standalone `examples/benchmark_paper` workload separate from the broader
`examples/benchmark` library/OIS workload and its seasoned-book revision. The historical
paper's approximately 50.9 ms, older OIS-inclusive approximately 62.7 ms and later
seasoned-library approximately 51.1 ms refer to different runs/configurations.
The paper benchmarks retain separate platform/dependency assumptions and are optional
root targets; the aggregation example is a default target (not a CTest benchmark run).

For the historical 500,000-instrument paper layout, logical output is 264,000,000 bytes;
64,034 padded eight-lane groups occupy 270,479,616 bytes. A 270 MiB write-only experiment
is a different byte count. Neither old timings nor their workloads were silently replaced.
No automated CI workflow or expensive hardware sweep was added by this review.

QuantLib-derived example material retains the notices in `THIRD_PARTY_NOTICES.md`.
