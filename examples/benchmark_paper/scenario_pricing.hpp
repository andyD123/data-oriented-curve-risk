#pragma once

#include "scenario_book.hpp"
#include "../support/curve_input.hpp"
#include "ladder/replicate.hpp"
#include "ladder/scan.hpp"
#include <span>

namespace scenario_demo {

// The formulas are independent of the source of the curve columns. The full
// table and the LRU cache both call these expressions in the same pricing loop.
inline constexpr auto fixed_cashflow_pv = [](double amount, auto discount) {
    return amount * discount;
};

inline constexpr auto floating_coupon_pv = [](
    double scale, auto projection_start, auto projection_end, auto discount) {
    return scale * (projection_start / projection_end - 1.0) * discount;
};

struct Scenarios {
    int discount_waves;
    int projection_waves;
    static constexpr double bump = 1e-5;
    static constexpr int maximum_columns = 256;

    explicit Scenarios(const demo::CurvePair& curves)
        : discount_waves(curves.discount.waves.K()),
          projection_waves(curves.projection.waves.K())
    {
        if (padded_count() > maximum_columns) {
            throw std::runtime_error("at most 128 waves are supported");
        }
    }

    int wave_count() const { return discount_waves + projection_waves; }
    int count() const { return 2 * wave_count(); }
    int padded_count() const { return (count() + 7) / 8 * 8; }
};

inline void fill_discount_column(
    const demo::Curve& curve, const Scenarios& scenarios, double time, double* values)
{
    const double base = curve.discount(time);
    const int count = scenarios.discount_waves;
    for (int wave = 1; wave <= count; ++wave) {
        const double overlap = curve.waves.overlap(wave, time);
        values[wave - 1] = base * std::exp(-Scenarios::bump * overlap);
        values[count + wave - 1] = base * std::exp(Scenarios::bump * overlap);
    }
    for (int scenario = 2 * count; scenario < scenarios.padded_count(); ++scenario) {
        values[scenario] = base;
    }
}

inline void fill_projection_column(
    const demo::Curve& curve, const Scenarios& scenarios, double time, double* values)
{
    const double base = curve.discount(time);
    const int offset = 2 * scenarios.discount_waves;
    const int count = scenarios.projection_waves;
    std::fill_n(values, offset, base);
    for (int wave = 1; wave <= count; ++wave) {
        const double overlap = curve.waves.overlap(wave, time);
        values[offset + wave - 1] = base * std::exp(-Scenarios::bump * overlap);
        values[offset + count + wave - 1] = base * std::exp(Scenarios::bump * overlap);
    }
    for (int scenario = scenarios.count(); scenario < scenarios.padded_count(); ++scenario) {
        values[scenario] = base;
    }
}

struct ScenarioTables {
    std::vector<double> discount;
    std::vector<double> projection;
};

inline void refresh_tables(const Book& book, const demo::CurvePair& curves,
                           const Scenarios& scenarios, ScenarioTables& tables)
{
    const int width = scenarios.padded_count();
    tables.discount.assign(book.times.size() * width, 0.0);
    tables.projection.assign(book.times.size() * width, 0.0);
    for (std::size_t date = 0; date < book.times.size(); ++date) {
        fill_discount_column(curves.discount, scenarios, book.times[date],
                             tables.discount.data() + date * width);
        fill_projection_column(curves.projection, scenarios, book.times[date],
                               tables.projection.data() + date * width);
    }
}

template<class Contribution>
void accumulate_scenarios(std::span<double> values, Contribution&& contribution)
{
    for (std::size_t scenario = 0; scenario < values.size(); ++scenario) {
        values[scenario] += contribution(scenario);
    }
}

template<class DiscountAt, class ProjectionAt>
void price_instrument(const Instrument& instrument, std::span<double> values,
                      DiscountAt& discount_at, ProjectionAt& projection_at)
{
    std::fill(values.begin(), values.end(), 0.0);
    for (const auto& cashflow : instrument.fixed) {
        const double* discount = discount_at(cashflow.date);
        accumulate_scenarios(values, [&](std::size_t scenario) {
            return fixed_cashflow_pv(cashflow.amount, discount[scenario]);
        });
    }
    for (const auto& coupon : instrument.floating) {
        const double* start = projection_at(coupon.start);
        const double* end = projection_at(coupon.end);
        const double* discount = discount_at(coupon.payment);
        accumulate_scenarios(values, [&](std::size_t scenario) {
            return floating_coupon_pv(
                coupon.scale, start[scenario], end[scenario], discount[scenario]);
        });
    }
}

inline void write_wave_deltas(
    const Scenarios& scenarios, const double* values, double* output)
{
    const int discount_count = scenarios.discount_waves;
    const int projection_count = scenarios.projection_waves;
    for (int wave = 0; wave < discount_count; ++wave) {
        output[wave] = (values[wave] - values[discount_count + wave])
                     / (2 * Scenarios::bump);
    }
    const int offset = 2 * discount_count;
    for (int wave = 0; wave < projection_count; ++wave) {
        output[discount_count + wave] =
            (values[offset + wave] - values[offset + projection_count + wave])
            / (2 * Scenarios::bump);
    }
}

template<class DiscountAt, class ProjectionAt>
void price_scenarios(const Book& book, const InstrumentOrder& order,
                     const Scenarios& scenarios, DiscountAt&& discount_at,
                     ProjectionAt&& projection_at, std::vector<double>& output)
{
    alignas(64) double values[Scenarios::maximum_columns];
    for (std::size_t instrument : order.indices) {
        price_instrument(book.instruments[instrument],
            {values, static_cast<std::size_t>(scenarios.padded_count())},
            discount_at, projection_at);
        write_wave_deltas(scenarios, values,
                          output.data() + instrument * scenarios.wave_count());
    }
}

struct PreparedRisk {
    std::vector<std::vector<ladder::UnitCashflow>> discount;
    std::vector<std::vector<ladder::ProjectionTerm>> projection;
};

inline PreparedRisk prepare_risk(const Book& book, const demo::CurvePair& curves)
{
    const auto discount = [&](double time) { return curves.discount.discount(time); };
    const auto projection = [&](double time) { return curves.projection.discount(time); };
    PreparedRisk risk;
    risk.discount.resize(book.instruments.size());
    risk.projection.resize(book.instruments.size());
    for (std::size_t index = 0; index < book.instruments.size(); ++index) {
        const auto& instrument = book.instruments[index];
        for (const auto& cashflow : instrument.fixed) {
            ladder::replicate_fixed(cashflow.amount, book.times[cashflow.date],
                                    discount, risk.discount[index]);
        }
        for (const auto& coupon : instrument.floating) {
            ladder::replicate_ibor(coupon.scale, book.times[coupon.start],
                book.times[coupon.end], book.times[coupon.payment], discount, projection,
                risk.discount[index], risk.projection[index]);
        }
        ladder::sort_by_time(risk.discount[index]);
    }
    return risk;
}

inline void calculate_scan(const PreparedRisk& risk, const demo::CurvePair& curves,
                           std::vector<double>& output)
{
    for (std::size_t instrument = 0; instrument < risk.discount.size(); ++instrument) {
        double* row = output.data() + instrument * curves.wave_count();
        ladder::scan_discount(curves.discount.waves, risk.discount[instrument], row);
        ladder::scan_projection(curves.projection.waves, risk.projection[instrument],
                                row + curves.discount.waves.K());
    }
}

// Baseline stays independent of the cache: obtain each factor by curve evaluation
// plus a wave shift for this scenario. The derivative units remain rate shifts.
inline double shifted_discount(const demo::Curve& curve, double time, int scenario,
                               int first_scenario, int wave_count)
{
    double value = curve.discount(time);
    const int local = scenario - first_scenario;
    if (local >= 0 && local < wave_count) {
        value *= std::exp(-Scenarios::bump * curve.waves.overlap(local + 1, time));
    } else if (local >= wave_count && local < 2 * wave_count) {
        value *= std::exp(Scenarios::bump
                       * curve.waves.overlap(local - wave_count + 1, time));
    }
    return value;
}

inline double price_one_scenario(const Book& book, const Instrument& instrument,
                                const demo::CurvePair& curves,
                                const Scenarios& scenarios, int scenario)
{
    const auto discount = [&](int date) {
        return shifted_discount(curves.discount, book.times[date], scenario,
                                0, scenarios.discount_waves);
    };
    const auto projection = [&](int date) {
        return shifted_discount(curves.projection, book.times[date], scenario,
                                2 * scenarios.discount_waves, scenarios.projection_waves);
    };
    double value = 0.0;
    for (const auto& cashflow : instrument.fixed) {
        value += fixed_cashflow_pv(cashflow.amount, discount(cashflow.date));
    }
    for (const auto& coupon : instrument.floating) {
        value += floating_coupon_pv(coupon.scale, projection(coupon.start),
                                    projection(coupon.end), discount(coupon.payment));
    }
    return value;
}

inline void calculate_repricing(const Book& book, const demo::CurvePair& curves,
                                const Scenarios& scenarios, std::vector<double>& output)
{
    const auto price = [&](const Instrument& instrument, int scenario) {
        return price_one_scenario(book, instrument, curves, scenarios, scenario);
    };
    for (int wave = 0; wave < scenarios.wave_count(); ++wave) {
        const bool discount = wave < scenarios.discount_waves;
        const int local = discount ? wave : wave - scenarios.discount_waves;
        const int up = discount ? local : 2 * scenarios.discount_waves + local;
        const int down = up + (discount ? scenarios.discount_waves : scenarios.projection_waves);
        for (std::size_t index = 0; index < book.instruments.size(); ++index) {
            const auto& instrument = book.instruments[index];
            output[index * scenarios.wave_count() + wave] =
                (price(instrument, up) - price(instrument, down)) / (2 * Scenarios::bump);
        }
    }
}

} // namespace scenario_demo
