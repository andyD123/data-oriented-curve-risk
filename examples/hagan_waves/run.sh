#!/bin/sh
# Build (CMake if present, else make), run the scan, compare against reference wave bumps, solve the hedge.
set -e
if command -v cmake >/dev/null 2>&1; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build >/dev/null
  SW=./build/scan_wave
else
  make -s; SW=./scan_wave
fi
$SW && python3 compare.py && python3 aad_check.py && python3 hagan_hedge.py

