// Minimal first-order example: a discount curve, three fixed payments,
// one linear-time Hagan-wave risk scan, and a repricing oracle.
// Build: cmake --build build --target curve_intro
// Run:   ./build/examples/curve_intro/curve_intro
#include "ladder/curve.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

int main() try {
    using namespace ladder;

    // Continuous zero rate 3%, with an open final Hagan wave.
    const Curve curve = Curve::flat({0.0, 0.5, 1.0, 2.0, 3.0}, 0.03);
    const std::vector<std::pair<double,double>> payments{
        {0.75, 1500.0}, {1.5, 2000.0}, {3.5, 10000.0}
    };

    // Discount to PV, retaining each payment's signed, dated contribution.
    std::vector<UnitCashflow> cashflows;
    for (const auto& [time,amount] : payments)
        replicate_fixed(amount, time, [&](double t) { return curve.df(t); },
                        cashflows);
    sort_by_time(cashflows);

    // All forward-wave sensitivities in one scan, not one reprice per bucket.
    const Stencils& waves = curve.stencils();
    std::vector<double> risk(waves.K());
    scan_discount(waves, cashflows, risk.data());

    // An independent repricing check: bump the selected forward-rate interval.
    const auto price_bumped = [&](int bucket, double shift) {
        double pv=0.0;
        for (const auto& [time,amount] : payments)
            pv += amount * curve.df_bumped(time,bucket,shift);
        return pv;
    };

    constexpr double eps=1e-5;
    std::puts("wave    first-order PV per unit forward rate    bump/reprice check");
    for (int k=1; k<=waves.K(); ++k) {
        const double oracle=(price_bumped(k,eps)-price_bumped(k,-eps))/(2*eps);
        const double diff=std::abs(risk[k-1]-oracle);
        if (diff > 2e-7*std::max({1.0,std::abs(risk[k-1]),std::abs(oracle)}))
            throw std::runtime_error("first-order curve-wave oracle mismatch");
        std::printf("%4d %37.9f %22.9f\n",k,risk[k-1],oracle);
    }
    std::puts("PASS: curve and first-order wave risk agree with repricing");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"FAIL curve_intro: %s\n",e.what());
    return 1;
}
