// Copy reference for the fully materialised risk output: time of a plain std::memcpy of a buffer the size of the
// padded output (default 273,504,000 bytes, the 500k seasoned book). Reads and writes that many bytes.
// Untuned library memcpy; a reference point for risk-time / copy-time, not a bandwidth ceiling.
#include "ladder/aligned_memory.hpp"
#include "../support/input.hpp"
#include "../support/timing.hpp"
#include <cstdio>
#include <cstring>

int main(int argc, char** argv)
{
    try {
        constexpr auto usage = "usage: copy_floor [bytes=273504000] [reps=1..100]";
        if (argc > 3) throw std::invalid_argument(usage);
        const auto bytes = argc > 1 ? demo::read_count(argv[1], 64, 4ULL*1024*1024*1024, usage) : 273504000ULL;
        const auto reps = argc > 2 ? demo::read_count(argv[2], 1, 100, usage) : 5;
        if (bytes % 64) throw std::invalid_argument("copy_floor: byte count must be a multiple of 64");
        void* from = ladder::allocate_aligned(bytes); void* to = ladder::allocate_aligned(bytes);
        std::memset(from, 1, bytes); std::memset(to, 2, bytes);
        const double ms = demo::measure_best(static_cast<int>(reps), [&]{ std::memcpy(to, from, bytes); });
        const unsigned char check = static_cast<unsigned char*>(to)[bytes / 2];
        ladder::free_aligned(from); ladder::free_aligned(to);
        std::printf("copy_floor: %zu bytes (%.2f MiB), best of %zu\n", (size_t)bytes, bytes/1048576.0, (size_t)reps);
        std::printf("memcpy: %.3f ms, %.2f GB/s copied (%.2f GB/s read+write)%s\n", ms, (bytes/1e9)/(ms/1e3), 2*(bytes/1e9)/(ms/1e3), check == 1 ? "" : " [check failed]");
        return check == 1 ? 0 : 1;
    } catch (const std::exception& e) { std::fprintf(stderr, "copy_floor: %s\n", e.what()); return 1; }
}
