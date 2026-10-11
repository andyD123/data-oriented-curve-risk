"""Adjoint (reverse-mode) reference for the wave sensitivities.
Under BoxShifted, the analytical PV is  PV(delta) = sum_i x_i * prod_k exp(-delta_k * ov_ki)  (+ projection terms),
with x_i discounted unit cashflows (unit_cashflows_wave.txt). This script tapes that expression
explicitly and runs the reverse pass at delta = 0: an exact derivative with no step size.
Expected: agreement with the scan at rounding (1e-16 relative); the tape has N*K nodes, the scan N+K operations.
"""
import numpy as np
BO, BP = [np.array(l.split(), float) for l in open('wave_buckets.txt')]; Ko, Kp = len(BO)-1, len(BP)-1
S = np.loadtxt('scan_wave_risk.txt')
lines = open('unit_cashflows_wave.txt').read().split('\n')[1:]
insts = []; i = 0
while i < len(lines) and lines[i].strip():
    nm, ne, npj = lines[i].split(); ne, npj = int(ne), int(npj)
    e = np.array([[float(v) for v in lines[i+1+j].split()] for j in range(ne)])
    p = np.array([[float(v) for v in lines[i+1+ne+j].split()] for j in range(npj)]).reshape(npj, 4)
    insts.append((nm, e, p)); i += 1 + ne + npj
def overlap(B, t, k): return max(t - B[k], 0.0) if k == len(B)-2 else min(max(t - B[k], 0.0), B[k+1] - B[k])
def adjoint(e, p):
    dbar = np.zeros(Ko + Kp); nodes = 0
    for t, x in e:                                                  # discount-curve waves
        ov = np.array([overlap(BO, t, k) for k in range(Ko)]); y = np.exp(-0.0*ov); prod = np.prod(y); nodes += 2*Ko + 1
        ybar = x * prod / y; dbar[:Ko] += ybar * (-ov * y)           # reverse pass
    for tp, a, b, w in p:                                           # projection-curve waves: w * P(a)/P(b)
        ov = np.array([overlap(BP, a, k) - overlap(BP, b, k) for k in range(Kp)]); y = np.exp(-0.0*ov); prod = np.prod(y); nodes += 2*Kp + 1
        ybar = w * prod / y; dbar[Ko:] += ybar * (-ov * y)
    return dbar, nodes
print(f"{'instrument':26s} {'max|AAD-scan|/max|scan|':>24s} {'tape nodes':>11s} {'scan ops':>9s}")
for idx, (nm, e, p) in enumerate(insts):
    d, n = adjoint(e, p)
    print(f"{nm:26s} {np.abs(d-S[idx]).max()/np.abs(S[idx]).max():24.1e} {n:11d} {2*len(e)+Ko+6*len(p)+Kp:9d}")
