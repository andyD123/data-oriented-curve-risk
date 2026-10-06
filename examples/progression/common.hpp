#pragma once
#include "ladder/aligned_memory.hpp"
#include "ladder/layout.hpp"
#include "ladder/scan.hpp"
#include "ladder/scan_wide_experiment.hpp"
#include <cmath>
#include <cstddef>
#include <vector>

namespace progression {

constexpr double days_per_year = 365.0;

inline double discount(double t)
{
    return std::exp(-0.035 * t);
}

inline std::vector<ladder::FixedFlow> schedule(double coupon = 5.0)
{
    std::vector<ladder::FixedFlow> flows;
    for (int year = 1; year <= 5; ++year) {
        flows.push_back({year * 365, coupon + (year == 5 ? 100.0 : 0.0)});
    }
    return flows;
}

inline double present_value(const std::vector<ladder::FixedFlow>& flows)
{
    double value = 0.0;
    for (const auto& flow : flows) {
        value += flow.amount * discount(flow.day / days_per_year);
    }
    return value;
}

inline ladder::Stencils annual_risk_grid()
{
    ladder::Stencils grid;
    grid.B = {0, 1, 2, 3, 4, 5};
    grid.validate();
    return grid;
}

inline std::vector<ladder::UnitCashflow>
risk_cashflows(const std::vector<ladder::FixedFlow>& flows)
{
    std::vector<ladder::UnitCashflow> result;
    for (const auto& flow : flows) {
        const double t = flow.day / days_per_year;
        result.push_back({t, flow.amount * discount(t)});
    }
    return result;
}

inline std::vector<ladder::InstrumentSpec> make_book(std::size_t count)
{
    std::vector<ladder::InstrumentSpec> book(count);
    for (std::size_t i = 0; i < count; ++i) {
        book[i].signature = 1; // all instruments share this schedule
        book[i].fixed = schedule(4.0 + 0.01 * double(i % 100));
    }
    return book;
}

inline std::vector<double> wide_risk(std::size_t count, int group_width)
{
    auto book = make_book(count);
    const auto grid = annual_risk_grid();

    auto layout = ladder::build_wide_fixed_layout(book, group_width, days_per_year);
    ladder::refresh_wide_fixed(layout, grid, discount);

    auto* raw = static_cast<double*>(
        ladder::allocate_aligned(layout.output_values * sizeof(double)));
    ladder::scan_wide_fixed<ladder::Store::streaming>(layout, grid, raw);

    std::vector<double> result(count * std::size_t(grid.K()));
    for (const auto& group : layout.groups) {
        for (int local = 0; local < group.n_valid; ++local) {
            const int block = local / ladder::LANES;
            const int lane = local % ladder::LANES;
            const std::size_t instrument = std::size_t(group.inst[std::size_t(local)]);
            for (int bucket = 0; bucket < grid.K(); ++bucket) {
                const std::size_t source = group.out_offset
                    + (std::size_t(bucket) * group.blocks + block) * ladder::LANES + lane;
                result[instrument * std::size_t(grid.K()) + bucket] = raw[source];
            }
        }
    }

    ladder::free_aligned(raw);
    return result;
}

} // namespace progression
