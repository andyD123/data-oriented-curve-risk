#!/usr/bin/env python3
"""Rewrite the curve blocks of rfr_curves.txt from the illustrative SOFR-style par OIS quotes.

Pure standard library. The node times (30 + 36 forward buckets) are kept so the benchmark bucket structure
is unchanged; the discount factors come from the bootstrap in examples/rfr_reconcile/rfr_reference.py
(annual par OIS, log-linear discount factors, flat forward beyond 30y). In an RFR market there is one
curve, so the "proj" block carries the same curve on its own node grid. The trailing instrument examples
in the file are preserved. Quotes are illustrative, not a market snapshot.

Usage: python3 make_rfr_curves.py [path/to/rfr_curves.txt]
"""
import importlib.util
import sys
from pathlib import Path

here = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("rfr_reference", here.parent / "rfr_reconcile" / "rfr_reference.py")
ref = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ref)

path = Path(sys.argv[1]) if len(sys.argv) > 1 else here / "rfr_curves.txt"
tok = path.read_text().split()
curve = ref.bootstrap(ref.QUOTES)

pos, nc, blocks = 1, int(tok[0]), []
for _ in range(nc):
    name, n = tok[pos], int(tok[pos + 1])
    times = [float(x) for x in tok[pos + 2:pos + 2 + 2 * n:2]]
    blocks.append((name, times))
    pos += 2 + 2 * n
tail = " ".join(tok[pos:])

lines = [str(nc)]
for name, times in blocks:
    lines.append(f"{name} {len(times)} " + " ".join(f"{t:.17g} {curve.df(t):.17g}" for t in times))
lines.append(tail)
path.write_text("\n".join(lines) + "\n")
print(f"wrote {path} ({nc} curves, SOFR-style)")
