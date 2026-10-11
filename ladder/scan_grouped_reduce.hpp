#pragma once
// Portfolio-level grouped first-order risk, adapted from PR #2 by damian123.
// Reuses the production GroupLayout and eight-lane primitives without
// modifying scan_grouped. Emits ONLY (Kd+Kp) aggregated risk doubles;
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
    const int Kd = Sd.K(), Kp = Sp.K();
    const double BK = Sd.B[Kd], BK1 = Sd.B[Kd - 1];
    scratch.prepare(Kd, Kp);

    for (const Group& G : L.groups) {
        const int nf = static_cast<int>(G.f_di.size());
        const int no = static_cast<int>(G.o_di.size());
        scratch.famt.resize(nf);
        scratch.oamt_p.resize(no);
        scratch.oamt_a.resize(no);
        scratch.oamt_b.resize(no);
        for (int q = 0; q < nf; ++q) {
            const double ratio = T.P[G.f_di[q][1]] / T.P[G.f_di[q][2]];
            scratch.famt[q] = vmul(vload(G.f_scale[q].v),
                                  vbroadcast(ratio - 1.0));
        }
        for (int q = 0; q < no; ++q) {
            const int ip = G.o_di[q][0], ia = G.o_di[q][1], ib = G.o_di[q][2];
            const double Dp = T.D[ip], Da = T.D[ia], Db = T.D[ib];
            const double f = Dp * Da / Db;
            const vec8 N = vload(G.o_N[q].v);
            // Same first-order signed dated-weight decomposition as scan_grouped.
            scratch.oamt_p[q] = vmul(N, vbroadcast((f - Dp) / Dp));
            scratch.oamt_a[q] = vmul(N, vbroadcast(f / Da));
            scratch.oamt_b[q] = vmul(N, vbroadcast(-f / Db));
        }

        vec8 running = vzero(), interior = vzero();
        vec8 suffix_cur = vzero(), beyond = vzero();
        int k = Kd, qf = nf - 1, qo = static_cast<int>(G.o_at.size()) - 1;
        const auto emit_discount = [&](int bucket) {
            if (bucket > Kd) return;
            const vec8 risk = (bucket == Kd && Sd.open_last)
                ? vneg(vadd(interior, beyond))
                : vneg(vfma(vbroadcast(Sd.len(bucket)), suffix_cur, interior));
            scratch.acc_d[bucket - 1] =
                vadd(scratch.acc_d[bucket - 1], risk);
        };
        for (int ci = static_cast<int>(G.col_di.size()) - 1; ci >= 0; --ci) {
            const int di = G.col_di[ci];
            const double t = T.t[di];
            while (k >= 1 && t < Sd.B[k]) {
                emit_discount(k + 1);
                suffix_cur = running;
                interior = vzero();
                --k;
            }
            vec8 amount = vload(G.col_amt[ci].v);
            while (qf >= 0 && G.f_col[qf] == ci) {
                amount = vadd(amount, scratch.famt[qf]);
                --qf;
            }
            while (qo >= 0 && G.o_at[qo].first == ci) {
                const int q = G.o_at[qo].second / 3;
                const int kind = G.o_at[qo].second % 3;
                amount = vadd(amount,
                    kind == 0 ? scratch.oamt_p[q] :
                    kind == 1 ? scratch.oamt_a[q] : scratch.oamt_b[q]);
                --qo;
            }
            const vec8 x = vmul(amount, vbroadcast(T.D[di]));
            running = vadd(running, x);
            if (t >= BK) {
                if (Sd.open_last)
                    beyond = vfma(x, vbroadcast(t - BK1), beyond);
            } else if (t >= Sd.B.front()) {
                interior = vfma(x, vbroadcast(t - Sd.B[k]), interior);
            }
        }
        while (k >= 1) {
            emit_discount(k + 1);
            suffix_cur = running;
            interior = vzero();
            --k;
        }
        emit_discount(1);

        std::fill(scratch.pacc.begin(), scratch.pacc.end(), vzero());
        for (int q = 0; q < nf; ++q) {
            const int ip = G.f_di[q][0];
            const int ia = G.f_di[q][1], ib = G.f_di[q][2];
            const double ta = T.t[ia], tb = T.t[ib];
            const vec8 w = vmul(vload(G.f_scale[q].v),
                               vbroadcast(T.P[ia] / T.P[ib] * T.D[ip]));
            for (int wave = T.bp[ia]; wave <= T.bp[ib]; ++wave) {
                scratch.pacc[wave] = vfma(
                    w, vbroadcast(Sp.psi(wave, ta) - Sp.psi(wave, tb)),
                    scratch.pacc[wave]);
            }
        }
        for (int wave = 1; wave <= Kp; ++wave)
            scratch.acc_p[wave - 1] =
                vadd(scratch.acc_p[wave - 1], scratch.pacc[wave]);
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
    if (total_ladder.size() != static_cast<size_t>(Sd.K() + Sp.K()))
        throw std::invalid_argument("scan_grouped_reduce: wrong output size");
    scan_grouped_reduce(L, Sd, Sp, total_ladder.data(), scratch);
}
} // namespace ladder
