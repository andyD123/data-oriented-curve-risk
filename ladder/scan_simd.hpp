#pragma once
// Grouped reverse scan: eight instruments per vector, backward walk over payment columns, stencil snapshots.
// Output per group: [K_disc][8] then [K_proj][8], bucket-major, contiguous. Risk per unit forward bump.
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include "lanes.hpp"
#include "layout.hpp"
#include "stencil.hpp"

namespace ladder {

enum class Store { normal, streaming };

template <Store policy>
inline void store8(double* p, vec8 v) { if constexpr (policy == Store::streaming) vstore_stream(p, v); else vstore(p, v); }

// Shared coupon evaluation and wave traversals, used by both trade-level
// materialisation and portfolio-only reduction. The consumer decides whether
// each wave is stored or accumulated; the numerical traversal is identical.
namespace detail {

inline size_t checked_grouped_product(size_t a, size_t b) {
    if (b && a > std::numeric_limits<size_t>::max() / b)
        throw std::length_error("grouped scan: output size overflow");
    return a*b;
}

inline size_t grouped_row_stride(int Kd, int Kp) {
    if (Kd < 0 || Kp < 0)
        throw std::invalid_argument("grouped scan: negative wave count");
    return checked_grouped_product(static_cast<size_t>(Kd) +
                                   static_cast<size_t>(Kp), LANES);
}

inline size_t grouped_output_entries(const GroupLayout& L, int Kd, int Kp) {
    return checked_grouped_product(L.groups.size(), grouped_row_stride(Kd, Kp));
}

// Values depend on the refreshed curve table, not on the output policy.
// f_di = IBOR payment/start/end table indices; o_di = OIS equivalents.
inline void evaluate_grouped_coupons(
    const Group& G, const DateTable& T,
    std::vector<vec8>& famt, std::vector<vec8>& oamt_p,
    std::vector<vec8>& oamt_a, std::vector<vec8>& oamt_b) {
    const int nf = static_cast<int>(G.f_di.size());
    const int no = static_cast<int>(G.o_di.size());
    famt.resize(nf);
    oamt_p.resize(no); oamt_a.resize(no); oamt_b.resize(no);
    for (int q=0; q<nf; ++q) {
        const double ratio = T.P[G.f_di[q][1]] / T.P[G.f_di[q][2]];
        famt[q] = vmul(vload(G.f_scale[q].v), vbroadcast(ratio - 1.0));
    }
    for (int q=0; q<no; ++q) {
        const double Dp=T.D[G.o_di[q][0]], Da=T.D[G.o_di[q][1]];
        const double Db=T.D[G.o_di[q][2]], f=Dp*Da/Db;
        const vec8 N=vload(G.o_N[q].v);
        // The signed risk-weight pieces +A at a, -A at b,
        // and +(A-C) at p are distributed over date columns.
        oamt_p[q] = vmul(N, vbroadcast((f - Dp) / Dp));
        oamt_a[q] = vmul(N, vbroadcast(f / Da));
        oamt_b[q] = vmul(N, vbroadcast(-f / Db));
    }
}

// A single authoritative reverse traversal. Emitting wave b occurs when
// k==b-1; suffix_cur is the sum strictly beyond boundary B[b]. The
// open final wave retains its uncapped tail moment beyond B[K].
template <class Emit>
inline void grouped_discount_waves(
    const Group& G, const DateTable& T, const Stencils& Sd,
    const std::vector<vec8>& famt, const std::vector<vec8>& oamt_p,
    const std::vector<vec8>& oamt_a, const std::vector<vec8>& oamt_b,
    Emit&& emit_wave) {
    const int Kd = Sd.K();
    vec8 running=vzero(), interior=vzero(), suffix_cur=vzero(), beyond=vzero();
    int k=Kd, qf=static_cast<int>(G.f_di.size())-1;
    int qo=static_cast<int>(G.o_at.size())-1;
    const double BK=Sd.B[Kd], BK1=Sd.B[Kd-1];
    const auto emit=[&](int b) {
        if (b > Kd) return;
        const vec8 risk=(b==Kd && Sd.open_last)
            ? vneg(vadd(interior,beyond))
            : vneg(vfma(vbroadcast(Sd.len(b)),suffix_cur,interior));
        emit_wave(b,risk);
    };
    for (int ci=static_cast<int>(G.col_di.size())-1; ci>=0; --ci) {
        const int di=G.col_di[ci];
        const double t=T.t[di];
        while (k>=1 && t<Sd.B[k]) {
            emit(k+1);
            suffix_cur=running;
            interior=vzero();
            --k;
        }
        vec8 amount=vload(G.col_amt[ci].v);
        while(qf>=0 && G.f_col[qf]==ci) {
            amount=vadd(amount,famt[qf]);
            --qf;
        }
        while(qo>=0 && G.o_at[qo].first==ci) {
            const int q=G.o_at[qo].second/3;
            const int kind=G.o_at[qo].second%3;
            amount=vadd(amount,kind==0 ? oamt_p[q] :
                        kind==1 ? oamt_a[q] : oamt_b[q]);
            --qo;
        }
        const vec8 x=vmul(amount,vbroadcast(T.D[di]));
        running=vadd(running,x);
        if(t>=BK) {
            if(Sd.open_last)
                beyond=vfma(x,vbroadcast(t-BK1),beyond);
        } else if(t>=Sd.B.front()) {
            interior=vfma(x,vbroadcast(t-Sd.B[k]),interior);
        }
    }
    while(k>=1) {
        emit(k+1);
        suffix_cur=running;
        interior=vzero();
        --k;
    }
    emit(1);
}

// The projection ladder has nonzero support only where an IBOR fixing
// interval crosses a wave. The same accumulation order serves both
// trade-level storage and portfolio-only aggregation.
template <class Emit>
inline void grouped_projection_waves(
    const Group& G, const DateTable& T, const Stencils& Sp,
    std::vector<vec8>& pacc, Emit&& emit_wave) {
    const int Kp=Sp.K();
    pacc.resize(static_cast<size_t>(Kp)+1);
    for(int kk=0; kk<=Kp; ++kk) pacc[kk]=vzero();
    for(int q=0; q<static_cast<int>(G.f_di.size()); ++q) {
        const int ip=G.f_di[q][0], ia=G.f_di[q][1], ib=G.f_di[q][2];
        const double ta=T.t[ia], tb=T.t[ib];
        const vec8 w=vmul(vload(G.f_scale[q].v),
                          vbroadcast(T.P[ia]/T.P[ib]*T.D[ip]));
        for(int kk=T.bp[ia]; kk<=T.bp[ib]; ++kk)
            pacc[kk]=vfma(w,vbroadcast(Sp.psi(kk,ta)-Sp.psi(kk,tb)),
                          pacc[kk]);
    }
    for(int kk=1; kk<=Kp; ++kk) emit_wave(kk,pacc[kk]);
}
} // namespace detail

// Low-level kernel: caller must supply a 64-byte-aligned buffer holding
// groups*(Kd+Kp)*8 doubles. Use the span overload for size checking.
template <Store policy = Store::normal>
inline void scan_grouped(const GroupLayout& L, const Stencils& Sd,
                         const Stencils& Sp, double* out) {
    Sd.validate(); Sp.validate();
    if (!L.refreshed || L.discount_grid != Sd.B || L.projection_grid != Sp.B)
        throw std::invalid_argument("scan_grouped: refresh the date table for these stencils first");
    if (!L.groups.empty() && (!out || reinterpret_cast<std::uintptr_t>(out)%64!=0))
        throw std::invalid_argument("scan_grouped: output must be non-null and 64-byte aligned");
    const DateTable& T=L.table;
    const int Kd=Sd.K(), Kp=Sp.K();
    const size_t stride=detail::grouped_row_stride(Kd,Kp);
    (void)detail::grouped_output_entries(L,Kd,Kp); // reject size overflow
    std::vector<vec8> famt,oamt_p,oamt_a,oamt_b,pacc;
    for(size_t gi=0; gi<L.groups.size(); ++gi) {
        const Group& G=L.groups[gi];
        double* out_d=out+gi*stride;
        double* out_p=out_d+static_cast<size_t>(Kd)*LANES;
        detail::evaluate_grouped_coupons(G,T,famt,oamt_p,oamt_a,oamt_b);
        detail::grouped_discount_waves(G,T,Sd,famt,oamt_p,oamt_a,oamt_b,
            [&](int wave,vec8 risk) {
                store8<policy>(out_d+static_cast<size_t>(wave-1)*LANES,risk);
            });
        detail::grouped_projection_waves(G,T,Sp,pacc,
            [&](int wave,vec8 risk) {
                store8<policy>(out_p+static_cast<size_t>(wave-1)*LANES,risk);
            });
    }
    if constexpr(policy==Store::streaming) vfence();
}

// Checked alternative. It validates exact buffer length, and uses the same
// zero-allocation inner loop and alignment contract as the pointer API.
template <Store policy = Store::normal>
inline void scan_grouped(const GroupLayout& L, const Stencils& Sd,
                         const Stencils& Sp, std::span<double> out) {
    Sd.validate(); Sp.validate();
    const size_t required=detail::grouped_output_entries(L,Sd.K(),Sp.K());
    if (out.size()!=required)
        throw std::invalid_argument("scan_grouped: wrong output size");
    scan_grouped<policy>(L,Sd,Sp,out.data());
}

// per-instrument view of the grouped output: K_disc then K_proj values for instrument i
inline void gather(const GroupLayout& L, int Kd, int Kp, const double* out, int group, int lane, double* dst)
{
    const double* g = out + (size_t)group * L.stride(Kd, Kp);
    for (int k = 0; k < Kd + Kp; ++k) dst[k] = g[(size_t)k * LANES + lane];
}

} // namespace ladder
