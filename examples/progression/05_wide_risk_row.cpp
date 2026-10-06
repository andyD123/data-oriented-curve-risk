// Step 5: many instruments sharing a schedule form one wide risk row.
// SIMD width stays a hardware detail; schedule-group width is a data decision.
#include "common.hpp"
#include <cstdio>

int main()
{
    constexpr std::size_t instruments = 2048;
    constexpr int schedule_group_width = 2048;

    const auto risk = progression::wide_risk(instruments, schedule_group_width);

    std::printf("%zu instruments, %zu risks written\n",
                instruments, risk.size());
    std::printf("first instrument, first bucket = %.6f\n", risk[0]);
}
