// Experiment: does a logical schedule group wider than one SIMD register
// improve the discount bucket/tail scan by amortising date/bucket control?
#include "ladder/aligned_memory.hpp"
#include "ladder/layout.hpp"
#include "ladder/scan_wide_experiment.hpp"
#include "ladder/stencil.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace ladder;

static double now_ms() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static std::size_t arg(const char* s, std::size_t lo, std::size_t hi) {
    std::string_view v(s); std::size_t x = 0;
    auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), x);
    if (ec != std::errc{} || p != v.data() + v.size() || x < lo || x > hi)
        throw std::invalid_argument("usage: wide_group_bench [N=8..1048576] [reps=1..50]");
    return x;
}
static double median(std::vector<double> x) {
    std::sort(x.begin(), x.end());
    return x[x.size()/2];
}
static const char* backend() {
#if defined(LADDER_LANES_AVX512)
    return "AVX-512";
#elif defined(LADDER_LANES_AVX2)
    return "AVX2";
#elif defined(LADDER_LANES_STDX)
    return "stdx";
#else
    return "portable";
#endif
}

static std::vector<double> canonical(const WideFixedLayout& L, const Stencils& S,
                                     const double* out, std::size_t N) {
    std::vector<double> c(N * static_cast<std::size_t>(S.K()), 0.0);
    for (const auto& G : L.groups) {
        for (int local = 0; local < G.n_valid; ++local) {
            const int block = local / LANES, lane = local % LANES;
            const std::size_t inst = static_cast<std::size_t>(G.inst[static_cast<std::size_t>(local)]);
            for (int k = 0; k < S.K(); ++k) {
                const std::size_t src = G.out_offset +
                    (static_cast<std::size_t>(k) * G.blocks + block) * LANES + lane;
                c[inst * static_cast<std::size_t>(S.K()) + k] = out[src];
            }
        }
    }
    return c;
}

int main(int argc, char** argv) try {
    if (argc > 3) throw std::invalid_argument("too many arguments");
    const std::size_t N = argc > 1 ? arg(argv[1], 8, 1048576) : 65536;
    const int reps = static_cast<int>(argc > 2 ? arg(argv[2], 1, 50) : 9);

    // 40 bounded annual forward buckets, 60 semiannual cashflows.
    Stencils S;
    for (int y = 0; y <= 40; ++y) S.B.push_back(static_cast<double>(y));
    S.validate();
    constexpr double DPY = 365.0;

    std::vector<InstrumentSpec> book(N);
    for (std::size_t i = 0; i < N; ++i) {
        auto& in = book[i];
        in.signature = 7; // deliberately one homogeneous schedule population
        const double coupon = 1.0 + 0.005 * static_cast<double>(i % 17);
        for (int h = 1; h <= 60; ++h) {
            const int day = static_cast<int>(std::lround(h * 0.5 * DPY));
            in.fixed.push_back({day, coupon + (h == 60 ? 100.0 : 0.0)});
        }
    }
    auto df = [](double t) { return std::exp(-0.035 * t); };
    const double outputs = static_cast<double>(N) * S.K();

    const std::vector<int> widths = {8,16,32,64,128,256,512,1024,2048,4096,8192,16384,32768,65536};
    std::vector<double> reference;
    double t8_stream = 0.0, t8_normal = 0.0;

    std::printf("WIDE SCHEDULE-GROUP EXPERIMENT\n");
    std::printf("backend=%s instruments=%zu cashflows_per_instrument=60 buckets=%d outputs=%.0f reps=%d\n",
                backend(), N, S.K(), outputs, reps);
    std::printf("logical width = instruments sharing one scalar date/bucket walk; hardware vector remains 8 doubles\n\n");
    std::printf("%8s %8s %10s %12s %12s %10s %10s\n",
                "width","groups","blocks","normal_ms","stream_ms","speedup","ns/output");

    for (int width : widths) {
        if (static_cast<std::size_t>(width) > N) break;
        const double b0 = now_ms();
        WideFixedLayout L = build_wide_fixed_layout(book, width, DPY);
        refresh_wide_fixed(L, S, df);
        const double build_ms = now_ms() - b0;
        (void)build_ms;

        double* out = static_cast<double*>(allocate_aligned(L.output_values * sizeof(double)));
        // Warm both paths before measuring.
        scan_wide_fixed<Store::normal>(L, S, out);
        scan_wide_fixed<Store::streaming>(L, S, out);

        std::vector<double> tn, ts;
        tn.reserve(reps); ts.reserve(reps);
        for (int r = 0; r < reps; ++r) {
            double t0 = now_ms();
            scan_wide_fixed<Store::normal>(L, S, out);
            tn.push_back(now_ms() - t0);
        }
        for (int r = 0; r < reps; ++r) {
            double t0 = now_ms();
            scan_wide_fixed<Store::streaming>(L, S, out);
            ts.push_back(now_ms() - t0);
        }
        const double mn = median(tn), ms = median(ts);
        if (width == 8) {
            t8_normal = mn; t8_stream = ms;
            reference = canonical(L, S, out, N);
        } else {
            const auto got = canonical(L, S, out, N);
            if (got.size() != reference.size()) throw std::runtime_error("canonical size mismatch");
            double worst = 0.0;
            for (std::size_t j = 0; j < got.size(); ++j) {
                const double scale = std::max(1.0, std::fabs(reference[j]));
                worst = std::max(worst, std::fabs(got[j] - reference[j]) / scale);
            }
            if (!(worst <= 2e-13)) throw std::runtime_error("wide result differs from width-8 reference");
        }

        int total_blocks = 0;
        for (const auto& G : L.groups) total_blocks += G.blocks;
        std::printf("%8d %8zu %10d %12.3f %12.3f %9.3fx %10.3f\n",
                    width, L.groups.size(), total_blocks, mn, ms,
                    t8_stream / ms, ms * 1e6 / outputs);
        free_aligned(out);
    }

    std::printf("\nwidth8 normal=%.3f ms stream=%.3f ms; speedups above use width-8 streaming as baseline\n",
                t8_normal, t8_stream);
    std::printf("correctness=PASS (every wider grouping reconciled to width-8 output)\n");
    std::printf("scope=fixed-cashflow discount ladder only; projection/IBOR/OIS not yet generalised\n");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "wide_group_bench: %s\n", e.what());
    return 1;
}
