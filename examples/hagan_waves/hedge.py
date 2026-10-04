"""Hagan wave hedging on top of the scan.
Reads: scan_wave_risk.txt (book wave risk from the scan, per unit forward bump), wave_hedges.txt (wave risk of
candidate par instruments per 1M notional, from QuantLib), wave_buckets.txt. Solves min ||H^T q - r||.
"""
import numpy as np
S = np.loadtxt('scan_wave_risk.txt'); book = S.sum(axis=0)
rows = [l.split() for l in open('wave_hedges.txt')]
names = [r[0] + ' ' + r[1] for r in rows]; H = np.array([[float(x) for x in r[2:]] for r in rows])
BO, BP = [np.array(l.split(), float) for l in open('wave_buckets.txt')]
Ko, Kp = len(BO) - 1, len(BP) - 1; bp = 1e-4
lab = [f"Eonia [{BO[k]:.2f},{BO[k+1]:.2f})" for k in range(Ko)] + [f"Euribor6M [{BP[k]:.2f},{BP[k+1]:.2f})" for k in range(Kp)]
print("book box-wave risk per 1bp, |risk| > 1:")
for k in np.argsort(-np.abs(book)):
    if abs(book[k] * bp) > 1: print(f"  {lab[k]:>24s} {book[k]*bp:10.1f}")
q, _, rank, _ = np.linalg.lstsq(H.T, book, rcond=None); resid = H.T @ q - book
print(f"\nhedge: {len(names)} par instruments vs {Ko+Kp} waves, rank {rank}; residual per bp max {abs(resid).max()*bp:.1f} (book max {abs(book).max()*bp:.1f})")
print("notionals, millions, receiver (+) / payer (-):")
for n, v in zip(names, q):
    if abs(v) > 0.005: print(f"  {n:8s} {v:8.3f}")
print(f"\nparallel 1bp: book {book.sum()*bp:.1f}, hedge {(H.T@q).sum()*bp:.1f}")
