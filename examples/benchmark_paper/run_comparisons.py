#!/usr/bin/env python3
"""Run the article's existing C++ performance comparisons, with correctness gates.

The examples remain separate experiments: their runtimes are not one additive
or controlled multi-row speedup ladder. Requires x86 AVX-512, CMake 3.16+, a
C++20 GCC/Clang toolchain with <experimental/simd>, and Python 3.9+.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TARGETS = {
    "paper": "bench_paper",
    "cache": "scenario_bench",
    "adjoint": "adjoint_bench",
    "aggregation": "aggregation",
    "write-floor": "write_floor",
}
DEFAULT_SUITES = ("paper", "cache", "adjoint", "aggregation")
PAPER_PADDED_BYTES_500K = 270479616


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and run the Wilmott paper's distinct, correctness-checked comparisons."
    )
    parser.add_argument(
        "--suite", choices=(*TARGETS, "all"), default="paper",
        help="comparison to run; 'all' runs paper, cache, adjoint, and aggregation (not the write-only reference)",
    )
    parser.add_argument("--full", action="store_true", help="500,000 instruments, three repetitions (slow and memory-heavy)")
    parser.add_argument("--instruments", type=int, help="override instrument count (default 2,000, or 500,000 with --full)")
    parser.add_argument("--repetitions", type=int, help="override repetition count (default 1, or 3 with --full)")
    parser.add_argument("--skip-repricing-baseline", action="store_true", help="skip only the slow per-scenario BASE in bench_paper")
    parser.add_argument("--build-dir", type=Path, default=Path("build/paper-comparisons"))
    parser.add_argument("--no-build", action="store_true", help="use already compiled Release targets")
    parser.add_argument("--output-dir", type=Path, help="where to save stdout logs and manifest.json (default <build-dir>/reader-results)")
    parser.add_argument("--dry-run", action="store_true", help="print intended commands without building or running")
    return parser.parse_args()


def command_line(command: list[str]) -> str:
    # Human-readable only; subprocess is always invoked with an argument list.
    import shlex
    return " ".join(shlex.quote(str(part)) for part in command)


def run_process(command: list[str], *, env: dict[str, str] | None = None,
                log: Path | None = None, dry_run: bool = False) -> str:
    print("$", command_line(command), flush=True)
    if dry_run:
        return ""
    result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            errors="replace", check=False)
    output = result.stdout or ""
    print(output, end="" if output.endswith("\n") else "\n", flush=True)
    if log is not None:
        log.write_text(output, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"{command[0]} exited with code {result.returncode}; see output above")
    return output


def find_binary(build: Path, target: str) -> Path:
    ext = ".exe" if os.name == "nt" else ""
    folder = "aggregation" if target == "aggregation" else "benchmark_paper"
    parent = build / "examples" / folder
    candidates = [parent / (target + ext), parent / "Release" / (target + ext)]
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError(f"missing target {target}: looked for {', '.join(map(str, candidates))}. "
                       "Build the Release targets first or omit --no-build.")


def git_head() -> str | None:
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
                                capture_output=True, text=True, check=False)
        return result.stdout.strip() if result.returncode == 0 else None
    except OSError:
        return None


def main() -> int:
    args = arguments()
    size = args.instruments if args.instruments is not None else (500000 if args.full else 2000)
    reps = args.repetitions if args.repetitions is not None else (3 if args.full else 1)
    if not 1 <= size <= 10000000:
        raise ValueError("instruments must be from 1 to 10,000,000")
    if not 1 <= reps <= 100:
        raise ValueError("repetitions must be from 1 to 100")
    suites = DEFAULT_SUITES if args.suite == "all" else (args.suite,)
    if "write-floor" in suites and size != 500000:
        raise ValueError("write-floor measures the recorded 500,000-trade padded output (270,479,616 bytes); "
                         "use --full or --instruments 500000, and do not equate it with a copy benchmark")
    build = args.build_dir if args.build_dir.is_absolute() else ROOT / args.build_dir
    results_dir = args.output_dir or (build / "reader-results")
    if not results_dir.is_absolute():
        results_dir = ROOT / results_dir
    print("Reader comparisons: independent experiments, not one interchangeable timing ladder.")
    print(f"Book size: {size:,}; repetitions: {reps}; suite(s): {', '.join(suites)}")
    print("The C++ programs themselves check their numerical outputs and return nonzero on failure.")
    print("Historical 50.9 ms figures use a different risk coordinate; current-source measurements replace neither their hardware nor their provenance.\n")

    targets = sorted(set(TARGETS[name] for name in suites))
    if not args.no_build:
        run_process(["cmake", "-S", str(ROOT), "-B", str(build),
                     "-DCMAKE_BUILD_TYPE=Release", "-DLADDER_BUILD_EXAMPLES=ON",
                     "-DLADDER_BUILD_PAPER_BENCHMARKS=ON", "-DLADDER_LANES=AVX512"],
                    dry_run=args.dry_run)
        run_process(["cmake", "--build", str(build), "--config", "Release",
                     "--target", *targets, "--parallel", "2"], dry_run=args.dry_run)
    if args.dry_run:
        for name in suites:
            print(f"# {name} -> target {TARGETS[name]}")
        return 0
    results_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "git_commit": git_head(),
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "build_directory": str(build),
        "instruments": size,
        "repetitions": reps,
        "suite": list(suites),
        "comparison_scopes_are_separate": True,
        "historical_50_9ms_source_is_theta_not_current_rate_coordinate": True,
        "runs": [],
    }
    try:
        for name in suites:
            binary = find_binary(build, TARGETS[name])
            env = dict(os.environ)
            if name == "paper":
                command = [str(binary), str(size),
                           "0" if args.skip_repricing_baseline else "1", str(reps)]
            elif name == "cache":
                # Compare random/sorted/grouped on the same book, with identical 64-column caches.
                env.update(LRU_CAPS="64", LRU_ALL="1")
                command = [str(binary), str(size), "0", str(reps)]
            elif name == "adjoint":
                command = [str(binary), str(size), str(reps)]
            elif name == "aggregation":
                command = [str(binary), str(size)]
            else:
                command = [str(binary), str(PAPER_PADDED_BYTES_500K), str(reps)]
            print(f"\n=== {name}: {TARGETS[name]} ===", flush=True)
            output = run_process(command, env=env,
                                 log=results_dir / f"{name}.txt")
            if name != "write-floor" and not re.search(r"correctness gate:\s*PASS\b", output):
                raise RuntimeError(f"{name} did not report a passing numerical correctness gate")
            manifest["runs"].append({
                "suite": name, "command": command, "exit_code": 0,
                "correctness_gate": "not applicable" if name == "write-floor" else "PASS",
                "log": f"{name}.txt",
                "environment_overrides": {"LRU_CAPS": "64", "LRU_ALL": "1"} if name == "cache" else {},
            })
    finally:
        (results_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"\nAll selected comparisons passed. Raw logs and provenance: {results_dir}")
    print("Report each method with its own workload, timed region, machine and coordinate convention.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"comparison runner: {exc}", file=sys.stderr)
        raise SystemExit(1)
