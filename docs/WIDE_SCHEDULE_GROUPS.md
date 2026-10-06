# Wide schedule-group experiment

This example tests a simple architectural question:

> If many instruments share the same schedule, should the logical group contain
> only one hardware SIMD vector, or should one schedule group contain many SIMD
> blocks and amortise the date/bucket walk across all of them?

The production grouped scan currently packs eight instruments into one group because
the AVX-512 implementation uses eight doubles per vector. That makes the financial
grouping width equal to the hardware register width. The experiment in
`examples/benchmark/wide_group_bench.cpp` separates those two concepts.

A logical schedule group may contain 8, 16, 32, ... instruments. The hardware
execution primitive remains eight doubles wide. For a logical group of 1,024
instruments, the kernel executes 128 eight-lane SIMD blocks inside one shared
date/bucket traversal.

## Why this matters

For a fixed cash flow at time `t`, both pricing and discount risk start from the
same discounted contribution

```text
x = cashflow * D(t)
```

The bucketed scan does slightly more work than a PV accumulation: it maintains the
running tail and the local bucket moment. Conceptually:

```cpp
for (date : shared_schedule) {
    D = broadcast(discount[date]);
    w = broadcast(local_bucket_weight);

    for (block : instrument_blocks) {
        x[block]        = amount[block] * D;
        running[block] += x[block];
        interior[block] = fma(x[block], w, interior[block]);
    }
}
```

The whole risk row is produced during this traversal. It does not reprice once per
bucket.

With an eight-instrument logical group, the scalar work around the vector arithmetic
(date traversal, bucket tests, boundary transitions, discount lookup and weight
selection) is paid once per SIMD register. With a wider logical group it is paid once
per schedule row and then reused by many SIMD blocks.

This is the same data-oriented idea used elsewhere in the project: make shared
structure explicit, then organise the hot loop around it.

## Measured experiment

The controlled workload used:

- 65,536 fixed-cashflow instruments;
- one common schedule;
- 60 cash flows per instrument;
- 40 discount-risk buckets;
- 2,621,440 risk outputs;
- single-threaded execution;
- nine timing repetitions;
- every wider-group output reconciled against the width-8 result.

The hardware SIMD width remained eight doubles for every row below.

### AVX-512 run

On the first GitHub-hosted runner that exposed AVX-512:

| Logical schedule-group width | Streaming time | Speedup vs width 8 | ns/output |
| ---: | ---: | ---: | ---: |
| 8 | 5.724 ms | 1.00x | 2.184 |
| 16 | 4.471 ms | 1.28x | 1.706 |
| 32 | 3.598 ms | 1.59x | 1.372 |
| 256 | 3.414 ms | 1.68x | 1.302 |
| 512 | 3.351 ms | 1.71x | 1.278 |
| 1,024 | 3.258 ms | 1.76x | 1.243 |
| 2,048 | 2.675 ms | 2.14x | 1.021 |

The 2,048-instrument row was about 2.14x faster than the existing one-vector-per-group
shape on that run.

### AVX2 runs

The first AVX2 run reached 2.026 ms at width 2,048, about 2.84x faster than its
width-8 baseline.

A later, noisier hosted run swept the logical width all the way to 65,536. It again
showed a broad benefit from wider rows, with the best observed result around the
hundreds-to-low-thousands range rather than at the largest possible group:

| Width | Streaming time | Speedup vs width 8 |
| ---: | ---: | ---: |
| 8 | 7.531 ms | 1.00x |
| 256 | 4.488 ms | 1.68x |
| 512 | 3.734 ms | 2.02x |
| 1,024 | 3.200 ms | 2.35x |
| 2,048 | 3.314 ms | 2.27x |
| 4,096 | 3.497 ms | 2.15x |
| 8,192 | 4.027 ms | 1.87x |
| 65,536 | 4.477 ms | 1.68x |

The non-monotonic hosted timings are a reason to treat this as an architectural
experiment, not a final tuning result. They nevertheless show that the current
eight-instrument grouping is not generally the best execution shape.

## Interpretation

The experiment suggests that a schedule group should represent a financial/data
relationship, not a CPU register width.

A more natural structure is:

```text
ScheduleGroup
    shared dates
    shared curve/date indices
    shared bucket geometry

    block 0: instruments   0..7
    block 1: instruments   8..15
    block 2: instruments  16..23
    ...
```

A production implementation would probably tile very large schedule groups into
cache-friendly physical blocks instead of making them arbitrarily wide. The current
measurements suggest testing tiles in the hundreds-to-low-thousands range.

## Relation to "risk at write speed"

The discount-risk row has the form

```text
cost ~= one ordered cash-flow traversal
        + small bucket/tail bookkeeping
        + writing the risk row
```

rather than

```text
cost ~= number_of_buckets * pricing_cost
```

As the traversal and control overhead shrink, writing the output becomes a larger
fraction of total work. This is why streaming stores matter and why the limiting
behaviour increasingly resembles an output-bandwidth problem.

That does **not** mean a full risk row is literally the same cost as one PV: a risk
calculation writes many more numbers and maintains extra accumulators. The useful
claim is that the whole row can be the same order of computational cost as a valuation,
with the unavoidable difference increasingly dominated by how much result data must
be written.

## Scope and limitations

This experiment deliberately isolates the hypothesis:

- fixed cash flows only;
- discount ladder only;
- no IBOR projection terms;
- no OIS terms;
- no change to the production `scan_grouped` path;
- no claim that these hosted-run timings generalise to all CPUs or portfolios.

The next step is to generalise the variable-width schedule-group representation to
the full discount + projection grouped scan and benchmark it on the paper portfolio.

## Running it

Configure an explicit backend and build the experiment:

```sh
cmake -S . -B build-avx2 -DCMAKE_BUILD_TYPE=Release \
  -DLADDER_LANES=AVX2 -DLADDER_BUILD_QUANTLIB=OFF \
  -DLADDER_BUILD_WIDE_GROUP_EXPERIMENT=ON
cmake --build build-avx2 --target wide_group_bench --parallel 2
./build-avx2/examples/benchmark/wide_group_bench 65536 9
```

Use `LADDER_LANES=AVX512` only on a machine that supports AVX-512.

The implementation is in:

- `ladder/scan_wide_experiment.hpp`
- `examples/benchmark/wide_group_bench.cpp`

The example is opt-in and does not alter the production `scan_grouped` implementation.
