# A simple progression

These examples are the recommended starting point. Each adds one idea and keeps
benchmarking, reporting and validation out of the main calculation.

1. **`01_present_value.cpp` — one PV**  
   Read cash flows, obtain discount factors and add `cashflow * discount`.

2. **`02_shared_dates.cpp` — reuse shared dates**  
   Several instruments use the same schedule, so discount factors are calculated
   once and reused across the row.

3. **`03_risk_row.cpp` — one traversal, a whole risk row**  
   The discounted cash flows become the inputs to the bucket-and-tail scan.
   One ordered traversal produces all bucketed discount risks.

4. **`04_wide_risk_row.cpp` — widen the schedule row**  
   A logical group may contain many instruments even though the hardware SIMD
   primitive is only eight doubles wide. Date and bucket control are therefore
   amortised over many SIMD blocks.

The progression is deliberately separate from the benchmark programs. Once the
four examples are clear, use `../READING_GUIDE.md` for the performance and
validation demonstrations.

The financial idea should remain visible in each file. Implementation machinery
belongs in `common.hpp` or the library, not in the tutorial driver.
