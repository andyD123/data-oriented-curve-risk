// Step 2: one date lookup returns a whole scenario row.
#include "common.hpp"
#include <cstdio>

int main()
{
    const auto cashflows = progression::schedule();
    progression::ScenarioRow pv{};

    for (const auto& flow : cashflows) {
        const auto discounts =
            progression::scenario_discounts(flow.day / progression::days_per_year);

        for (std::size_t scenario = 0; scenario < pv.size(); ++scenario) {
            pv[scenario] += flow.amount * discounts[scenario];
        }
    }

    for (std::size_t scenario = 0; scenario < pv.size(); ++scenario) {
        std::printf("scenario %zu -> PV %.6f\n", scenario, pv[scenario]);
    }
}
