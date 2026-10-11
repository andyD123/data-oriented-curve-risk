#!/usr/bin/env python3
"""Optional *live* QuantLib OIS/lag wave-gamma reconciliation.

Builds real QuantLib OvernightIndexedCoupon objects and reprices their amounts
under four separate log-discount curves. Bumps are Hagan instantaneous-forward
box waves, NOT parallel-zero-rate bumps or quote-rebootstrap sensitivities.

Run: python -m pip install 'QuantLib>=1.33'
     python examples/cross_gamma/quantlib/live_ois_cross_gamma.py --scenario all

QuantLib is intentionally NOT needed by the C++ example or archived CTest.
"""
from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from typing import Sequence


def overlap(boundaries: Sequence[float], k: int, t: float) -> float:
    """Integrated forward bump on [B[k], B[k+1]) (zero-based index)."""
    return max(0.0, min(t - boundaries[k], boundaries[k + 1] - boundaries[k]))


@dataclass(frozen=True)
class ReducedOis:
    a: float
    b: float
    p: float
    A: float  # signed N D(p) D(a)/D(b)
    C: float  # signed N D(p)

    def gradient(self, B: Sequence[float], k: int) -> float:
        d = overlap(B, k, self.p)
        r = overlap(B, k, self.b) - overlap(B, k, self.a)
        return -(self.A - self.C) * d + self.A * r

    def frozen_hessian(self, B: Sequence[float], j: int, k: int) -> float:
        hj = lambda t: overlap(B, j, t)
        hk = lambda t: overlap(B, k, t)
        return (self.A * hj(self.a) * hk(self.a)
                - self.A * hj(self.b) * hk(self.b)
                + (self.A - self.C) * hj(self.p) * hk(self.p))

    def lag_correction(self, B: Sequence[float], j: int, k: int) -> float:
        rj = overlap(B, j, self.b) - overlap(B, j, self.a)
        rk = overlap(B, k, self.b) - overlap(B, k, self.a)
        sj = overlap(B, j, self.p) - overlap(B, j, self.b)
        sk = overlap(B, k, self.p) - overlap(B, k, self.b)
        return -self.A * (rj * sk + sj * rk)

    def exact_hessian(self, B: Sequence[float], j: int, k: int) -> float:
        qj = (overlap(B, j, self.p) + overlap(B, j, self.a)
              - overlap(B, j, self.b))
        qk = (overlap(B, k, self.p) + overlap(B, k, self.a)
              - overlap(B, k, self.b))
        return (self.A * qj * qk
                - self.C * overlap(B, j, self.p) * overlap(B, k, self.p))


def assert_near(label: str, observed: float, expected: float,
                atol: float, rtol: float) -> None:
    err = abs(observed - expected)
    bound = atol + rtol * max(abs(observed), abs(expected))
    print(f"  {label:29} QL/reprice {observed:+.9f}  theory {expected:+.9f}"
          f"  error={err:.4g}  limit={bound:.4g}")
    if not (math.isfinite(observed) and math.isfinite(expected)) or err > bound:
        raise AssertionError(f"{label}: error {err} > {bound}")


def model_unit_checks() -> None:
    """Dependency-free algebra gate, useful even without QuantLib."""
    B = (0., 1., 2., 3., 4.)
    a, b, p, A, C = 1.2, 2.8, 3.25, 1.14e6, 1.11e6
    c = ReducedOis(a, b, p, A, C)
    for j in range(len(B) - 1):
        for k in range(len(B) - 1):
            assert math.isclose(c.frozen_hessian(B,j,k)+c.lag_correction(B,j,k),
                                c.exact_hessian(B,j,k),rel_tol=2e-14,abs_tol=2e-9)
    print("PASS: offline reduced-rank OIS identity")


def run_live(name: str) -> None:
    try:
        import QuantLib as ql
    except ImportError as exc:
        raise RuntimeError("Live mode needs pip install 'QuantLib>=1.33'") from exc

    today = ql.Date(13, ql.October, 2026)
    calendar = ql.UnitedStates(ql.UnitedStates.GovernmentBond)
    dc = ql.Actual365Fixed()
    if name == "standard":
        a = calendar.adjust(ql.Date(4, ql.January, 2027), ql.Following)
        b = calendar.adjust(ql.Date(6, ql.April, 2027), ql.Following)
        p = calendar.advance(b, 2, ql.Days)
        far = calendar.advance(p, 6, ql.Months)
        dates = [today, a, b, p, far]
    elif name == "stress":
        a = calendar.adjust(ql.Date(5, ql.January, 2047), ql.Following)
        b = calendar.adjust(ql.Date(6, ql.January, 2048), ql.Following)
        p = calendar.advance(b, 65, ql.Days)  # deliberately nonmarket-standard
        far = calendar.advance(p, 1, ql.Years)
        dates = [today, a, b, p, far]
    elif name == "interior":
        # The accrual end is NOT a wave boundary: wave 3 covers both
        # accrued and post-accrual days, giving -2*A*r_k*s_k != 0
        # on the same wave diagonal.
        a = calendar.adjust(ql.Date(4, ql.January, 2027), ql.Following)
        b = calendar.adjust(ql.Date(6, ql.April, 2027), ql.Following)
        p = calendar.advance(b, 8, ql.Days)
        left = ql.Date(29, ql.March, 2027)
        right = ql.Date(13, ql.April, 2027)
        far = calendar.advance(p, 6, ql.Months)
        assert a < left < b < right < p
        dates = [today, a, left, right, p, far]
    else:
        raise ValueError(name)
    assert today < a < b < p < far
    ql.Settings.instance().evaluationDate = today

    B = tuple(dc.yearFraction(today, d) for d in dates)
    assert all(B[i] < B[i+1] for i in range(len(B)-1))
    N, flat_rate = 1_000_000.0, 0.035
    handle = ql.RelinkableYieldTermStructureHandle()
    index = ql.Sofr(handle)
    coupon = ql.OvernightIndexedCoupon(p, N, a, b, index)

    def curve_for(shocks: Sequence[float]):
        discounts = []
        for t in B:
            x = sum(shocks[j]*overlap(B, j, t) for j in range(len(B)-1))
            discounts.append(math.exp(-flat_rate*t-x))
        discounts[0] = 1.0
        curve = ql.DiscountCurve(dates, discounts, dc, calendar)
        curve.enableExtrapolation()
        return curve

    def ql_price(shocks: Sequence[float]) -> float:
        handle.linkTo(curve_for(shocks))
        return coupon.amount()*handle.discount(p)

    base_shocks = [0.0] * (len(B)-1)
    price0 = ql_price(base_shocks)
    D_a, D_b, D_p = (handle.discount(d) for d in (a,b,p))
    # QL's payment accrual might differ from the overnight index accrual.
    # Keep the exact scaling used by the coupon, not an assumed notional.
    tau_index = index.dayCounter().yearFraction(a,b)
    scale = N * coupon.accrualPeriod() / tau_index
    ois = ReducedOis(dc.yearFraction(today,a),
                     dc.yearFraction(today,b),
                     dc.yearFraction(today,p),
                     scale*D_p*D_a/D_b,scale*D_p)
    print(f"\nQuantLib {getattr(ql,'__version__','unknown')} — {name}")
    print(f"  start={a}, end={b}, pay={p}; calendar-day lag={p-b}")
    print(f"  forward start={B[1]:.6f} y, accrual={B[2]-B[1]:.6f} y,"
          f" lag={B[3]-B[2]:.6f} y")
    assert_near("base PV",price0,ois.A-ois.C,atol=1e-4,rtol=1e-9)

    def repriced(j: int, u: float, k: int | None = None, v: float = 0.0):
        shock = base_shocks.copy()
        shock[j] += u
        if k is not None:
            shock[k] += v
        return ql_price(shock)

    j, k = (2, 3) if name == "interior" else (1, 2)
    # In the interior case j covers both sides of b; k is a lag-only wave.
    if name == "interior":
        assert B[j] < ois.b < B[j + 1]
        assert ois.lag_correction(B,j,j) != 0.0
    h = 1e-4
    first = (repriced(j,h)-repriced(j,-h))/(2*h)
    assert_near("first-order accrual",first,ois.gradient(B,j),atol=0.05,rtol=8e-6)
    true_h = ois.exact_hessian(B,j,k)
    frozen_h = ois.frozen_hessian(B,j,k)
    correction = ois.lag_correction(B,j,k)
    assert_near("rank-two decomposition",frozen_h+correction,true_h,atol=1e-6,rtol=1e-11)
    print(f"  frozen gamma={frozen_h:+.6f}, correction={correction:+.6f},"
          f" true gamma={true_h:+.6f}")

    errors = []
    for eps in (0.008,0.004,0.002):
        pp=repriced(j,eps,k,eps)
        pm=repriced(j,eps,k,-eps)
        mp=repriced(j,-eps,k,eps)
        mm=repriced(j,-eps,k,-eps)
        observed=(pp-pm-mp+mm)/(4*eps*eps)
        assert_near(f"mixed gamma h={eps}",observed,true_h,atol=5.0,rtol=7e-5)
        errors.append(abs(observed-true_h))
    if errors[2] >= 0.40*errors[1]+1e-4:
        raise AssertionError("central mixed difference not converging quadratically")
    if name == "interior":
        # This is the new validation gate. Earlier standard/stress tests put
        # b exactly at a wave boundary and could not probe diagonal r_j*s_j.
        frozen_diagonal = ois.frozen_hessian(B,j,j)
        diagonal_correction = ois.lag_correction(B,j,j)
        exact_diagonal = ois.exact_hessian(B,j,j)
        assert abs(diagonal_correction) > 10.0
        assert_near("interior diagonal decomposition",
                    frozen_diagonal+diagonal_correction,
                    exact_diagonal,atol=1e-7,rtol=1e-12)
        print(f"  interior wave={j+1}: frozen Hjj={frozen_diagonal:+.9f},"
              f" correction={diagonal_correction:+.9f},"
              f" corrected Hjj={exact_diagonal:+.9f}")
        previous_error = None
        for eps in (0.008, 0.004, 0.002):
            observed = (repriced(j,eps)-2.0*price0+repriced(j,-eps))/(eps*eps)
            assert_near(f"diagonal gamma h={eps}",observed,exact_diagonal,
                        atol=0.10,rtol=2e-5)
            err = abs(observed-exact_diagonal)
            if previous_error is not None and err > 0.48*previous_error+0.01:
                raise AssertionError("interior diagonal FD not converging quadratically")
            previous_error = err

        # Entire 5-by-5 Hessian, not only the selected j,k entry.
        # Reprices the QuantLib coupon for every diagonal and unique pair.
        full_h=0.004
        checked=0
        for u in range(len(B)-1):
            for v in range(u,len(B)-1):
                expected=ois.exact_hessian(B,u,v)
                if u==v:
                    observed=(repriced(u,full_h)-2*price0+
                              repriced(u,-full_h))/(full_h*full_h)
                else:
                    observed=(repriced(u,full_h,v,full_h)-
                              repriced(u,full_h,v,-full_h)-
                              repriced(u,-full_h,v,full_h)+
                              repriced(u,-full_h,v,-full_h))/(4*full_h*full_h)
                assert_near(f"full Hessian [{u},{v}]",observed,expected,
                            atol=0.12,rtol=2e-5)
                checked+=1
        print(f"  independent QuantLib complete Hessian: {checked} symmetric entries PASS")
    handle.linkTo(curve_for(base_shocks))
    print(f"PASS: live QuantLib OIS payment-lag gamma ({name})")


def main() -> int:
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario",choices=("standard","stress","interior","all","offline"),
                        default="offline")
    args=parser.parse_args()
    model_unit_checks()
    if args.scenario != "offline":
        for scenario in (("standard","stress","interior") if args.scenario == "all"
                         else (args.scenario,)):
            run_live(scenario)
    return 0


if __name__=="__main__":
    try:
        sys.exit(main())
    except Exception as e:
        print(f"FAIL: {e}",file=sys.stderr)
        sys.exit(1)
