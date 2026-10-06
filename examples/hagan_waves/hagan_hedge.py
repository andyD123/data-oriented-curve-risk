#!/usr/bin/env python3
"""Hagan's maturity-aligned hedge solve on a named instrument population.

The default paper7 population is the seven-instrument book used for the hedge
reported in the paper. extended8 adds the 35-year bond used only to test the
open final wave. The hedge matrix is upper-triangular after transposition, so the
solve below is explicit back-substitution rather than a generic dense solve.
"""
from __future__ import annotations
import argparse
from pathlib import Path
import numpy as np
BP = 1e-4
BOUNDARY_TEST_BOND = "bond_35y_beyond_last_pillar"

def instrument_names(path: Path) -> list[str]:
    tokens = iter(path.read_text().split())
    count = int(next(tokens)); names = []
    for _ in range(count):
        name = next(tokens); nd = int(next(tokens)); npj = int(next(tokens))
        names.append(name)
        for _ in range(nd): next(tokens); next(tokens)
        for _ in range(npj):
            for _ in range(4): next(tokens)
    if next(tokens, None) is not None: raise ValueError("unexpected trailing data")
    return names

def selected_rows(names, population):
    if population == "extended8": return list(range(len(names)))
    if population == "paper7":
        rows = [i for i,n in enumerate(names) if n != BOUNDARY_TEST_BOND]
        if len(rows) != 7: raise ValueError(f"paper7 expected 7 instruments, found {len(rows)}")
        return rows
    raise ValueError(f"unknown population: {population}")

def snap(B, years): return [B[np.argmin(np.abs(B-y))] for y in years]
def waves(T):
    e=[0.0]+list(T)
    return [(e[j-1], e[j] if j < len(e)-1 else np.inf) for j in range(1,len(e))]
def mask(B, lo, hi):
    return np.array([(B[k] >= lo-1e-9) and (B[k+1] <= hi+1e-9) for k in range(len(B)-1)])
def aggregate(v, BO, BPj, wO, wP):
    Ko=len(BO)-1
    return np.array([v[:Ko][mask(BO,lo,hi)].sum() for lo,hi in wO] +
                    [v[Ko:][mask(BPj,lo,hi)].sum() for lo,hi in wP])

def back_substitute(upper, rhs):
    scale=max(1.0,float(np.max(np.abs(upper))))
    if not np.allclose(np.tril(upper,-1),0.0,atol=1e-12*scale,rtol=0.0):
        raise ValueError("hedge system is not upper-triangular")
    x=np.zeros_like(rhs,dtype=float)
    for row in range(len(rhs)-1,-1,-1):
        d=upper[row,row]
        if abs(d) <= 1e-14*scale: raise ValueError(f"singular hedge matrix at row {row}")
        x[row]=(rhs[row]-np.dot(upper[row,row+1:],x[row+1:]))/d
    return x

def run(directory: Path, population: str):
    names=instrument_names(directory/"unit_cashflows_wave.txt")
    selected=selected_rows(names,population)
    S=np.loadtxt(directory/"scan_wave_risk.txt")
    if S.ndim==1: S=S[None,:]
    if S.shape[0] != len(names): raise ValueError("risk matrix row count does not match names")
    book=S[selected].sum(axis=0)
    rows=[l.split() for l in (directory/"wave_hedges.txt").read_text().splitlines() if l.strip()]
    hedge_names=[r[0]+" "+r[1] for r in rows]
    H=np.array([[float(x) for x in r[2:]] for r in rows])
    BO,BPj=[np.array(l.split(),float) for l in (directory/"wave_buckets.txt").read_text().splitlines() if l.strip()]
    wO=waves(snap(BO,[2,3,4,5,6,7,8,9,10])); wP=waves(snap(BPj,[3,4,5,6,7,8,9,10]))
    agg=lambda v: aggregate(v,BO,BPj,wO,wP)
    r=agg(book); Hh=np.array([agg(h) for h in H]); A=Hh.T
    a=back_substitute(A,-r); resid=A@a+r
    print(f"population: {population}")
    print("instruments:")
    for i in selected: print(f"  {names[i]}")
    print(f"waves: {len(wO)} Eonia at OIS maturities, {len(wP)} Euribor at swap maturities")
    print("solver: explicit back-substitution of H.T a = -g")
    print(f"residual per bp max {abs(resid).max()*BP:.2e}  (book largest wave per bp {abs(r).max()*BP:.1f})")
    print("hedge notionals (millions; negative = pay):")
    for n,v in zip(hedge_names,a):
        if abs(v)>0.0005: print(f"  {n:8s} {v:9.3f}")
    print(f"parallel 1bp: book {book.sum()*BP:.1f}, hedge {-(A@a).sum()*BP:.1f}")
    if abs(resid).max()*BP > 1e-10: raise RuntimeError("hedge residual exceeds 1e-10 per bp")
    return 0

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--book",choices=("paper7","extended8"),default="paper7")
    p.add_argument("--directory",type=Path,default=Path("."))
    a=p.parse_args()
    return run(a.directory,a.book)
if __name__=="__main__": raise SystemExit(main())
