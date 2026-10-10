#pragma once
// Grouped reverse scan: eight instruments per vector, backward walk over payment columns, stencil snapshots.
// Output per group: [K_disc][8] then [K_proj][8], bucket-major, contiguous. Risk per unit forward bump.
#include <cstdint>
#include <vector>
#include "lanes.hpp"
#include "layout.hpp"
#include "stencil.hpp"

namespace ladder {

enum class Store { normal, streaming };

template <Store policy>
inline void store8(double* p, vec8 v) { if constexpr (policy == Store::streaming) vstore_stream(p, v); else vstore(p, v); }

template <Store policy = Store::normal>
inline void scan_grouped(const GroupLayout& L, const Stencils& Sd, const Stencils& Sp, double* out)
{
    Sd.validate(); Sp.validate();
    if (!L.refreshed || L.discount_grid != Sd.B || L.projection_grid != Sp.B)
        throw std::invalid_argument("scan_grouped: refresh the date table for these stencils first");
    if (!L.groups.empty() && (!out || reinterpret_cast<std::uintptr_t>(out) % 64 != 0))
        throw std::invalid_argument("scan_grouped: output must be non-null and 64-byte aligned");
    const DateTable& T = L.table;
    const int Kd = Sd.K(), Kp = Sp.K();
    const size_t stride = L.stride(Kd, Kp);
    std::vector<vec8> famt, oamt_p, oamt_a, oamt_b, pacc((size_t)Kp + 1);
    for (size_t gi = 0; gi < L.groups.size(); ++gi) {
        const Group& G = L.groups[gi];
        double* out_d = out + gi * stride;
        double* out_p = out_d + (size_t)Kd * LANES;

        // per-coupon per-lane amounts that depend on the curves (broadcast factor x per-lane scale)
        const int nf = (int)G.f_di.size(), no = (int)G.o_di.size();
        famt.resize(nf); oamt_p.resize(no); oamt_a.resize(no); oamt_b.resize(no);
        for (int q = 0; q < nf; ++q) {                       // IBOR amount = scale * (P(a)/P(b) - 1)
            double ratio = T.P[G.f_di[q][1]] / T.P[G.f_di[q][2]];
            famt[q] = vmul(vload(G.f_scale[q].v), vbroadcast(ratio - 1.0));
        }
        for (int q = 0; q < no; ++q) {                       // OIS unit cashflows: +x at p, +x at a, -x at b, -N D(p) at p
            const double Dp = T.D[G.o_di[q][0]], Da = T.D[G.o_di[q][1]], Db = T.D[G.o_di[q][2]];
            const double f = Dp * Da / Db;                   // x = N f ; entries are divided by the column's D(t) below, so pre-divide
            vec8 N = vload(G.o_N[q].v);
            oamt_p[q] = vmul(N, vbroadcast((f - Dp) / Dp)); // column p: (x - N Dp) / Dp
            oamt_a[q] = vmul(N, vbroadcast(f / Da));         // column a:  x / Da
            oamt_b[q] = vmul(N, vbroadcast(-f / Db));        // column b: -x / Db
        }

        // ---- discount ladder: backward column walk; bucket b accumulates while k == b-1
        vec8 running = vzero(), interior = vzero(), suffix_cur = vzero(), beyond = vzero();   // beyond: open last stencil
        int k = Kd; int qf = nf - 1; int qo = (int)G.o_at.size() - 1;
        const double BK = Sd.B[Kd], BK1 = Sd.B[Kd-1];
        auto emit = [&](int b) {
            if (b > Kd) return;
            vec8 r = (b == Kd && Sd.open_last) ? vneg(vadd(interior, beyond))
                                               : vneg(vfma(vbroadcast(Sd.len(b)), suffix_cur, interior));
            store8<policy>(out_d + (size_t)(b - 1) * LANES, r);
        };
        for (int ci = (int)G.col_di.size() - 1; ci >= 0; --ci) {
            const int di = G.col_di[ci]; const double t = T.t[di];
            while (k >= 1 && t < Sd.B[k]) { emit(k + 1); suffix_cur = running; interior = vzero(); --k; }
            vec8 amt = vload(G.col_amt[ci].v);
            while (qf >= 0 && G.f_col[qf] == ci) { amt = vadd(amt, famt[qf]); --qf; }
            while (qo >= 0 && G.o_at[qo].first == ci) {        // OIS entries on this column
                const int q = G.o_at[qo].second / 3, kind = G.o_at[qo].second % 3;
                amt = vadd(amt, kind == 0 ? oamt_p[q] : kind == 1 ? oamt_a[q] : oamt_b[q]); --qo;
            }
            vec8 x = vmul(amt, vbroadcast(T.D[di]));
            running = vadd(running, x);
            if (t >= BK) { if (Sd.open_last) beyond = vfma(x, vbroadcast(t - BK1), beyond); }   // beyond the last boundary
            else if (t >= Sd.B.front()) interior = vfma(x, vbroadcast(t - Sd.B[k]), interior);                        // k == bucket(t) - 1 here
        }
        while (k >= 1) { emit(k + 1); suffix_cur = running; interior = vzero(); --k; }
        emit(1);

        // ---- projection ladder: interior only, forward over IBOR coupons
        for (int kk = 0; kk <= Kp; ++kk) pacc[kk] = vzero();
        for (int q = 0; q < nf; ++q) {
            const int ip = G.f_di[q][0], ia = G.f_di[q][1], ib = G.f_di[q][2];
            const double ta = T.t[ia], tb = T.t[ib];
            vec8 w = vmul(vload(G.f_scale[q].v), vbroadcast(T.P[ia] / T.P[ib] * T.D[ip]));
            for (int kk = T.bp[ia]; kk <= T.bp[ib]; ++kk) pacc[kk] = vfma(w, vbroadcast(Sp.psi(kk, ta) - Sp.psi(kk, tb)), pacc[kk]);
        }
        for (int kk = 1; kk <= Kp; ++kk) store8<policy>(out_p + (size_t)(kk - 1) * LANES, pacc[kk]);
    }
    if constexpr (policy == Store::streaming) vfence();
}

// per-instrument view of the grouped output: K_disc then K_proj values for instrument i
inline void gather(const GroupLayout& L, int Kd, int Kp, const double* out, int group, int lane, double* dst)
{
    const double* g = out + (size_t)group * L.stride(Kd, Kp);
    for (int k = 0; k < Kd + Kp; ++k) dst[k] = g[(size_t)k * LANES + lane];
}

// Direct In-Register Aggregation: accumulates book-level risk ladder into total_ladder (size Kd + Kp)
// entirely inside L1 cache / SIMD registers, avoiding writing individual group outputs to DRAM.
inline void scan_grouped_reduce(const GroupLayout& L, const Stencils& Sd, const Stencils& Sp, double* total_ladder)
{
    Sd.validate(); Sp.validate();
    if (!L.refreshed || L.discount_grid != Sd.B || L.projection_grid != Sp.B)
        throw std::invalid_argument("scan_grouped_reduce: refresh the date table for these stencils first");
    if (!total_ladder)
        throw std::invalid_argument("scan_grouped_reduce: null output total_ladder");

    const DateTable& T = L.table;
    const int Kd = Sd.K(), Kp = Sp.K();
    constexpr int STACK_LIMIT = 128;
    vec8 stack_famt[STACK_LIMIT];
    vec8 stack_oamt_p[STACK_LIMIT];
    vec8 stack_oamt_a[STACK_LIMIT];
    vec8 stack_oamt_b[STACK_LIMIT];
    vec8 stack_pacc[STACK_LIMIT];
    std::vector<vec8> heap_famt, heap_oamt_p, heap_oamt_a, heap_oamt_b, heap_pacc;

    std::vector<vec8> acc_d(Kd, vzero());
    std::vector<vec8> acc_p(Kp, vzero());

    for (size_t gi = 0; gi < L.groups.size(); ++gi) {
        const Group& G = L.groups[gi];
        const int nf = (int)G.f_di.size(), no = (int)G.o_di.size();
        vec8* famt = stack_famt;
        if (nf > STACK_LIMIT) {
            heap_famt.resize(nf);
            famt = heap_famt.data();
        }
        vec8* oamt_p = stack_oamt_p;
        vec8* oamt_a = stack_oamt_a;
        vec8* oamt_b = stack_oamt_b;
        if (no > STACK_LIMIT) {
            heap_oamt_p.resize(no); heap_oamt_a.resize(no); heap_oamt_b.resize(no);
            oamt_p = heap_oamt_p.data(); oamt_a = heap_oamt_a.data(); oamt_b = heap_oamt_b.data();
        }

        for (int q = 0; q < nf; ++q) {
            double ratio = T.P[G.f_di[q][1]] / T.P[G.f_di[q][2]];
            famt[q] = vmul(vload(G.f_scale[q].v), vbroadcast(ratio - 1.0));
        }
        for (int q = 0; q < no; ++q) {
            const double Dp = T.D[G.o_di[q][0]], Da = T.D[G.o_di[q][1]], Db = T.D[G.o_di[q][2]];
            const double f = Dp * Da / Db;
            vec8 N = vload(G.o_N[q].v);
            oamt_p[q] = vmul(N, vbroadcast((f - Dp) / Dp));
            oamt_a[q] = vmul(N, vbroadcast(f / Da));
            oamt_b[q] = vmul(N, vbroadcast(-f / Db));
        }

        vec8 running = vzero(), interior = vzero(), suffix_cur = vzero(), beyond = vzero();
        int k = Kd; int qf = nf - 1; int qo = (int)G.o_at.size() - 1;
        const double BK = Sd.B[Kd], BK1 = Sd.B[Kd-1];
        auto emit_acc = [&](int b) {
            if (b > Kd) return;
            vec8 r = (b == Kd && Sd.open_last) ? vneg(vadd(interior, beyond))
                                               : vneg(vfma(vbroadcast(Sd.len(b)), suffix_cur, interior));
            acc_d[b - 1] = vadd(acc_d[b - 1], r);
        };

        for (int ci = (int)G.col_di.size() - 1; ci >= 0; --ci) {
            const int di = G.col_di[ci];
            const double t = T.t[di];
            while (k >= 1 && t < Sd.B[k]) {
                emit_acc(k + 1);
                suffix_cur = running;
                interior = vzero();
                --k;
            }
            vec8 amt = vload(G.col_amt[ci].v);
            while (qf >= 0 && G.f_col[qf] == ci) { amt = vadd(amt, famt[qf]); --qf; }
            while (qo >= 0 && G.o_at[qo].first == ci) {
                const int q = G.o_at[qo].second / 3, kind = G.o_at[qo].second % 3;
                amt = vadd(amt, kind == 0 ? oamt_p[q] : kind == 1 ? oamt_a[q] : oamt_b[q]); --qo;
            }
            vec8 x = vmul(amt, vbroadcast(T.D[di]));
            running = vadd(running, x);
            if (t >= BK) { if (Sd.open_last) beyond = vfma(x, vbroadcast(t - BK1), beyond); }
            else if (t >= Sd.B.front()) interior = vfma(x, vbroadcast(t - Sd.B[k]), interior);
        }
        while (k >= 1) { emit_acc(k + 1); suffix_cur = running; interior = vzero(); --k; }
        emit_acc(1);

        vec8* pacc = stack_pacc;
        if (Kp + 1 > STACK_LIMIT) {
            heap_pacc.assign((size_t)Kp + 1, vzero());
            pacc = heap_pacc.data();
        } else {
            for (int kk = 0; kk <= Kp; ++kk) pacc[kk] = vzero();
        }
        for (int q = 0; q < nf; ++q) {
            const int ip = G.f_di[q][0], ia = G.f_di[q][1], ib = G.f_di[q][2];
            const double ta = T.t[ia], tb = T.t[ib];
            vec8 w = vmul(vload(G.f_scale[q].v), vbroadcast(T.P[ia] / T.P[ib] * T.D[ip]));
            for (int kk = T.bp[ia]; kk <= T.bp[ib]; ++kk) pacc[kk] = vfma(w, vbroadcast(Sp.psi(kk, ta) - Sp.psi(kk, tb)), pacc[kk]);
        }
        for (int kk = 1; kk <= Kp; ++kk) acc_p[kk - 1] = vadd(acc_p[kk - 1], pacc[kk]);
    }

    for (int k = 0; k < Kd; ++k) total_ladder[k] = vhsum(acc_d[k]);
    for (int k = 0; k < Kp; ++k) total_ladder[Kd + k] = vhsum(acc_p[k]);
}

} // namespace ladder
