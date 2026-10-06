#pragma once

// Reporting and numerical gates live here, not in the pricing example.
#include "scenario_cache.hpp"
#include "scenario_pricing.hpp"
#include <cstdio>
#include <span>
#include <string>

namespace scenario_demo {

struct AccuracyReport {
    static constexpr double tolerance = 1e-6;
    bool passed = true;
    double largest_error = 0.0;
    std::string worst_case;
    std::size_t cases_checked = 0;

    double compare(std::span<const double> actual, std::span<const double> expected,
                   const std::string& name)
    {
        bool finite = actual.size() == expected.size();
        double error = 0.0;
        for (std::size_t index = 0; index < std::min(actual.size(), expected.size()); ++index) {
            finite = finite && std::isfinite(actual[index]) && std::isfinite(expected[index]);
            const double scale = std::max(std::fabs(expected[index]), 1e4);
            error = std::max(error, std::fabs(actual[index] - expected[index]) / scale);
        }
        if (!finite || !(error <= tolerance)) {
            passed = false;
            std::fprintf(stderr, "scenario_bench: invalid result in %s (error %.1e)\n",
                         name.c_str(), error);
        }
        if (cases_checked++ == 0 || error > largest_error) {
            largest_error = error;
            worst_case = name;
        }
        return error;
    }
};

struct CacheResult {
    int capacity;
    const char* order;
    double milliseconds;
    CacheCounts counts;
};

struct Measurements {
    double table_build = 0.0;
    double scalar_scan = 0.0;
    double table_random = 0.0;
    double table_sorted = 0.0;
    double baseline = -1.0;
    double baseline_error = 0.0;
    std::vector<CacheResult> cache;
    AccuracyReport accuracy;
};

inline const char* instruction_set()
{
#if defined(__AVX512F__)
    return "AVX-512";
#elif defined(__AVX2__)
    return "AVX2";
#else
    return "compiler baseline";
#endif
}

inline std::string compiler_name()
{
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown compiler";
#endif
}

inline std::string grouped_digits(std::size_t value)
{
    const auto digits = std::to_string(value);
    std::string result;
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (index && (digits.size() - index) % 3 == 0) {
            result += ',';
        }
        result += digits[index];
    }
    return result;
}

inline void print_configuration(const Book& book, const Scenarios& scenarios)
{
    std::printf("scenario_bench: wave scenarios priced from an LRU date cache, single thread\n");
    std::printf("build:     %s instructions; %s\n", instruction_set(), compiler_name().c_str());
    std::printf("book:      %s instruments (bonds and vanilla swaps); %s distinct curve dates\n",
        grouped_digits(book.instruments.size()).c_str(), grouped_digits(book.times.size()).c_str());
    std::printf("scenarios: %d (%d discount waves + %d projection waves, up and down), padded to %d\n",
        scenarios.count(), scenarios.discount_waves, scenarios.projection_waves,
        scenarios.padded_count());
    std::printf("outputs:   %s sensitivities (%d per instrument, per unit forward-rate shift)\n",
        grouped_digits(book.instruments.size() * scenarios.wave_count()).c_str(),
        scenarios.wave_count());
}

inline void print_terms(const Book& book, const Scenarios& scenarios, int repetitions)
{
    const auto dates = count_used_dates(book);
    std::printf("\nterms\n");
    std::printf("  column      %d scenario factors at one date (%s bytes)\n",
        scenarios.padded_count(), grouped_digits(scenarios.padded_count() * 8).c_str());
    std::printf("  misses      column computations, both curves; the floor is %s, each column once\n",
        grouped_digits(dates.total()).c_str());
    std::printf("              (%s discount-curve dates + %s projection-curve dates)\n",
        grouped_digits(dates.discount).c_str(), grouped_digits(dates.projection).c_str());
    std::printf("  orders      random = shuffled; sorted = type, start date, tenor;\n"
                "              grouped = start date, type, tenor (prototype ordering)\n");
    std::printf("  full table  every date's column computed up front (%.1f MB per curve)\n",
        book.times.size() * scenarios.padded_count() * 8 / 1e6);
    std::printf("  time        wall clock per curve update, best of %d run%s\n",
        repetitions, repetitions == 1 ? "" : "s");
}

inline void print_cache_rows(const Measurements& measured, const Scenarios& scenarios,
                             double output_count)
{
    std::printf("\nresult: LRU cache, cost of one curve update\n");
    std::printf("  %7s %9s  %-8s %12s %14s %13s %9s\n",
                "columns", "cache", "order", "time", "per output", "misses", "hit rate");
    for (const auto& row : measured.cache) {
        const double kilobytes = row.capacity * scenarios.padded_count() * 8 / 1e3;
        char cache_size[32];
        std::snprintf(cache_size, sizeof cache_size, kilobytes < 1000 ? "%.0f KB" : "%.1f MB",
                      kilobytes < 1000 ? kilobytes : kilobytes / 1000);
        const double hit_rate = 100.0 * (1.0 - double(row.counts.misses) / row.counts.lookups);
        std::printf("  %7d %9s  %-8s %9.1f ms %11.2f ns %13s %8.2f%%\n",
            row.capacity, cache_size, row.order, row.milliseconds,
            row.milliseconds * 1e6 / output_count,
            grouped_digits(row.counts.misses).c_str(), hit_rate);
    }
}

inline void print_cache_comparisons(const Measurements& measured,
                                   const std::vector<int>& capacities, std::size_t floor)
{
    for (int capacity : capacities) {
        const CacheResult* random = nullptr;
        const CacheResult* grouped = nullptr;
        for (const auto& row : measured.cache) {
            if (row.capacity != capacity) {
                continue;
            }
            if (std::string(row.order) == "random") random = &row;
            if (std::string(row.order) == "grouped") grouped = &row;
        }
        if (random && grouped) {
            std::printf("  at %d columns: grouped is %.1fx faster than random%s\n",
                capacity, random->milliseconds / grouped->milliseconds,
                grouped->counts.misses == floor ? ", at the miss floor" : "");
        } else if (grouped) {
            std::printf("  at %d columns: random order not run "
                        "(skipped above 100,000 trades unless LRU_ALL is set)\n", capacity);
        }
    }
}

inline void print_context(const Measurements& measured, double output_count, std::size_t dates)
{
    std::printf("\ncontext: same book, other methods\n");
    const auto row = [&](const char* name, double milliseconds, bool baseline = false) {
        std::printf("  %-56s %9.1f ms %11.2f ns per output",
                    name, milliseconds, milliseconds * 1e6 / output_count);
        if (measured.baseline >= 0.0 && !baseline) {
            std::printf(" %8.0fx vs BASE", measured.baseline / milliseconds);
        }
        std::printf("\n");
    };
    if (measured.baseline >= 0.0) row("BASE  per-scenario repricing", measured.baseline, true);
    row("full table, random order", measured.table_random);
    row("full table, sorted order", measured.table_sorted);
    std::printf("  %-56s %9.1f ms %11.2f ns per date (both curves)\n",
        "full table build, per curve update", measured.table_build,
        measured.table_build * 1e6 / dates);
    row("scalar reverse scan, sensitivities only (no scenarios)", measured.scalar_scan);
}

inline void print_accuracy(const Measurements& measured)
{
    const auto& accuracy = measured.accuracy;
    std::printf("\naccuracy (each case compared with the scalar scan, "
                "relative to max(|value|, 1e4))\n");
    std::printf("  largest difference over %zu runs: %.1e (%s)\n",
        accuracy.cases_checked, accuracy.largest_error, accuracy.worst_case.c_str());
    if (measured.baseline >= 0.0) std::printf("  BASE: %.1e\n", measured.baseline_error);
    std::printf("  central differences use eps = 1e-5; truncation error is expected\n");
    std::printf("\ncorrectness gate: %s (each case agrees to %.0e; every value finite)\n",
        accuracy.passed ? "PASS" : "FAIL", AccuracyReport::tolerance);
}

inline void print_report(const Book& book, const Scenarios& scenarios, int repetitions,
                         const std::vector<int>& capacities, const Measurements& measured)
{
    const double output_count = double(book.instruments.size()) * scenarios.wave_count();
    print_configuration(book, scenarios);
    print_terms(book, scenarios, repetitions);
    print_cache_rows(measured, scenarios, output_count);
    print_cache_comparisons(measured, capacities, count_used_dates(book).total());
    print_context(measured, output_count, book.times.size());
    print_accuracy(measured);
}

} // namespace scenario_demo
