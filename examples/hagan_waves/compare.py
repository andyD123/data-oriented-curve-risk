"""Scan vs QuantLib box-wave bumps on the shipped cubic curves. Residual must fall ~4x when eps halves."""
import numpy as np
S = np.loadtxt('scan_wave_risk.txt'); R4 = np.loadtxt('wave_risk_0.0001.txt'); R2 = np.loadtxt('wave_risk_5e-05.txt')
names = [l.split()[0] for l in open('unit_cashflows_wave.txt').readlines()[1:] if l[0].isalpha()]
print(f"{'instrument':26s} {'max rel (1e-4)':>14s} {'max rel (5e-5)':>14s} {'eps^2 ratio':>11s}")
for i, nm in enumerate(names):
    sc = np.abs(S[i]).max(); r4 = np.abs(S[i]-R4[i]).max()/sc; r2 = np.abs(S[i]-R2[i]).max()/sc
    ratio = np.abs(S[i]-R4[i]).sum() / max(np.abs(S[i]-R2[i]).sum(), 1e-300)
    print(f"{nm:26s} {r4:14.1e} {r2:14.1e} {ratio:11.2f}")
