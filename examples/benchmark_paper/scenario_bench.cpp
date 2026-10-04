#include "../example_dir.hpp"
// The intermediate optimisation: bump-and-reprice with a scenario-vector curve.
// One lookup per date returns the discount (and projection) factors for every wave scenario as one aligned column;
// pricing a cashflow is then one broadcast and S multiply-adds across the column, which auto-vectorises.
// Sorting instruments so that those sharing payment dates are adjacent keeps a small set of columns hot.
//   BASE      per-scenario repricing, bracket search + exp per cashflow per scenario (the §3 loop)
//   SV-rand   scenario-vector curve, instruments in random order
//   SV-sort   scenario-vector curve, instruments sorted by schedule signature (shared dates adjacent)
//   SCAN      scalar reverse scan (no scenarios at all)
// Same book type as examples/benchmark_paper/adjoint_bench.cpp: bonds + vanilla swaps, example curves, 30+36 waves.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
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
    for (int cap : caps) for (int o = 0; o < 2; ++o) {
        if (o == 0 && cap < 1024 && N > 100000 && !std::getenv("LRU_ALL")) continue;                     // random order with small caps thrashes: sweep at 100k only
        const auto& ord = o ? sort_order : rand_order; double best = 1e30; size_t mi = 0, lo = 0;
        for (int r = 0; r < reps; ++r) { double t0 = now_ms(); price_lru(ord, cap, mi, lo); best = std::min(best, now_ms() - t0); }
        lru_rows.push_back({cap, o ? "sorted" : "random", best, mi, lo}); }

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

    const double nout = (double)N * K;
    std::printf("N=%zu bonds + vanilla swaps, %d+%d waves, %d scenarios (padded %d), %zu unique dates, scenario table %.1f MB per curve\n", N, Ko, Kp, S, SP, U, U * SP * 8 / 1e6);
    std::printf("scenario-vector vs scan: max rel diff %.1e (central difference at eps=1e-5, values > 1e4)\n", w);
    auto row = [&](const char* nm, double ms) { std::printf("%-58s %10.1f ms %9.2f ns/sens %8s\n", nm, ms, ms * 1e6 / nout, t_base > 0 ? (std::to_string((int)std::lround(t_base / ms)) + "x").c_str() : "-"); };
    if (run_base) row("BASE  per-scenario repricing, bracket + exp per cashflow", t_base);
    row("SV    scenario-vector curve, random instrument order", t_rand);
    row("SV    scenario-vector curve, instruments sorted by schedule", t_sort);
    row("      scenario table build (per curve update)", t_table);
    row("SCAN  scalar reverse scan, no scenarios", t_scan);
    std::printf("\non-demand LRU date cache (DR3 design), capacity per curve in date columns of %d doubles:\n", SP);
    std::printf("%-8s %-7s %10s %10s %12s %10s\n", "capacity", "order", "time ms", "ns/sens", "misses", "hit rate");
    for (auto& r : lru_rows) std::printf("%-8d %-7s %10.1f %10.2f %12zu %9.2f%%\n", r.cap, r.ord, r.ms, r.ms * 1e6 / nout, r.miss, 100.0 * (1.0 - (double)r.miss / r.look));
    std::printf("(distinct dates in the book: %zu; a cold-start miss per distinct date per curve is the floor)\n", U);
}
