# Paper claim -> code and evidence

This page distinguishes **current executable checks** from **historical timing records**.
Do not combine rows from different machines/runs into one benchmark table.

| Claim | Current command / source | Evidence status |
|---|---|---|
| Box-wave derivative, capped/open terminal wave | `ctest --preset release` | Current independent-oracle tests. |
| Scalar discount scan is O(N+K) once records are sorted | `ladder/scan.hpp` | Current implementation uses monotone forward/backward boundary cursors. |
| Scenario date cache is correct | `scenario_bench 2000 1 1` / CTest | Current gated comparison with scalar scan. |
| 500k headline workload shape: 3,540 dates, 64,034 groups | `recorded_500k.txt` | Historical raw record. |
| Historical 50.91 ms / 1,128x result | `recorded_500k.txt` | Historical Sapphire Rapids VM timing. That source emitted interval-integrated-forward theta units. Current `bench_paper.cpp` emits rate-shift delta units; rerun before assigning a new timing to current source. |
| Current rate-unit paper kernel gate | `bench_paper 2000 1 1`, CTest | Normal and streaming outputs checked separately against scan and baseline. |
| Fresh 500k rate-unit smoke | `recorded_2026-10-06_rate_units_500k.txt` | Unpinned cloud validation, not publication timing. |
| Adjoint mechanism | `adjoint_bench`, CTest | Current scan/direct/tape comparison gate. |
| Historical 431/726/8784 ms adjoint row | paper/README values | Original raw timing log has not been recovered. |
| Fresh 500k adjoint check | `recorded_2026-10-06_adjoint_500k.txt` | Current cloud validation, separate from historical values. |
| Schedule ordering reduces cache misses | `scenario_bench`, `recorded_lru_*` | Reproducible miss counts. Fresh current record is `recorded_2026-10-06_lru_500k_rate_units.txt`. The manuscript 10,921/1,310 ms pair is still not tied to a committed raw run. |
| Historical write-only comparison | 270 MiB / 15.7 ms | Original source/log is not currently in the repository. |
| Current exact-size write reference | `write_floor`, `recorded_2026-10-06_write_floor.txt` | Reproducible current source, different host, not publication timing. |
| Eight-instrument wave reconciliation | `scan_wave`, CTest replay; optional `ql_waves` | Recorded replay is current; fresh QuantLib generation requires QuantLib. |
| Seven-instrument Hagan hedge | `python3 examples/hagan_waves/hagan_hedge.py --book paper7` | Named population; explicit back-substitution. |
| Quote-risk composition | optional CTest `quantlib_quote_risk_reconciliation` | Source generates two quote steps and checker Richardson-extrapolates direct risk and Jacobian with a failure gate. Fresh QuantLib run still required. |

## Publication rule

A timing in the paper should name the exact recorded file or frozen release that supports it. If a source change
alters the timed calculation, including output-unit scaling, retain old timings as historical records and generate
a new timing rather than silently relabelling them.
