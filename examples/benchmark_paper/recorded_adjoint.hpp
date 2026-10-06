#pragma once

// Details of the illustrative recorded reverse-mode comparison. This expression
// matches the first derivative at zero shift; it is not a finite-shock pricing
// model for arbitrary product coupons, nor an implementation of a general AAD tool.
#include "ladder/scan.hpp"
#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace adjoint_demo {

struct TapeNode {
    int wave;
    double local_derivative;
    int parent;
};

struct TapeWorkspace {
    std::vector<TapeNode> nodes;
    std::vector<double> values;
    std::vector<double> adjoints;

    TapeWorkspace()
    {
        // Keep the historical reservation outside the timed calculation.
        nodes.reserve(1 << 20);
        values.reserve(1 << 20);
        adjoints.reserve(1 << 20);
    }
};

template<class Overlap>
int record_product(TapeWorkspace& tape, double weight, int waves,
                   int first_wave, Overlap&& overlap_at)
{
    int parent = -1;
    double value = weight;
    for (int wave = 1; wave <= waves; ++wave) {
        const double overlap = overlap_at(wave);
        const double exponential = std::exp(-0.0 * overlap);
        value = value * exponential;
        tape.nodes.push_back({first_wave + wave - 1, -overlap * exponential, parent});
        tape.values.push_back(value);
        parent = static_cast<int>(tape.nodes.size()) - 1;
    }
    return parent;
}

inline void reverse_tape(TapeWorkspace& tape, const std::vector<int>& leaves,
                         std::span<double> gradient)
{
    tape.adjoints.assign(tape.nodes.size(), 0.0);
    for (int leaf : leaves) {
        tape.adjoints[leaf] = 1.0;
    }
    for (int index = static_cast<int>(tape.nodes.size()) - 1; index >= 0; --index) {
        const auto& node = tape.nodes[index];
        const double adjoint = tape.adjoints[index];
        if (adjoint == 0.0) continue;

        // exp(0) = 1: the root's stored value is its original input weight.
        const double parent_value = node.parent >= 0
            ? tape.values[node.parent] : tape.values[index];
        gradient[node.wave] += adjoint * parent_value * node.local_derivative;
        if (node.parent >= 0) {
            tape.adjoints[node.parent] += adjoint;
        }
    }
}

inline void recorded_gradient(const ladder::Stencils& discount_waves,
                              const ladder::Stencils& projection_waves,
                              std::span<const ladder::UnitCashflow> cashflows,
                              std::span<const ladder::ProjectionTerm> coupons,
                              std::span<double> gradient, TapeWorkspace& tape)
{
    std::fill(gradient.begin(), gradient.end(), 0.0);
    tape.nodes.clear();
    tape.values.clear();
    // This allocation remains per instrument INSIDE the timing, as before.
    std::vector<int> leaves;
    leaves.reserve(cashflows.size() + coupons.size());
    for (const auto& cashflow : cashflows) {
        leaves.push_back(record_product(tape, cashflow.x, discount_waves.K(), 0,
            [&](int wave) { return discount_waves.overlap(wave, cashflow.t); }));
    }
    for (const auto& coupon : coupons) {
        leaves.push_back(record_product(tape, coupon.w, projection_waves.K(), discount_waves.K(),
            [&](int wave) {
                return -(projection_waves.psi(wave, coupon.a)
                       - projection_waves.psi(wave, coupon.b));
            }));
    }
    reverse_tape(tape, leaves, gradient);
}

} // namespace adjoint_demo
