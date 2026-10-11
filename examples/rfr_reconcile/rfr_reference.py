#!/usr/bin/env python3
"""Independent reference for the RFR (single overnight-indexed discount curve) reconciliation.

Standard library only. Two modes:

  rfr_reference.py generate [dir]   write the fixtures: wave_buckets.txt, unit_cashflows_wave.txt,
                                    wave_risk_<eps>.txt (central differences through full valuation)
                                    and quotes.txt (the illustrative par-OIS quotes)
  rfr_reference.py check [dir]      read scan_wave_risk.txt (written by the C++ scan_wave) and run the
                                    reconciliation: direct-overlap oracle, explicit adjoint, finite-
                                    difference convergence, market-quote risk and the hedge solve

The curve is a single SOFR-style curve: par OIS quotes (annual fixed leg against the daily-compounded
overnight rate) bootstrapped with log-linear discount factors, so the interval forwards are piecewise flat.
Floating legs are compounded overnight legs, N*(D(a)/D(b)-1) paid at p, so no projection curve exists.
The quotes are ILLUSTRATIVE, not a market snapshot. Times are years from the valuation date; accruals
are year fractions of the schedule and no calendar logic is involved.
"""
import math
import sys
from pathlib import Path

PILLARS = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20, 25, 30]
QUOTES = [0.0420, 0.0390, 0.0375, 0.0370, 0.0370, 0.0372, 0.0375, 0.0378, 0.0381, 0.0385,
          0.0392, 0.0398, 0.0395, 0.0388, 0.0380]
EPSILONS = ['0.0004', '0.0002', '0.0001', '5e-05', '2.5e-05']


# ---------------------------------------------------------------------------- curve
class Curve:
    """Log-linear discount factors on boundaries B[0]=0 < B[1] < ... < B[K]; flat forward beyond B[K]."""

    def __init__(self, boundaries, log_d):
        self.B, self.L = list(boundaries), list(log_d)

    def log_df(self, t):
        B, L = self.B, self.L
        if t <= 0.0:
            return 0.0
        K = len(B) - 1
        if t >= B[K]:
            return L[K] + (t - B[K]) * (L[K] - L[K - 1]) / (B[K] - B[K - 1])
        k = 1
        while t >= B[k]:
            k += 1
        a = (t - B[k - 1]) / (B[k] - B[k - 1])
        return L[k - 1] + a * (L[k] - L[k - 1])

    def df(self, t):
        return math.exp(self.log_df(t))


def overlap(B, k, t):
    """Overlap of wave k (1-based) with [0, t]; the last wave is open-ended."""
    K = len(B) - 1
    inside = max(t - B[k - 1], 0.0)
    return inside if k == K else min(inside, B[k] - B[k - 1])


def bootstrap(quotes):
    """Par OIS bootstrap, annual fixed leg: 1 - D(T_n) = S_n * sum_j D(j), j = 1..T_n."""
    B, L = [0.0], [0.0]
    for T, S in zip(PILLARS, quotes):
        def residual(x):
            c = Curve(B + [float(T)], L + [x])
            return 1.0 - math.exp(x) - S * sum(c.df(j) for j in range(1, T + 1))
        lo, hi = -8.0, 1.0
        for _ in range(200):                     # residual decreases with x
            mid = 0.5 * (lo + hi)
            lo, hi = (mid, hi) if residual(mid) > 0 else (lo, mid)
        B.append(float(T)); L.append(0.5 * (lo + hi))
    return Curve(B, L)


class Shifted:
    """Curve with box shifts delta (one per wave) applied to the instantaneous forward."""

    def __init__(self, base, delta):
        self.base, self.delta = base, delta

    def df(self, t):
        B = self.base.B
        s = sum(d * overlap(B, k + 1, t) for k, d in enumerate(self.delta) if d)
        return math.exp(self.base.log_df(t) - s)


# ---------------------------------------------------------------------------- instruments
def swap(periods, fixed, spread=0.0):
    """Payer OIS-style swap: periods = [(notional, a, b, p)]; pays fixed, receives compounded RFR + spread."""
    return {'kind': 'swap', 'periods': periods, 'fixed': fixed, 'spread': spread}


def bond(flows):
    return {'kind': 'bond', 'flows': flows}


def annual(start, end, notional=1.0e6, lag=0.0):
    n = round(end - start)
    return [(notional, start + j, start + j + 1.0, start + j + 1.0 + lag) for j in range(n)]


def book():
    amort = [(1.0e6 * (1.0 - j / 7.0), float(j), float(j + 1), float(j + 1)) for j in range(7)]
    stub = [(1.0e6, 0.0, 2 / 12, 2 / 12)]
    stub += [(1.0e6, 2 / 12 + j, 2 / 12 + j + 1.0, 2 / 12 + j + 1.0) for j in range(5)]
    return [
        ('ois_5y_spot', swap(annual(0, 5), 0.0370)),
        ('ois_5y_1y_forward', swap(annual(1, 6), 0.0372)),
        ('bond_4p5pct_seasoned', bond([(0.35 + 0.5 * j, 22500.0 + (1.0e6 if j == 6 else 0.0)) for j in range(7)])),
        ('zero_coupon_2p4y', bond([(2.4, 1.0e6)])),
        ('ois_5y_lag_2d', swap(annual(0, 5, lag=2 / 365), 0.0370)),
        ('amortising_7y', swap(amort, 0.0375)),
        ('stub_2m_spread_25bp', swap(stub, 0.0370, spread=0.0025)),
        ('bond_35y_beyond_pillar', bond([(float(j), 40000.0 + (1.0e6 if j == 35 else 0.0)) for j in range(1, 36)])),
    ]


def hedge_instruments(quotes):
    return [(f'hedge_ois_{T}y', swap([(1.0e6, float(j), float(j + 1), float(j + 1)) for j in range(T)], S))
            for T, S in zip(PILLARS, quotes)]


def pv(inst, curve):
    """Full valuation through the supplied discount function."""
    if inst['kind'] == 'bond':
        return sum(x * curve.df(t) for t, x in inst['flows'])
    total = 0.0
    for N, a, b, p in inst['periods']:
        tau = b - a
        total += N * ((curve.df(a) / curve.df(b) - 1.0) + (inst['spread'] - inst['fixed']) * tau) * curve.df(p)
    return total


def unit_cashflows(inst, curve):
    """Replicate into dated present values with sign: the scan's input."""
    if inst['kind'] == 'bond':
        return [(t, x * curve.df(t)) for t, x in inst['flows']]
    out = []
    for N, a, b, p in inst['periods']:
        tau, Dp = b - a, curve.df(p)
        X = N * Dp * curve.df(a) / curve.df(b)
        out += [(p, X), (a, X), (b, -X), (p, -N * Dp), (p, N * (inst['spread'] - inst['fixed']) * tau * Dp)]
    return out


# ---------------------------------------------------------------------------- fixtures
def fmt(v):
    return repr(float(v))


def write_fixtures(d):
    quotes = QUOTES
    curve = bootstrap(quotes)
    insts = book() + hedge_instruments(quotes)
    K = len(PILLARS)
    (d / 'quotes.txt').write_text(' '.join(fmt(q) for q in quotes) + '\n')
    (d / 'wave_buckets.txt').write_text(' '.join(fmt(b) for b in curve.B) + '\n0 1\n')
    lines = [str(len(insts))]
    for name, inst in insts:
        recs = unit_cashflows(inst, curve)
        lines.append(f'{name} {len(recs)} 0')
        lines += [f'{fmt(t)} {fmt(x)}' for t, x in recs]
    (d / 'unit_cashflows_wave.txt').write_text('\n'.join(lines) + '\n')
    for eps in EPSILONS:
        e = float(eps)
        rows = []
        for name, inst in insts:
            row = []
            for k in range(K):
                up, dn = [0.0] * K, [0.0] * K
                up[k], dn[k] = e, -e
                row.append((pv(inst, Shifted(curve, up)) - pv(inst, Shifted(curve, dn))) / (2 * e))
            rows.append(' '.join(fmt(v) for v in row + [0.0]))
        (d / f'wave_risk_{eps}.txt').write_text('\n'.join(rows) + '\n')
    print(f'wrote fixtures for {len(insts)} instruments, K={K} waves into {d}')


# ---------------------------------------------------------------------------- checks
def matrix(path):
    return [[float(v) for v in ln.split()] for ln in path.read_text().splitlines() if ln.strip()]


def read_fixture(d):
    toks = iter((d / 'unit_cashflows_wave.txt').read_text().split())
    n = int(next(toks)); out = []
    for _ in range(n):
        name, ne, npj = next(toks), int(next(toks)), int(next(toks))
        assert npj == 0
        out.append((name, [(float(next(toks)), float(next(toks))) for _ in range(ne)]))
    return out


def check(d):
    B = [float(v) for v in (d / 'wave_buckets.txt').read_text().split('\n')[0].split()]
    K = len(B) - 1
    fixture = read_fixture(d)
    scan = [r[:K] for r in matrix(d / 'scan_wave_risk.txt')]
    fd = {e: [r[:K] for r in matrix(d / f'wave_risk_{e}.txt')] for e in EPSILONS}
    nbook = 8
    ok = True

    # 1. direct-overlap oracle
    worst = 0.0
    for (name, recs), s in zip(fixture, scan):
        for k in range(1, K + 1):
            ref = math.fsum(-x * overlap(B, k, t) for t, x in recs)
            worst = max(worst, abs(ref - s[k - 1]) / max(1.0, max(abs(v) for v in s)))
    print(f'direct-overlap oracle vs scan: max scaled difference {worst:.2e}')
    ok &= worst < 1e-11

    # 2. explicit adjoint (taped expression, reverse pass at delta = 0)
    print(f"\n{'instrument':26s} {'max|AAD-scan|/max|scan|':>24s} {'adjoint N*K':>12s} {'scan 2N+K':>10s}")
    for (name, recs), s in list(zip(fixture, scan))[:nbook]:
        dbar = [0.0] * K; nodes = 0
        for t, x in recs:
            ov = [overlap(B, k, t) for k in range(1, K + 1)]
            y = [math.exp(-0.0 * o) for o in ov]; prod = math.prod(y); nodes += K
            for k in range(K):
                dbar[k] += x * prod / y[k] * (-ov[k] * y[k])
        err = max(abs(a - b) for a, b in zip(dbar, s)) / max(abs(v) for v in s)
        print(f'{name:26s} {err:24.1e} {nodes:12d} {2 * len(recs) + K:10d}')
        ok &= err < 1e-13

    # 3. finite-difference convergence (residual must fall ~4x when eps halves)
    print(f"\n{'instrument':26s} " + ' '.join(f'{e:>10s}' for e in EPSILONS) + '   ratios')
    for i, (name, _) in enumerate(fixture[:nbook]):
        res = [sum(abs(a - b) for a, b in zip(scan[i], fd[e][i])) for e in EPSILONS]
        ratios = [res[j] / max(res[j + 1], 1e-300) for j in range(len(res) - 1)]
        print(f'{name:26s} ' + ' '.join(f'{r:10.2e}' for r in res) + '   ' + ' '.join(f'{r:.2f}' for r in ratios[:3]))
        ok &= 3.5 < ratios[0] < 4.5
    rich = 0.0
    for i in range(nbook):
        a, b = fd['5e-05'][i], fd['2.5e-05'][i]
        r = [(4 * y - x) / 3 for x, y in zip(a, b)]
        rich = max(rich, max(abs(u - v) for u, v in zip(r, scan[i])) / max(abs(v) for v in scan[i]))
    print(f'Richardson (5e-05, 2.5e-05) vs scan: max relative to each instrument largest wave {rich:.1e}')
    ok &= rich < 1e-8

    # 4. market-quote risk: wave risk x bootstrap Jacobian vs full re-bootstrap
    quotes = [float(v) for v in (d / 'quotes.txt').read_text().split()]
    insts = [inst for _, inst in book()]
    base = bootstrap(quotes)

    def f_of(q):
        c = bootstrap(q)
        return [-(c.L[m] - c.L[m - 1]) / (c.B[m] - c.B[m - 1]) for m in range(1, K + 1)]

    def total_pv(q):
        c = bootstrap(q)
        return sum(pv(i, c) for i in insts)

    def central(fn, j, h):
        up, dn = quotes[:], quotes[:]
        up[j] += h; dn[j] -= h
        a, b = fn(up), fn(dn)
        return ([(x - y) / (2 * h) for x, y in zip(a, b)] if isinstance(a, list) else (a - b) / (2 * h))

    def rich_derivative(fn, j, h=1e-4):
        d1, d2 = central(fn, j, h), central(fn, j, h / 2)
        if isinstance(d1, list):
            return [(4 * y - x) / 3 for x, y in zip(d1, d2)]
        return (4 * d2 - d1) / 3

    book_wave = [math.fsum(scan[i][k] for i in range(nbook)) for k in range(K)]
    worst = 0.0; largest = 0.0; nonzero = 0
    for j in range(K):
        direct = rich_derivative(total_pv, j)
        Jcol = rich_derivative(f_of, j)
        nonzero += sum(1 for v in Jcol if abs(v) > 1e-9)
        pred = math.fsum(g * jf for g, jf in zip(book_wave, Jcol))
        worst = max(worst, abs(pred - direct)); largest = max(largest, abs(direct))
    print(f'\nquote risk ({K} quotes): max |wave x Jacobian - re-bootstrap| = {worst:.1e}; largest sensitivity {largest:.1e} '
          f'per unit rate; ratio {worst / largest:.1e}; Jacobian non-zeros {nonzero} of {K * K}')
    ok &= worst / largest < 1e-8

    # 5. hedge: one wave per par OIS at its maturity, lower-triangular, back-substitution
    H = scan[nbook:]
    h = [0.0] * K
    for k in range(K - 1, -1, -1):
        rem = -book_wave[k] - sum(h[i] * H[i][k] for i in range(k + 1, K))
        h[k] = rem / H[k][k]
    resid = max(abs(sum(h[i] * H[i][k] for i in range(K)) + book_wave[k]) for k in range(K))
    # position hedged: the sum of the 8 book instruments' own ladders
    print(f'hedge solve: lower-triangular back-substitution residual {resid:.1e} per unit wave '
          f'(book ladder scale {max(abs(v) for v in book_wave):.1e})')
    print('hedge notionals (1M units):', ' '.join(f'{T}y:{v:.3f}' for T, v in zip(PILLARS, h)))
    ok &= resid < 1e-6 * max(abs(v) for v in book_wave)

    print('\nreconciliation:', 'PASS' if ok else 'FAIL')
    return 0 if ok else 1


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else ''
    d = Path(sys.argv[2] if len(sys.argv) > 2 else '.').resolve()
    if mode == 'generate':
        write_fixtures(d)
    elif mode == 'check':
        sys.exit(check(d))
    else:
        sys.exit(__doc__)
