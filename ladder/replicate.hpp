#pragma once
// Supported algebraic leg formulas -> first-order signed risk weights.
// Discount factors are supplied by callables; the library knows no curve, calendar,
// fixing history, coupon pricer or option model. Adapters must supply the actual
// forecast periods and separate already-fixed amounts. OIS below supports only
// the displayed fully-forecast discount-ratio formula, not arbitrary OIS conventions.
#include <vector>
#include "unit_cashflow.hpp"

namespace ladder {

// fixed amount c paid at t:  x = c D(t)
template <class DF>
inline void replicate_fixed(double c, double t, DF df, std::vector<UnitCashflow>& out) { out.push_back({t, c * df(t)}); }

// IBOR coupon with amount scale*(P(a)/P(b) - 1) paid at t (scale = N * accrual / tau_index), discounted on D.
template <class DF, class PF>
inline void replicate_ibor(double scale, double a, double b, double t, DF df, PF pf,
                           std::vector<UnitCashflow>& out, std::vector<ProjectionTerm>& proj)
{
    double ratio = pf(a) / pf(b), Dp = df(t);
    out.push_back({t, scale * (ratio - 1.0) * Dp});
    proj.push_back({t, a, b, scale * ratio * Dp});
}

// compounded overnight coupon N*(D(a)/D(b) - 1) paid at t (N = nominal * accrual / tau_index) on the discount curve:
// four unit cashflows  (+x at t) (+x at a) (-x at b) (-N D(t) at t),  x = N D(t) D(a)/D(b)
template <class DF>
inline void replicate_ois(double N, double a, double b, double t, DF df, std::vector<UnitCashflow>& out)
{
    double Dt = df(t), x = N * Dt * df(a) / df(b);
    out.push_back({t, x}); out.push_back({a, x}); out.push_back({b, -x}); out.push_back({t, -N * Dt});
}

} // namespace ladder
