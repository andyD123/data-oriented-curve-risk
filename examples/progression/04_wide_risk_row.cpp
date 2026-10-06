// Step 4: many instruments sharing one schedule use one wide row.
// SIMD width is only an execution detail; the logical group can be much wider.
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
