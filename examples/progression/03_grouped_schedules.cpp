// Step 3: instruments sharing a schedule reuse the same scenario rows.
#include "common.hpp"
#include <array>
#include <cstdio>

int main()
{
    const auto dates = progression::schedule();

    std::array<progression::ScenarioRow, 5> discount_rows{};
    for (std::size_t date = 0; date < dates.size(); ++date) {
        discount_rows[date] =
            progression::scenario_discounts(dates[date].day / progression::days_per_year);
    }

    for (double coupon : {4.0, 5.0, 6.0}) {
        const auto instrument = progression::schedule(coupon);
        progression::ScenarioRow pv{};

        for (std::size_t date = 0; date < instrument.size(); ++date) {
            for (std::size_t scenario = 0; scenario < pv.size(); ++scenario) {
                pv[scenario] += instrument[date].amount * discount_rows[date][scenario];
            }
        }

        std::printf("coupon %.1f -> base scenario PV %.6f\n", coupon, pv[1]);
    }
}
