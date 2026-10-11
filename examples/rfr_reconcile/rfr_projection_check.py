#!/usr/bin/env python3
"""Reconciliation of the projection pass (term-rate / IBOR legs on a second curve) against an independent reference.

Standard library only; reuses the single-curve reference (rfr_reference.py) for the log-linear curve, the box-shifted
curve and the par-OIS bootstrap. Two modes:

  rfr_projection_check.py generate [dir] [--curve FILE]
                                          write wave_buckets.txt, unit_cashflows_wave.txt (discount unit cashflows and
                                          projection terms) and wave_risk_<eps>.txt (central differences through full
                                          valuation, box shifts applied to the discount and the projection curve)
  rfr_projection_check.py check [dir]     read scan_wave_risk.txt (written by the C++ scan_wave) and check: finite-
                                          difference convergence (residual falls 4x per halving), Richardson agreement,
                                          zero risk beyond maturity

Scope: this checks the discount and projection WAVE ladders of term-rate swaps, with the two curves shifted
independently. It does NOT reconcile the cross-curve calibration Jacobian block (projection curve bootstrapped from
term quotes with OIS discounting); the projection curve here is a given log-linear curve, ILLUSTRATIVE, not a market
snapshot. Times are years from the valuation date; accruals are year fractions and no calendar logic is involved.
"""
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from rfr_reference import Curve, Shifted, bootstrap, overlap, QUOTES, EPSILONS, fmt, matrix, quotes_from_curve_file  # noqa: E402


def projection_curve(disc):
    """Illustrative projection curve: the OIS curve plus a smooth basis, log-linear on the same nodes."""
    L = [l - 0.0035 * b - 0.00005 * b * b for l, b in zip(disc.L, disc.B)]
    return Curve(disc.B, L)


def ibor_swap(start, end, per_year, fixed, spread=0.0, notional=1.0e6):
    """Receive floating (forecast off the projection curve), pay fixed; coupons (N, a, b, p) with p = b."""
    n = round((end - start) * per_year)
    step = 1.0 / per_year
    return {'periods': [(notional, start + j * step, start + (j + 1) * step, start + (j + 1) * step) for j in range(n)],
            'fixed': fixed, 'spread': spread}


def book():
    return [
        ('ibor_5y_semiannual', ibor_swap(0, 5, 2, 0.0400)),
        ('ibor_7y_1y_forward', ibor_swap(1, 8, 2, 0.0410)),
        ('ibor_3y_quarterly_spread', ibor_swap(0, 3, 4, 0.0395, spread=0.0010)),
        ('ibor_32y_beyond_pillar', ibor_swap(0, 32, 2, 0.0420)),
    ]


def pv(inst, D, P):
    total = 0.0
    for N, a, b, p in inst['periods']:
        tau = b - a
        total += N * ((P.df(a) / P.df(b) - 1.0) + (inst['spread'] - inst['fixed']) * tau) * D.df(p)
    return total


def records(inst, D, P):
    """Discount unit cashflows (projected coupon amount at p) and projection terms (p, a, b, w)."""
    unit, proj = [], []
    for N, a, b, p in inst['periods']:
        tau, Dp, ratio = b - a, D.df(p), P.df(a) / P.df(b)
        unit.append((p, N * ((ratio - 1.0) + (inst['spread'] - inst['fixed']) * tau) * Dp))
        proj.append((p, a, b, N * Dp * ratio))
    return unit, proj


def write_fixtures(d, quotes=None):
    D = bootstrap(QUOTES if quotes is None else quotes); P = projection_curve(D); K = len(D.B) - 1
    insts = book()
    row = ' '.join(fmt(b) for b in D.B)
    (d / 'wave_buckets.txt').write_text(row + '\n' + row + '\n')
    lines = [str(len(insts))]
    for name, inst in insts:
        unit, proj = records(inst, D, P)
        lines.append(f'{name} {len(unit)} {len(proj)}')
        lines += [f'{fmt(t)} {fmt(x)}' for t, x in unit]
        lines += [f'{fmt(p)} {fmt(a)} {fmt(b)} {fmt(w)}' for p, a, b, w in proj]
    (d / 'unit_cashflows_wave.txt').write_text('\n'.join(lines) + '\n')
    for eps in EPSILONS:
        e = float(eps)
        rows = []
        for name, inst in insts:
            row_v = []
            for curve_index in (0, 1):                       # discount waves, then projection waves
                for k in range(K):
                    up, dn = [0.0] * K, [0.0] * K
                    up[k], dn[k] = e, -e
                    if curve_index == 0:
                        hi, lo = pv(inst, Shifted(D, up), P), pv(inst, Shifted(D, dn), P)
                    else:
                        hi, lo = pv(inst, D, Shifted(P, up)), pv(inst, D, Shifted(P, dn))
                    row_v.append((hi - lo) / (2 * e))
            rows.append(' '.join(fmt(v) for v in row_v))
        (d / f'wave_risk_{eps}.txt').write_text('\n'.join(rows) + '\n')
    print(f'wrote projection fixtures for {len(insts)} term-rate swaps, K={K} waves per curve into {d}')


def check(d):
    B = [float(v) for v in (d / 'wave_buckets.txt').read_text().split('\n')[0].split()]
    K = len(B) - 1
    names = [n for n, _ in book()]
    scan = matrix(d / 'scan_wave_risk.txt')
    fd = {e: matrix(d / f'wave_risk_{e}.txt') for e in EPSILONS}
    ok = True
    print(f"{'instrument':28s} " + ' '.join(f'{e:>10s}' for e in EPSILONS) + '   ratios')
    for i, name in enumerate(names):
        res = [sum(abs(a - b) for a, b in zip(scan[i], fd[e][i])) for e in EPSILONS]
        ratios = [res[j] / max(res[j + 1], 1e-300) for j in range(len(res) - 1)]
        print(f'{name:28s} ' + ' '.join(f'{r:10.2e}' for r in res) + '   ' + ' '.join(f'{r:.2f}' for r in ratios[:3]))
        ok &= 3.5 < ratios[0] < 4.5
    rich_d = rich_p = 0.0
    for i in range(len(names)):
        a, b = fd['5e-05'][i], fd['2.5e-05'][i]
        r = [(4 * y - x) / 3 for x, y in zip(a, b)]
        scale = max(abs(v) for v in scan[i])
        rich_d = max(rich_d, max(abs(u - v) for u, v in zip(r[:K], scan[i][:K])) / scale)
        rich_p = max(rich_p, max(abs(u - v) for u, v in zip(r[K:], scan[i][K:])) / scale)
    print(f'Richardson (5e-05, 2.5e-05) vs scan, relative to each swap largest wave: discount {rich_d:.1e}, projection {rich_p:.1e}')
    ok &= rich_d < 1e-8 and rich_p < 1e-8
    # projection risk of a coupon wholly before wave k must be exactly zero for bounded waves; check the 5y swap past 5y
    s = scan[0]
    beyond = max(abs(v) for k, v in enumerate(s[K:], start=1) if B[k - 1] >= 5.0 + 1e-9)
    print(f'5y swap: largest projection-wave risk starting at or after maturity: {beyond:.1e}')
    ok &= beyond == 0.0
    print('\nprojection reconciliation:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    d = Path(sys.argv[2] if len(sys.argv) > 2 else '.').resolve()
    if mode == 'generate':
        curve_file = sys.argv[sys.argv.index('--curve') + 1] if '--curve' in sys.argv else None
        write_fixtures(d, quotes_from_curve_file(curve_file) if curve_file else None)
    elif mode == 'check':
        sys.exit(check(d))
    else:
        sys.exit(__doc__)
