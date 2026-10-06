# A simple progression

Read these in order. Each example adds one idea and stays deliberately small.

1. **`01_present_value.cpp` — ordinary pricing**  
   Cash flows, discount factors, one PV.

2. **`02_vector_curve.cpp` — one lookup, many scenarios**  
   A date lookup returns a contiguous row of discount factors. The pricing loop is
   still the same cashflow accumulation, but it now advances several scenarios together.

3. **`03_grouped_schedules.cpp` — keep shared rows hot**  
   Instruments on the same schedule reuse the same date/scenario rows. This is the
   reason for sorting and grouping: reuse the data you already loaded.

4. **`04_risk_row.cpp` — remove the scenarios**  
   For bucketed forward risk, the local bucket contribution plus the reusable tail
   lets one ordered cashflow traversal produce the whole risk row.

5. **`05_wide_risk_row.cpp` — widen the instrument row**  
   If many instruments share a schedule, the date/bucket walk is shared across many
   SIMD blocks. The financial group width is no longer tied to one AVX register.

That is the same progression as the original experiment:

```text
ordinary repricing
    -> scenario-vector curve
    -> sorting/grouping for reuse
    -> bucket + tail scan
    -> wide schedule-row scan
```

Benchmarking, command-line parsing and validation are intentionally not in these files.
They live in the benchmark programs after the idea has been introduced.
