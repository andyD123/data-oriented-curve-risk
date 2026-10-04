"""Hagan's hedge solve as he states it (2015, eqs 2.2, 2.7-2.10): waves at the hedge instruments' maturities,
one wave per hedge, sensitivity matrix lower-triangular per curve, exact back-substitution. Built from the
pillar-wave ladders (adjacent box shifts add, so a hedge-maturity wave is the sum of the pillar waves inside it)."""
import numpy as np
S=np.loadtxt('scan_wave_risk.txt'); book=S.sum(axis=0)
rows=[l.split() for l in open('wave_hedges.txt')]; hn=[r[0]+' '+r[1] for r in rows]; H=np.array([[float(x) for x in r[2:]] for r in rows])
BO,BP=[np.array(l.split(),float) for l in open('wave_buckets.txt')]; Ko,Kp=len(BO)-1,len(BP)-1
bp=1e-4
def snap(B,yrs): return [B[np.argmin(np.abs(B-y))] for y in yrs]             # hedge maturities are pillars; snap to them
oisT=snap(BO,[2,3,4,5,6,7,8,9,10]); irsT=snap(BP,[3,4,5,6,7,8,9,10])
def waves(T):                                                                # [0,T1),[T1,T2),...,[T_{K-1},inf)  (his 2.2b: last wave open)
    e=[0.0]+list(T); return [(e[j-1], e[j] if j<len(e)-1 else np.inf) for j in range(1,len(e))]
wO,wP=waves(oisT),waves(irsT)
def mask(B,lo,hi): return np.array([(B[k]>=lo-1e-9) and (B[k+1]<=hi+1e-9) for k in range(len(B)-1)])
def agg(v): return np.array([v[:Ko][mask(BO,lo,hi)].sum() for lo,hi in wO]+[v[Ko:][mask(BP,lo,hi)].sum() for lo,hi in wP])
assert all(mask(BO,lo,hi).sum()>0 for lo,hi in wO) and all(mask(BP,lo,hi).sum()>0 for lo,hi in wP)
r=agg(book); Hh=np.array([agg(h) for h in H])                               # 17 hedges x 17 waves
nO=len(oisT)
tri = np.allclose(np.triu(Hh[:nO,:nO],1),0,atol=1e-6*abs(Hh).max()) and np.allclose(np.triu(Hh[nO:,nO:],1),0,atol=1e-6*abs(Hh).max())
a=np.linalg.solve(Hh.T,-r); resid=Hh.T@a+r
print(f"waves: {len(wO)} Eonia at OIS maturities, {len(wP)} Euribor at swap maturities; matrix lower-triangular per curve block: {tri}")
print(f"exact solve, residual per bp max {abs(resid).max()*bp:.2e}  (book largest wave per bp {abs(r).max()*bp:.1f})")
print("hedge notionals (millions; negative = pay):")
for n,v in zip(hn,a):
    if abs(v)>0.0005: print(f"  {n:8s} {v:9.3f}")
lab=[f"Eonia [{lo:.2f},{'inf' if np.isinf(hi) else f'{hi:.2f}'})" for lo,hi in wO]+[f"Euribor [{lo:.2f},{'inf' if np.isinf(hi) else f'{hi:.2f}'})" for lo,hi in wP]
print("book ladder per bp on Hagan's waves:")
for l,v in zip(lab,r*bp): print(f"  {l:24s} {v:9.1f}")
print(f"parallel 1bp: book {book.sum()*bp:.1f}, hedge {-(Hh.T@a).sum()*bp:.1f}")
