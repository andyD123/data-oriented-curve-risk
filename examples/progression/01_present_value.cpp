// Step 1: price one fixed-cashflow instrument.
#include "common.hpp"
#include <cstdio>

int main()
{
    const auto cashflows = progression::schedule();
    const double pv = progression::present_value(cashflows);

    std::printf("PV = %.6f\n", pv);
}
