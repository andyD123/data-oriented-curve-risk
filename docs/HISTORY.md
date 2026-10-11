# Evidence and benchmark provenance

See [BENCHMARKS.md](../BENCHMARKS.md) and [PAPER_EVIDENCE.md](PAPER_EVIDENCE.md).

- Earlier timing records from a different workload and input were removed. All current timings are in
  `benchmarks/results/` and were produced from the source in this repository.
- Curve input: `examples/benchmark/rfr_curves.txt` (and its copy in `examples/benchmark_paper/`) is an illustrative
  SOFR-style par OIS bootstrap (`examples/benchmark/make_rfr_curves.py`); the projection block carries the same curve,
  as in a one-curve RFR market. It is not a market snapshot.
- Linux GCC configurations were executed. Windows/MSVC and Apple/ARM were not, so no portability claim is made
  for them.
- Output units: first derivatives per unit additive instantaneous-forward shift, matching the paper. Output
  is 66 sensitivities per instrument for the 30 + 36 node grids; 500,000 instruments occupy 273,504,000 bytes
  padded (64,750 groups) against 264,000,000 bytes logical.
