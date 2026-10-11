#pragma once
// Small log-linear discount curve with exact Hagan box-wave bumps.
// Adapted from damian123's PR #2. Times are year fractions from valuation
// time 0, not calendar dates. This is interpolation, not market bootstrap,
// coupon construction, fixing/calendar logic or a QuantLib substitute.
//
// Node discount factors satisfy D(0) = 1. An initial positive boundary
// B[0] is permitted: use the implicit origin (0, 1) to interpolate to
// the first supplied node. The final log-discount slope is extrapolated
// beyond B[K]. Box bumps are limited by Stencils::open_last independently.
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>
#include "stencil.hpp"

namespace ladder {

class Curve {
public:
    // Rates z(t_i) continuously compounded from valuation time 0:
    // D(t_i) = exp(-z(t_i)*t_i). Zero at the origin is redundant: its
    // continuously extended value comes from the next positive node.
    static Curve from_zero_rates(std::vector<double> boundaries,
                                 const std::vector<double>& zero_rates,
                                 bool open_last = true) {
        Stencils waves{std::move(boundaries), open_last};
        validate_inputs(waves, zero_rates.size());
        std::vector<double> logs;
        logs.reserve(zero_rates.size());
        for (size_t i = 0; i < zero_rates.size(); ++i) {
            if (!std::isfinite(zero_rates[i]))
                throw std::invalid_argument("Curve: nonfinite zero rate");
            const double ld = -zero_rates[i] * waves.B[i];
            if (!std::isfinite(ld))
                throw std::invalid_argument("Curve: zero-rate exponent overflow");
            logs.push_back(ld);
        }
        return Curve(std::move(waves), std::move(logs));
    }

    static Curve flat(std::vector<double> boundaries, double rate,
                      bool open_last = true) {
        Stencils waves{std::move(boundaries), open_last};
        waves.validate();
        if (waves.B.front() < 0.0 || !std::isfinite(rate))
            throw std::invalid_argument("Curve: invalid first boundary or flat rate");
        std::vector<double> logs;
        logs.reserve(waves.B.size());
        for (double t : waves.B) {
            const double ld = -rate * t;
            if (!std::isfinite(ld))
                throw std::invalid_argument("Curve: flat-rate exponent overflow");
            logs.push_back(ld);
        }
        return Curve(std::move(waves), std::move(logs));
    }

    // Explicit discount factors at all supplied nodes. If B[0] == 0,
    // D(B[0]) MUST be exactly 1. If B[0] > 0, D(B[0]) is retained:
    // log D(t) is linear between the implicit (0,0) and (B[0],log D[0]).
    static Curve from_dfs(std::vector<double> boundaries,
                          const std::vector<double>& discounts,
                          bool open_last = true) {
        Stencils waves{std::move(boundaries), open_last};
        validate_inputs(waves, discounts.size());
        if (waves.B.front() == 0.0 && discounts.front() != 1.0)
            throw std::invalid_argument("Curve: discount at valuation origin must be 1");
        std::vector<double> logs;
        logs.reserve(discounts.size());
        for (double D : discounts) {
            if (!std::isfinite(D) || !(D > 0.0))
                throw std::invalid_argument("Curve: discount factor must be finite and positive");
            logs.push_back(std::log(D));
        }
        return Curve(std::move(waves), std::move(logs));
    }

    // Read-only: changing a stencil without rebuilding its discount
    // factors would invalidate the interpolation and bump contract.
    const Stencils& stencils() const noexcept { return waves_; }
    const std::vector<double>& log_discount_nodes() const noexcept { return logD_; }

    // Piecewise log-linear interpolation, including the implicit
    // valuation origin. Prior to the first node and past the last node,
    // extrapolate the corresponding flat instantaneous-forward slope.
    double log_df(double t) const {
        require_finite(t, "Curve::log_df: nonfinite time");
        const auto& B = waves_.B;
        if (t <= B.front())
            return initial_log_slope() * t;
        const int k = waves_.bucket(t);
        const double weight = (t - B[k-1]) / waves_.len(k);
        return std::fma(weight, logD_[k] - logD_[k-1], logD_[k-1]);
    }

    double df(double t) const {
        const double D = std::exp(log_df(t));
        if (!std::isfinite(D))
            throw std::overflow_error("Curve::df: discount-factor overflow");
        return D;
    }

    // The continuous limit at time zero is the short-end forward slope.
    double zero_rate(double t) const {
        require_finite(t, "Curve::zero_rate: nonfinite time");
        return t == 0.0 ? -initial_log_slope() : -log_df(t) / t;
    }

    // Simple forward over a *positive* period. expm1 avoids subtracting
    // nearly equal discount factors for very short fixing intervals.
    double forward_rate(double t1, double t2) const {
        require_finite(t1, "Curve::forward_rate: nonfinite start");
        require_finite(t2, "Curve::forward_rate: nonfinite end");
        if (!(t2 > t1))
            throw std::invalid_argument("Curve::forward_rate: end must exceed start");
        const double rate = std::expm1(log_df(t1) - log_df(t2)) / (t2 - t1);
        if (!std::isfinite(rate))
            throw std::overflow_error("Curve::forward_rate: overflow");
        return rate;
    }

    // Exact discount-factor response to a unit-forward-wave bump:
    // D_delta(t) = D(t)*exp(-delta*h_k(t)), with 1-based wave index k.
    // Bounded final bumps saturate after B[K]; open-final bumps extend.
    double df_bumped(double t, int k, double delta) const {
        if (k < 1 || k > waves_.K())
            throw std::out_of_range("Curve::df_bumped: invalid wave index");
        require_finite(t, "Curve::df_bumped: nonfinite time");
        require_finite(delta, "Curve::df_bumped: nonfinite bump");
        const double D = std::exp(log_df(t) - delta * waves_.overlap(k, t));
        if (!std::isfinite(D))
            throw std::overflow_error("Curve::df_bumped: discount-factor overflow");
        return D;
    }

private:
    Stencils waves_;
    std::vector<double> logD_;

    Curve(Stencils waves, std::vector<double> logs)
        : waves_(std::move(waves)), logD_(std::move(logs)) {}

    static void require_finite(double value, const char* error) {
        if (!std::isfinite(value)) throw std::invalid_argument(error);
    }

    static void validate_inputs(const Stencils& waves, size_t count) {
        waves.validate();
        if (waves.B.front() < 0.0)
            throw std::invalid_argument("Curve: first node before valuation origin");
        if (count != waves.B.size())
            throw std::invalid_argument("Curve: node count mismatch");
    }

    double initial_log_slope() const noexcept {
        // If B[0] > 0, D(0)=1 is the implicit first node; otherwise
        // derive the short-end slope from the first two explicit nodes.
        return waves_.B.front() > 0.0
            ? logD_.front() / waves_.B.front()
            : (logD_[1] - logD_[0]) / waves_.len(1);
    }
};

} // namespace ladder
