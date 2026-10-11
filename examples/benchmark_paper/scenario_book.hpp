#pragma once

// Synthetic schedules for the scenario-cache experiment; pure primitive dates without OO calendar bloat.
// Keep the original draw order and day rounding: changing either changes the book.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <numeric>
#include <random>
#include <unordered_map>
#include <vector>

namespace scenario_demo {

inline constexpr double days_per_year = 365.0;

struct FixedCashflow {
    int date;
    double amount;
};

struct FloatingCoupon {
    int payment;
    int start;
    int end;
    double scale;
};

struct Instrument {
    int signature;
    std::vector<FixedCashflow> fixed;
    std::vector<FloatingCoupon> floating;
};

struct Book {
    std::vector<Instrument> instruments;
    std::vector<double> times;
};

inline Instrument make_instrument(
    int start, int years, int type, double notional, double rate)
{
    const auto payment_day = [start](int period, int frequency) {
        return start + static_cast<int>(std::lround(period * 365.25 / frequency));
    };
    Instrument instrument;
    instrument.signature = type * 100000 + start * 100 + years;

    if (type == 0) {
        for (int period = 1; period <= 2 * years; ++period) {
            const double redemption = period == 2 * years ? notional : 0.0;
            instrument.fixed.push_back({
                payment_day(period, 2), notional * rate / 2 + redemption});
        }
    } else {
        for (int period = 1; period <= years; ++period) {
            instrument.fixed.push_back({payment_day(period, 1), -notional * rate});
        }
        for (int period = 1; period <= 2 * years; ++period) {
            instrument.floating.push_back({payment_day(period, 2),
                payment_day(period - 1, 2), payment_day(period, 2), notional});
        }
    }
    return instrument;
}

inline std::vector<Instrument> generate_instruments(std::size_t count)
{
    std::mt19937_64 random(42);
    std::uniform_int_distribution<int> start_day(2, 61), years(1, 29), type(0, 1);
    std::uniform_real_distribution<double> coupon_rate(0.01, 0.06);
    const double notionals[] = {1e6, 2.5e6, 5e6, 1e7};
    std::vector<Instrument> instruments(count);

    for (auto& instrument : instruments) {
        const int start = start_day(random);
        const int maturity = years(random);
        const int kind = type(random);
        const double notional = notionals[random() % 4];
        const double rate = coupon_rate(random);
        instrument = make_instrument(start, maturity, kind, notional, rate);
    }
    return instruments;
}

inline std::vector<int> collect_dates(const std::vector<Instrument>& instruments)
{
    std::vector<int> days;
    for (const auto& instrument : instruments) {
        for (const auto& cashflow : instrument.fixed) {
            days.push_back(cashflow.date);
        }
        for (const auto& coupon : instrument.floating) {
            days.insert(days.end(), {coupon.payment, coupon.start, coupon.end});
        }
    }
    std::sort(days.begin(), days.end());
    days.erase(std::unique(days.begin(), days.end()), days.end());
    return days;
}

inline Book make_book(std::size_t count)
{
    Book book{generate_instruments(count), {}};
    const auto days = collect_dates(book.instruments);
    std::unordered_map<int, int> date_index;
    for (std::size_t index = 0; index < days.size(); ++index) {
        date_index[days[index]] = static_cast<int>(index);
        book.times.push_back(days[index] / days_per_year);
    }
    for (auto& instrument : book.instruments) {
        for (auto& cashflow : instrument.fixed) {
            cashflow.date = date_index.at(cashflow.date);
        }
        for (auto& coupon : instrument.floating) {
            coupon.payment = date_index.at(coupon.payment);
            coupon.start = date_index.at(coupon.start);
            coupon.end = date_index.at(coupon.end);
        }
    }
    return book;
}

struct InstrumentOrder {
    const char* name;
    std::vector<std::size_t> indices;
};

inline std::array<InstrumentOrder, 3> make_orders(const Book& book)
{
    std::array<InstrumentOrder, 3> orders{{{"random", {}}, {"sorted", {}}, {"grouped", {}}}};
    for (auto& order : orders) {
        order.indices.resize(book.instruments.size());
        std::iota(order.indices.begin(), order.indices.end(), 0);
    }
    std::shuffle(orders[0].indices.begin(), orders[0].indices.end(), std::mt19937_64(7));
    const auto signature = [&](std::size_t index) {
        return book.instruments[index].signature;
    };
    const auto grouped_key = [](int signature) {
        return ((signature / 100) % 1000) * 1000
             + (signature / 100000) * 100 + signature % 100;
    };
    std::stable_sort(orders[1].indices.begin(), orders[1].indices.end(),
        [&](auto left, auto right) { return signature(left) < signature(right); });
    std::stable_sort(orders[2].indices.begin(), orders[2].indices.end(),
        [&](auto left, auto right) {
            return grouped_key(signature(left)) < grouped_key(signature(right));
        });
    return orders;
}

struct DateCounts {
    std::size_t discount = 0;
    std::size_t projection = 0;

    std::size_t total() const { return discount + projection; }
};

inline DateCounts count_used_dates(const Book& book)
{
    std::vector<bool> discount(book.times.size()), projection(book.times.size());
    for (const auto& instrument : book.instruments) {
        for (const auto& cashflow : instrument.fixed) {
            discount[cashflow.date] = true;
        }
        for (const auto& coupon : instrument.floating) {
            discount[coupon.payment] = true;
            projection[coupon.start] = true;
            projection[coupon.end] = true;
        }
    }
    return {static_cast<std::size_t>(std::count(discount.begin(), discount.end(), true)),
            static_cast<std::size_t>(std::count(projection.begin(), projection.end(), true))};
}

} // namespace scenario_demo
