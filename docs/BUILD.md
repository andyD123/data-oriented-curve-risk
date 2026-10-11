<<<<<<< HEAD
# Build, presets and CLion

Open the repository root: `CMakeLists.txt` is here, not inside an archive or wrapper directory.
A C++20 compiler and CMake 3.16 or newer are required. Zero external dependencies or network
fetch are needed for the build.
=======
# Build, test and development guide

The repository is a header-only C++20 library with CMake test and example targets. Start from the repository root; `CMakeLists.txt` is at the top level. CMake 3.16+ and a C++20 compiler are required. No network fetch or QuantLib installation is needed for the default build.

## Standard build
>>>>>>> origin/main

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

<<<<<<< HEAD
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

Default targets (single-ISA build) are `test_ladder`, `test_boundaries`, `test_demo_support`, `bench_library`, `aggregation` and `scan_wave`; the `release` preset builds the vectorised ones as `_avx2` and `_avx512` pairs (below).
CTest runs the original suite, independent boundary/oracle tests and a small benchmark
correctness gate. With Python 3 installed it also runs an isolated wave-fixture replay
and malformed-input checks (standard library only). Demonstrator helper and aggregation tests
are included as well; use `ctest --test-dir build -N` to list the selected tests. The `release`
preset adds the `_avx512` variants and, on a machine with AVX-512, a gated run of `scenario_bench`
with its malformed-input checks.
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
`release-avx512`, `release-avx2` and `release-portable`.
The configure presets set both `CMAKE_BUILD_TYPE=Release` (single-configuration generators)
and `CMAKE_CONFIGURATION_TYPES=Release` (multi-configuration generators). Every build
preset explicitly selects `configuration: Release`, equivalent to `--config Release`;
the test presets select Release too. The former `debug` preset has been removed.
With CMake 3.21 or newer:
=======
`LADDER_LANES=AUTO` is the default. It uses ISA features already enabled for the compiler target; it does not silently turn on `-march=native`. Choose an explicit backend for reproducible builds:

```sh
cmake -S . -B build-portable -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=PORTABLE
cmake -S . -B build-avx2     -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=AVX2
cmake -S . -B build-avx512   -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=AVX512
cmake -S . -B build-stdx     -DCMAKE_BUILD_TYPE=Release -DLADDER_LANES=STDX
```

`PORTABLE` uses compiler-vectorisable loops; `AVX2` and `AVX512` require matching x86 execution hardware; `STDX` requires `<experimental/simd>` in the chosen standard library. `LADDER_NATIVE_ARCH=ON` explicitly requests a host-specific `-march=native` build.

On GCC/Clang, the supported vector backends allow fused multiply-add contraction (`-ffp-contract=fast`). Supported MSVC compilers use `/fp:contract`. Contraction, summation order and compiler choices may change last-bit rounding: numerical tests use stated tolerances unless a separate bitwise contract is explicitly tested. Streaming stores do not imply the portable backend has ISA-specific non-temporal instructions.

## Shipped CMake presets

With CMake 3.21+:
>>>>>>> origin/main

```sh
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

<<<<<<< HEAD
The `release` preset builds both instruction sets. Each vectorised target exists twice,
built against `ladder_avx2` (`-mavx2 -mfma`) and `ladder_avx512`
(`-mavx512f -mavx512dq -mfma -mprefer-vector-width=512`); both add `-ffp-contract=fast`, and under
MSVC they are `/arch:AVX2` and `/arch:AVX512` with `/fp:contract` (VS 2022 and later), so every
compiler may fuse `a*b+c` into one FMA. Contraction changes rounding in the last bits, and each
compiler chooses where to apply it, so results agree across compilers to tolerance, not bit for bit:
`test_ladder_avx2` / `test_ladder_avx512`, `test_boundaries_avx2` / `test_boundaries_avx512`,
`bench_library_avx2` / `bench_library_avx512`. The scalar examples (`aggregation`,
`scan_wave`) are built once. The adjoint comparison (`adjoint_bench`), the
write-only reference (`write_floor`) and the LRU date-cache example (`scenario_bench`) are built as well
(`scenario_bench` uses the AVX-512 variant under dual ISA).
Configure checks whether the build host can execute AVX-512: both variants
are always built, but on a host without AVX-512 CTest runs only the `_avx2` tests and the
`_avx512` binaries must not be run there. The equivalent cache settings are
`-DLADDER_LANES=AVX2 -DLADDER_DUAL_ISA=ON`.

The single-ISA presets are unchanged: `release-avx512`, `release-avx2`, `release-portable`
(no intrinsics; use it when neither AVX2 nor AVX-512 is wanted). No extra
`-DCMAKE_BUILD_TYPE=Release` or `--config Release` is needed: the presets supply them.
In CLion, reload CMake after pulling and enable the `release` profile; the shared run
configurations in `.run/` name the `release` targets, including both ISA variants.
Explicit Debug builds can still be configured separately for diagnosis; they are not part
of the shipped presets.

`scan_wave [data-and-output-directory]` optionally selects a separate replay directory;
its no-argument CLion run uses build-directory copies. The CTest replay always selects
a temporary directory. No generated result needs to overwrite a source-tree fixture.
=======
`release` builds both `_avx2` and `_avx512` versions of supported vector targets. The configure-time hardware check determines whether this machine can **run** AVX-512 tests. Both variants may be built on a machine that cannot execute the AVX-512 binary; such tests are not scheduled by CTest on that host.

The other presets are `release-avx2`, `release-avx512`, `release-portable` and `release-paper`. The paper preset enables the specialised AVX-512 targets. All shipped presets configure and test Release builds; configure Debug explicitly when diagnosing failures.

## Optional build switches

| CMake setting | Default | Effect |
|---|---|---|
| `LADDER_BUILD_EXAMPLES` | `ON` | Build library and demonstration executables |
| `BUILD_TESTING` | `ON` | Register the CTest correctness checks |
| `LADDER_BUILD_QUANTLIB` | `AUTO` | Find installed QuantLib for optional C++ reference generators; `ON` requires it, `OFF` disables it |
| `LADDER_BUILD_PAPER_BENCHMARKS` | `OFF` | Build `bench_paper`, `adjoint_bench` and related x86/AVX-512 tools |
| `LADDER_BUILD_GAMMA_BENCHMARK` | `OFF` | Build the optional mixed-book diagonal-gamma benchmark |
| `LADDER_BUILD_REDUCE_BENCHMARK` | `OFF` | Build the portfolio-only reduction comparison |
| `LADDER_BUILD_WIDE_GROUP_EXPERIMENT` | `OFF` | Build the variable-width schedule-group experiment |
| `LADDER_DUAL_ISA` | `OFF` | Build separate AVX2 and AVX-512 test/example variants |
| `LADDER_NATIVE_ARCH` | `OFF` | Opt into host-specific compiler instructions |

`LADDER_BUILD_PAPER_BENCHMARKS=ON` needs the GCC/Clang paper-benchmark dependencies, an x86 target and `<experimental/simd>`. Under the dual-ISA preset, CMake can also enable these targets when its dependency checks succeed. Missing optional tools must not be mistaken for a failure of the portable library.

## The curve example

The standalone [curve introduction](../examples/curve_intro/README.md)
is built with the ordinary example targets:

```sh
cmake --build build --target curve_intro
./build/examples/curve_intro/curve_intro
```

`test_curve` exercises curve factories, shifted first boundaries, the
valuation origin, zero/simple-forward rates, bounded/open final-wave
shocks, and agreement with the production scalar scan.

## Grouped output buffers

The low-level grouped and gamma kernels require caller-owned, 64-byte-aligned
AoSoA output buffers. For new code prefer their **exact-size**
`std::span<double>` overloads, which validate the output extent before
calling the same kernel. The portfolio reduction also provides sized
spans and optional reusable scratch. The checked Hessian-view constructor
accepts two input spans but **does not own their memory**.

See the [grouped data-flow and ownership guide](GROUPED_DATA_FLOW.md).

## Correctness tests

The standard suites cover scalar/grouped discount and projection risk, wave boundaries, malformed inputs, first-order portfolio reduction, optional grouped gamma and its scalar oracle, example smoke tests, and the recorded financial fixtures. Some tests also require Python 3, using only its standard library.

```sh
ctest --test-dir build --output-on-failure
ctest --test-dir build -N
```

For targeted checks:

```sh
ctest --test-dir build -R '^(grouped_reduce|rfr_reconcile_replay)$' --output-on-failure
ctest --test-dir build -R '^(cross_gamma|gamma_grouped)' --output-on-failure
```

The RFR replay runs the recorded fixtures **and** regenerates an independent illustrative overnight-rate curve and book in a temporary directory. It checks wave risk, adjoints, the bootstrap quote Jacobian, hedging and a spline negative control. It does not require QuantLib.

The independent live QuantLib OIS gamma exercise is *separate* from the dependency-free CTest build. To reproduce the focused CI:

```sh
python -m pip install 'QuantLib==1.43'
python examples/cross_gamma/quantlib/live_ois_cross_gamma.py --scenario all
```

The optional C++ QuantLib examples (`ql_waves`, `ql_examples` and `scan_reconcile`) require a separately installed QuantLib library:

```sh
cmake -S . -B build-ql -DCMAKE_BUILD_TYPE=Release -DLADDER_BUILD_QUANTLIB=ON
cmake --build build-ql --parallel 2
```

The `AUTO` setting silently skips these generators when the library is absent, but does not download dependencies. Keep newly generated outputs distinct from immutable recorded example fixtures.

## Examples, paths and IDEs

`ladder_example` copies each example's text/Python data to its build directory. Examples enter their configured working directory, so CLion runs do not normally need manual working-directory changes.

```sh
./build/examples/benchmark/bench_library 20000 0 7
```

This example takes an instrument count, a bump/reprice-baseline flag, a repetition count and an optional curve-data path. Failed numerical comparisons, invalid arguments and missing/malformed inputs return nonzero exit codes.

For optional benchmark commands and workload descriptions, use [`BENCHMARKS.md`](../BENCHMARKS.md) rather than mixing timing conditions into the build instructions. For example order and financial interpretation, use the [demonstrator reading guide](../examples/READING_GUIDE.md).

In CLion, enable the appropriate CMake profile after checkout and reload CMake when changing ISA options. The shared `.run/` configurations name the compiled Release targets. Windows users can build the ordinary library and supported tests with MSVC; the paper's GCC/Clang `<experimental/simd>` benchmarks are separate, with WSL/MinGW as alternatives. Windows/MSVC and Apple/ARM support must not be inferred solely from successful Linux CI.

No generated output should overwrite a committed source-tree fixture.
>>>>>>> origin/main
