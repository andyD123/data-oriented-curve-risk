// Write-only reference for the fully materialised risk output.
// Default bytes = the 64,034-group padded buffer used by the 500k paper book.
// This is a fresh reproducible reference, not the source of the historical 270 MiB / 15.7 ms number.
#include "ladder/aligned_memory.hpp"
#include "../support/input.hpp"
#include "../support/timing.hpp"
#include <cstdio>
#include <cstring>
#include <immintrin.h>

namespace {
constexpr std::size_t paper_padded_bytes = 270479616;
void stream_zero(void* storage, std::size_t bytes)
{
    auto* output = static_cast<double*>(storage);
    const std::size_t doubles = bytes / sizeof(double);
    const __m512d zero = _mm512_setzero_pd();
    for (std::size_t index = 0; index < doubles; index += 8)
        _mm512_stream_pd(output + index, zero);
    _mm_sfence();
}
}
int main(int argc, char** argv)
{
    try {
        constexpr auto usage = "usage: write_floor [bytes=270479616] [reps=1..100]";
        if (argc > 3) throw std::invalid_argument(usage);
        const auto bytes = argc > 1
            ? demo::read_count(argv[1], 64, 4ULL*1024*1024*1024, usage) : paper_padded_bytes;
        const auto reps = argc > 2 ? demo::read_count(argv[2], 1, 100, usage) : 5;
        if (bytes % 64) throw std::invalid_argument("write_floor: byte count must be a multiple of 64");
        void* storage = ladder::allocate_aligned(bytes);
        std::memset(storage, 0, bytes);
        const double ms = demo::measure_best(static_cast<int>(reps), [&]{ stream_zero(storage, bytes); });
        ladder::free_aligned(storage);
        const double gbps = (bytes/1e9)/(ms/1e3);
        std::printf("write_floor: %zu bytes (%.2f MiB), best of %zu\n",bytes,bytes/1048576.0,reps);
        std::printf("streaming stores: %.3f ms, %.2f GB/s\n",ms,gbps);
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"write_floor: %s\n",e.what()); return 1;
    }
}
