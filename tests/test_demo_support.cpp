// Tests for the small demonstration helpers, not replacements for the scan oracle.
#include "examples/support/input.hpp"
#include "examples/support/timing.hpp"
#include "examples/benchmark_paper/scenario_cache.hpp"
#include "examples/benchmark_paper/scenario_pricing.hpp"
#include "examples/benchmark_paper/scenario_report.hpp"
#include <array>
#include <cstdio>
#include <limits>

namespace {
int checks = 0;

void require(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

template<class Work>
void rejects(Work&& work)
{
    bool caught = false;
    try { work(); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "invalid input was accepted");
}

void test_timing()
{
    int calls = 0;
    const double once = demo::measure_once([&] { ++calls; });
    require(calls == 1 && once >= 0.0, "measure_once");
    int inspected = 0;
    const double best = demo::measure_best(3, [&] { ++calls; }, [&] {
        require(calls == inspected + 2, "check follows its measured work");
        ++inspected;
    });
    require(calls == 4 && inspected == 3 && best >= 0.0, "measure_best call count");
    rejects([&] { demo::measure_best(0, [&] { ++calls; }); });
    require(calls == 4, "invalid repetitions must not execute work");
    bool caught = false;
    try { demo::measure_once([] { throw std::runtime_error("work failure"); }); }
    catch (const std::runtime_error&) { caught = true; }
    require(caught, "work exceptions must propagate");
}

void test_cache()
{
    rejects([] { scenario_demo::DateCache cache(1, 8); });
    rejects([] { scenario_demo::DateCache cache(-1, 8); });
    rejects([] { scenario_demo::DateCache cache(2, 0); });
    scenario_demo::DateCache cache(2, 8);
    int fills = 0;
    const auto fill = [&](int date, double* column) {
        ++fills;
        std::fill_n(column, 8, double(date));
    };
    const double* start = cache.fetch(3, fill);
    const double* end = cache.fetch(4, fill);
    require(start[0] == 3.0 && end[0] == 4.0, "two live projection columns");
    cache.fetch(3, fill); // 3 is MRU; inserting 5 must evict 4, not 3.
    cache.fetch(5, fill);
    require(start[0] == 3.0 && fills == 3, "LRU keeps the recently used column");
    cache.fetch(3, fill);
    cache.fetch(3, fill);
    require(fills == 3 && cache.counts().lookups == 6, "cache hit path");
    cache.fetch(4, fill);
    require(fills == 4 && cache.counts().misses == 4, "evicted column is refilled");
}

void test_expressions()
{
    using namespace scenario_demo;
    require(fixed_cashflow_pv(100.0, 0.9) == 90.0, "fixed cashflow expression");
    const auto coupon = floating_coupon_pv(100.0, 0.99, 0.98, 0.95);
    require(coupon == 100.0 * (0.99 / 0.98 - 1.0) * 0.95, "coupon expression");
    require(fixed_cashflow_pv(2.0, 0.25L) == 0.5L, "generic expression");
    std::array<double, 4> values{1, 2, 3, 4};
    accumulate_scenarios(values, [](std::size_t scenario) { return double(scenario + 1); });
    require(values == std::array<double, 4>{2, 4, 6, 8}, "scenario traversal");
}

void test_comparison_gate()
{
    const std::vector<double> expected{0.0, 100.0};
    scenario_demo::AccuracyReport good;
    good.compare(expected, expected, "equal");
    require(good.passed, "equal results rejected");
    for (const auto& values : std::vector<std::vector<double>>{
             {1e8, 100.0}, {std::numeric_limits<double>::quiet_NaN(), 100.0}, {0.0}}) {
        scenario_demo::AccuracyReport bad;
        bad.compare(values, expected, "intentional negative test");
        require(!bad.passed, "comparison gate accepted a bad result");
    }
}
} // namespace

int main()
{
    try {
        test_timing();
        test_cache();
        test_expressions();
        test_comparison_gate();
        require(demo::read_count("23", 1, 100, "bad count") == 23, "valid count");
        rejects([] { demo::read_count("1x", 1, 100, "bad count"); });
        std::printf("demo support: %d checks, PASS\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "demo support: %s\n", error.what());
        return 1;
    }
}
