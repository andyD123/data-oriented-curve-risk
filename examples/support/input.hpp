#pragma once

#include <charconv>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace demo {

inline std::size_t read_count(
    std::string_view text,
    std::size_t minimum,
    std::size_t maximum,
    const char* error)
{
    std::size_t value = 0;
    const auto [end, status] = std::from_chars(
        text.data(), text.data() + text.size(), value);

    if (status != std::errc{} || end != text.data() + text.size()
        || value < minimum || value > maximum) {
        throw std::invalid_argument(error);
    }
    return value;
}

} // namespace demo
