// Does grouping instruments by schedule let a small date cache serve a large book?
// The financial expressions and the common pricing loop are in scenario_pricing.hpp.
// The timing helper measures only its lambda; reports and checks run afterwards.
#include "../example_dir.hpp"
#include "../support/input.hpp"
#include "../support/timing.hpp"
#include "scenario_book.hpp"
#include "scenario_cache.hpp"
#include "scenario_pricing.hpp"
#include "scenario_report.hpp"
#include <cstdlib>

namespace scenario_demo {
namespace {

constexpr const char* usage =
    "usage: scenario_bench [N=1..10000000] [baseline=0|1] [reps=1..1000] [curve_file]
"
    "       environment: LRU_CAPS=c1,c2,... (each 2..1000000), LRU_ALL=1";

struct Options {
    std::size_t instruments = 100000;
    bool baseline = true;
    int repetitions = 3;
    const char* curve_file = "quantlib_example_curves.txt";
    std::vector<int> capacities{16, 64, 128, 256, 1024, 4096};
    bool all_orders = false;
};

std::vector<int> read_capacities(const char* text)
{
    std::vector<int> capacities;
    std::string_view remaining(text);
    while (true) {
        const auto comma = remaining.find(',');
        capacities.push_back(static_cast<int>(demo::read_count(
            remaining.substr(0, comma), 2, 1000000, usage)));
        if (comma == std::string_view::npos) {
            return capacities;
        }
        remaining.remove_prefix(comma + 1);
    }
}

Options read_options(int argc, char** argv)
{
    if (argc > 5) throw std::invalid_argument(usage);
    Options options;
    if (argc > 1) options.instruments = demo::read_count(argv[1], 1, 10000000, usage);
    if (argc > 2) options.baseline = demo::read_count(argv[2], 0, 1, usage) != 0;
    if (argc > 3) options.repetitions = static_cast<int>(demo::read_count(argv[3], 1, 1000, usage));
    if (argc > 4) options.curve_file = argv[4];
    if (const char* text = std::getenv("LRU_CAPS")) options.capacities = read_capacities(text);
    options.all_orders = std::getenv("LRU_ALL") != nullptr;
    return options;
}

CacheCounts price_from_cache(const Book& book, const InstrumentOrder& order,
                            const demo::CurvePair& curves, const Scenarios& scenarios,
                            int capacity, std::vector<double>& output)
{
    // Construction/destruction stay INSIDE the timed call, as in the original.
    DateCache discount_cache(capacity, scenarios.padded_count());
    DateCache projection_cache(capacity, scenarios.padded_count());
    const auto discount_at = [&](int date) {
        return discount_cache.fetch(date, [&](int index, double* column) {
            fill_discount_column(curves.discount, scenarios, book.times[index], column);
        });
    };
    const auto projection_at = [&](int date) {
        return projection_cache.fetch(date, [&](int index, double* column) {
            fill_projection_column(curves.projection, scenarios, book.times[index], column);
        });
    };
    price_scenarios(book, order, scenarios, discount_at, projection_at, output);
    return {discount_cache.counts().misses + projection_cache.counts().misses,
            discount_cache.counts().lookups + projection_cache.counts().lookups};
}

void measure_table_orders(const Book& book, const std::array<InstrumentOrder, 3>& orders,
                          const Scenarios& scenarios, const ScenarioTables& tables,
                          int repetitions, const std::vector<double>& reference,
                          std::vector<double>& output, Measurements& measured)
{
    const auto discount_at = [&](int date) {
        return tables.discount.data() + std::size_t(date) * scenarios.padded_count();
    };
    const auto projection_at = [&](int date) {
        return tables.projection.data() + std::size_t(date) * scenarios.padded_count();
    };
    double* times[] = {&measured.table_random, &measured.table_sorted};
    for (int order = 0; order < 2; ++order) {
        *times[order] = demo::measure_best(repetitions, [&] {
            price_scenarios(book, orders[order], scenarios, discount_at, projection_at, output);
        });
        measured.accuracy.compare(output, reference,
            std::string("full table, ") + orders[order].name + " order");
    }
}

void measure_cache_orders(const Book& book, const std::array<InstrumentOrder, 3>& orders,
                          const demo::CurvePair& curves, const Scenarios& scenarios,
                          const Options& options, const std::vector<double>& reference,
                          std::vector<double>& output, Measurements& measured)
{
    for (int capacity : options.capacities) {
        for (const auto& order : orders) {
            const bool expensive_random_case = std::string_view(order.name) == "random"
                && capacity < 1024 && options.instruments > 100000;
            if (expensive_random_case && !options.all_orders) continue;

            CacheCounts counts;
            const double milliseconds = demo::measure_best(options.repetitions, [&] {
                counts = price_from_cache(book, order, curves, scenarios, capacity, output);
            });
            measured.accuracy.compare(output, reference,
                "LRU cache, " + std::to_string(capacity) + " columns, " + order.name + " order");
            measured.cache.push_back({capacity, order.name, milliseconds, counts});
        }
    }
}

int run_experiment(const Options& options)
{
    const auto curves = demo::read_curves(options.curve_file);
    const Scenarios scenarios(curves);
    const auto book = make_book(options.instruments);
    const auto orders = make_orders(book);
    Measurements measured;
    ScenarioTables tables;
    measured.table_build = demo::measure_best(options.repetitions, [&] {
        refresh_tables(book, curves, scenarios, tables);
    });

    // Preparing risk records and allocating result buffers remain outside timings.
    const auto prepared = prepare_risk(book, curves);
    std::vector<double> reference(options.instruments * scenarios.wave_count());
    std::vector<double> output(reference.size());
    measured.scalar_scan = demo::measure_best(options.repetitions, [&] {
        calculate_scan(prepared, curves, reference);
    });

    measure_table_orders(book, orders, scenarios, tables, options.repetitions,
                         reference, output, measured);
    measure_cache_orders(book, orders, curves, scenarios, options, reference, output, measured);
    if (options.baseline) {
        // Keep its separate output allocation outside the single timed run.
        std::vector<double> baseline_output(reference.size());
        measured.baseline = demo::measure_once([&] {
            calculate_repricing(book, curves, scenarios, baseline_output);
        });
        measured.baseline_error = measured.accuracy.compare(baseline_output, reference, "BASE");
    }
    print_report(book, scenarios, options.repetitions, options.capacities, measured);
    return measured.accuracy.passed ? 0 : 1;
}

} // namespace
} // namespace scenario_demo

int main(int argc, char** argv)
{
    try {
        enter_example_dir();
        return scenario_demo::run_experiment(scenario_demo::read_options(argc, argv));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "scenario_bench: %s
", error.what());
        return 1;
    }
}
