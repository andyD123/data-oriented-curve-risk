# Evidence and benchmark provenance

Historical records, units and provenance notes for the benchmark files. See also [BENCHMARKS.md](../BENCHMARKS.md) and [PAPER_EVIDENCE.md](PAPER_EVIDENCE.md).

The fresh review evidence is in `VALIDATION.md`. Linux GCC/Clang configurations were
executed; Windows/MSVC and Apple/ARM were not executed in that review. The default build
no longer imposes GNU/x86 flags, and its core tests/benchmark use C++ aligned new/delete,
but that is not a claim of tested portability on those unexecuted systems.

Existing `recorded_*` files remain historical evidence, not results of this review.
`examples/benchmark_paper/recorded_500k.txt` predates the October 2026 rate-unit alignment:
that historical run emitted interval-integrated-forward (`theta`) sensitivities. Current
`bench_paper.cpp` emits the manuscript/library convention, per unit additive instantaneous-
forward shift (`delta`). The old timing is retained and labelled rather than silently
reinterpreted; rerun the current source before attaching a new timing to it.
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
