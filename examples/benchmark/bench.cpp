#include "../example_dir.hpp"
#include "ladder/aligned_memory.hpp"
// Two-curve bucketed-risk benchmark on the curves of QuantLib's MulticurveBootstrapping example
// (node discount factors in quantlib_example_curves.txt, written by examples/quantlib_reconcile).
//   BASE  bump-and-reprice: AoS instruments, virtual npv, central differences on every stencil of both curves
//   SCAN  scalar reverse scan per instrument (ladder/scan.hpp) on unit cashflows
//   GRP   eight instruments per vector, shared date table, streaming output (ladder/scan_simd.hpp)
#include <charconv>
#include <fstream>
#include <string_view>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <random>
#include <vector>
#include "ladder/stencil.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
#include "ladder/layout.hpp"
#include "ladder/scan_simd.hpp"
using namespace ladder;
constexpr double DPY = 365.0;

struct Curve {                                             // log-linear on the example's bootstrapped nodes
    Stencils S; std::vector<double> logD;
    double df(double t) const { int k = S.bucket(t); double a = (t - S.B[k-1]) / S.len(k); return std::exp(logD[k-1] + a * (logD[k] - logD[k-1])); }
    Curve bumped(int k, double delta) const { Curve c = *this; for (int j = k; j <= S.K(); ++j) c.logD[j] -= delta * S.len(k); return c; }   // box bump of stencil k
};
#ifndef LADDER_CURVE_DATA_FILE
#define LADDER_CURVE_DATA_FILE "quantlib_example_curves.txt"
#endif
static void load_curves(const char* path, Curve& d, Curve& p) {
    std::ifstream in(path); int nc;
    if (!in || !(in >> nc) || nc != 2) throw std::runtime_error("cannot read two curves from " + std::string(path));
    for (Curve* c : {&d, &p}) {
        std::string name; int n;
        if (!(in >> name >> n) || n < 2 || n > 100000) throw std::runtime_error("invalid curve header");
        c->S.B.resize(n); c->logD.resize(n);
        for (int i=0; i<n; ++i) {
            double t, D;
            if (!(in >> t >> D) || !std::isfinite(t) || !std::isfinite(D) || !(D > 0.))
                throw std::runtime_error("invalid curve time/discount factor");
            c->S.B[i]=t; c->logD[i]=std::log(D);
        }
        c->S.validate();
    }
}
static size_t argument(const char* text, size_t lo, size_t hi) {
    std::string_view s(text); size_t value=0;
    auto [end, ec]=std::from_chars(s.data(), s.data()+s.size(), value);
    if (ec != std::errc{} || end != s.data()+s.size() || value < lo || value > hi)
        throw std::invalid_argument("usage: bench [N=1..10000000] [baseline=0|1] [reps=1..1000] [curve_file]");
    return value;
}

// ---- BASE: array-of-structs instruments with virtual pricing
struct Instrument { virtual ~Instrument() = default; virtual double npv(const Curve& d, const Curve& p) const = 0; };
struct Bond : Instrument { std::vector<FixedFlow> cfs; double npv(const Curve& d, const Curve&) const override { double s = 0; for (auto& c : cfs) s += c.amount * d.df(c.day / DPY); return s; } };
struct Swap : Instrument { std::vector<FixedFlow> fixed; std::vector<FloatFlow> flt; std::vector<OisFlow> ois;
    double npv(const Curve& d, const Curve& p) const override { double s = 0;
        for (auto& c : fixed) s += c.amount * d.df(c.day / DPY);
        for (auto& f : flt) s += f.scale * (p.df(f.a_day / DPY) / p.df(f.b_day / DPY) - 1.0) * d.df(f.pay_day / DPY);
        for (auto& o : ois) s += o.N * (d.df(o.a_day / DPY) / d.df(o.b_day / DPY) - 1.0) * d.df(o.pay_day / DPY);
        return s; } };

static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// ---- report helpers: build description and thousands separators
static const char* lanes_name() {
#if defined(LADDER_LANES_AVX512)
    return "AVX-512 intrinsics";
#elif defined(LADDER_LANES_AVX2)
    return "AVX2 + FMA intrinsics";
#elif defined(LADDER_LANES_STDX)
    return "std::experimental::simd";
#else
    return "portable (compiler auto-vectorised)";
#endif
}
static std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER);
#else
    return "unknown compiler";
#endif
}
static std::string size_text(double bytes) {
    char b[32];
    if (bytes >= 1e6) std::snprintf(b, sizeof b, "%.1f MB", bytes / 1e6); else std::snprintf(b, sizeof b, "%.1f KB", bytes / 1e3);
    return b;
}
static std::string grouped(size_t v) {
    std::string d = std::to_string(v), r;
    for (size_t k = 0; k < d.size(); ++k) { if (k && (d.size() - k) % 3 == 0) r += ','; r += d[k]; }
    return r;
}

int main(int argc, char** argv) try
{
    enter_example_dir();
    if (argc > 5) throw std::invalid_argument("too many benchmark arguments");
    size_t N = argc > 1 ? argument(argv[1],1,10000000) : 100000;
    int run_base = argc > 2 ? static_cast<int>(argument(argv[2],0,1)) : 1;
    int reps = argc > 3 ? static_cast<int>(argument(argv[3],1,1000)) : 3;
    Curve cd, cp; load_curves(argc > 4 ? argv[4] : LADDER_CURVE_DATA_FILE, cd, cp);
    bool valid = true;
    const int Kd = cd.S.K(), Kp = cp.S.K();
    auto df = [&](double t){ return cd.df(t); }; auto pf = [&](double t){ return cp.df(t); };

    // ---- portfolio: bonds, IBOR swaps, OIS swaps; 60 start offsets x 29 maturities x 3 types of signature
    // seasoned book: schedules walk backwards from the last regular payment date (maturity); remaining life is random,
    // so seasoned and new trades on the same maturity grid share groups. Grouping key = (type, maturity day).
    std::mt19937_64 rng(42); std::uniform_int_distribution<int> mat_off(2, 61), years_d(1, 29), type_d(0, 2); std::uniform_real_distribution<double> rate_d(0.01, 0.06);
    const double notionals[] = {1e6, 2.5e6, 5e6, 1e7};
    std::vector<InstrumentSpec> book(N); std::vector<std::unique_ptr<Instrument>> aos; aos.reserve(N); size_t ncf = 0;
    for (size_t i = 0; i < N; ++i) {
        int y = years_d(rng), ty = type_d(rng); int rem = 1 + (int)(rng() % y); int maturity = mat_off(rng) + (int)std::lround(y * 365.25);
        double notional = notionals[rng() % 4], r = rate_d(rng);
        auto day = [&](int n_from_end, int per_year){ return maturity - (int)std::lround(n_from_end * 365.25 / per_year); };
        InstrumentSpec& in = book[i]; in.signature = ty * 100000 + maturity;
        if (ty == 0) { auto b = std::make_unique<Bond>(); for (int j = 2*rem - 1; j >= 0; --j) in.fixed.push_back({day(j, 2), notional * r / 2 + (j == 0 ? notional : 0.0)}); b->cfs = in.fixed; aos.push_back(std::move(b)); }
        else { auto sw = std::make_unique<Swap>();
            for (int j = rem - 1; j >= 0; --j) in.fixed.push_back({day(j, 1) + (ty == 2 ? 2 : 0), -notional * r});
            if (ty == 1) for (int j = 2*rem - 1; j >= 0; --j) in.flt.push_back({day(j, 2), day(j + 1, 2), day(j, 2), notional});
            else for (int j = rem - 1; j >= 0; --j) in.ois.push_back({day(j, 1) + 2, day(j + 1, 1), day(j, 1), notional});
            sw->fixed = in.fixed; sw->flt = in.flt; sw->ois = in.ois; aos.push_back(std::move(sw)); }
        ncf += in.fixed.size() + in.flt.size() + in.ois.size();
    }
    const double nout = (double)N * (Kd + Kp);
    std::printf("bench_library: curve sensitivities for a seasoned book, single thread\n");
    std::printf("build:   %s backend; %s\n", lanes_name(), compiler_name().c_str());
    std::printf("book:    %s instruments (bonds, IBOR swaps, OIS swaps); %s cashflows, %.1f per instrument\n",
                grouped(N).c_str(), grouped(ncf).c_str(), (double)ncf / N);
    std::printf("outputs: %d sensitivities per instrument (%d discount-curve nodes + %d projection-curve nodes); %s values, %s\n",
                Kd + Kp, Kd, Kp, grouped(N * (Kd + Kp)).c_str(), size_text(nout * 8).c_str());
    std::fflush(stdout);

    // ---- SCAN: replicate once (layout), scan per run
    double t0 = now_ms();
    std::vector<std::vector<UnitCashflow>> ucf(N); std::vector<std::vector<ProjectionTerm>> pt(N);
    auto replicate_all = [&]() { for (size_t i = 0; i < N; ++i) { ucf[i].clear(); pt[i].clear();
        for (auto& c : book[i].fixed) replicate_fixed(c.amount, c.day / DPY, df, ucf[i]);
        for (auto& f : book[i].flt) replicate_ibor(f.scale, f.a_day / DPY, f.b_day / DPY, f.pay_day / DPY, df, pf, ucf[i], pt[i]);
        for (auto& o : book[i].ois) replicate_ois(o.N, o.a_day / DPY, o.b_day / DPY, o.pay_day / DPY, df, ucf[i]);
        sort_by_time(ucf[i]); } };
    replicate_all(); double t_rep = now_ms() - t0;
    std::vector<double> sd(N * Kd), sp(N * Kp); double best_scan = 1e30;
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); for (size_t i = 0; i < N; ++i) { scan_discount(cd.S, ucf[i], &sd[i * Kd]); scan_projection(cp.S, pt[i], &sp[i * Kp]); } best_scan = std::min(best_scan, now_ms() - t0); }
    double best_rep = 1e30; for (int r = 0; r < reps; ++r) { t0 = now_ms(); replicate_all(); best_rep = std::min(best_rep, now_ms() - t0); }

    // ---- GRP
    t0 = now_ms(); GroupLayout L = build_layout(book, DPY); double t_layout = now_ms() - t0;
    double best_tab = 1e30, best_n = 1e30, best_s = 1e30;
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); refresh_table(L, cd.S, cp.S, df, pf); best_tab = std::min(best_tab, now_ms() - t0); }
    double* out = (double*)ladder::allocate_aligned(L.groups.size() * L.stride(Kd, Kp) * sizeof(double));
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); scan_grouped<Store::normal>(L, cd.S, cp.S, out); best_n = std::min(best_n, now_ms() - t0); }
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); scan_grouped<Store::streaming>(L, cd.S, cp.S, out); best_s = std::min(best_s, now_ms() - t0); }
    double worst = 0; std::vector<double> buf(Kd + Kp);
    for (size_t gi = 0; gi < L.groups.size(); ++gi) for (int l = 0; l < L.groups[gi].n_valid; ++l) { int i = L.groups[gi].inst[l]; gather(L, Kd, Kp, out, (int)gi, l, buf.data());
        double scale = 1.0; for (int k = 0; k < Kd; ++k) scale = std::max(scale, std::fabs(sd[i*Kd+k])); for (int k = 0; k < Kp; ++k) scale = std::max(scale, std::fabs(sp[i*Kp+k]));
        for (int k = 0; k < Kd; ++k) worst = std::max(worst, std::fabs(buf[k] - sd[i*Kd+k]) / scale); for (int k = 0; k < Kp; ++k) worst = std::max(worst, std::fabs(buf[Kd+k] - sp[i*Kp+k]) / scale); }
    for (double v : sd) valid = valid && std::isfinite(v);
    for (double v : sp) valid = valid && std::isfinite(v);
    for (size_t j=0; j<L.groups.size()*L.stride(Kd,Kp); ++j) valid = valid && std::isfinite(out[j]);
    valid = valid && std::isfinite(worst) && worst < 1e-9;

    // ---- BASE
    double t_base = -1, w_base = 0;
    if (run_base) { std::vector<double> bd(N * Kd), bp(N * Kp); const double eps = 1e-5; t0 = now_ms();
        for (int k = 1; k <= Kd; ++k) { Curve up = cd.bumped(k, eps), dn = cd.bumped(k, -eps); for (size_t i = 0; i < N; ++i) bd[i*Kd+k-1] = (aos[i]->npv(up, cp) - aos[i]->npv(dn, cp)) / (2*eps); }
        for (int k = 1; k <= Kp; ++k) { Curve up = cp.bumped(k, eps), dn = cp.bumped(k, -eps); for (size_t i = 0; i < N; ++i) bp[i*Kp+k-1] = (aos[i]->npv(cd, up) - aos[i]->npv(cd, dn)) / (2*eps); }
        t_base = now_ms() - t0;
        double w = 0; for (size_t i = 0; i < N; ++i) { for (int k = 0; k < Kd; ++k) { double a = bd[i*Kd+k], b = sd[i*Kd+k], s = std::max(std::fabs(a), std::fabs(b)); if (s > 1e4) w = std::max(w, std::fabs(a-b)/s); } }
        for (size_t i=0; i<bd.size(); ++i)
            valid = valid && std::isfinite(bd[i]) && std::fabs(bd[i]-sd[i]) <= 1e-2 + 1e-6*std::max(std::fabs(bd[i]),std::fabs(sd[i]));
        for (size_t i=0; i<bp.size(); ++i)
            valid = valid && std::isfinite(bp[i]) && std::fabs(bp[i]-sp[i]) <= 1e-2 + 1e-6*std::max(std::fabs(bp[i]),std::fabs(sp[i]));
        w_base = w; }

    const double t_scan = best_rep + best_scan, t_grp = best_tab + best_s;
    size_t live = 0; for (auto& g : L.groups) live += g.n_valid;
    const double padded_bytes = (double)L.groups.size() * L.stride(Kd, Kp) * sizeof(double), logical_bytes = nout * 8;

    std::printf("\nterms\n");
    std::printf("  output            one sensitivity: one instrument against one curve node\n");
    std::printf("  SCAN              reference method: scalar ladder, one instrument at a time\n");
    std::printf("  GRP               grouped method: 8 instruments at a time, one per SIMD lane\n");
    std::printf("  date table        discount and projection factors at each unique payment date, shared by all groups\n");
    std::printf("  per curve update  work that is repeated every time the curves change\n");
    std::printf("  time              wall clock, best of %d run%s\n", reps, reps == 1 ? "" : "s");

    std::printf("\nresult: cost of one curve update                       time     per output%s\n", run_base ? "   vs BASE" : "");
    auto result = [&](const char* name, double ms) {
        std::printf("  %-44s %10.2f ms %10.3f ns", name, ms, ms * 1e6 / nout);
        if (run_base) std::printf(" %8.0fx", t_base / ms);
        std::printf("\n"); };
    if (run_base) result("BASE  bump-and-reprice, every node up and down", t_base);
    result("SCAN  replication + scalar ladder", t_scan);
    result("GRP   date-table refresh + kernel", t_grp);
    std::printf("  GRP is %.1fx faster than SCAN per curve update\n", t_scan / t_grp);

    std::printf("\nbreakdown\n");
    auto part = [&](const char* name, double ms, double units, const char* unit, const char* note) {
        std::printf("  %-44s %10.2f ms %10.3f ns per %s%s\n", name, ms, ms * 1e6 / units, unit, note); };
    part("SCAN  replication of unit cashflows (1)", best_rep, (double)ncf, "cashflow", "");
    part("SCAN  scalar ladder", best_scan, nout, "output", "");
    part("GRP   kernel, normal stores", best_n, nout, "output", "");
    part("GRP   kernel, streaming stores (2)", best_s, nout, "output", "   <- used in result");
    std::printf("  (1) each cashflow is re-expressed as unit cashflows whose amounts depend on the curves, so it is redone per update\n");
    std::printf("  (2) streaming (non-temporal) stores write the output straight to memory, bypassing the cache\n");

    std::printf("\naccuracy\n");
    std::printf("  GRP vs SCAN max difference %.1e (relative to each instrument's largest sensitivity)\n", worst);
    if (run_base) std::printf("  BASE vs SCAN max difference %.1e (relative, over discount sensitivities above 1e4)\n", w_base);

    std::printf("\nsetup (one-off, not repeated per curve update)\n");
    std::printf("  first replication %.0f ms (includes memory allocation); group layout %.0f ms\n", t_rep, t_layout);
    std::printf("  %s unique payment dates in the date table; %s groups of 8\n", grouped(L.table.day.size()).c_str(), grouped(L.groups.size()).c_str());
    std::printf("  lane occupancy %.1f%% (share of SIMD lanes holding a real instrument)\n", 100.0 * live / (L.groups.size() * LANES));
    std::printf("  storage %s padded vs %s logical (+%.1f%% for empty lanes)\n", size_text(padded_bytes).c_str(), size_text(logical_bytes).c_str(),
                100.0 * (padded_bytes / logical_bytes - 1.0));
    ladder::free_aligned(out);
    std::printf("\ncorrectness gate: %s (GRP agrees with SCAN to 1e-9 relative; every value finite%s)\n", valid ? "PASS" : "FAIL",
                run_base ? "; BASE agrees with SCAN to 1e-2 absolute + 1e-6 relative" : "");
    return valid ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "bench: %s\n", e.what()); return 1;
}
