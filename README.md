# Data-Oriented Curve Risk

A standalone reproducible example of data-oriented interest-rate curve risk using
**box-wave sensitivities**, a **reverse scan**, deal-axis SIMD, and **QuantLib reconciliation**.

The project separates the finance reference from the risk kernel:

1. `quantlib_reference` builds QuantLib 1.33's Eonia and Euribor 6M multi-curve example
   with its shipped cubic pricing interpolation.
2. A `BoxShifted` term structure overlays a local instantaneous-forward bump on one
   pillar interval. QuantLib reprices the instruments under `+eps/-eps` shifts and writes
   the finite-difference reference ladder.
3. The same QuantLib cashflows are normalised to dated **unit cashflows** and written as
   flat data.
4. `scan_wave` contains no QuantLib types and no pricing curve. It consumes only bucket
   boundaries and unit cashflows and evaluates the whole ladder with the reverse scan.
5. `compare.py` verifies `O(eps^2)` convergence of QuantLib central differences to the scan;
   `aad_check.py` differentiates the same wave valuation without a step size and checks
   agreement at rounding.

The point is representational: QuantLib remains the source of truth for schedules,
coupons, compounding, curve construction and valuation, while the hot risk calculation
operates on the smaller data representation exposed by those conventions.

## Risk convention

The implementation differentiates with respect to an **instantaneous forward-rate shift**
`delta_k` on `[B[k-1], B[k])`:

```text
D_delta(t) = D(t) * exp(-delta_k * overlap_k(t))
overlap_k(t) = clamp(t - B[k-1], 0, B[k] - B[k-1])
```

For a discounted unit cashflow `x_i`, the discount-curve ladder is therefore

```text
dPV/ddelta_k = -[
    sum_{B[k-1] <= t_i < B[k]} x_i (t_i - B[k-1])
  + (B[k]-B[k-1]) sum_{t_i >= B[k]} x_i
]
```

This is the convention used by `ladder/stencil.hpp`, `ladder/scan.hpp`, `BoxShifted`, and
all recorded reconciliation files.

## Build with QuantLib installed

Ubuntu 24.04 packages QuantLib 1.33:

```bash
sudo apt install build-essential cmake libquantlib0-dev python3 python3-numpy
./run.sh
```

Or explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cmake --build build --target run_reconciliation
cmake --build build --target run_hedge       # optional; numpy required
```

On Windows, point CMake at a QuantLib installation in the usual way (`CMAKE_PREFIX_PATH`
or `QuantLib_DIR`) and run `run.ps1`.

## Reproducible container

The supplied `Dockerfile` pins the environment to Ubuntu 24.04, whose repository package is
QuantLib 1.33:

```bash
docker build -t data-oriented-curve-risk .
docker run --rm data-oriented-curve-risk
```

## Build without QuantLib

The geometry-only scan can be built independently:

```bash
cmake -S . -B build-scan \
  -DCURVE_RISK_BUILD_QUANTLIB_REFERENCE=OFF \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build-scan -j
```

This mode tests the data-oriented kernel but does **not** regenerate the finance reference.

## Expected reconciliation

Recorded QuantLib 1.33 outputs are included under `recorded/`. On the seven-instrument
cubic-curve set, the scan and the explicit adjoint agree at roughly `1e-18 ... 1e-16`
relative (zero-coupon bitwise), while QuantLib central finite differences converge to the
scan at the expected factor of about four per halving of `eps` until rounding dominates.

`recorded/benchmark_500k_library.txt` records the current library benchmark: 500,000
instruments, 66 sensitivities each and 264 MB of output, with the 8-lane streaming scan at
62.68 ms / 1.899 ns per sensitivity on the recorded Sapphire Rapids-class run.

## Layout

```text
include/ladder/          Reverse-scan, stencil, layout and SIMD support
src/quantlib_reference.cpp
                        QuantLib finance reference and bump/reprice harness
src/scan_wave.cpp       Data-oriented reverse-scan executable
scripts/                Reconciliation, AAD and hedge checks
recorded/               Recorded QuantLib outputs and benchmark evidence
```
