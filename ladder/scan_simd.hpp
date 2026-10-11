#pragma once
// Grouped reverse scan. Hardware vectors are eight doubles wide. A group may hold
// several of those blocks (build_layout's group_lanes) so one date and bucket walk
// is shared. The published benchmarks use one block per group.
// Output for group g starts at vec8_prefix * (K_disc + K_proj) * 8 doubles:
//   for each bucket, `blocks` consecutive eight-lane vectors, discount then projection.
// Risk per unit forward bump.
#include <cstddef>
#include <cstdint>
#include <vector>
#include "lanes.hpp"
#include "layout.hpp"
#include "stencil.hpp"

namespace ladder {

enum class Store { normal, streaming };

// Which part of the grouped kernel to run. `all` is the timed production path.
// The other three exist so a benchmark can attribute amount formation, the
// discount walk and the projection walk. They are not summed into the published times.
enum class ScanPart { all, amounts, discount, projection };

template <Store policy>
inline void store8(double* p, vec8 v) { if constexpr (policy == Store::streaming) vstore_stream(p, v); else vstore(p, v); }

// Reused across groups inside one kernel call. One instance per caller; not shared across threads.
struct GroupedScanScratch {
    std::vector<vec8> famt, oamt_p, oamt_a, oamt_b, pacc;
    std::vector<vec8> running, interior, suffix_cur, beyond;
    std::vector<vec8> acc_d, acc_p;
};

namespace detail {

// Per-coupon per-lane amounts for one eight-wide block. famt/oamt_* have one entry per coupon.
// When the group has no realised overnight factor the discount-leg expression is the original one,
// so the width-8 kernel stays on the same rounding.
inline void load_one_block_amounts(const Group& G, const DateTable& T, int block,
                                   vec8* famt, vec8* oamt_p, vec8* oamt_a, vec8* oamt_b) {
    const int blocks = G.blocks;
    const int nf = (int)G.f_di.size(), no = (int)G.o_di.size();
    for (int q = 0; q < nf; ++q) {
        const double ratio = T.P[G.f_di[q][1]] / T.P[G.f_di[q][2]];
        famt[q] = vmul(vload(G.f_scale[(size_t)q * blocks + block].v), vbroadcast(ratio - 1.0));
    }
    for (int q = 0; q < no; ++q) {
        const double Dp = T.D[G.o_di[q][0]], Da = T.D[G.o_di[q][1]], Db = T.D[G.o_di[q][2]];
        const double f = Dp * Da / Db;
        const vec8 N = vload(G.o_N[(size_t)q * blocks + block].v);
        if (!G.realised_ois) {
            oamt_p[q] = vmul(N, vbroadcast((f - Dp) / Dp));
            oamt_a[q] = vmul(N, vbroadcast(f / Da));
            oamt_b[q] = vmul(N, vbroadcast(-f / Db));
        } else {
            // x = (N*H) * D(p) * D(a) / D(b), minus N*D(p). H is carried in o_NH = sum N*realised.
            const vec8 NH = vload(G.o_NH[(size_t)q * blocks + block].v);
            oamt_p[q] = vadd(vmul(NH, vbroadcast(f / Dp)), vneg(N));
            oamt_a[q] = vmul(NH, vbroadcast(f / Da));
            oamt_b[q] = vmul(NH, vbroadcast(-f / Db));
        }
    }
}

template <Store policy, ScanPart part, bool Reduce>
inline void scan_grouped_impl(const GroupLayout& L, const Stencils& Sd, const Stencils& Sp, double* out,
                              GroupedScanScratch& scratch, size_t g_begin, size_t g_end) {
    Sd.validate(); Sp.validate();
    if (!L.refreshed || L.discount_grid != Sd.B || L.projection_grid != Sp.B)
        throw std::invalid_argument("scan_grouped: refresh the date table for these stencils first");
    if (g_end > L.groups.size()) g_end = L.groups.size();
    if (g_begin > g_end) throw std::invalid_argument("scan_grouped: empty group range is inverted");
    const bool any = g_begin < g_end;
    if constexpr (!Reduce) {
        if (any && (!out || reinterpret_cast<std::uintptr_t>(out) % 64 != 0))
            throw std::invalid_argument("scan_grouped: output must be non-null and 64-byte aligned");
    } else if (!out) {
        throw std::invalid_argument("scan_grouped_reduce: null output total_ladder");
    }

    const DateTable& T = L.table;
    const int Kd = Sd.K(), Kp = Sp.K();
    int max_blocks = 1, max_nf = 0, max_no = 0;
    for (size_t gi = g_begin; gi < g_end; ++gi) {
        const Group& G = L.groups[gi];
        max_blocks = std::max(max_blocks, G.blocks);
        max_nf = std::max(max_nf, (int)G.f_di.size());
        max_no = std::max(max_no, (int)G.o_di.size());
    }
    scratch.famt.resize((size_t)max_blocks * max_nf);
    scratch.oamt_p.resize((size_t)max_blocks * max_no);
    scratch.oamt_a.resize((size_t)max_blocks * max_no);
    scratch.oamt_b.resize((size_t)max_blocks * max_no);
    scratch.pacc.resize((size_t)max_blocks * ((size_t)Kp + 1));
    scratch.running.resize(max_blocks);
    scratch.interior.resize(max_blocks);
    scratch.suffix_cur.resize(max_blocks);
    scratch.beyond.resize(max_blocks);
    if constexpr (Reduce) {
        scratch.acc_d.assign(Kd, vzero());
        scratch.acc_p.assign(Kp, vzero());
    }

    const bool do_amounts = part != ScanPart::projection;
    const bool do_discount = part == ScanPart::all || part == ScanPart::discount;
    const bool do_projection = part == ScanPart::all || part == ScanPart::projection;
    volatile double sink = 0.0;

    for (size_t gi = g_begin; gi < g_end; ++gi) {
        const Group& G = L.groups[gi];
        const int B = G.blocks;
        const int nf = (int)G.f_di.size(), no = (int)G.o_di.size();
        double* out_g = nullptr;
        if constexpr (!Reduce) {
            if (part != ScanPart::amounts)
                out_g = out + G.vec8_prefix * (size_t)(Kd + Kp) * LANES;
        }

        if (do_amounts || do_discount) {
            for (int b = 0; b < B; ++b) {
                if (nf == 0 && no == 0) break;
                load_one_block_amounts(G, T, b,
                    nf ? scratch.famt.data() + (size_t)b * nf : nullptr,
                    no ? scratch.oamt_p.data() + (size_t)b * no : nullptr,
                    no ? scratch.oamt_a.data() + (size_t)b * no : nullptr,
                    no ? scratch.oamt_b.data() + (size_t)b * no : nullptr);
            }
        }
        if (part == ScanPart::amounts) {
            alignas(64) double lane[LANES];
            if (nf) { vstore(lane, scratch.famt[(size_t)(B - 1) * nf + (nf - 1)]); sink += lane[0]; }
            if (no) { vstore(lane, scratch.oamt_p[(size_t)(B - 1) * no + (no - 1)]); sink += lane[0]; }
            continue;
        }

        if (do_discount) {
            for (int b = 0; b < B; ++b)
                scratch.running[b] = scratch.interior[b] = scratch.suffix_cur[b] = scratch.beyond[b] = vzero();
            int k = Kd, qf = nf - 1, qo = (int)G.o_at.size() - 1;
            const double BK = Sd.B[Kd], BK1 = Sd.B[Kd - 1];
            auto emit = [&](int bucket) {
                if (bucket > Kd) return;
                const size_t base = (size_t)(bucket - 1) * B;
                for (int b = 0; b < B; ++b) {
                    const vec8 r = (bucket == Kd && Sd.open_last)
                        ? vneg(vadd(scratch.interior[b], scratch.beyond[b]))
                        : vneg(vfma(vbroadcast(Sd.len(bucket)), scratch.suffix_cur[b], scratch.interior[b]));
                    if constexpr (Reduce) scratch.acc_d[bucket - 1] = vadd(scratch.acc_d[bucket - 1], r);
                    else store8<policy>(out_g + (base + b) * LANES, r);
                }
            };
            for (int ci = (int)G.col_di.size() - 1; ci >= 0; --ci) {
                const int di = G.col_di[ci];
                const double t = T.t[di];
                while (k >= 1 && t < Sd.B[k]) {
                    emit(k + 1);
                    for (int b = 0; b < B; ++b) { scratch.suffix_cur[b] = scratch.running[b]; scratch.interior[b] = vzero(); }
                    --k;
                }
                const bool beyond_last = t >= BK;
                const bool in_interior = !beyond_last && t >= Sd.B.front();
                const vec8 D = vbroadcast(T.D[di]);
                const vec8 w = in_interior ? vbroadcast(t - Sd.B[k]) : vzero();
                const vec8 wb = (beyond_last && Sd.open_last) ? vbroadcast(t - BK1) : vzero();
                const int qf_hi = qf;
                while (qf >= 0 && G.f_col[qf] == ci) --qf;
                const int qf_lo = qf + 1;
                const int qo_hi = qo;
                while (qo >= 0 && G.o_at[qo].first == ci) --qo;
                const int qo_lo = qo + 1;
                for (int b = 0; b < B; ++b) {
                    vec8 amt = vload(G.col_amt[(size_t)ci * B + b].v);
                    for (int q = qf_hi; q >= qf_lo; --q) amt = vadd(amt, scratch.famt[(size_t)b * nf + q]);
                    for (int e = qo_hi; e >= qo_lo; --e) {
                        const int q = G.o_at[e].second / 3, kind = G.o_at[e].second % 3;
                        const vec8* src = kind == 0 ? scratch.oamt_p.data() : kind == 1 ? scratch.oamt_a.data() : scratch.oamt_b.data();
                        amt = vadd(amt, src[(size_t)b * no + q]);
                    }
                    const vec8 x = vmul(amt, D);
                    scratch.running[b] = vadd(scratch.running[b], x);
                    if (beyond_last) { if (Sd.open_last) scratch.beyond[b] = vfma(x, wb, scratch.beyond[b]); }
                    else if (in_interior) scratch.interior[b] = vfma(x, w, scratch.interior[b]);
                }
            }
            while (k >= 1) {
                emit(k + 1);
                for (int b = 0; b < B; ++b) { scratch.suffix_cur[b] = scratch.running[b]; scratch.interior[b] = vzero(); }
                --k;
            }
            emit(1);
        }

        if (do_projection) {
            for (int i = 0; i < B * (Kp + 1); ++i) scratch.pacc[i] = vzero();
            for (int b = 0; b < B; ++b) {
                for (int q = 0; q < nf; ++q) {
                    const int ip = G.f_di[q][0], ia = G.f_di[q][1], ib = G.f_di[q][2];
                    const double ta = T.t[ia], tb = T.t[ib];
                    const vec8 weight = vmul(vload(G.f_scale[(size_t)q * B + b].v), vbroadcast(T.P[ia] / T.P[ib] * T.D[ip]));
                    for (int kk = T.bp[ia]; kk <= T.bp[ib]; ++kk)
                        scratch.pacc[(size_t)kk * B + b] = vfma(weight, vbroadcast(Sp.psi(kk, ta) - Sp.psi(kk, tb)), scratch.pacc[(size_t)kk * B + b]);
                }
            }
            for (int kk = 1; kk <= Kp; ++kk) {
                for (int b = 0; b < B; ++b) {
                    const vec8 r = scratch.pacc[(size_t)kk * B + b];
                    if constexpr (Reduce) scratch.acc_p[kk - 1] = vadd(scratch.acc_p[kk - 1], r);
                    else store8<policy>(out_g + ((size_t)Kd + (kk - 1)) * B * LANES + (size_t)b * LANES, r);
                }
            }
        }
    }
    if constexpr (Reduce) {
        for (int k = 0; k < Kd; ++k) out[k] = vhsum(scratch.acc_d[k]);
        for (int k = 0; k < Kp; ++k) out[Kd + k] = vhsum(scratch.acc_p[k]);
    } else if constexpr (policy == Store::streaming) {
        if (part != ScanPart::amounts) vfence();
    }
    if (sink == 1e300) out[0] += sink; // keep the amount-only sink live without changing finite output
}

} // namespace detail

template <Store policy = Store::normal>
inline void scan_grouped(const GroupLayout& L, const Stencils& Sd, const Stencils& Sp, double* out,
                         size_t g_begin = 0, size_t g_end = static_cast<size_t>(-1)) {
    GroupedScanScratch scratch;
    detail::scan_grouped_impl<policy, ScanPart::all, false>(L, Sd, Sp, out, scratch, g_begin, g_end);
}

template <Store policy = Store::normal>
inline void scan_grouped_part(ScanPart part, const GroupLayout& L, const Stencils& Sd, const Stencils& Sp, double* out,
                              size_t g_begin = 0, size_t g_end = static_cast<size_t>(-1)) {
    GroupedScanScratch scratch;
    if (part == ScanPart::amounts) detail::scan_grouped_impl<policy, ScanPart::amounts, false>(L, Sd, Sp, out, scratch, g_begin, g_end);
    else if (part == ScanPart::discount) detail::scan_grouped_impl<policy, ScanPart::discount, false>(L, Sd, Sp, out, scratch, g_begin, g_end);
    else if (part == ScanPart::projection) detail::scan_grouped_impl<policy, ScanPart::projection, false>(L, Sd, Sp, out, scratch, g_begin, g_end);
    else detail::scan_grouped_impl<policy, ScanPart::all, false>(L, Sd, Sp, out, scratch, g_begin, g_end);
}

// per-instrument view of the grouped output: K_disc then K_proj values for instrument lane of a group
inline void gather(const GroupLayout& L, int Kd, int Kp, const double* out, int group, int lane, double* dst) {
    const Group& G = L.groups[group];
    const double* g = out + G.vec8_prefix * (size_t)(Kd + Kp) * LANES;
    const int block = lane / LANES, sub = lane % LANES;
    for (int k = 0; k < Kd + Kp; ++k) dst[k] = g[((size_t)k * G.blocks + block) * LANES + sub];
}

// Book-level ladder in registers. Same walk as scan_grouped; nothing is written per instrument.
inline void scan_grouped_reduce(const GroupLayout& L, const Stencils& Sd, const Stencils& Sp, double* total_ladder) {
    GroupedScanScratch scratch;
    detail::scan_grouped_impl<Store::normal, ScanPart::all, true>(L, Sd, Sp, total_ladder, scratch, 0, static_cast<size_t>(-1));
}

} // namespace ladder
