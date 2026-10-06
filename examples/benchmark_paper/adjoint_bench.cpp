// Compare the library scan, direct overlap summation and an illustrative tape.
// Each computes first derivatives per unit forward-rate shift. Record preparation
// is outside the timings; the direct and tape implementations remain independent
// of the library's scan traversal. Historical results are not rerun by this edit.
#include "../example_dir.hpp"
#include "../support/curve_input.hpp"
#include "../support/input.hpp"
#include "../support/timing.hpp"
#include "recorded_adjoint.hpp"
#include "ladder/replicate.hpp"
#include <random>

namespace adjoint_demo {
namespace {

struct RiskRecords {
    std::vector<ladder::UnitCashflow> discount;
    std::vector<ladder::ProjectionTerm> projection;
};

RiskRecords make_instrument_records(const demo::CurvePair& curves, int start, int years,
                                   int type, double notional, double rate)
{
    const auto time = [start](int period, int frequency) {
        return (start + static_cast<int>(std::lround(period * 365.25 / frequency))) / 365.0;
    };
    const auto discount = [&](double t) { return curves.discount.discount(t); };
    const auto projection = [&](double t) { return curves.projection.discount(t); };
    RiskRecords records;
    if (type == 0) {
        for (int period = 1; period <= 2 * years; ++period) {
            const double redemption = period == 2 * years ? notional : 0.0;
            ladder::replicate_fixed(notional * rate / 2 + redemption,
                                    time(period, 2), discount, records.discount);
        }
    } else {
        for (int period = 1; period <= years; ++period) {
            ladder::replicate_fixed(-notional * rate, time(period, 1), discount, records.discount);
        }
        for (int period = 1; period <= 2 * years; ++period) {
            ladder::replicate_ibor(notional, time(period - 1, 2), time(period, 2), time(period, 2),
                                   discount, projection, records.discount, records.projection);
        }
    }
    ladder::sort_by_time(records.discount);
    return records;
}

std::vector<RiskRecords> make_risk_records(std::size_t count, const demo::CurvePair& curves)
{
    std::mt19937_64 random(42);
    std::uniform_int_distribution<int> start_day(2, 61), years(1, 29), type(0, 1);
    std::uniform_real_distribution<double> coupon_rate(0.01, 0.06);
    const double notionals[] = {1e6, 2.5e6, 5e6, 1e7};
    std::vector<RiskRecords> book(count);
    for (auto& instrument : book) {
        // Preserve the random draw order of this demonstrator, not bench_paper's
        // different generator. The two books share a specification, not every draw.
        const int start = start_day(random);
        const int maturity = years(random);
        const int kind = type(random);
        const double notional = notionals[random() % 4];
        const double rate = coupon_rate(random);
        instrument = make_instrument_records(curves, start, maturity, kind, notional, rate);
    }
    return book;
}

void scan_gradient(const demo::CurvePair& curves, const RiskRecords& instrument,
                   std::span<double> gradient)
{
    ladder::scan_discount(curves.discount.waves, instrument.discount, gradient.data());
    ladder::scan_projection(curves.projection.waves, instrument.projection,
                            gradient.data() + curves.discount.waves.K());
}

void direct_gradient(const demo::CurvePair& curves, const RiskRecords& instrument,
                     std::span<double> gradient)
{
    const auto& discount = curves.discount.waves;
    const auto& projection = curves.projection.waves;
    for (int wave = 1; wave <= discount.K(); ++wave) {
        double total = 0.0;
        for (const auto& cashflow : instrument.discount) {
            total -= cashflow.x * discount.overlap(wave, cashflow.t);
        }
        gradient[wave - 1] = total;
    }
    for (int wave = 1; wave <= projection.K(); ++wave) {
        double total = 0.0;
        for (const auto& coupon : instrument.projection) {
            total += coupon.w * (projection.psi(wave, coupon.a) - projection.psi(wave, coupon.b));
        }
        gradient[discount.K() + wave - 1] = total;
    }
}

template<class Calculate>
void calculate_ladders(const std::vector<RiskRecords>& book, int waves,
                       std::vector<double>& output, Calculate&& calculate)
{
    for (std::size_t index = 0; index < book.size(); ++index) {
        const std::span<double> row(output.data() + index * waves, static_cast<std::size_t>(waves));
        calculate(book[index], row);
    }
}

double largest_difference(const std::vector<double>& actual, const std::vector<double>& expected)
{
    double largest = 0.0;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (!std::isfinite(actual[index]) || !std::isfinite(expected[index])) {
            throw std::runtime_error("non-finite adjoint result");
        }
        const double scale = std::max(1.0, std::fabs(expected[index]));
        largest = std::max(largest, std::fabs(actual[index] - expected[index]) / scale);
    }
    return largest;
}

void print_results(std::size_t instruments, const demo::CurvePair& curves,
                   double scan_ms, double direct_ms, double tape_ms,
                   double direct_error, double tape_error, std::size_t tape_nodes)
{
    const double output_count = double(instruments) * curves.wave_count();
    std::printf("N=%zu bonds+vanilla swaps, %d+%d waves; direct vs scan %.1e, tape vs scan %.1e\n",
        instruments, curves.discount.waves.K(), curves.projection.waves.K(), direct_error, tape_error);
    const auto row = [&](const char* name, double milliseconds) {
        std::printf("%-40s %10.1f ms %9.2f ns/sensitivity\n",
                    name, milliseconds, milliseconds * 1e6 / output_count);
    };
    row("SCAN  scalar library scan", scan_ms);
    row("DIRECT N*K overlap adjoint, no tape", direct_ms);
    row("TAPE  reverse mode with recorded tape", tape_ms);
    std::printf("tape nodes in the final instrument: %zu\n", tape_nodes);
    std::printf("correctness gate: PASS (finite results; scaled difference <= 1e-8)\n");
}

int run_experiment(std::size_t count, int repetitions)
{
    const auto curves = demo::read_curves("quantlib_example_curves.txt");
    const auto book = make_risk_records(count, curves);
    std::vector<double> scan(count * curves.wave_count());
    std::vector<double> direct(scan.size());
    std::vector<double> recorded(scan.size());
    TapeWorkspace tape;

    const auto scan_ms = demo::measure_best(repetitions, [&] {
        calculate_ladders(book, curves.wave_count(), scan,
            [&](const auto& instrument, auto row) { scan_gradient(curves, instrument, row); });
    });
    const auto direct_ms = demo::measure_best(repetitions, [&] {
        calculate_ladders(book, curves.wave_count(), direct,
            [&](const auto& instrument, auto row) { direct_gradient(curves, instrument, row); });
    });
    const auto tape_ms = demo::measure_best(repetitions, [&] {
        calculate_ladders(book, curves.wave_count(), recorded, [&](const auto& instrument, auto row) {
            recorded_gradient(curves.discount.waves, curves.projection.waves,
                               instrument.discount, instrument.projection, row, tape);
        });
    });

    const auto direct_error = largest_difference(direct, scan);
    const auto tape_error = largest_difference(recorded, scan);
    if (direct_error > 1e-8 || tape_error > 1e-8) {
        throw std::runtime_error("adjoint comparison failed");
    }
    print_results(count, curves, scan_ms, direct_ms, tape_ms,
                  direct_error, tape_error, tape.nodes.size());
    return 0;
}

} // namespace
} // namespace adjoint_demo

int main(int argc, char** argv)
{
    try {
        enter_example_dir();
        constexpr auto usage = "usage: adjoint_bench [N=1..10000000] [reps=1..1000]";
        if (argc > 3) throw std::invalid_argument(usage);
        const auto count = argc > 1 ? demo::read_count(argv[1], 1, 10000000, usage) : 100000;
        const auto repeats = argc > 2 ? demo::read_count(argv[2], 1, 1000, usage) : 3;
        return adjoint_demo::run_experiment(count, static_cast<int>(repeats));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "adjoint_bench: %s\n", error.what());
        return 1;
    }
}
