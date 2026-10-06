// Step 3: one cashflow traversal gives the whole bucketed risk row.
#include "common.hpp"
#include <array>
#include <cstdio>

int main()
{
    const auto grid = progression::annual_risk_grid();
    const auto cashflows = progression::risk_cashflows(progression::schedule());

    std::array<double, 5> risk{};
    ladder::scan_discount(grid, cashflows, risk.data());

    for (std::size_t bucket = 0; bucket < risk.size(); ++bucket) {
        std::printf("year %zu risk = %.6f\n", bucket + 1, risk[bucket]);
    }
}
