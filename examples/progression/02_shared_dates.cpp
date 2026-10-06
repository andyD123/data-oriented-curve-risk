// Step 2: instruments on the same schedule reuse the same discount values.
#include "common.hpp"
#include <array>
#include <cstdio>

int main()
{
    const auto cashflows = progression::schedule();
    std::array<double, 5> discount_at_date{};

    for (std::size_t i = 0; i < cashflows.size(); ++i) {
        discount_at_date[i] =
            progression::discount(cashflows[i].day / progression::days_per_year);
    }

    for (double coupon : {4.0, 5.0, 6.0}) {
        const auto instrument = progression::schedule(coupon);
        double pv = 0.0;
        for (std::size_t i = 0; i < instrument.size(); ++i) {
            pv += instrument[i].amount * discount_at_date[i];
        }
        std::printf("coupon %.1f -> PV %.6f\n", coupon, pv);
    }
}
