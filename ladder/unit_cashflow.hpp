#pragma once
// Unit cashflows are signed first-order risk weights dV/dlog D(t).
// For a fixed amount they equal its present value. Product/ratio weights are not
// an exact finite-shock cashflow decomposition; the scan computes first derivatives.
namespace ladder {

struct UnitCashflow { double t; double x; };           // present value x at time t

// A projection-curve term w * P(a)/P(b): its discount-curve part is already a UnitCashflow at t_pay;
// this carries what the projection ladder needs. w = scale * D(t_pay) * P(a)/P(b) at the base curves.
struct ProjectionTerm { double t_pay, a, b, w; };

} // namespace ladder
