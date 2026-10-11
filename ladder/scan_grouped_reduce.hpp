#pragma once
// Portfolio-level grouped first-order risk, adapted from PR #2 by damian123.
// Shares the exact coupon evaluations and wave traversals with scan_grouped,
// but accumulates rather than materialising per-instrument ladders.
// Emits ONLY (Kd+Kp) aggregated risk doubles;
// no per-instrument AoSoA risk buffer is written.
#include <algorithm>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>
#include "scan_simd.hpp"

namespace ladder {

// Caller-owned, reusable storage: one instance per concurrent worker.
// The public convenience overload constructs scratch once per invocation.
struct GroupReduceScratch {
    std::vector<vec8> famt, oamt_p, oamt_a, oamt_b;
    std::vector<vec8> pacc, acc_d, acc_p;

    void prepare(int Kd, int Kp) {
        acc_d.assign(static_cast<size_t>(Kd), vzero());
        acc_p.assign(static_cast<size_t>(Kp), vzero());
        pacc.resize(static_cast<size_t>(Kp) + 1);
    }
};

// Horizontal sum through the existing lane API. In particular, do not
// introduce AVX-specific intrinsics into the scalar/portable expression.
inline double grouped_reduce_horizontal_sum(vec8 value) {
    alignas(64) double lanes[LANES];
    vstore(lanes, value);
    double result = 0.0;
    for (int lane = 0; lane < LANES; ++lane) result += lanes[lane];
    return result;
}

inline void scan_grouped_reduce(const GroupLayout& L,
                                const Stencils& Sd,
                                const Stencils& Sp,
                                double* total_ladder,
                                GroupReduceScratch& scratch) {
    Sd.validate();
    Sp.validate();
    if (!L.refreshed || L.discount_grid != Sd.B ||
        L.projection_grid != Sp.B)
        throw std::invalid_argument(
            "scan_grouped_reduce: refresh the date table for these stencils first");
    if (!total_ladder)
        throw std::invalid_argument("scan_grouped_reduce: null output");
    const DateTable& T = L.table;
    const int Kd=Sd.K(), Kp=Sp.K();
    scratch.prepare(Kd,Kp);
    for(const Group& G : L.groups) {
        detail::evaluate_grouped_coupons(
            G,T,scratch.famt,scratch.oamt_p,scratch.oamt_a,scratch.oamt_b);
        detail::grouped_discount_waves(
            G,T,Sd,scratch.famt,scratch.oamt_p,scratch.oamt_a,scratch.oamt_b,
            [&](int wave,vec8 risk) {
                scratch.acc_d[wave-1]=vadd(scratch.acc_d[wave-1],risk);
            });
        detail::grouped_projection_waves(G,T,Sp,scratch.pacc,
            [&](int wave,vec8 risk) {
                scratch.acc_p[wave-1]=vadd(scratch.acc_p[wave-1],risk);
            });
    }
    // Only the final Kd+Kp values are written to memory. No aligned
    // output requirement: each is a scalar portfolio total.
    for (int wave = 0; wave < Kd; ++wave)
        total_ladder[wave] = grouped_reduce_horizontal_sum(scratch.acc_d[wave]);
    for (int wave = 0; wave < Kp; ++wave)
        total_ladder[Kd + wave] = grouped_reduce_horizontal_sum(scratch.acc_p[wave]);
}

inline void scan_grouped_reduce(const GroupLayout& L,
                                const Stencils& Sd,
                                const Stencils& Sp,
                                double* total_ladder) {
    GroupReduceScratch scratch;
    scan_grouped_reduce(L, Sd, Sp, total_ladder, scratch);
}

// Sized-output form for callers who want explicit buffer validation.
inline void scan_grouped_reduce(const GroupLayout& L,
                                const Stencils& Sd,
                                const Stencils& Sp,
                                std::span<double> total_ladder,
                                GroupReduceScratch& scratch) {
    Sd.validate();
    Sp.validate();
    if (total_ladder.size() != static_cast<size_t>(Sd.K()) + static_cast<size_t>(Sp.K()))
        throw std::invalid_argument("scan_grouped_reduce: wrong output size");
    scan_grouped_reduce(L, Sd, Sp, total_ladder.data(), scratch);
}

// Convenience checked overload for one-off callers without retained scratch.
inline void scan_grouped_reduce(const GroupLayout& L,
                                const Stencils& Sd,
                                const Stencils& Sp,
                                std::span<double> total_ladder) {
    GroupReduceScratch scratch;
    scan_grouped_reduce(L, Sd, Sp, total_ladder, scratch);
}
} // namespace ladder
