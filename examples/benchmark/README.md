# benchmark — library kernel on a seasoned book

Bonds, IBOR swaps and compounded-OIS swaps on the Eonia/Euribor6M curves of QuantLib's MulticurveBootstrapping example.
Schedules are walked back from the last regular payment date with a random remaining life, so the book is seasoned;
the grouping key is (type, maturity day) and groups mix seasoned and new trades on the same grid.

```
g++ -O3 -std=c++20 -march=native -mprefer-vector-width=512 -ffp-contract=fast -I../.. -Wno-unused-result bench.cpp -o bench
./bench 500000 0 3        # N, run bump-and-reprice baseline (needs ~1 GB more at 500k), repetitions
```
Recorded on one Sapphire Rapids core (2.1 GHz, VM):
- `recorded_500k_seasoned.txt`: 500k, 96.5% lane occupancy, streaming stores 51.1 ms (1.55 ns/sensitivity), normal 65.3 ms,
  grouped vs scalar 3.4e-15.
- `recorded_200k_seasoned_with_baseline.txt`: 200k with the bump-and-reprice baseline, 14,913 ms vs 21.7 ms streaming (688x).

The paper's §9 headline (unseasoned bonds + vanilla swaps, 50.9 ms) is `../benchmark_paper`; this example is the library
form on a different book and is reported separately.
