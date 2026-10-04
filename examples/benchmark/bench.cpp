#include "../example_dir.hpp"
// Two-curve bucketed-risk benchmark on the curves of QuantLib's MulticurveBootstrapping example
// (node discount factors in quantlib_example_curves.txt, written by examples/quantlib_reconcile).
//   BASE  bump-and-reprice: AoS instruments, virtual npv, central differences on every stencil of both curves
//   SCAN  scalar reverse scan per instrument (ladder/scan.hpp) on unit cashflows
//   GRP   eight instruments per vector, shared date table, streaming output (ladder/scan_simd.hpp)
#include <chrono>
#include <cmath>
#include <cstdio>
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
static void load_curves(const char* path, Curve& d, Curve& p) {
    FILE* in = std::fopen(path, "r"); int nc; (void)std::fscanf(in, "%d", &nc);
    for (Curve* c : {&d, &p}) { char nm[16]; int n; (void)std::fscanf(in, "%15s %d", nm, &n); c->S.B.resize(n); c->logD.resize(n);
        for (int i = 0; i < n; ++i) { double t, D; (void)std::fscanf(in, "%lf %lf", &t, &D); c->S.B[i] = t; c->logD[i] = std::log(D); } }
    std::fclose(in);
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

int main(int argc, char** argv)
{
    enter_example_dir();
    size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000; int run_base = argc > 2 ? std::atoi(argv[2]) : 1; int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    Curve cd, cp; load_curves("quantlib_example_curves.txt", cd, cp);
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
    std::printf("N=%zu instruments (bonds / IBOR swaps / OIS swaps), %.1f cashflows each, stencils %d+%d, output %.0f MB\n", N, (double)ncf / N, Kd, Kp, N * (Kd + Kp) * 8 / 1e6);

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
    double* out = (double*)std::aligned_alloc(64, L.groups.size() * L.stride(Kd, Kp) * sizeof(double));
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); scan_grouped<Store::normal>(L, cd.S, cp.S, out); best_n = std::min(best_n, now_ms() - t0); }
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); scan_grouped<Store::streaming>(L, cd.S, cp.S, out); best_s = std::min(best_s, now_ms() - t0); }
    double worst = 0; std::vector<double> buf(Kd + Kp);
    for (size_t gi = 0; gi < L.groups.size(); ++gi) for (int l = 0; l < L.groups[gi].n_valid; ++l) { int i = L.groups[gi].inst[l]; gather(L, Kd, Kp, out, (int)gi, l, buf.data());
        double scale = 1.0; for (int k = 0; k < Kd; ++k) scale = std::max(scale, std::fabs(sd[i*Kd+k])); for (int k = 0; k < Kp; ++k) scale = std::max(scale, std::fabs(sp[i*Kp+k]));
        for (int k = 0; k < Kd; ++k) worst = std::max(worst, std::fabs(buf[k] - sd[i*Kd+k]) / scale); for (int k = 0; k < Kp; ++k) worst = std::max(worst, std::fabs(buf[Kd+k] - sp[i*Kp+k]) / scale); }
    std::printf("GRP vs SCAN max rel diff %.1e\n", worst);

    // ---- BASE
    double t_base = -1;
    if (run_base) { std::vector<double> bd(N * Kd), bp(N * Kp); const double eps = 1e-5; t0 = now_ms();
        for (int k = 1; k <= Kd; ++k) { Curve up = cd.bumped(k, eps), dn = cd.bumped(k, -eps); for (size_t i = 0; i < N; ++i) bd[i*Kd+k-1] = (aos[i]->npv(up, cp) - aos[i]->npv(dn, cp)) / (2*eps); }
        for (int k = 1; k <= Kp; ++k) { Curve up = cp.bumped(k, eps), dn = cp.bumped(k, -eps); for (size_t i = 0; i < N; ++i) bp[i*Kp+k-1] = (aos[i]->npv(cd, up) - aos[i]->npv(cd, dn)) / (2*eps); }
        t_base = now_ms() - t0;
        double w = 0; for (size_t i = 0; i < N; ++i) { for (int k = 0; k < Kd; ++k) { double a = bd[i*Kd+k], b = sd[i*Kd+k], s = std::max(std::fabs(a), std::fabs(b)); if (s > 1e4) w = std::max(w, std::fabs(a-b)/s); } }
        std::printf("BASE vs SCAN max rel diff %.1e (central difference, values > 1e4)\n", w); }

    const double nout = (double)N * (Kd + Kp);
    auto row = [&](const char* name, double ms) { std::printf("%-46s %10.2f ms %9.3f ns/output %10s\n", name, ms, ms * 1e6 / nout, t_base > 0 ? (std::to_string((int)std::lround(t_base / ms)) + "x").c_str() : "-"); };
    std::printf("\n");
    if (run_base) row("BASE bump-and-reprice", t_base);
    row("SCAN scalar, per instrument", best_scan);
    row("SCAN replication of unit cashflows (per curve update)", best_rep);
    row("GRP  date table refresh (per curve update)", best_tab);
    row("GRP  8-lane, normal stores", best_n);
    row("GRP  8-lane, streaming stores", best_s);
    { size_t live = 0; for (auto& g : L.groups) live += g.n_valid; std::printf("one-off: replication %.0f ms, group layout %.0f ms; unique dates %zu; groups %zu; lane occupancy %.1f%%\n", t_rep, t_layout, L.table.day.size(), L.groups.size(), 100.0 * live / (L.groups.size() * LANES)); }
    std::free(out);
}
