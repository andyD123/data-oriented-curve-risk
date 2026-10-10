# Build, presets and CLion

Open the repository root: `CMakeLists.txt` is here, not inside an archive or wrapper directory.
A C++20 compiler and CMake 3.16 or newer are required. Zero external dependencies or network
fetch are needed for the build.

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
(`-mavx512f -mavx512dq -mfma -mprefer-vector-width=512`); both add `-ffp-contract=fast`, and under
MSVC they are `/arch:AVX2` and `/arch:AVX512` with `/fp:contract` (VS 2022 and later), so every
compiler may fuse `a*b+c` into one FMA. Contraction changes rounding in the last bits, and each
compiler chooses where to apply it, so results agree across compilers to tolerance, not bit for bit:
`test_ladder_avx2` / `test_ladder_avx512`, `test_boundaries_avx2` / `test_boundaries_avx512`,
`bench_library_avx2` / `bench_library_avx512`. The scalar examples (`aggregation`,
`scan_wave`) are built once. `release` also builds the LRU date
cache example `scenario_bench` with the AVX-512 variant, on any compiler including MSVC. With
GCC/Clang and `<experimental/simd>`, it also builds the AVX-512 paper benchmarks
(`bench_paper`, `adjoint_bench`); with other compilers they are skipped with a message.
Configure checks whether the build host can execute AVX-512: both variants
are always built, but on a host without AVX-512 CTest runs only the `_avx2` tests and the
`_avx512` binaries must not be run there. The equivalent cache settings are
`-DLADDER_LANES=AVX2 -DLADDER_DUAL_ISA=ON`.

The single-ISA presets are unchanged: `release-avx512`, `release-avx2`, `release-portable`
(no intrinsics; use it when neither AVX2 nor AVX-512 is wanted) and `release-paper`. No extra
`-DCMAKE_BUILD_TYPE=Release` or `--config Release` is needed: the presets supply them.
In CLion, reload CMake after pulling and enable the `release` profile; the shared run
configurations in `.run/` name the `release` targets, including both ISA variants.
WSL or MinGW with GCC 13+/libstdc++ is the route for `bench_paper` and `adjoint_bench` on Windows.
Under MSVC those two are not built, so their CLion configurations (`bench_paper 100k quick`,
`bench_paper 500k (paper section 9)` and `adjoint_bench 500k`) show as missing targets.
Explicit Debug builds can still be configured separately for diagnosis; they are not part
of the shipped presets.

The historical `bench_paper` target retains its earlier input-handling and comparison
assumptions. The refactored `scenario_bench` and `adjoint_bench` have explicit numerical gates;
this does not retroactively certify historical timing records.

`scan_wave [data-and-output-directory]` optionally selects a separate replay directory;
its no-argument CLion run uses build-directory copies. The CTest replay always selects
a temporary directory. No generated result needs to overwrite a source-tree fixture.
