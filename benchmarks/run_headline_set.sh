#!/usr/bin/env bash
# Unpinned headline set for the paper (500k seasoned book, one thread, best-of-N inside each run).
# Usage: benchmarks/run_headline_set.sh <abs avx2 build dir> <abs portable build dir> <abs out dir> [abs curve file]
# Both build dirs need bench_library; the avx2 dir also needs write_floor, adjoint_bench, scenario_bench and
# aggregation. Programs that take no curve argument read rfr_curves.txt from their working directory, so put the
# curve you want there first (copy it into the build dir, never over the committed file).
# Run on an idle machine. Writes raw program output only.
set -euo pipefail
A=${1:?avx2 build dir}; P=${2:?portable build dir}; OUT=${3:?output dir}; CURVE=${4:-}
mkdir -p "$OUT"
cd "$A/examples/benchmark"
./bench_library 500000 1 3 $CURVE > "$OUT/bench_library_avx2_500k.txt" 2>&1
( cd "$P/examples/benchmark" && ./bench_library 500000 1 3 $CURVE > "$OUT/bench_library_portable_500k.txt" 2>&1 )
: > "$OUT/working_set_sweep_avx2.txt"
for n in 10000 30000 100000 300000 500000; do
  echo "== N=$n" >> "$OUT/working_set_sweep_avx2.txt"
  ./bench_library $n 0 5 $CURVE 2>&1 | grep -E "^book:|^  date table|^  GRP   date-table|^  GRP   kernel, streaming|unique payment dates|lane occupancy" >> "$OUT/working_set_sweep_avx2.txt"
done
cd "$A/examples/benchmark_paper"
for i in 1 2 3; do ./write_floor 273504000 15; done > "$OUT/write_floor.txt" 2>&1
./adjoint_bench 500000 2 > "$OUT/adjoint_bench_500k.txt" 2>&1
LRU_CAPS=16,32,64,128 ./scenario_bench 100000 0 3 > "$OUT/scenario_bench_100k.txt" 2>&1
( cd "$A/examples/aggregation" && ./aggregation 100000 > "$OUT/aggregation_100k.txt" 2>&1 )
echo done > "$OUT/DONE"
