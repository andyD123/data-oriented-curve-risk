#!/usr/bin/env bash
# Measurement campaign for the 500k seasoned book (default: the committed placeholder curve; pass a curve file to use another, e.g. docs/BOE_CURVE.md). Pins single-thread runs to one logical CPU.
# Usage: benchmarks/run_pinned_campaign.sh <absolute build-dir> <absolute out-dir> [cpu=2] [absolute curve file]
# Writes raw program output only; summarise with benchmarks/summarise_campaign.py.
# Does not overwrite benchmarks/results/. The committed threads.txt under
# benchmarks/results/i7-12700K_pinned/ is the old unpinned protocol (taskset on the whole
# chip, no LADDER_THREAD_CPUS). Leave that file alone.
set -euo pipefail
B=${1:?build dir}; OUT=${2:?output dir}; CPU=${3:-2}; CURVE=${4:-}
case "$OUT" in
  *benchmarks/results/i7-12700K*|*benchmarks/results/i7-12700K_pinned*)
    echo "refusing to write into a committed results directory: $OUT" >&2
    exit 1
    ;;
esac
mkdir -p "$OUT"; ALL="0-$(( $(nproc) - 1 ))"
lscpu > "$OUT/lscpu.txt"; uname -a > "$OUT/uname.txt"

# One hardware thread from each core, when the sibling lists are readable.
# Do not hard-code efficiency-core ids: this host may not expose core_type.
one_sibling_per_core() {
  local seen="" list first cpus=()
  local f
  for f in /sys/devices/system/cpu/cpu[0-9]*/topology/thread_siblings_list; do
    [[ -r "$f" ]] || continue
    list=$(tr -d ' \n' < "$f")
    [[ -n "$list" ]] || continue
    case " $seen " in
      *" $list "*) continue ;;
    esac
    seen+=" $list"
    first=${list%%,*}
    cpus+=("$first")
  done
  if ((${#cpus[@]})); then
    (IFS=,; echo "${cpus[*]}")
  fi
}

cd "$B/examples/benchmark"
# 1. full runs with the bump-and-reprice ladder (BASE, SOA, TABLE, SCAN, GRP), 3 repetitions inside each run
for i in 1 2 3; do taskset -c "$CPU" ./bench_library 500000 1 3 $CURVE > "$OUT/full_$i.txt" 2>&1; done
# 2. fifteen independent process runs of SCAN and GRP (no baseline), one repetition each
for i in $(seq 1 15); do taskset -c "$CPU" ./bench_library 500000 0 1 $CURVE > "$OUT/grp_$i.txt" 2>&1; done
# 3. thread scaling. Pin worker i to one sibling of core i when the topology is readable.
#    The committed benchmarks/results/i7-12700K_pinned/threads.txt did not do this.
PIN=$(one_sibling_per_core || true)
{
  echo "# committed i7-12700K_pinned/threads.txt is the old unpinned protocol (taskset -c the whole chip, no LADDER_THREAD_CPUS)"
  if [[ -n "${PIN:-}" ]]; then
    echo "# this run pins worker i to LADDER_THREAD_CPUS[i % n]: $PIN"
    echo "# the printed GB/s figure is output bytes, not a memory-controller counter"
    LADDER_THREADS=1,2,4,6,8,12 LADDER_THREAD_CPUS="$PIN" taskset -c "$ALL" ./bench_library 500000 0 15 $CURVE
  else
    echo "# thread_siblings_list unreadable; workers are not pinned inside the process"
    LADDER_THREADS=1,2,4,6,8,12 taskset -c "$ALL" ./bench_library 500000 0 15 $CURVE
  fi
} > "$OUT/threads.txt" 2>&1
# 3b. phase split. Not part of the published tables. Discount and projection each include amount formation.
LADDER_PHASES=1 taskset -c "$CPU" ./bench_library 500000 0 3 $CURVE > "$OUT/phases.txt" 2>&1
# 4. worst case for occupancy: one instrument per group
LADDER_UNIQUE_SIG=1 taskset -c "$CPU" ./bench_library 500000 0 5 $CURVE > "$OUT/one_per_group.txt" 2>&1
# 4b. incremental insert of 20,000 instruments into a copy of the layout (vs full rebuild)
LADDER_INSERT=20000 taskset -c "$CPU" ./bench_library 500000 0 1 $CURVE > "$OUT/incremental_insert_20000.txt" 2>&1
# 5. write-only and copy references, pinned, three runs each
cd "$B/examples/benchmark_paper"
for i in 1 2 3; do taskset -c "$CPU" ./write_floor 273504000 15; done > "$OUT/write_floor.txt" 2>&1
for i in 1 2 3; do taskset -c "$CPU" ./copy_floor 273504000 15; done > "$OUT/copy_floor.txt" 2>&1
# Optional. Uncore events usually need root. A failure must not fail the campaign.
# Set LADDER_PERF=1 to attempt it. The result is not a published bandwidth number.
if [[ "${LADDER_PERF:-}" == 1 ]] && command -v perf >/dev/null 2>&1; then
  perf stat -e cycles,instructions,unc_m_cas_count.rd,unc_m_cas_count.wr \
    -- taskset -c "$CPU" "$B/examples/benchmark/bench_library" 500000 0 1 $CURVE \
    > "$OUT/perf_stat.txt" 2>&1 || echo "perf stat failed (uncore events often need root); campaign continues" | tee -a "$OUT/perf_stat.txt"
fi
echo done > "$OUT/DONE"
