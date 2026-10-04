#pragma once
// Unit cashflows: every leg is replicated into dated present values with sign. The scan sees only these.
namespace ladder {

struct UnitCashflow { double t; double x; };           // present value x at time t

// A projection-curve term w * P(a)/P(b): its discount-curve part is already a UnitCashflow at t_pay;
// this carries what the projection ladder needs. w = scale * D(t_pay) * P(a)/P(b) at the base curves.
struct ProjectionTerm { double t_pay, a, b, w; };

} // namespace ladder
