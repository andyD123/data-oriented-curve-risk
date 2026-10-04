#include "../example_dir.hpp"
// The intermediate optimisation: bump-and-reprice with a scenario-vector curve.
// One lookup per date returns the discount (and projection) factors for every wave scenario as one aligned column;
// pricing a cashflow is then one broadcast and S multiply-adds across the column, which auto-vectorises.
// Sorting instruments so that those sharing payment dates are adjacent keeps a small set of columns hot.
//   BASE      per-scenario repricing, bracket search + exp per cashflow per scenario (the §3 loop)
//   SV-rand   scenario-vector curve, instruments in random order
//   SV-sort   scenario-vector curve, instruments sorted by schedule signature (shared dates adjacent)
//   LRU       on-demand date cache; orders random, sorted (type, start, tenor) and grouped (start, type, tenor; prototype)
//   SCAN      scalar reverse scan (no scenarios at all)
// Same book type as examples/benchmark_paper/adjoint_bench.cpp: bonds + vanilla swaps, example curves, 30+36 waves.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>
#include "ladder/stencil.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
using namespace ladder;
constexpr double DPY = 365.0;
struct Curve { Stencils S; std::vector<double> logD;
    double df(double t) const { int k = S.bucket(t); double a = (t - S.B[k-1]) / S.len(k); return std::exp(logD[k-1] + a * (logD[k] - logD[k-1])); } };
static Curve read_curve(FILE* in) { char nm[16]; int n; (void)std::fscanf(in, "%15s %d", nm, &n); Curve c; c.S.B.resize(n); c.logD.resize(n);
    for (int i = 0; i < n; ++i) { double t, D; (void)std::fscanf(in, "%lf %lf", &t, &D); c.S.B[i] = t; c.logD[i] = std::log(D); } return c; }
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

// Vectorised LRU date cache, populated on demand: one entry per date holds the whole scenario column.
// Miss: locate the interval once (base discount factor), fill all S scenario values, evict the least recently used entry.
struct LruColumns {
    int cap, SP; std::vector<int> key, prev, next; std::vector<double> col; std::unordered_map<int,int> where; int head = -1, tail = -1, used = 0;
    size_t misses = 0, lookups = 0; int mru_key = -1, mru_slot = -1;
    LruColumns(int cap_, int SP_) : cap(cap_), SP(SP_), key(cap_, -1), prev(cap_, -1), next(cap_, -1), col((size_t)cap_ * SP_) { where.reserve(cap_ * 2); }
    void unlink(int s) { if (prev[s] >= 0) next[prev[s]] = next[s]; else head = next[s]; if (next[s] >= 0) prev[next[s]] = prev[s]; else tail = prev[s]; }
    void push_front(int s) { prev[s] = -1; next[s] = head; if (head >= 0) prev[head] = s; head = s; if (tail < 0) tail = s; }
    template <class Fill> const double* fetch(int d, Fill fill) {
        ++lookups;
        if (d == mru_key) return &col[(size_t)mru_slot * SP];                       // consecutive same date: no bookkeeping
        auto it = where.find(d); int s;
        if (it != where.end()) { s = it->second; unlink(s); push_front(s); }
        else { ++misses;
            if (used < cap) s = used++; else { s = tail; unlink(s); where.erase(key[s]); }
            key[s] = d; where[d] = s; push_front(s); fill(d, &col[(size_t)s * SP]); }
        mru_key = d; mru_slot = s; return &col[(size_t)s * SP];
    }
};
struct Fix { int d; double amt; };                 // d: date index into the scenario table
struct Flt { int p, a, b; double scale; };
struct Inst { int sig; std::vector<Fix> fix; std::vector<Flt> flt; };

// ---- report helpers
static const char* isa_name() {
#if defined(__AVX512F__)
    return "AVX-512";
#elif defined(__AVX2__)
    return "AVX2";
#else
    return "baseline x86-64 (no AVX2)";
#endif
}
static std::string compiler_name() {
#if defined(__clang__)
    return std::string("Clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("GCC ") + __VERSION__;
#elif defined(_MSC_VER)
    return "MSVC " + std::to_string(_MSC_FULL_VER / 10000000) + "." + std::to_string(_MSC_FULL_VER / 100000 % 100) + "." + std::to_string(_MSC_FULL_VER % 100000);
#else
    return "unknown compiler";
#endif
}
static std::string grouped_digits(size_t v) {
    std::string d = std::to_string(v), r;
    for (size_t k = 0; k < d.size(); ++k) { if (k && (d.size() - k) % 3 == 0) r += ','; r += d[k]; }
    return r;
}

int main(int argc, char** argv) {
    enter_example_dir();
    size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000; int run_base = argc > 2 ? std::atoi(argv[2]) : 1; int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    FILE* in = std::fopen(argc > 4 ? argv[4] : "quantlib_example_curves.txt", "r"); int nc; (void)std::fscanf(in, "%d", &nc);
    Curve cd = read_curve(in), cp = read_curve(in); std::fclose(in);
    const int Ko = cd.S.K(), Kp = cp.S.K(), K = Ko + Kp; const double eps = 1e-5;
    const int S = 2 * K, SP = (S + 7) / 8 * 8;       // scenarios: disc up/down per wave, then proj up/down; padded to 8

    // ---- book (bonds + vanilla swaps), integer days
    std::mt19937_64 rng(42); std::uniform_int_distribution<int> start_d(2, 61), years_d(1, 29), type_d(0, 1); std::uniform_real_distribution<double> rate_d(0.01, 0.06);
    const double notionals[] = {1e6, 2.5e6, 5e6, 1e7};
    struct Raw { int sig; std::vector<std::pair<int,double>> fix; std::vector<std::array<int,3>> flt; std::vector<double> sc; };
    std::vector<Raw> raw(N); std::vector<int> days;
    for (size_t i = 0; i < N; ++i) {
        int s = start_d(rng), y = years_d(rng), ty = type_d(rng); double notional = notionals[rng() % 4], r = rate_d(rng);
        auto day = [&](int n, int per_year){ return s + (int)std::lround(n * 365.25 / per_year); };
        Raw& R = raw[i]; R.sig = ty * 100000 + s * 100 + y;
        if (ty == 0) for (int j = 1; j <= 2*y; ++j) R.fix.push_back({day(j, 2), notional * r / 2 + (j == 2*y ? notional : 0.0)});
        else { for (int j = 1; j <= y; ++j) R.fix.push_back({day(j, 1), -notional * r});
               for (int j = 1; j <= 2*y; ++j) { R.flt.push_back({day(j, 2), day(j-1, 2), day(j, 2)}); R.sc.push_back(notional); } }
        for (auto& f : R.fix) days.push_back(f.first); for (auto& f : R.flt) for (int d : f) days.push_back(d);
    }
    std::sort(days.begin(), days.end()); days.erase(std::unique(days.begin(), days.end()), days.end());
    std::unordered_map<int,int> idx; for (size_t i = 0; i < days.size(); ++i) idx[days[i]] = (int)i;
    std::vector<Inst> book(N);
    for (size_t i = 0; i < N; ++i) { book[i].sig = raw[i].sig;
        for (auto& f : raw[i].fix) book[i].fix.push_back({idx[f.first], f.second});
        for (size_t q = 0; q < raw[i].flt.size(); ++q) book[i].flt.push_back({idx[raw[i].flt[q][0]], idx[raw[i].flt[q][1]], idx[raw[i].flt[q][2]], raw[i].sc[q]}); }
    const size_t U = days.size();
    std::vector<double> T(U); for (size_t u = 0; u < U; ++u) T[u] = days[u] / DPY;

    // ---- scenario-vector tables: one aligned column of SP doubles per date, per curve (refreshed per curve update)
    auto fill_D = [&](int u, double* D) { double t = T[u], d0 = cd.df(t);
        for (int k = 1; k <= Ko; ++k) { double o = cd.S.overlap(k, t); D[k-1] = d0 * std::exp(-eps * o); D[Ko+k-1] = d0 * std::exp(eps * o); }
        for (int s = 2*Ko; s < SP; ++s) D[s] = d0; };
    auto fill_P = [&](int u, double* P) { double t = T[u], p0 = cp.df(t);
        for (int s = 0; s < 2*Ko; ++s) P[s] = p0;
        for (int k = 1; k <= Kp; ++k) { double o = cp.S.overlap(k, t); P[2*Ko+k-1] = p0 * std::exp(-eps * o); P[2*Ko+Kp+k-1] = p0 * std::exp(eps * o); }
        for (int s = S; s < SP; ++s) P[s] = p0; };
    std::vector<double, std::allocator<double>> Dt, Pt; double t_table = 1e30;
    auto build_tables = [&]() {
        Dt.assign(U * SP, 0.0); Pt.assign(U * SP, 0.0);
        for (size_t u = 0; u < U; ++u) { double t = T[u], d0 = cd.df(t), p0 = cp.df(t); double* D = &Dt[u * SP]; double* P = &Pt[u * SP];
            for (int k = 1; k <= Ko; ++k) { double o = cd.S.overlap(k, t); D[k-1] = d0 * std::exp(-eps * o); D[Ko+k-1] = d0 * std::exp(eps * o); P[k-1] = p0; P[Ko+k-1] = p0; }
            for (int k = 1; k <= Kp; ++k) { double o = cp.S.overlap(k, t); P[2*Ko+k-1] = p0 * std::exp(-eps * o); P[2*Ko+Kp+k-1] = p0 * std::exp(eps * o); D[2*Ko+k-1] = d0; D[2*Ko+Kp+k-1] = d0; }
            for (int s = S; s < SP; ++s) { D[s] = d0; P[s] = p0; } } };
    for (int r = 0; r < reps; ++r) { double t0 = now_ms(); build_tables(); t_table = std::min(t_table, now_ms() - t0); }

    std::vector<double> out_sv(N * K), out_ref(N * K);
    auto price_sv = [&](const std::vector<size_t>& order) {
        alignas(64) double pv[256];
        for (size_t ii : order) { const Inst& I = book[ii];
            for (int s = 0; s < SP; ++s) pv[s] = 0.0;
            for (const Fix& f : I.fix) { const double* D = &Dt[(size_t)f.d * SP]; const double a = f.amt;
                for (int s = 0; s < SP; ++s) pv[s] += a * D[s]; }                                        // one broadcast, SP FMAs
            for (const Flt& f : I.flt) { const double* Pa = &Pt[(size_t)f.a * SP]; const double* Pb = &Pt[(size_t)f.b * SP]; const double* D = &Dt[(size_t)f.p * SP]; const double sc = f.scale;
                for (int s = 0; s < SP; ++s) pv[s] += sc * (Pa[s] / Pb[s] - 1.0) * D[s]; }
            double* o = &out_sv[ii * K];
            for (int k = 0; k < Ko; ++k) o[k] = (pv[k] - pv[Ko + k]) / (2 * eps);
            for (int k = 0; k < Kp; ++k) o[Ko + k] = (pv[2*Ko + k] - pv[2*Ko + Kp + k]) / (2 * eps);
        } };
    std::vector<size_t> rand_order(N); std::iota(rand_order.begin(), rand_order.end(), 0); std::shuffle(rand_order.begin(), rand_order.end(), std::mt19937_64(7));
    std::vector<size_t> sort_order(N); std::iota(sort_order.begin(), sort_order.end(), 0);
    std::stable_sort(sort_order.begin(), sort_order.end(), [&](size_t a, size_t b){ return book[a].sig < book[b].sig; });
    double t_rand = 1e30, t_sort = 1e30;
    for (int r = 0; r < reps; ++r) { double t0 = now_ms(); price_sv(rand_order); t_rand = std::min(t_rand, now_ms() - t0); }
    for (int r = 0; r < reps; ++r) { double t0 = now_ms(); price_sv(sort_order); t_sort = std::min(t_sort, now_ms() - t0); }

    // ---- on-demand LRU cache, capacity sweep, random vs sorted order
    auto price_lru = [&](const std::vector<size_t>& order, int cap, size_t& miss, size_t& look) {
        LruColumns cD(cap, SP), cP(cap, SP); alignas(64) double pv[256];
        for (size_t ii : order) { const Inst& I = book[ii];
            for (int s2 = 0; s2 < SP; ++s2) pv[s2] = 0.0;
            for (const Fix& f : I.fix) { const double* D = cD.fetch(f.d, fill_D); const double a = f.amt; for (int s2 = 0; s2 < SP; ++s2) pv[s2] += a * D[s2]; }
            for (const Flt& f : I.flt) { const double* Pa = cP.fetch(f.a, fill_P); const double* Pb = cP.fetch(f.b, fill_P); const double* D = cD.fetch(f.p, fill_D); const double sc = f.scale;
                for (int s2 = 0; s2 < SP; ++s2) pv[s2] += sc * (Pa[s2] / Pb[s2] - 1.0) * D[s2]; }
            double* o = &out_sv[ii * K];
            for (int k = 0; k < Ko; ++k) o[k] = (pv[k] - pv[Ko + k]) / (2 * eps);
            for (int k = 0; k < Kp; ++k) o[Ko + k] = (pv[2*Ko + k] - pv[2*Ko + Kp + k]) / (2 * eps);
        }
        miss = cD.misses + cP.misses; look = cD.lookups + cP.lookups; };
    struct LruRow { int cap; const char* ord; double ms; size_t miss, look; };
    std::vector<LruRow> lru_rows;
    std::vector<int> caps = {16, 64, 128, 256, 1024, 4096}; if (const char* e = std::getenv("LRU_CAPS")) { caps.clear(); for (const char* p = e; *p; ) { caps.push_back(std::atoi(p)); while (*p && *p != 0x2c) ++p; if (*p) ++p; } }
    // PROTOTYPE ordering: group trades by shared schedule (start date first, then type, then tenor), so bonds and swaps
    // that pay on the same dates are adjacent. sig = type * 100000 + start * 100 + years.
    std::vector<size_t> grouped_order_prototype(N); std::iota(grouped_order_prototype.begin(), grouped_order_prototype.end(), 0);
    auto grid_key_prototype = [](int sig) { return ((sig / 100) % 1000) * 1000 + (sig / 100000) * 100 + sig % 100; };
    std::stable_sort(grouped_order_prototype.begin(), grouped_order_prototype.end(),
                     [&](size_t a, size_t b){ return grid_key_prototype(book[a].sig) < grid_key_prototype(book[b].sig); });
    const std::vector<size_t>* lru_orders[3] = {&rand_order, &sort_order, &grouped_order_prototype};
    const char* lru_order_names[3] = {"random", "sorted", "grouped"};
    for (int cap : caps) for (int o = 0; o < 3; ++o) {
        if (o == 0 && cap < 1024 && N > 100000 && !std::getenv("LRU_ALL")) continue;                     // random order with small caps thrashes: sweep at 100k only
        const auto& ord = *lru_orders[o]; double best = 1e30; size_t mi = 0, lo = 0;
        for (int r = 0; r < reps; ++r) { double t0 = now_ms(); price_lru(ord, cap, mi, lo); best = std::min(best, now_ms() - t0); }
        lru_rows.push_back({cap, lru_order_names[o], best, mi, lo}); }

    // ---- scalar scan reference (unit cashflows on base curves)
    std::vector<std::vector<UnitCashflow>> ucf(N); std::vector<std::vector<ProjectionTerm>> pt(N);
    auto df = [&](double t){ return cd.df(t); }; auto pf = [&](double t){ return cp.df(t); };
    for (size_t i = 0; i < N; ++i) { for (auto& f : book[i].fix) replicate_fixed(f.amt, T[f.d], df, ucf[i]);
        for (auto& f : book[i].flt) replicate_ibor(f.scale, T[f.a], T[f.b], T[f.p], df, pf, ucf[i], pt[i]); sort_by_time(ucf[i]); }
    double t_scan = 1e30;
    for (int r = 0; r < reps; ++r) { double t0 = now_ms(); for (size_t i = 0; i < N; ++i) { scan_discount(cd.S, ucf[i], &out_ref[i*K]); scan_projection(cp.S, pt[i], &out_ref[i*K+Ko]); } t_scan = std::min(t_scan, now_ms() - t0); }
    double w = 0; for (size_t j = 0; j < out_sv.size(); ++j) { double s = std::max(std::fabs(out_ref[j]), 1e4); w = std::max(w, std::fabs(out_sv[j] - out_ref[j]) / s); }

    // ---- BASE: per scenario, per instrument, bracket search + exp per cashflow
    double t_base = -1;
    if (run_base) { std::vector<double> ob(N * K); double t0 = now_ms();
        auto price = [&](const Inst& I, int s) { double v = 0;
            auto Ds = [&](double t){ double d = cd.df(t); if (s < Ko) d *= std::exp(-eps * cd.S.overlap(s + 1, t)); else if (s < 2*Ko) d *= std::exp(eps * cd.S.overlap(s - Ko + 1, t)); return d; };
            auto Ps = [&](double t){ double p = cp.df(t); if (s >= 2*Ko && s < 2*Ko + Kp) p *= std::exp(-eps * cp.S.overlap(s - 2*Ko + 1, t)); else if (s >= 2*Ko + Kp) p *= std::exp(eps * cp.S.overlap(s - 2*Ko - Kp + 1, t)); return p; };
            for (auto& f : I.fix) v += f.amt * Ds(T[f.d]);
            for (auto& f : I.flt) v += f.scale * (Ps(T[f.a]) / Ps(T[f.b]) - 1.0) * Ds(T[f.p]);
            return v; };
        for (int k = 0; k < Ko; ++k) for (size_t i = 0; i < N; ++i) ob[i*K+k] = (price(book[i], k) - price(book[i], Ko + k)) / (2*eps);
        for (int k = 0; k < Kp; ++k) for (size_t i = 0; i < N; ++i) ob[i*K+Ko+k] = (price(book[i], 2*Ko + k) - price(book[i], 2*Ko + Kp + k)) / (2*eps);
        t_base = now_ms() - t0; }

    // distinct dates per curve: the miss floor (each column computed once per curve)
    size_t ndisc = 0, nproj = 0;
    { std::vector<char> ud(U, 0), up(U, 0);
      for (const Inst& I : book) { for (const Fix& f : I.fix) ud[f.d] = 1; for (const Flt& f : I.flt) { ud[f.p] = 1; up[f.a] = 1; up[f.b] = 1; } }
      for (size_t u = 0; u < U; ++u) { ndisc += ud[u]; nproj += up[u]; } }
    const size_t floor_misses = ndisc + nproj;

    const double nout = (double)N * K;
    std::printf("scenario_bench: wave scenarios priced from an LRU date cache, single thread\n");
    std::printf("build:     %s instructions; %s\n", isa_name(), compiler_name().c_str());
    std::printf("book:      %s instruments (bonds and vanilla swaps); %s distinct payment dates\n", grouped_digits(N).c_str(), grouped_digits(U).c_str());
    std::printf("scenarios: %d per instrument (%d discount-curve + %d projection-curve nodes, each bumped up and down), padded to %d\n", S, Ko, Kp, SP);
    std::printf("outputs:   %s sensitivities (%d per instrument, central differences of the up/down pairs)\n", grouped_digits(N * K).c_str(), K);

    std::printf("\nterms\n");
    std::printf("  column      the factors at one date under every scenario: %d doubles, %s bytes\n", SP, grouped_digits((size_t)SP * 8).c_str());
    std::printf("  LRU cache   holds a fixed number of columns per curve; a miss computes a column, a hit reuses it\n");
    std::printf("  misses      column computations, both curves together; the floor is %s, each column once\n", grouped_digits(floor_misses).c_str());
    std::printf("              (%s discount-curve dates + %s projection-curve dates)\n", grouped_digits(ndisc).c_str(), grouped_digits(nproj).c_str());
    std::printf("  orders      random = shuffled; sorted = type, then start date, then tenor;\n");
    std::printf("              grouped = start date, then type, then tenor, so trades sharing a schedule are adjacent (prototype ordering)\n");
    std::printf("  full table  every date's column computed up front (%.1f MB per curve), no cache\n", U * SP * 8 / 1e6);
    std::printf("  time        wall clock per curve update, best of %d run%s\n", reps, reps == 1 ? "" : "s");

    std::printf("\nresult: LRU cache, cost of one curve update\n");
    std::printf("  %7s %9s  %-8s %12s %14s %13s %9s\n", "columns", "cache", "order", "time", "per output", "misses", "hit rate");
    for (auto& r : lru_rows) {
        const double kb = (double)r.cap * SP * 8 / 1e3;
        char cache[32]; if (kb < 1000) std::snprintf(cache, sizeof cache, "%.0f KB", kb); else std::snprintf(cache, sizeof cache, "%.1f MB", kb / 1e3);
        std::printf("  %7d %9s  %-8s %9.1f ms %11.2f ns %13s %8.2f%%\n", r.cap, cache, r.ord, r.ms, r.ms * 1e6 / nout,
                    grouped_digits(r.miss).c_str(), 100.0 * (1.0 - (double)r.miss / r.look)); }
    for (int cap : caps) {
        const LruRow* rr = nullptr; const LruRow* rg = nullptr;
        for (auto& r : lru_rows) if (r.cap == cap) { if (std::string(r.ord) == "random") rr = &r; if (std::string(r.ord) == "grouped") rg = &r; }
        if (rr && rg) std::printf("  at %d columns: grouped is %.1fx faster than random%s\n", cap, rr->ms / rg->ms,
                                  rg->miss == floor_misses ? ", at the miss floor" : "");
        else if (rg && !rr) std::printf("  at %d columns: random order not run (skipped above 100,000 trades unless LRU_ALL=1)\n", cap); }

    std::printf("\ncontext: same book, other methods\n");
    auto ctx = [&](const char* nm, double ms, bool is_base = false) {
        std::printf("  %-56s %9.1f ms %11.2f ns per output", nm, ms, ms * 1e6 / nout);
        if (run_base) { if (is_base) std::printf(" %17s", "-"); else std::printf(" %8.0fx vs BASE", t_base / ms); }
        std::printf("\n"); };
    if (run_base) ctx("BASE  per-scenario repricing, bracket + exp per cashflow", t_base, true);
    ctx("full table, random order", t_rand);
    ctx("full table, sorted order", t_sort);
    std::printf("  %-56s %9.1f ms %11.2f ns per date (both curves)\n", "full table build, per curve update", t_table, t_table * 1e6 / U);
    ctx("scalar reverse scan, sensitivities only (no scenarios)", t_scan);

    std::printf("\naccuracy\n");
    std::printf("  scenario results vs scalar scan: max difference %.1e, relative to max(|value|, 1e4), checked on the last LRU cache run;\n", w);
    std::printf("  this is the truncation error of the central difference at eps = 1e-5\n");
}
