#!/usr/bin/env python3
"""Reproduce the paper's quote-risk composition check."""
from __future__ import annotations
import argparse, math, subprocess
from pathlib import Path
def run(executable):
    result=subprocess.run([str(executable)],capture_output=True,text=True,timeout=300)
    print(result.stdout,end="")
    if result.returncode: raise RuntimeError(result.stderr)
def read_scan(path):
    lines=[line.split() for line in path.read_text().splitlines() if line.strip()]
    if len(lines)%3: raise ValueError("scan_risk.txt must contain three lines per instrument")
    ladders=[]; kd=kp=0
    for row in range(0,len(lines),3):
        d=[float(v) for v in lines[row+1]]; p=[float(v) for v in lines[row+2]]
        kd,kp=len(d),len(p); ladders.append(d+p)
    if len(ladders)!=7: raise ValueError(f"quote-risk check expects 7 instruments, got {len(ladders)}")
    return [math.fsum(r[c] for r in ladders) for c in range(kd+kp)],kd,kp
def read_quote(path):
    rows=[]
    for line in path.read_text().splitlines():
        if not line.strip(): continue
        f=line.split(); rows.append((f[0],int(f[1]),float(f[2]),[float(v) for v in f[3:]]))
    return rows
def richardson(a,b): return (4*b-a)/3
def check(directory,max_relative,expected_cross):
    book,kd,kp=read_scan(directory/"scan_risk.txt")
    coarse=read_quote(directory/"quote_risk_1e-06.txt"); fine=read_quote(directory/"quote_risk_5e-07.txt")
    largest_error=largest_direct=0.0; cross=0; worst=None
    for a,b in zip(coarse,fine):
        if a[:2]!=b[:2]: raise ValueError("quote-risk row identities differ")
        direct=richardson(a[2],b[2]); jac=[richardson(x,y) for x,y in zip(a[3],b[3])]
        if len(jac)!=kd+kp: raise ValueError("Jacobian width mismatch")
        predicted=math.fsum(j*g for j,g in zip(jac,book)); error=abs(predicted-direct)
        if error>largest_error: largest_error=error; worst=(a[0],a[1],predicted,direct)
        largest_direct=max(largest_direct,abs(direct))
        if a[0]=="eonia": cross += sum(abs(v)>1e-6 for v in jac[kd:])
    relative=largest_error/max(largest_direct,1.0)
    print(f"quote-risk rows: {len(coarse)}")
    print(f"cross block Eonia quote -> Euribor forward: {cross}")
    print(f"max absolute error: {largest_error:.6g}")
    print(f"max error / largest sensitivity: {relative:.3e}")
    if worst: print(f"worst row: {worst}")
    if cross!=expected_cross: raise RuntimeError(f"expected {expected_cross} cross-block entries, got {cross}")
    if not math.isfinite(relative) or relative>max_relative:
        raise RuntimeError(f"quote-risk relative error {relative:.3e} exceeds {max_relative:.3e}")
    print("quote-risk correctness gate: PASS")
def main():
    p=argparse.ArgumentParser(); p.add_argument("--directory",type=Path,default=Path("."))
    p.add_argument("--generate",nargs=2,type=Path); p.add_argument("--max-relative",type=float,default=1e-10)
    p.add_argument("--expected-cross",type=int,default=289); a=p.parse_args()
    if a.generate:
        for e in a.generate: run(e.resolve())
    check(a.directory.resolve(),a.max_relative,a.expected_cross); return 0
if __name__=="__main__": raise SystemExit(main())
