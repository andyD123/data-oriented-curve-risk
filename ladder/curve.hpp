#pragma once
// Simple zero curve / discount curve with box stencil support.
// Continuous zero rate z(t) or log discount factor ln D(t) with linear interpolation,
// which is equivalent to piecewise flat forwards (the exact no-leakage stencil class).
// Designed for speed: zero heap allocations during evaluation, inline fast lookups.
// Supported node layouts: B[0] == 0 (then D(B[0]) must be exactly 1) or B[0] > 0, in which case the implicit origin
// (0, ln D = 0) is the first node and ln D is linear between it and (B[0], ln D[0]). B[0] < 0 is rejected.
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include "stencil.hpp"

namespace ladder {

struct Curve {
    Stencils S;                  // box stencils: boundaries B[0]..B[K]
    std::vector<double> logD;    // log discount factors at boundaries B[i]

    Curve() = default;

    static void check_first_node(const std::vector<double>& B) {
        if (B.empty() || !(B.front() >= 0.0)) {
            throw std::invalid_argument("Curve: first node must be at or after the valuation origin (t = 0)");
        }
    }

    // Construct from boundaries B and continuously compounded zero rates z (D(t) = exp(-z*t))
    static Curve from_zero_rates(const std::vector<double>& B, const std::vector<double>& z) {
        if (B.size() != z.size()) {
            throw std::invalid_argument("Curve::from_zero_rates: B and z size mismatch");
        }
        Curve c;
        c.S.B = B;
        c.S.validate();
        check_first_node(B);
        c.logD.resize(B.size());
        for (size_t i = 0; i < B.size(); ++i) {
            if (!std::isfinite(z[i])) {
                throw std::invalid_argument("Curve::from_zero_rates: non-finite zero rate");
            }
            c.logD[i] = -z[i] * B[i];
        }
        return c;
    }

    // Construct from boundaries B and a flat zero rate r
    static Curve flat(const std::vector<double>& B, double r) {
        if (!std::isfinite(r)) {
            throw std::invalid_argument("Curve::flat: non-finite rate");
        }
        Curve c;
        c.S.B = B;
        c.S.validate();
        check_first_node(B);
        c.logD.resize(B.size());
        for (size_t i = 0; i < B.size(); ++i) {
            c.logD[i] = -r * B[i];
        }
        return c;
    }

    // Construct from boundaries B and discount factors D
    static Curve from_dfs(const std::vector<double>& B, const std::vector<double>& D) {
        if (B.size() != D.size()) {
            throw std::invalid_argument("Curve::from_dfs: B and D size mismatch");
        }
        Curve c;
        c.S.B = B;
        c.S.validate();
        check_first_node(B);
        c.logD.resize(B.size());
        for (size_t i = 0; i < B.size(); ++i) {
            if (!std::isfinite(D[i]) || D[i] <= 0.0) {
                throw std::invalid_argument("Curve::from_dfs: discount factor must be positive and finite");
            }
            c.logD[i] = std::log(D[i]);
        }
        if (B.front() == 0.0 && D.front() != 1.0) {
            throw std::invalid_argument("Curve::from_dfs: discount factor at the valuation origin must be 1");
        }
        return c;
    }

    // Discount factor at time t: log-linear on nodes
    inline double df(double t) const {
        if (t <= 0.0) return 1.0;
        if (t < S.B[0]) return std::exp(logD[0] * (t / S.B[0]));   // implicit origin (0, 0) to the first node
        if (t == S.B[0]) return std::exp(logD[0]);
        int k = S.bucket(t);
        double a = (t - S.B[k-1]) / S.len(k);
        return std::exp(logD[k-1] + a * (logD[k] - logD[k-1]));
    }

    // Continuously compounded zero rate at time t
    inline double zero_rate(double t) const {
        if (t <= 1e-12) return S.B[0] > 0.0 ? -logD[0] / S.B[0] : 0.0;
        return -std::log(df(t)) / t;
    }

    // Simple forward rate over [t1, t2]
    inline double forward_rate(double t1, double t2) const {
        double dt = t2 - t1;
        if (dt <= 1e-12) return 0.0;
        return (df(t1) / df(t2) - 1.0) / dt;
    }

    // Exact box bump of stencil k by delta: forward +delta on [B[k-1], B[k])
    inline double df_bumped(double t, int k, double delta) const {
        return df(t) * std::exp(-delta * S.overlap(k, t));
    }
};

} // namespace ladder
