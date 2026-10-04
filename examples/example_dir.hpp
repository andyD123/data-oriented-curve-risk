#pragma once
// CMake copies example inputs into a build directory. An explicit directory is
// useful for isolated replays; without CMake or an override, retain the current one.
#include <filesystem>
#include <stdexcept>
#include <string>
inline void enter_example_dir(const char* directory = nullptr) {
#if defined(LADDER_EXAMPLE_DIR)
    if (!directory) directory = LADDER_EXAMPLE_DIR;
#endif
    if (!directory) return;
    std::error_code ec;
    std::filesystem::current_path(directory, ec);
    if (ec) throw std::runtime_error(std::string("cannot enter ") + directory + ": " + ec.message());
}
