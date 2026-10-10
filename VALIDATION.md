# Code review and validation — 4 October 2026

## Review basis

The corrected upload was `files - 2026-10-04T112726.620.zip`, SHA-256
`8704d170e43603411ff198dcff06a06dac36143f9d6fa1daa9e29fce13e56ae4`.
The source root in its nested archive was `reworking_project/ladder_repo/`.
At the start, GitHub main was already `39777a2ac3560afaf0ea505ce386614802b47096`,
not the earlier README-only state. Changes in this review build on that main;
they do not restore the obsolete prototype or remove its additional tests/results.
Existing edited files were checked against their Git blob hashes. In particular,
the structural-identity extension in main's original test file was retained.
During review, main advanced to `67823562e3d822513705c24f7b9c12621f183ab2`.
The final patch is based on that commit and preserves its shared CLion run configurations,
existing preset names, benchmark targets, data-directory startup and new LRU source/results.
The AVX-512 historical benchmarks are now explicitly optional (`release-paper` preset).
The new default/portable build, including retained aggregation and CLion-style data paths,
was rebuilt and its four CTest tests rerun after this reconciliation.

## Material fixes

A cashflow before the first boundary wrongly contributed negative interior overlap
in both scalar and grouped scans. For boundaries `{0,1,2}`, PV 100 at `t=-0.5`,
both returned first-wave risk **+50 instead of 0**. The corrected implementations
exclude that contribution. Tests exercise shifted origins as well as origin zero.

The previously corrected terminal-wave behaviour remains intact:

| Record | Bounded final wave | Open final wave |
|---|---:|---:|
| PV 100 at t=2 | -100 | -100 |
| PV 100 at t=2.5 | -100 | -150 |

Other changes reject malformed stencils, non-finite/unsorted scalar records, reversed
projection endpoints, invalid day scales and invalid curve factors. Grouped execution
detects missing refreshes, stale risk-grid bucket caches and null/misaligned output.
The raw buffer API still requires callers to provide enough storage and not corrupt
public layout internals. Arbitrary floating-point overflow is not a newly guaranteed domain.

The default CMake build now uses AUTO without implicit native-ISA selection, validates
backend names, scopes ISA flags and maintains a zero-dependency quarantine. Its tests and
library benchmark use C++ aligned new/delete instead of `std::aligned_alloc`.
`scan_wave` validates complete inputs before writing results; the benchmark validates
arguments/data and gates both discount and projection comparisons, including near-zero
values. Comparison failures return non-zero exit codes. Historical paper kernels and recorded
outputs were not changed. Manuscripts and private session notes are not part of this patch.

## Executed checks

Host: Linux x86-64, AMD EPYC 9V74 (AVX2/FMA/AVX-512 available).
Compilers: GCC 14.2.0 and Clang 17.0.0. CMake 3.31.6.

| Configuration | CTest result |
|---|---|
| GCC Release, AUTO (no native flags) | 4/4 passed |
| GCC Release, PORTABLE | 4/4 passed |
| GCC Release, AVX2 (+FMA, no native flags) | 4/4 passed |
| GCC Release, AVX512 (+FMA) | 4/4 passed |
| GCC Release, STDX (baseline target) | 4/4 passed |
| GCC Release, STDX + native, prefer-vector-width=512 | 4/4 passed |
| Clang Release, PORTABLE | 4/4 passed |
| GCC Debug, PORTABLE, AddressSanitizer + UndefinedBehaviorSanitizer | 4/4 passed |

Each configuration runs the original tests, **15,640 new boundary/oracle/input checks**,
a 256-instrument local benchmark correctness gate, and isolated example tests.
The new oracle uses its own direct overlap calculation, not `Stencils::overlap` or
`psi`. It covers every boundary and neighbouring representable values, times before
the first and beyond the last boundary, one-wave stencils, empty books/instruments,
signed/cancelling records, open projection tails, unrelated/partially shared schedules,
partial groups, output canaries and both store policies. The original parallel-shift
and coarsening identities also pass. Tests remain active in Release builds.

The fresh replay covers **8 recorded instruments, 30+36 waves**. Maximum scaled
scan/direct-oracle difference was **1.610e-16**. It also passes the comparison to the
recorded reference finite differences at epsilon 5e-5 (configured scaled tolerance
2e-7). Deliberately missing/truncated/malformed inputs, non-finite data, invalid arguments
and deliberately corrupted comparison data are rejected. All replay outputs are written
to temporary directories; committed fixtures remain immutable.

These tests verify the algebra and replay of supplied risk weights. They do not certify
that every financial instrument adapter has supplied the right weights.

## Performance scope

A small, unpinned VM smoke comparison used the same 20,000-instrument seasoned library
book and best-of-seven timings, with the original and reviewed STDX/native builds.
Both reported grouped/scalar scaled agreement of 3.4e-15. In that comparison, original
versus reviewed scalar scan was 8.09 vs 9.42 ms, normal grouped stores 4.55 vs 4.84 ms,
and streaming grouped stores 3.41 vs 3.18 ms. These are preliminary observations, not
statistical evidence of a speedup. Validation adds scalar/preparation cost; grouped
timings also vary with the VM. No 500,000-instrument paper benchmark was rerun or retimed.
The final benchmark additionally labels logical and padded storage separately.

## Reference Engine and Portability Scope

The library strictly maintains zero external dependencies. **No legacy external
bootstrap or monolithic pricing engine is required or linked.**
Windows/MSVC and Apple/ARM were not executed; removal of their obvious build
obstacles is not a successful test on those platforms.

Reference benchmarks use published market quotes (Eonia and Euribor 6M quotes).
Fixed-base wave bumps must not be confused with quote bumps plus a full
rebootstrap, and curve reference dates must be converted explicitly.

The freshly added LRU scenario benchmark and full historical paper benchmark were not
rerun in this review. Their source/results and run configurations were retained, rather
than representing them as newly certified measurements. The core tests and default
library/replay executables are the tested build surface.

No release/publication tag or new automatic CI/hardware-sweep workflow was created.

## Follow-up validation — 6 October 2026

This addendum records later repository changes; it does not rewrite the 4 October review above.
The follow-up environment was an Intel Xeon Platinum 8573C cloud container with GCC 14.2.0 and AVX-512
available. Zero external dependencies were used.

- `scan_discount` now uses monotone forward/backward boundary cursors: O(N+K) once records are sorted.
- `bench_paper.cpp` now emits per-unit instantaneous-forward-rate shifts, has an unequal-bucket unit self-check,
  checks normal and streaming output independently, checks the repeated-valuation baseline, and returns non-zero
  on a failed gate. The historical 50.91 ms file is retained and labelled as pre-alignment theta units.
- `hagan_hedge.py` defaults to named `paper7`, excluding the 35y terminal-wave test bond, and uses explicit
  back-substitution. `extended8` is a separate experiment.
- `write_floor.cpp` provides a reproducible exact-size write reference. The original historical 270 MiB / 15.7 ms
  source/log is still unavailable and remains labelled historical.

Executed after these changes on the supplied tree before upload: release AVX2/AVX-512 CTest **passed**,
including the paper-kernel gate. A portable ASan/UBSan build passed.
The small paper gate gave about 8e-14 discount and 4e-16 projection grouped/scan differences, and BASE/scan
about 3e-8. Current-source 500k smoke logs are committed separately and are not replacements for historical
publication timings.

Still outstanding: recovery or manuscript
replacement of the exact historical LRU 10,921/1,310 ms run; recovery of the original 15.7 ms write-only and
431/726/8784 ms adjoint raw logs; a version-matched release tag and an explicit project licence.
