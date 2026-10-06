// Compare the same risk values stored in contiguous ladders and labelled maps.
// Read run_experiment first. Timing is a helper; the operations being measured
// have their own names and are below. The historical default is 100,000 trades.
#include "../support/input.hpp"
#include "../support/timing.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace aggregation_demo {
namespace {

constexpr int wave_count = 66;
constexpr std::size_t instruments_per_book = 500;
constexpr std::size_t books_per_desk = 10;
using OrderedLadder = std::map<std::string, double>;
using HashedLadder = std::unordered_map<std::string, double>;

struct Inputs {
    std::vector<std::string> labels;
    std::vector<double> flat;
    std::vector<OrderedLadder> ordered;
    std::vector<HashedLadder> hashed;
};

Inputs prepare_inputs(std::size_t count)
{
    Inputs input;
    for (int wave = 0; wave < wave_count; ++wave) {
        input.labels.push_back((wave < 30 ? "EONIA_" : "EUR6M_")
                               + std::to_string(wave) + "M");
    }
    std::mt19937_64 random(1);
    std::uniform_real_distribution<double> value(-1e3, 1e3);
    input.flat.resize(count * wave_count);
    std::generate(input.flat.begin(), input.flat.end(), [&] { return value(random); });
    input.ordered.resize(count);
    input.hashed.resize(count);
    for (std::size_t instrument = 0; instrument < count; ++instrument) {
        for (int wave = 0; wave < wave_count; ++wave) {
            const double risk = input.flat[instrument * wave_count + wave];
            input.ordered[instrument][input.labels[wave]] = risk;
            input.hashed[instrument][input.labels[wave]] = risk;
        }
    }
    return input;
}

void add_ladder(std::span<double> total, std::span<const double> contribution)
{
    const auto add = [](double accumulated, double value) { return accumulated + value; };
    std::transform(total.begin(), total.end(), contribution.begin(), total.begin(), add);
}

void aggregate_contiguous(const std::vector<double>& input, std::vector<double>& total)
{
    std::fill(total.begin(), total.end(), 0.0);
    for (std::size_t offset = 0; offset < input.size(); offset += wave_count) {
        add_ladder(total, {input.data() + offset, wave_count});
    }
}

template<class Map>
void aggregate_maps(const std::vector<Map>& input, Map& total)
{
    // Clearing/rebuilding the output map remains inside the measured operation.
    total.clear();
    for (const auto& instrument : input) {
        for (const auto& [tenor, value] : instrument) {
            total[tenor] += value;
        }
    }
}

struct Hierarchy {
    std::vector<double> books;
    std::vector<double> desks;
};

Hierarchy make_hierarchy(std::size_t count)
{
    const auto books = (count + instruments_per_book - 1) / instruments_per_book;
    const auto desks = (books + books_per_desk - 1) / books_per_desk;
    return {std::vector<double>(books * wave_count), std::vector<double>(desks * wave_count)};
}

void aggregate_hierarchy(const std::vector<double>& input, Hierarchy& output)
{
    std::fill(output.books.begin(), output.books.end(), 0.0);
    std::fill(output.desks.begin(), output.desks.end(), 0.0);
    const auto count = input.size() / wave_count;
    for (std::size_t instrument = 0; instrument < count; ++instrument) {
        const auto book = instrument / instruments_per_book;
        add_ladder({output.books.data() + book * wave_count, wave_count},
                   {input.data() + instrument * wave_count, wave_count});
    }
    for (std::size_t book = 0; book < output.books.size() / wave_count; ++book) {
        const auto desk = book / books_per_desk;
        add_ladder({output.desks.data() + desk * wave_count, wave_count},
                   {output.books.data() + book * wave_count, wave_count});
    }
}

void check_results(const Inputs& input, const std::vector<double>& flat_total,
                   const OrderedLadder& ordered_total, const HashedLadder& hashed_total,
                   const Hierarchy& hierarchy)
{
    for (int wave = 0; wave < wave_count; ++wave) {
        long double expected = 0.0L;
        for (std::size_t offset = wave; offset < input.flat.size(); offset += wave_count) {
            expected += static_cast<long double>(input.flat[offset]);
        }
        double desk_total = 0.0;
        for (std::size_t offset = wave; offset < hierarchy.desks.size(); offset += wave_count) {
            desk_total += hierarchy.desks[offset];
        }
        const auto agrees = [expected](double actual) {
            return std::isfinite(actual)
                && std::fabs(static_cast<long double>(actual) - expected)
                    <= 2e-11L * std::max(1e4L, std::fabs(expected));
        };
        if (!agrees(flat_total[wave]) || !agrees(ordered_total.at(input.labels[wave]))
            || !agrees(hashed_total.at(input.labels[wave])) || !agrees(desk_total)) {
            throw std::runtime_error("aggregation comparison failed");
        }
    }
}

struct Measurements {
    double flat;
    double hierarchy;
    double ordered;
    double hashed;
};

void print_results(std::size_t count, const Measurements& measured)
{
    const double values = double(count) * wave_count;
    std::printf("%zu instruments x 66 sensitivities, book-level aggregation (one core):\n", count);
    std::printf("  contiguous ordered ladders, vector add        %8.2f ms   (%.2f ns/value)\n",
                measured.flat, measured.flat * 1e6 / values);
    std::printf("  contiguous, two-level hierarchy (book, desk)  %8.2f ms\n", measured.hierarchy);
    std::printf("  std::unordered_map per instrument            %8.1f ms   %.0fx slower\n",
                measured.hashed, measured.hashed / measured.flat);
    std::printf("  std::map per instrument                      %8.1f ms   %.0fx slower\n",
                measured.ordered, measured.ordered / measured.flat);
    std::printf("  memory: flat %.0f MB vs maps ~%.0f MB (historical node-size estimate)\n",
                values * 8 / 1e6, values * (48.0 + 32 + 8) / 1e6);
    std::printf("correctness gate: PASS (all representations and desk totals checked)\n");
}

int run_experiment(std::size_t count)
{
    const auto input = prepare_inputs(count);
    std::vector<double> total(wave_count);
    OrderedLadder ordered_total;
    HashedLadder hashed_total;
    auto hierarchy = make_hierarchy(count);
    Measurements measured;

    measured.flat = demo::measure_best(5, [&] {
        aggregate_contiguous(input.flat, total);
    });
    measured.ordered = demo::measure_best(3, [&] {
        aggregate_maps(input.ordered, ordered_total);
    });
    measured.hashed = demo::measure_best(3, [&] {
        aggregate_maps(input.hashed, hashed_total);
    });
    measured.hierarchy = demo::measure_best(5, [&] {
        aggregate_hierarchy(input.flat, hierarchy);
    });

    check_results(input, total, ordered_total, hashed_total, hierarchy);
    print_results(count, measured);
    return 0;
}

} // namespace
} // namespace aggregation_demo

int main(int argc, char** argv)
{
    try {
        constexpr auto usage = "usage: aggregation [instruments=1..1000000]";
        if (argc > 2) throw std::invalid_argument(usage);
        const auto count = argc > 1 ? demo::read_count(argv[1], 1, 1000000, usage) : 100000;
        return aggregation_demo::run_experiment(count);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "aggregation: %s\n", error.what());
        return 1;
    }
}
