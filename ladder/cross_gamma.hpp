#pragma once
// Research continuation of Lars's 10 October 2026 scan_discount_hessian draft.
// This OPTIONAL second pass computes only diagonal curvature of fixed discounted
// cashflows. Off-diagonal curvature is a lazy view of the existing first-order
// Hagan-wave ladder. This is NOT an OIS/IBOR general-Hessian implementation.
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>
#include "stencil.hpp"
#include "unit_cashflow.hpp"

namespace ladder {

inline void validate_cross_gamma_stencils(const Stencils& S) {
    if (S.B.size() < 2) throw std::invalid_argument("cross-gamma: empty stencil");
    for (size_t i = 0; i < S.B.size(); ++i)
        if (!std::isfinite(S.B[i]) || (i && !(S.B[i] > S.B[i - 1])))
            throw std::invalid_argument("cross-gamma: invalid stencil boundaries");
}

// Input records must be finite and in ascending time order. O(N + K) after sort.
// Does not change or rerun first-order risk. Returns one diagonal entry per wave.
inline void scan_discount_gamma_diagonal(const Stencils& S,
                                         std::span<const UnitCashflow> cf,
                                         double* diagonal) {
    validate_cross_gamma_stencils(S);
    if (!diagonal) throw std::invalid_argument("scan_discount_gamma_diagonal: null output");
    const int K = S.K();
    std::vector<double> interior2(static_cast<size_t>(K) + 1, 0.0);
    std::vector<double> suffix(static_cast<size_t>(K) + 1, 0.0);
    int bucket = 1;
    double previous = -std::numeric_limits<double>::infinity();
    for (const auto& c : cf) {
        if (!std::isfinite(c.t) || !std::isfinite(c.x) || c.t < previous)
            throw std::invalid_argument("scan_discount_gamma_diagonal: finite, sorted records required");
        previous = c.t;
        if (c.t <= S.B.front()) continue; // essential pre-first-boundary correction
        while (bucket < K && c.t >= S.B[bucket]) ++bucket;
        if (c.t >= S.B[K]) {
            if (S.open_last) {
                const double dt = c.t - S.B[K - 1];
                interior2[K] += c.x * dt * dt;
            }
        } else {
            const double dt = c.t - S.B[bucket - 1];
            interior2[bucket] += c.x * dt * dt;
        }
    }
    double running = 0.0;
    int k = K;
    for (auto it = cf.rbegin(); it != cf.rend(); ++it) {
        while (k >= 1 && it->t < S.B[k]) {
            suffix[k] = running;
            --k;
        }
        running += it->x;
    }
    while (k >= 1) {
        suffix[k] = running;
        --k;
    }
    for (int j = 1; j < K; ++j) {
        const double len = S.len(j);
        diagonal[j - 1] = interior2[j] + len * len * suffix[j];
    }
    const double last_len = S.open_last ? 0.0 : S.len(K);
    diagonal[K - 1] = interior2[K] + last_len * last_len * suffix[K];
}

// O(K) storage, O(1) element access, O(K) Hessian-vector product.
// Off diagonal valid for linear fixed-amount discounted cashflows only.
struct DiscountCrossGammaView {
    const Stencils& stencils;
    std::span<const double> gradient;
    std::span<const double> diagonal;

    DiscountCrossGammaView(const Stencils& S,
                           std::span<const double> g,
                           std::span<const double> d)
        : stencils(S), gradient(g), diagonal(d) {
        validate_cross_gamma_stencils(S);
        if (g.size() != static_cast<size_t>(S.K()) ||
            d.size() != static_cast<size_t>(S.K()))
            throw std::invalid_argument("DiscountCrossGammaView: wrong vector size");
    }

    double at(int row, int col) const {
        const int K = stencils.K();
        if (row < 0 || col < 0 || row >= K || col >= K)
            throw std::out_of_range("DiscountCrossGammaView: bucket index");
        if (row == col) return diagonal[row];
        const int lo = std::min(row, col);
        const int hi = std::max(row, col);
        return -stencils.len(lo + 1) * gradient[hi];
    }

    void multiply(std::span<const double> v, std::span<double> out) const {
        const int K = stencils.K();
        if (v.size() != static_cast<size_t>(K) || out.size() != static_cast<size_t>(K))
            throw std::invalid_argument("DiscountCrossGammaView::multiply: wrong size");
        std::vector<double> r(static_cast<size_t>(K)); // safe even when v and out alias
        double upper = 0.0;
        for (int j = K - 1; j >= 0; --j) {
            r[j] = -stencils.len(j + 1) * upper;
            upper += gradient[j] * v[j];
        }
        double lower = 0.0;
        for (int j = 0; j < K; ++j) {
            r[j] += diagonal[j] * v[j] - gradient[j] * lower;
            lower += stencils.len(j + 1) * v[j];
        }
        std::copy(r.begin(), r.end(), out.begin());
    }
};

// Fully forecast single-curve OIS: A = signed N D(p) D(a)/D(b),
// where a <= b <= p. Separate discount/forecast curves and fixed/partly
// realised coupons are outside this contract.
struct OisLagGammaTerm { double a, b, p, A; };

inline void validate_ois_lag_term(const OisLagGammaTerm& c) {
    if (!std::isfinite(c.a) || !std::isfinite(c.b) ||
        !std::isfinite(c.p) || !std::isfinite(c.A) ||
        c.a > c.b || c.b > c.p)
        throw std::invalid_argument("OisLagGammaTerm: invalid dates/coefficient");
}

// Add exact difference between nonlinear OIS Hessian and Hessian formed from
// frozen signed first-order dated cashflows: -A*(r*s^T + s*r^T),
// r_j = overlap_j(b)-overlap_j(a), s_j = overlap_j(p)-overlap_j(b).
inline double ois_lag_gamma_correction(const Stencils& S,
                                       const OisLagGammaTerm& c,
                                       int j, int k) {
    validate_cross_gamma_stencils(S);
    validate_ois_lag_term(c);
    if (j < 0 || k < 0 || j >= S.K() || k >= S.K())
        throw std::out_of_range("ois_lag_gamma_correction: bucket index");
    const double rj = S.overlap(j + 1, c.b) - S.overlap(j + 1, c.a);
    const double rk = S.overlap(k + 1, c.b) - S.overlap(k + 1, c.a);
    const double sj = S.overlap(j + 1, c.p) - S.overlap(j + 1, c.b);
    const double sk = S.overlap(k + 1, c.p) - S.overlap(k + 1, c.b);
    return -c.A * (rj * sk + sj * rk);
}

// Adds correction to an existing H*v, without materialising H.
// O(number_of_coupons*K) arithmetic and no dense output.
inline void add_ois_lag_gamma_product(const Stencils& S,
                                      std::span<const OisLagGammaTerm> coupons,
                                      std::span<const double> v,
                                      std::span<double> out) {
    validate_cross_gamma_stencils(S);
    const int K = S.K();
    if (v.size() != static_cast<size_t>(K) || out.size() != static_cast<size_t>(K))
        throw std::invalid_argument("add_ois_lag_gamma_product: wrong size");
    for (const auto& c : coupons) {
        validate_ois_lag_term(c);
        double rv = 0.0, sv = 0.0;
        for (int k = 0; k < K; ++k) {
            const double r = S.overlap(k + 1, c.b) - S.overlap(k + 1, c.a);
            const double s = S.overlap(k + 1, c.p) - S.overlap(k + 1, c.b);
            rv += r * v[k];
            sv += s * v[k];
        }
        for (int k = 0; k < K; ++k) {
            const double r = S.overlap(k + 1, c.b) - S.overlap(k + 1, c.a);
            const double s = S.overlap(k + 1, c.p) - S.overlap(k + 1, c.b);
            out[k] -= c.A * (r * sv + s * rv);
        }
    }
}

} // namespace ladder
