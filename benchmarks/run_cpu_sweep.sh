#!/usr/bin/env bash
# Per-CPU variation: for each logical CPU, the kernel (best of 7), the write-only reference and a memcpy (best of 15),
# all pinned to that CPU. Usage: benchmarks/run_cpu_sweep.sh <abs build dir> <abs out dir> [abs curve file] [cpus...]
set -euo pipefail
B=${1:?build dir}; OUT=${2:?out dir}; CURVE=${3:-}; shift 3 || true
CPUS=${*:-0 2 4 6 8 10}
mkdir -p "$OUT"
for c in $CPUS; do
  ( cd "$B/examples/benchmark" && taskset -c "$c" ./bench_library 500000 0 7 $CURVE > "$OUT/cpu${c}_kernel.txt" 2>&1 )
  ( cd "$B/examples/benchmark_paper" && taskset -c "$c" ./write_floor 273504000 15 > "$OUT/cpu${c}_write.txt" 2>&1; taskset -c "$c" ./copy_floor 273504000 15 > "$OUT/cpu${c}_copy.txt" 2>&1 )
done
echo done > "$OUT/DONE"
