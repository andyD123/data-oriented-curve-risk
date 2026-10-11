#pragma once
// Simple zero curve / discount curve with box stencil support.
// Continuous zero rate z(t) or log discount factor ln D(t) with linear interpolation,
// which is equivalent to piecewise flat forwards (the exact no-leakage stencil class).
// Designed for speed: zero heap allocations during evaluation, inline fast lookups.
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

    // Construct from boundaries B and continuously compounded zero rates z (D(t) = exp(-z*t))
    static Curve from_zero_rates(const std::vector<double>& B, const std::vector<double>& z) {
        if (B.size() != z.size()) {
            throw std::invalid_argument("Curve::from_zero_rates: B and z size mismatch");
        }
        Curve c;
        c.S.B = B;
        c.S.validate();
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
        c.logD.resize(B.size());
        for (size_t i = 0; i < B.size(); ++i) {
            if (!std::isfinite(D[i]) || D[i] <= 0.0) {
                throw std::invalid_argument("Curve::from_dfs: discount factor must be positive and finite");
            }
            c.logD[i] = std::log(D[i]);
        }
        return c;
    }

    // Discount factor at time t: log-linear on nodes
    inline double df(double t) const {
        if (t <= S.B[0]) return 1.0;
        int k = S.bucket(t);
        double a = (t - S.B[k-1]) / S.len(k);
        return std::exp(logD[k-1] + a * (logD[k] - logD[k-1]));
    }

    // Continuously compounded zero rate at time t
    inline double zero_rate(double t) const {
        if (t <= 1e-12) return 0.0;
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
