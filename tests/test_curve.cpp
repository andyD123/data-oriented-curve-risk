// Curve node-layout contract: B[0] == 0, or B[0] > 0 with the implicit origin (0, ln D = 0) as the first node.
// Regression for a flat curve whose first node is positive returning a discount factor of 1 at that node.
#include "../ladder/curve.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

using ladder::Curve;

static int failures = 0;
static void close(double got, double want, double tol, const char* what) {
    if (!(std::fabs(got - want) <= tol)) { std::printf("FAIL %s: got %.17g want %.17g\n", what, got, want); ++failures; }
}
template <class F> static void rejects(F f, const char* what) {
    try { f(); std::printf("FAIL %s: not rejected\n", what); ++failures; } catch (const std::invalid_argument&) {}
}

int main() {
    // A flat 5% curve beginning at year 1 must give exp(-0.05) there, not 1.
    const std::vector<double> B{1.0, 2.0, 5.0};
    const auto flat = Curve::flat(B, 0.05);
    close(flat.df(1.0), std::exp(-0.05), 1e-15, "flat 5% df at first node (year 1)");
    close(flat.df(0.5), std::exp(-0.025), 1e-15, "flat 5% df between the origin and the first node");
    close(flat.df(0.0), 1.0, 0.0, "df at the origin");
    close(flat.df(3.0), std::exp(-0.15), 1e-15, "flat 5% df inside the grid");
    close(flat.zero_rate(1.0), 0.05, 1e-15, "zero rate at first node");
    close(flat.zero_rate(0.0), 0.05, 1e-15, "short zero rate limit");

    const auto zr = Curve::from_zero_rates({0.15, 0.6, 1.2}, {0.028, 0.032, 0.038});
    close(zr.df(0.15), std::exp(-0.028 * 0.15), 1e-15, "from_zero_rates node fidelity at shifted origin");
    const auto dfs = Curve::from_dfs({0.5, 1.0}, {0.98, 0.95});
    close(dfs.df(0.5), 0.98, 1e-15, "from_dfs retains a non-unit first node");
    close(dfs.df(0.25), std::sqrt(0.98), 1e-15, "implicit-origin log interpolation");

    // Zero-origin layout is unchanged.
    const auto zero = Curve::flat({0.0, 1.0, 2.0}, 0.03);
    close(zero.df(0.0), 1.0, 0.0, "origin node");
    close(zero.df(1.0), std::exp(-0.03), 1e-15, "zero-origin node");

    // Unsupported layouts are rejected.
    rejects([] { Curve::flat({-0.5, 1.0}, 0.03); }, "negative first node (flat)");
    rejects([] { Curve::from_zero_rates({-0.5, 1.0}, {0.03, 0.03}); }, "negative first node (zero rates)");
    rejects([] { Curve::from_dfs({-0.5, 1.0}, {1.0, 0.97}); }, "negative first node (dfs)");
    rejects([] { Curve::from_dfs({0.0, 1.0}, {0.99, 0.97}); }, "discount factor at the origin must be 1");

    if (failures) { std::printf("test_curve: %d failure(s)\n", failures); return 1; }
    std::puts("test_curve: PASS");
    return 0;
}
