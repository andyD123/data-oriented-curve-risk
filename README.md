# ladder — bucketed curve risk by reverse scan

Companion code for *Reworking the Inner Loop: Convention-Manufactured Computational Shape, Reverse Scans,
and the Memory Wall in Rates Risk* (A. Drakeford, Wilmott Magazine, 2026).

Risk to bucket forward bumps (Hagan box stencils) for any leg replicated into unit cashflows, computed by one
forward and one backward pass over date-sorted cashflows — no loop over buckets — and, grouped eight
instruments per AVX-512 vector with a shared date table, at a few nanoseconds per sensitivity.

## Layout

```
ladder/                header-only, C++20, namespace ladder
  stencil.hpp          Stencils: boundaries + box profile. overlap(k, t), bucket(t). Pure geometry.
  unit_cashflow.hpp    UnitCashflow {t, x}; ProjectionTerm {t_pay, a, b, w}
  scan.hpp             scan_discount(), scan_projection()   scalar scans, risk per unit forward bump
  replicate.hpp        fixed / IBOR / compounded-OIS legs -> unit cashflows (discount factors via callables)
  layout.hpp           InstrumentSpec -> GroupLayout (eight per group by schedule signature) + DateTable
  scan_simd.hpp        scan_grouped<Store::normal|streaming>()  eight-lane column walk; gather()
  lanes.hpp            the vector primitive set (eleven functions), four backends selected at compile time:
                       STDX std::experimental::simd (default); AVX512 and AVX2 intrinsics; PORTABLE plain loops
                       relying on auto-vectorisation (no intrinsics, any target); AUTO picks from the target flags
tests/test_ladder.cpp  scan vs explicit adjoint (1e-16), finite difference eps^2 convergence (ratio 4.00),
                       OIS lag-0 telescoping, exact zeros past maturity, grouped vs scalar (1e-15),
                       streaming == normal stores (bitwise)
examples/
  quantlib_reconcile/  QuantLib 1.33's MulticurveBootstrapping + Bonds examples, LogLinear curves,
                       node bumps; seven instruments reconciled to the finite-difference floor
  hagan_waves/         same instruments on the shipped *cubic* curves with box-wave risk: scan vs QuantLib
                       wave bumps (eps^2 convergence), vs an explicit adjoint (rounding), hedge solve
  benchmark_paper/     the paper's §9 configuration exactly (bonds + vanilla swaps, standalone kernel, 50.9 ms)
  benchmark/           library kernel on a broader book (bonds + IBOR swaps + OIS swaps, 62.7 ms); reported separately
  aggregation/         contiguous ladders vs per-instrument maps (1.7 ms vs 320–373 ms at 100k x 66)
```

## Build

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/test_ladder
cmake -S . -B build-avx2 -DLADDER_LANES=AVX2 && cmake --build build-avx2 && ./build-avx2/test_ladder
cmake -S . -B build-port -DLADDER_LANES=PORTABLE && cmake --build build-port && ./build-port/test_ladder
```
GCC 13 or later. All backends pass the same tests on AVX-512, AVX2-only and SSE2-only targets. Measured at 100k
instruments on one Sapphire Rapids core, streaming stores: AVX512 9.9 ms, AVX2 9.6 ms, STDX 8.4–12.0 ms depending on
target flags, PORTABLE 21–22 ms. Streaming stores exist only on the x86 intrinsics backends; the others fall back to
normal stores and lose that gain. The examples under `quantlib_reconcile/` and `hagan_waves/` need QuantLib C++
(`apt install libquantlib0-dev` gives 1.33 on Ubuntu 24.04) and numpy; each has its own `run.sh` or Makefile.

## Opening the project

`CMakePresets.json` defines the build profiles — `release` (std::experimental::simd), `release-avx512`,
`release-avx2`, `release-portable`, `debug` — and CLion, VS Code (CMake Tools) and Visual Studio pick them up on open.
From a shell:

```
cmake --preset release && cmake --build --preset release && ctest --preset release
```

**CLion.** Open the repository folder. The presets appear as CMake profiles, and the shared run configurations in
`.run/` appear in the run menu with their arguments set: `test_ladder`, `bench_paper 500k (paper section 9)`,
`scenario_bench 100k (LRU date cache)`, `adjoint_bench 500k`, `bench_library 500k (seasoned book)`, `aggregation`,
`scan_wave`, and the QuantLib harnesses. Each example starts in its own build directory, where its data files are
copied, so no working directory needs setting.

The code needs GCC 13+ with libstdc++ (`std::experimental::simd`, `std::aligned_alloc`); MSVC has neither. On Windows,
add a WSL toolchain in CLion (`Settings → Build, Execution, Deployment → Toolchains → + → WSL`, Ubuntu 24.04 with
`build-essential cmake libquantlib0-dev`) and put it first; the presets then build under it. The QuantLib targets are
created only when QuantLib is found.

## The algorithm in one place

`ladder/scan.hpp`:
```
forward  pass: interior[k] += x_i * (t_i - B[k-1])          one FMA per unit cashflow
backward pass: running    += x_i;  at each boundary B[k]: suffix[k] = running
combine:       dPV/ddelta_k = -( interior[k] + len(k) * suffix[k] )
```
The condition for exactness is that a stencil's bump leaks nothing outside its own interval; then every
discount factor past it moves by one factor and the tail factorises. Box stencils satisfy it on any pricing
curve (`examples/hagan_waves`). Node bumps of a cubic spline do not, and the scan correctly disagrees
with them (`examples/quantlib_reconcile`, Cubic variant).

## Recorded numbers (one Sapphire Rapids core, 2.1 GHz, VM)

`examples/benchmark`, 500,000 instruments, stencils 30 + 36, 264 MB output:

| variant | time | ns / sensitivity |
|---|---|---|
| bump-and-reprice (AoS, virtual npv, central differences) | 49,599 ms | 1,503 |
| scalar scan per instrument | 407 ms | 12.3 |
| eight-lane grouped, normal stores | 72 ms | 2.19 |
| eight-lane grouped, streaming stores | 63 ms | 1.90 |
| date-table refresh per curve update | 0.12 ms | — |

Streaming-store floor for 264 MB on this core: ~15 ms. Reconciliation numbers are in each example's README.
