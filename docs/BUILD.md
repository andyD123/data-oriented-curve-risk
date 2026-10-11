# Build, test and development guide

The repository is a header-only C++20 library with CMake test and example targets. Start from the repository root; `CMakeLists.txt` is at the top level. CMake 3.16+ and a C++20 compiler are required. No network fetch or QuantLib installation is needed for the default build.

## Standard build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

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

```sh
cmake --preset release
cmake --build --preset release --parallel 2
ctest --preset release
```

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
