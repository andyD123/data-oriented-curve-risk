#!/usr/bin/env bash
# Measurement campaign for the 500k seasoned book (default: the committed placeholder curve; pass a curve file to use another, e.g. docs/BOE_CURVE.md). Pins single-thread runs to one logical CPU.
# Usage: benchmarks/run_pinned_campaign.sh <absolute build-dir> <absolute out-dir> [cpu=2] [absolute curve file]
# Writes raw program output only; summarise with benchmarks/summarise_campaign.py.
set -euo pipefail
B=${1:?build dir}; OUT=${2:?output dir}; CPU=${3:-2}; CURVE=${4:-}
mkdir -p "$OUT"; ALL="0-$(( $(nproc) - 1 ))"
lscpu > "$OUT/lscpu.txt"; uname -a > "$OUT/uname.txt"
cd "$B/examples/benchmark"
# 1. full runs with the bump-and-reprice ladder (BASE, SOA, TABLE, SCAN, GRP), 3 repetitions inside each run
for i in 1 2 3; do taskset -c "$CPU" ./bench_library 500000 1 3 $CURVE > "$OUT/full_$i.txt" 2>&1; done
# 2. fifteen independent process runs of SCAN and GRP (no baseline), one repetition each
for i in $(seq 1 15); do taskset -c "$CPU" ./bench_library 500000 0 1 $CURVE > "$OUT/grp_$i.txt" 2>&1; done
# 3. thread scaling over groups (all logical CPUs allowed), best of 15
LADDER_THREADS=1,2,4,6,8,12 taskset -c "$ALL" ./bench_library 500000 0 15 $CURVE > "$OUT/threads.txt" 2>&1
# 4. worst case for occupancy: one instrument per group
LADDER_UNIQUE_SIG=1 taskset -c "$CPU" ./bench_library 500000 0 5 $CURVE > "$OUT/one_per_group.txt" 2>&1
# 4b. incremental insert of 20,000 instruments into a copy of the layout (vs full rebuild)
LADDER_INSERT=20000 taskset -c "$CPU" ./bench_library 500000 0 1 $CURVE > "$OUT/incremental_insert_20000.txt" 2>&1
# 5. write-only and copy references, pinned, three runs each
cd "$B/examples/benchmark_paper"
for i in 1 2 3; do taskset -c "$CPU" ./write_floor 273504000 15; done > "$OUT/write_floor.txt" 2>&1
for i in 1 2 3; do taskset -c "$CPU" ./copy_floor 273504000 15; done > "$OUT/copy_floor.txt" 2>&1
echo done > "$OUT/DONE"
