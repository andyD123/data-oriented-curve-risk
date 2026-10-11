# Grouped risk: data flow and API ownership

The grouped implementation separates **financial schedules**, **curve
evaluation** and **risk calculation**. The starting point is
[`layout.hpp`](../ladder/layout.hpp); the scan is in
[`scan_simd.hpp`](../ladder/scan_simd.hpp); portfolio-only risk is in
[`scan_grouped_reduce.hpp`](../ladder/scan_grouped_reduce.hpp).
For the financial meaning of a box wave, start with the
[minimal first-order curve example](../examples/curve_intro/README.md).

## One book and one shared date table

```text
InstrumentSpec[]                 signed fixed / IBOR / OIS coupon inputs
     |
     | build_layout(book, days_per_year)
     v
GroupLayout: eight lanes per group, per-group date columns
     + DateTable: unique integer days shared across the book
     |
     | refresh_table(layout, Sd, Sp, df, pf)
     v
DateTable: time, discount D(t), projection P(t), wave buckets
     |
     +-- scan_grouped(...)              -> padded per-instrument AoSoA risk
     +-- scan_grouped_reduce(...)       -> Kd+Kp portfolio totals
     +-- scan_grouped_gamma_diagonal(..)-> discounted gamma diagonal
                                  |
                                  +-- lazy cross-gamma view and H*v
```

`InstrumentSpec.signature` describes a *grouping key*, such as coupon
conventions and the last regular payment date. It is **not** a curve ID.
Members with the same signature are packed into groups of eight
double-precision lanes. Correctness does not require identical
payment dates: `build_layout` unions the dates of members and uses a
zero amount where one lane has no cashflow. Only
`Group.n_valid` lanes correspond to real instruments.

`GroupLayout` holds the groups and a `DateTable`, whose ascending
`day` keys are converted to year fractions `t` by
`days_per_year`. `refresh_table` evaluates the discount/projection
factors at those shared times, fills `D` and `P`, and caches the
discount/projection wave buckets `bo` and `bp`. Call it after changing
curve **values or bucket boundaries**. A layout is reusable only
while the instruments' schedules and day-count-to-time conversion
remain unchanged.

## Field names and scan invariants

| Field | Meaning |
|---|---|
| `col_di`, `col_amt` | Group's sorted payment-date table indices and eight fixed-leg amounts per column |
| `f_di`, `f_col`, `f_scale` | IBOR payment/start/end table indices, its payment column and eight coupon scales |
| `o_di`, `o_at`, `o_N` | Overnight payment/start/end table indices, sorted (column, encoded coupon term), and eight coupon weights |
| `pacc` | SIMD projection-risk accumulator for one wave |
| `running` | Running signed discounted cashflow mass in the backward traversal |
| `interior` | Current bucket's local time-weighted cashflow moment |
| `suffix_cur` | Cashflow mass beyond the right end of the emitted wave |
| `beyond` | Uncapped moment beyond the last boundary, used for an open final wave |

For `o_at`, the encoded term kind is **0: payment**,
**1: accrual start**, **2: accrual end**. The per-group
first-order scan traverses columns backwards, emitting a bucket
when its right boundary has been crossed. The projection scan
accumulates only waves reached by each IBOR fixing interval.

**The per-trade scan and direct portfolio reducer now share the same
coupon-weight evaluator and both wave traversals**. Only the
result sink differs: store each eight-lane row or add that row to
the portfolio accumulator. This avoids maintaining two competing
copies of the numerical calculation.

## Output layout and checked buffers

The full risk output is a contiguous **array of structures of arrays**:

```text
group 0: [Kd discount waves][8 lanes each] [Kp projection waves][8 lanes each]
group 1: [Kd discount waves][8 lanes each] [Kp projection waves][8 lanes each]
...
```

Size is `groups * (Kd+Kp) * 8` doubles. The raw pointer kernel needs
a caller-owned **64-byte-aligned** output buffer. The checked
`std::span<double>` overload additionally validates its *exact*
length before invoking the same kernel:

```cpp
using namespace ladder;
GroupLayout L = build_layout(book, 365.0);
refresh_table(L, Sd, Sp, df, pf);
const size_t entries=L.groups.size()*L.stride(Sd.K(),Sp.K());
double* output=static_cast<double*>(allocate_aligned(entries*sizeof(double)));

scan_grouped<Store::normal>(
    L, Sd, Sp, std::span<double>(output,entries));

std::vector<double> portfolio(Sd.K()+Sp.K());
GroupReduceScratch scratch;
scan_grouped_reduce(L,Sd,Sp,std::span<double>(portfolio),scratch);

// Read only a real lane, not the zero-filled padding.
std::vector<double> one(Sd.K()+Sp.K());
gather(L,Sd.K(),Sp.K(),std::span<const double>(output,entries),
       0,0,std::span<double>(one));

free_aligned(output);
```

Include `ladder/aligned_memory.hpp` alongside
`ladder/scan_grouped_reduce.hpp` for this example. The checked
`gather` validates both buffer lengths and the live group/lane
index. A padded SIMD lane is **not** another instrument.

For a portfolio-only question, skip the full output allocation and
call `scan_grouped_reduce` directly: it returns only `Kd+Kp`
scalars. The optional `GroupReduceScratch` is reused across curve
snapshots, and must be owned by one caller/worker. The exact reduction
is sensitive to floating-point addition order, so it is compared
against the grouped/scalar oracles to tolerance, not promised to be
bitwise identical to a different aggregation order.

## Borrowed gamma views

`scan_grouped_gamma_diagonal` computes the **discount**
diagonal gamma only (IBOR projection held fixed). Its trade-level
output contains `groups * Kd * 8` doubles with the same 64-byte
alignment policy; a portfolio-only variant returns just `Kd` doubles.

`GroupedDiscountCrossGammaView` borrows **both stencils, the
layout and both materialised risk buffers**. Its checked constructor
accepts `std::span<const double>` inputs of the exact first-order
and discount-gamma shapes. Span sizes are validated; **memory is
not owned or copied**. The user must retain the buffers, layout
and curve snapshot until the view is no longer used and must not
refresh or overwrite them while the view is active.

Gamma view wave indices are **zero based**; `Stencils::overlap`
takes a **one-based** index. For a fully forecast single-curve
overnight coupon, the diagonal correction is already in the
gamma buffer and the additional off-diagonal payment-lag
correction is computed on demand. Do not apply it twice.
See [the grouped cross-gamma contract](GROUPED_CROSS_GAMMA_STAGE2.md).

Checked spans prevent accidental wrong-sized outputs but do not
guard against concurrently mutating buffers or passing a span
backed by an already-destroyed allocation. Use one
`GroupGammaScratch` per worker and do not share mutable scratch
between simultaneous calculations. This is a caller-owned
C++ library, not a pricing-engine/curve-market convention layer.
