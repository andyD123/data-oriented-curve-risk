#pragma once
// Scalar reverse scan. Risk per unit forward bump of each stencil.
//   discount ladder:    d PV / d delta_k = -( sum_{in k} x_i * (t_i - B[k-1])  +  len(k) * sum_{t_i >= B[k]} x_i )
//   projection ladder:  d PV / d delta_k = sum_terms w * (psi_k(a) - psi_k(b))    (no tail: it cancels)
#include <algorithm>
#include <span>
#include <vector>
#include "stencil.hpp"
#include "unit_cashflow.hpp"

namespace ladder {

// cashflows must be sorted by t ascending. out has K entries.
inline void scan_discount(const Stencils& S, std::span<const UnitCashflow> cf, double* out)
{
    const int K = S.K(); const double BK = S.B[K];
    std::vector<double> interior(K + 1, 0.0), suffix(K + 1, 0.0);
    // forward pass: interior term. A cashflow at or beyond B[K] belongs to the tail of every stencil up to K;
    // for stencil K it is either tail mass (capped last stencil) or interior with uncapped weight (open last stencil).
    for (const auto& c : cf) {
        int k = S.bucket(c.t);
        if (c.t >= BK) { if (S.open_last) interior[K] += c.x * (c.t - S.B[K-1]); }
        else interior[k] += c.x * (c.t - S.B[k-1]);
    }
    double running = 0.0; int k = K;                                                                // backward pass
    for (auto it = cf.rbegin(); it != cf.rend(); ++it) {
        while (k >= 1 && it->t < S.B[k]) { suffix[k] = running; --k; }
        running += it->x;
    }
    while (k >= 1) { suffix[k] = running; --k; }
    for (int kk = 1; kk < K; ++kk) out[kk-1] = -(interior[kk] + S.len(kk) * suffix[kk]);
    out[K-1] = -(interior[K] + (S.open_last ? 0.0 : S.len(K)) * suffix[K]);
}

inline void scan_projection(const Stencils& S, std::span<const ProjectionTerm> terms, double* out)
{
    const int K = S.K();
    for (int k = 0; k < K; ++k) out[k] = 0.0;
    for (const auto& r : terms)
        for (int k = S.bucket(r.a); k <= S.bucket(r.b); ++k) out[k-1] += r.w * (S.psi(k, r.a) - S.psi(k, r.b));
}

inline void sort_by_time(std::vector<UnitCashflow>& cf) { std::sort(cf.begin(), cf.end(), [](const UnitCashflow& a, const UnitCashflow& b){ return a.t < b.t; }); }

} // namespace ladder
