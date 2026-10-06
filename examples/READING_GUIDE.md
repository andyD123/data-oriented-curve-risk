# Reading the examples

Start with [`progression/`](progression/README.md). It follows the same causal sequence as
the original scenario-cache prototype, but each step is now a small standalone example:

1. ordinary PV;
2. scenario-vector curve — one lookup returns a whole row;
3. sorting/grouping — instruments sharing dates reuse those rows;
4. bucket-and-tail scan — remove repeated scenarios and produce the whole risk row;
5. wide schedule row — amortise the date/bucket walk over many SIMD blocks.

The old prototype stated this progression directly as BASE -> scenario vector -> sorted/grouped
reuse -> LRU cache -> SCAN. That teaching order was good; the new files preserve it without
making the reader wade through benchmark machinery first.

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
