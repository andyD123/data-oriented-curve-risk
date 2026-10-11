#pragma once
// Optional Stage-2 grouped discount gamma. Uses the *existing* eight-lane
// layout, date table and SIMD primitives. No changes to first-order kernels.
// Only the DISCOUNT-curve diagonal is computed. Pure projection and mixed
// discount/projection Hessian blocks are out of scope.
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <span>
#include <vector>
#include "scan_simd.hpp"

namespace ladder {

// One scratch instance per concurrent caller, reused across groups and runs.
struct GroupGammaScratch {
    std::vector<vec8> ibor, ois_p, ois_a, ois_b, ois_diag, portfolio_acc;
    std::vector<double> hvp_work;
    std::vector<std::uint64_t> tags;
    std::uint64_t epoch = 0;

    void prepare(const Group& G, int K) {
        ibor.resize(G.f_di.size());
        ois_p.resize(G.o_di.size());
        ois_a.resize(G.o_di.size());
        ois_b.resize(G.o_di.size());
        if (ois_diag.size() < static_cast<size_t>(K + 1))
            ois_diag.resize(K + 1);
        if (tags.size() < static_cast<size_t>(K + 1))
            tags.resize(K + 1, 0);
        if (++epoch == 0) {
            std::fill(tags.begin(), tags.end(), 0);
            epoch = 1;
        }
    }
};

inline size_t grouped_gamma_stride(int Kd) {
    return static_cast<size_t>(Kd) * LANES;
}

// Output mode is a compile-time choice: either [group][discount wave][8]
// (64-byte aligned), or *only* K aggregated portfolio diagonal values.
// Both are fully forecast same-curve OIS corrected, including payment lag.
// IBOR amounts depend on projection, held fixed for discount-only gamma.
template <Store policy, bool PortfolioAggregate>
inline void scan_grouped_gamma_diagonal_kernel(
    const GroupLayout& L, const Stencils& Sd, double* out,
    GroupGammaScratch& scratch) {
    Sd.validate();
    if (!L.refreshed || L.discount_grid != Sd.B)
        throw std::invalid_argument("grouped gamma: refresh the date table for these waves first");
    if constexpr (PortfolioAggregate) {
        if (!out) throw std::invalid_argument("grouped gamma portfolio: null output");
        scratch.portfolio_acc.resize(Sd.K());
        std::fill(scratch.portfolio_acc.begin(), scratch.portfolio_acc.end(), vzero());
    } else {
        if (!L.groups.empty() && (!out || reinterpret_cast<std::uintptr_t>(out) % 64 != 0))
            throw std::invalid_argument("grouped gamma: output must be 64-byte aligned");
    }
    const DateTable& T = L.table;
    const int K = Sd.K();
    const double BK = Sd.B[K], BK1 = Sd.B[K - 1];
    const size_t stride = grouped_gamma_stride(K);

    for (size_t gi = 0; gi < L.groups.size(); ++gi) {
        const Group& G = L.groups[gi];
        scratch.prepare(G, K);
        const int nf = static_cast<int>(G.f_di.size());
        const int no = static_cast<int>(G.o_di.size());
        for (int q = 0; q < nf; ++q) {
            const double ratio = T.P[G.f_di[q][1]] / T.P[G.f_di[q][2]];
            scratch.ibor[q] = vmul(vload(G.f_scale[q].v), vbroadcast(ratio - 1.0));
        }
        for (int q = 0; q < no; ++q) {
            const int ip = G.o_di[q][0], ia = G.o_di[q][1], ib = G.o_di[q][2];
            const double Dp = T.D[ip], Da = T.D[ia], Db = T.D[ib];
            const double f = Dp * Da / Db;
            const vec8 N = vload(G.o_N[q].v);
            scratch.ois_p[q] = vmul(N, vbroadcast((f - Dp) / Dp));
            scratch.ois_a[q] = vmul(N, vbroadcast(f / Da));
            scratch.ois_b[q] = vmul(N, vbroadcast(-f / Db));
            const double a = T.t[ia], b = T.t[ib], p = T.t[ip];
            if (p < b) throw std::invalid_argument(
                "grouped gamma: OIS payment precedes accrual end");

            // r = h(b)-h(a), s = h(p)-h(b) live on consecutive time
            // intervals. Their intersection lies in at most the one wave
            // containing b. Thus diagonal correction -2*A*r*s takes ONE
            // vector update per coupon rather than an O(K) bucket sweep.
            const int wave = Sd.bucket(b);
            const double r = Sd.overlap(wave, b) - Sd.overlap(wave, a);
            const double s = Sd.overlap(wave, p) - Sd.overlap(wave, b);
            if (r != 0.0 && s != 0.0) {
                if (scratch.tags[wave] != scratch.epoch) {
                    scratch.tags[wave] = scratch.epoch;
                    scratch.ois_diag[wave] = vzero();
                }
                scratch.ois_diag[wave] = vfma(
                    N, vbroadcast(-2.0 * f * r * s), scratch.ois_diag[wave]);
            }
        }

        double* dst = nullptr;
        if constexpr (!PortfolioAggregate) dst = out + gi * stride;
        vec8 running = vzero(), interior2 = vzero();
        vec8 suffix_cur = vzero(), beyond2 = vzero();
        int k = K, qf = nf - 1, qo = static_cast<int>(G.o_at.size()) - 1;
        const auto emit = [&](int bucket) {
            if (bucket > K) return;
            vec8 val = (bucket == K && Sd.open_last)
                ? vadd(interior2, beyond2)
                : vfma(vbroadcast(Sd.len(bucket) * Sd.len(bucket)), suffix_cur, interior2);
            if (scratch.tags[bucket] == scratch.epoch)
                val = vadd(val, scratch.ois_diag[bucket]);
            if constexpr (PortfolioAggregate)
                scratch.portfolio_acc[bucket - 1] =
                    vadd(scratch.portfolio_acc[bucket - 1], val);
            else
                store8<policy>(dst + static_cast<size_t>(bucket - 1) * LANES, val);
        };
        for (int ci = static_cast<int>(G.col_di.size()) - 1; ci >= 0; --ci) {
            const int di = G.col_di[ci];
            const double t = T.t[di];
            while (k >= 1 && t < Sd.B[k]) {
                emit(k + 1);
                suffix_cur = running;
                interior2 = vzero();
                --k;
            }
            vec8 amt = vload(G.col_amt[ci].v);
            while (qf >= 0 && G.f_col[qf] == ci) {
                amt = vadd(amt, scratch.ibor[qf]); --qf;
            }
            while (qo >= 0 && G.o_at[qo].first == ci) {
                const int q = G.o_at[qo].second / 3;
                const int kind = G.o_at[qo].second % 3;
                amt = vadd(amt, kind == 0 ? scratch.ois_p[q]
                                           : kind == 1 ? scratch.ois_a[q] : scratch.ois_b[q]);
                --qo;
            }
            const vec8 x = vmul(amt, vbroadcast(T.D[di]));
            running = vadd(running, x);
            if (t >= BK) {
                if (Sd.open_last) {
                    const double dt = t - BK1;
                    beyond2 = vfma(x, vbroadcast(dt * dt), beyond2);
                }
            } else if (t >= Sd.B.front()) {
                const double dt = t - Sd.B[k];
                interior2 = vfma(x, vbroadcast(dt * dt), interior2);
            }
        }
        while (k >= 1) {
            emit(k + 1);
            suffix_cur = running;
            interior2 = vzero();
            --k;
        }
        emit(1);
    }
    if constexpr (PortfolioAggregate) {
        alignas(64) double lane_sum[LANES];
        for (int bucket = 0; bucket < K; ++bucket) {
            vstore(lane_sum, scratch.portfolio_acc[bucket]);
            double total = 0.0;
            for (int lane = 0; lane < LANES; ++lane) total += lane_sum[lane];
            out[bucket] = total;
        }
    } else if constexpr (policy == Store::streaming) vfence();
}

template <Store policy = Store::normal>
inline void scan_grouped_gamma_diagonal(
    const GroupLayout& L, const Stencils& Sd, double* out,
    GroupGammaScratch& scratch) {
    scan_grouped_gamma_diagonal_kernel<policy, false>(L, Sd, out, scratch);
}

// Returns K portfolio values, not N*K values. There is still a coupon
// traversal; eliminating output memory traffic does not eliminate the work.
inline void scan_grouped_gamma_portfolio_diagonal(
    const GroupLayout& L, const Stencils& Sd, double* out,
    GroupGammaScratch& scratch) {
    scan_grouped_gamma_diagonal_kernel<Store::normal, true>(L, Sd, out, scratch);
}

// Convenience overload, avoids per-GROUP allocations but creates scratch
// once per call. Reuse explicit GroupGammaScratch for repeated curves.
template <Store policy = Store::normal>
inline void scan_grouped_gamma_diagonal(
    const GroupLayout& L, const Stencils& Sd, double* out) {
    GroupGammaScratch scratch;
    scan_grouped_gamma_diagonal<policy>(L, Sd, out, scratch);
}


// Zero-copy lookup over the existing first-order AoSoA and the corrected
// gamma diagonal. Both buffers and the refreshed layout must outlive the
// view. H[j,j] is ALREADY OIS-corrected; off-diagonal entries include the
// exact lag correction on demand, not an extra N*K*K output allocation.
struct GroupedDiscountCrossGammaView {
    const GroupLayout& layout;
    const Stencils& discount_waves;
    int Kp;
    const double* first_order;
    const double* diagonal;

    GroupedDiscountCrossGammaView(const GroupLayout& L, const Stencils& Sd,
                                  int projection_buckets, const double* delta,
                                  const double* gamma)
        : layout(L), discount_waves(Sd), Kp(projection_buckets),
          first_order(delta), diagonal(gamma) {
        Sd.validate();
        if (Kp < 0 || !L.refreshed || L.discount_grid != Sd.B ||
            (!L.groups.empty() && (!delta || !gamma)))
            throw std::invalid_argument("grouped gamma view: stale table or null buffers");
    }

    const Group& group(size_t gi, int lane) const {
        if (gi >= layout.groups.size() || lane < 0 ||
            lane >= layout.groups[gi].n_valid)
            throw std::out_of_range("grouped gamma view: invalid group/lane");
        return layout.groups[gi];
    }

    double gradient(size_t gi, int lane, int j) const {
        return first_order[gi * layout.stride(discount_waves.K(), Kp)
                           + static_cast<size_t>(j) * LANES + lane];
    }
    double gamma_diagonal(size_t gi, int lane, int j) const {
        return diagonal[gi * grouped_gamma_stride(discount_waves.K())
                        + static_cast<size_t>(j) * LANES + lane];
    }

    double at(size_t gi, int lane, int j, int k) const {
        const Group& G = group(gi, lane);
        const int K = discount_waves.K();
        if (j < 0 || k < 0 || j >= K || k >= K)
            throw std::out_of_range("grouped gamma view: wave index");
        if (j == k) return gamma_diagonal(gi, lane, j);
        const int lo = std::min(j, k), hi = std::max(j, k);
        double result = -discount_waves.len(lo + 1) * gradient(gi, lane, hi);
        for (size_t q = 0; q < G.o_di.size(); ++q) {
            const double N = G.o_N[q].v[lane];
            if (N == 0.0) continue;
            const int ip = G.o_di[q][0], ia = G.o_di[q][1], ib = G.o_di[q][2];
            const DateTable& T = layout.table;
            const double A = N * T.D[ip] * T.D[ia] / T.D[ib];
            const double a = T.t[ia], b = T.t[ib], p = T.t[ip];
            if (p < b) throw std::invalid_argument(
                "grouped gamma view: OIS payment before accrual end");
            const double rj = discount_waves.overlap(j + 1, b) - discount_waves.overlap(j + 1, a);
            const double rk = discount_waves.overlap(k + 1, b) - discount_waves.overlap(k + 1, a);
            const double sj = discount_waves.overlap(j + 1, p) - discount_waves.overlap(j + 1, b);
            const double sk = discount_waves.overlap(k + 1, p) - discount_waves.overlap(k + 1, b);
            result -= A * (rj * sk + sj * rk);
        }
        return result;
    }

    // O(K + K*number_of_OIS_coupons). Because diagonal already includes
    // the OIS correction, only the OFF-DIAGONAL part of each rank-two term
    // is applied below. This avoids the otherwise easy double-count bug.
    // The caller owns reusable scratch; v and out may be the same span.
    void multiply(size_t gi, int lane, std::span<const double> v,
                  std::span<double> out, GroupGammaScratch& scratch) const {
        const Group& G = group(gi, lane);
        const int K = discount_waves.K();
        if (v.size() != static_cast<size_t>(K) || out.size() != static_cast<size_t>(K))
            throw std::invalid_argument("grouped gamma HVP: wrong vector shape");
        scratch.hvp_work.resize(K);
        auto& result = scratch.hvp_work;
        double upper = 0.0;
        for (int j = K - 1; j >= 0; --j) {
            result[j] = -discount_waves.len(j + 1) * upper;
            upper += gradient(gi, lane, j) * v[j];
        }
        double lower = 0.0;
        for (int j = 0; j < K; ++j) {
            result[j] += gamma_diagonal(gi, lane, j) * v[j] -
                         gradient(gi, lane, j) * lower;
            lower += discount_waves.len(j + 1) * v[j];
        }
        for (size_t q = 0; q < G.o_di.size(); ++q) {
            const double N = G.o_N[q].v[lane];
            if (N == 0.0) continue;
            const int ip = G.o_di[q][0], ia = G.o_di[q][1], ib = G.o_di[q][2];
            const DateTable& T = layout.table;
            const double a = T.t[ia], b = T.t[ib], p = T.t[ip];
            if (p < b) throw std::invalid_argument(
                "grouped gamma HVP: OIS payment before accrual end");
            const double A = N * T.D[ip] * T.D[ia] / T.D[ib];
            double rv = 0.0, sv = 0.0;
            for (int j = 0; j < K; ++j) {
                const double r = discount_waves.overlap(j + 1, b) - discount_waves.overlap(j + 1, a);
                const double s = discount_waves.overlap(j + 1, p) - discount_waves.overlap(j + 1, b);
                rv += r * v[j];
                sv += s * v[j];
            }
            for (int j = 0; j < K; ++j) {
                const double r = discount_waves.overlap(j + 1, b) - discount_waves.overlap(j + 1, a);
                const double s = discount_waves.overlap(j + 1, p) - discount_waves.overlap(j + 1, b);
                result[j] -= A * (r * sv + s * rv - 2.0 * r * s * v[j]);
            }
        }
        std::copy(result.begin(), result.end(), out.begin());
    }
};

} // namespace ladder
