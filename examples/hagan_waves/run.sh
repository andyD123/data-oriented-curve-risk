#!/bin/sh
# Build (CMake if present, else make), generate the QuantLib reference on the shipped cubic curves,
# run the scan, compare against QuantLib's wave bumps, solve the hedge.
set -e
if command -v cmake >/dev/null 2>&1; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null && cmake --build build >/dev/null
  QW=./build/ql_waves; SW=./build/scan_wave
else
  make -s; QW=./ql_waves; SW=./scan_wave
fi
$QW && $SW && python3 compare.py && python3 aad_check.py && python3 hagan_hedge.py
