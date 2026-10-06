# Reading the examples

Start with the four small examples in [`progression/`](progression/README.md).
They are deliberately a progression; each adds only one idea:

1. [`01_present_value.cpp`](progression/01_present_value.cpp) — one fixed-cashflow PV.
2. [`02_shared_dates.cpp`](progression/02_shared_dates.cpp) — several instruments reuse the same date/discount data.
3. [`03_risk_row.cpp`](progression/03_risk_row.cpp) — one ordered cashflow traversal produces the whole bucketed risk row.
4. [`04_wide_risk_row.cpp`](progression/04_wide_risk_row.cpp) — many instruments sharing a schedule form one logical row; SIMD width is only an execution detail.

Read those first. The benchmark programs below are evidence and tuning tools, not the tutorial.

## Repeated scenario valuation and the date cache

1. [`benchmark_paper/scenario_bench.cpp`](benchmark_paper/scenario_bench.cpp):
   prepare the book, refresh the full table, obtain the scan reference, compare
   processing orders, then report the results.
2. [`scenario_pricing.hpp`](benchmark_paper/scenario_pricing.hpp):
   `fixed_cashflow_pv` and `floating_coupon_pv` name the financial expressions.
   `price_scenarios` uses the same accumulation for full-table and cached columns.
   The two paths supply different column-lookup lambdas, not different pricing code.
3. [`scenario_book.hpp`](benchmark_paper/scenario_book.hpp) constructs the synthetic
   schedules and the three processing orders. It preserves this example's original
   random draw order and day rounding; these are not QuantLib-generated calendars.
4. [`scenario_cache.hpp`](benchmark_paper/scenario_cache.hpp) contains only the LRU
   bookkeeping. Pricing sees `fetch(date, fill_column)`.
5. [`scenario_report.hpp`](benchmark_paper/scenario_report.hpp) contains the console
   report and the numerical comparison gate.

A floating coupon keeps the start and end projection columns live simultaneously.
The cache therefore requires at least **two** slots. `LRU_CAPS=1` is rejected; it used
to let the second fetch overwrite the first column. The 64-column example is unchanged.
The scenario columns are contiguous `std::vector<double>` storage; this code does not
claim that the allocator provides 64-byte alignment.

## Scan, direct overlap and recorded reverse mode

[`benchmark_paper/adjoint_bench.cpp`](benchmark_paper/adjoint_bench.cpp) names each
operation separately: `make_risk_records`, `scan_gradient`, `direct_gradient`,
`calculate_ladders` and `print_results`. The recorded prototype is isolated in
[`recorded_adjoint.hpp`](benchmark_paper/recorded_adjoint.hpp), so it does not obscure
the experiment driver.

Record preparation and the large tape reservations stay outside the timing. The
per-instrument leaf allocation, tape recording and reverse traversal remain inside.
The scan calls the **existing library implementation**, including its current bucket
lookup cost. This edit does not relabel it as an unqualified linear implementation.
All outputs here are first derivatives per unit forward-rate shift.

## Risk aggregation

[`aggregation/agg.cpp`](aggregation/agg.cpp) separates `aggregate_contiguous`,
`aggregate_maps` and `aggregate_hierarchy`. The two map variants share the same
operation; only the container type changes. `add_ladder` expresses an elementwise sum,
not a reduction across buckets.

The default remains 100,000 instruments and 66 values each. The default hierarchy
contains 200 books of 500 instruments, then 20 desks of 10 books. An optional smaller
instrument count is useful for tests. Clearing and rebuilding the result containers
remains inside the timed operations.

## What this pass does not change

`ladder/`, recorded `.txt` and `.svg` evidence, `bench_paper.cpp`, the seasoned library
benchmark and the QuantLib harnesses are unchanged. Their further presentation work
is not claimed complete. The historical headline executable still has its own
integrated-forward units; this readability edit does not resolve that separate issue.

No new benchmark framework or runtime type-erased callback layer is introduced.
The support functions and expression lambdas are ordinary C++ templates.
