#pragma once

// Implementation detail of the date-cache demonstration. Pricing sees only
// fetch(date, fill_column). No virtual dispatch or std::function is involved.
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace scenario_demo {

struct CacheCounts {
    std::size_t misses = 0;
    std::size_t lookups = 0;
};

class DateCache {
public:
    DateCache(int capacity, int column_width)
        : capacity_(checked_capacity(capacity)), width_(checked_width(column_width)),
          keys_(capacity_, -1), previous_(capacity_, -1), next_(capacity_, -1),
          values_(static_cast<std::size_t>(capacity_) * width_)
    {
        locations_.reserve(static_cast<std::size_t>(capacity_) * 2);
    }

    // date is a non-negative index in the prepared date table.
    // The pointer remains valid until its slot is evicted. With capacity >= 2,
    // fetching start then end leaves both projection columns live for one coupon.
    template<class FillColumn>
    const double* fetch(int date, FillColumn&& fill_column)
    {
        ++counts_.lookups;
        if (date == most_recent_date_) {
            return column(most_recent_slot_);
        }
        const auto found = locations_.find(date);
        int slot;
        if (found != locations_.end()) {
            slot = found->second;
            unlink(slot);
            put_first(slot);
        } else {
            ++counts_.misses;
            slot = acquire_slot();
            keys_[slot] = date;
            locations_[date] = slot;
            put_first(slot);
            fill_column(date, column(slot));
        }
        most_recent_date_ = date;
        most_recent_slot_ = slot;
        return column(slot);
    }

    CacheCounts counts() const { return counts_; }

private:
    static int checked_capacity(int capacity)
    {
        // Pricing an IBOR coupon keeps two projection columns live at once.
        if (capacity < 2 || capacity > 1000000) {
            throw std::invalid_argument("date cache needs 2..1000000 columns");
        }
        return capacity;
    }

    static int checked_width(int width)
    {
        if (width < 1 || width > 256) {
            throw std::invalid_argument("date cache needs 1..256 values per column");
        }
        return width;
    }

    double* column(int slot)
    {
        return values_.data() + static_cast<std::size_t>(slot) * width_;
    }

    int acquire_slot()
    {
        if (used_ < capacity_) {
            return used_++;
        }
        const int slot = last_;
        unlink(slot);
        locations_.erase(keys_[slot]);
        return slot;
    }

    void unlink(int slot)
    {
        if (previous_[slot] >= 0) {
            next_[previous_[slot]] = next_[slot];
        } else {
            first_ = next_[slot];
        }
        if (next_[slot] >= 0) {
            previous_[next_[slot]] = previous_[slot];
        } else {
            last_ = previous_[slot];
        }
    }

    void put_first(int slot)
    {
        previous_[slot] = -1;
        next_[slot] = first_;
        if (first_ >= 0) {
            previous_[first_] = slot;
        }
        first_ = slot;
        if (last_ < 0) {
            last_ = slot;
        }
    }

    int capacity_;
    int width_;
    std::vector<int> keys_;
    std::vector<int> previous_;
    std::vector<int> next_;
    std::vector<double> values_;
    std::unordered_map<int, int> locations_;
    int first_ = -1;
    int last_ = -1;
    int used_ = 0;
    int most_recent_date_ = -1;
    int most_recent_slot_ = -1;
    CacheCounts counts_;
};

} // namespace scenario_demo
