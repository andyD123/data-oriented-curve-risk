#include "ladder/aligned_memory.hpp"
// Tests for ladder/: scalar scan, grouped SIMD scan, replication, exactness.
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>
#include "../ladder/stencil.hpp"
#include "../ladder/unit_cashflow.hpp"
#include "../ladder/scan.hpp"
#include "../ladder/replicate.hpp"
#include "../ladder/curve.hpp"
#include "../ladder/layout.hpp"
#include "../ladder/scan_simd.hpp"
using namespace ladder;

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++fails; std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static Curve make_curve(std::vector<double> B, double r0, double slope) {
    std::vector<double> z(B.size());
    for (size_t i = 0; i < B.size(); ++i) z[i] = r0 + slope * B[i];
    return Curve::from_zero_rates(B, z);
}

int main()
{
    const double DPY = 365.0;
    std::vector<double> Bd, Bp;
    for (int i = 0; i <= 20; ++i) Bd.push_back(i * 0.5);                    // 20 semi-annual stencils to 10y
    for (int i = 0; i <= 40; ++i) Bp.push_back(0.0055 + i * 0.25);          // projection stencils from a settlement offset, quarterly
    Curve cd = make_curve(Bd, 0.02, 0.001), cp = make_curve(Bp, 0.025, 0.0012);
    auto df = [&](double t){ return cd.df(t); }; auto pf = [&](double t){ return cp.df(t); };

    // ---- random book: bonds, IBOR swaps, OIS swaps (integer days)
    // schedules generated backwards from the last regular payment date (maturity), as backward-rolled schedules are;
    // the grouping key is (type, maturity day): seasoned and new trades on the same grid share a group
    std::mt19937_64 rng(7); std::uniform_int_distribution<int> years(1, 9), mat_off(2, 40), type(0, 2), remaining(1, 9);
    std::uniform_real_distribution<double> rate(0.01, 0.05);
    const int N = 600; std::vector<InstrumentSpec> book(N);
    std::vector<std::vector<UnitCashflow>> ucf(N); std::vector<std::vector<ProjectionTerm>> pterms(N);
    for (int i = 0; i < N; ++i) {
        int y = years(rng), ty = type(rng); int rem = std::min(y, remaining(rng));     // rem = years still to run (seasoning)
        int maturity = mat_off(rng) + (int)std::lround(y * 365.25); double notional = 1e6, r = rate(rng);
        InstrumentSpec& in = book[i]; in.signature = ty * 100000 + maturity;
        auto day = [&](int n_from_end, int per_year){ return maturity - (int)std::lround(n_from_end * 365.25 / per_year); };   // walk back from maturity
        if (ty == 0) { for (int j = 2*rem - 1; j >= 0; --j) in.fixed.push_back({day(j, 2), notional * r / 2 + (j == 0 ? notional : 0.0)}); }
        else if (ty == 1) {
            for (int j = rem - 1; j >= 0; --j) in.fixed.push_back({day(j, 1), -notional * r});
            for (int j = 2*rem - 1; j >= 0; --j) in.flt.push_back({day(j, 2), day(j + 1, 2), day(j, 2), notional});
        } else {
            for (int j = rem - 1; j >= 0; --j) { in.fixed.push_back({day(j, 1) + 2, -notional * r}); in.ois.push_back({day(j, 1) + 2, day(j + 1, 1), day(j, 1), notional}); }
        }
        for (auto& c : in.fixed) replicate_fixed(c.amount, c.day / DPY, df, ucf[i]);
        for (auto& f : in.flt) replicate_ibor(f.scale, f.a_day / DPY, f.b_day / DPY, f.pay_day / DPY, df, pf, ucf[i], pterms[i]);
        for (auto& o : in.ois) replicate_ois(o.N, o.a_day / DPY, o.b_day / DPY, o.pay_day / DPY, df, ucf[i]);
        sort_by_time(ucf[i]);
    }
    const int Kd = cd.S.K(), Kp = cp.S.K();

    // ---- 1. scalar scan vs explicit adjoint (N x K tape) and vs finite difference with eps^2 scaling
    double worst_adj = 0, worst_ratio_lo = 1e9, worst_ratio_hi = 0;
    for (int i = 0; i < N; ++i) {
        std::vector<double> sd(Kd), sp(Kp); scan_discount(cd.S, ucf[i], sd.data()); scan_projection(cp.S, pterms[i], sp.data());
        double scale = 1e-300; for (double v : sd) scale = std::max(scale, std::fabs(v));
        // adjoint: PV(delta) = sum x_j exp(-delta_k ov_kj) -> dPV/ddelta_k = -sum x_j ov_kj
        for (int k = 1; k <= Kd; ++k) { double a = 0; for (auto& c : ucf[i]) a -= c.x * cd.S.overlap(k, c.t); worst_adj = std::max(worst_adj, std::fabs(a - sd[k-1]) / scale); }
        // finite difference through df_bumped on the replicated unit cashflows (here the curve bump is exact)
        auto pv = [&](int k, double d){ double s = 0; for (auto& c : book[i].fixed) s += c.amount * cd.df_bumped(c.day / DPY, k, d);
            for (auto& f : book[i].flt) s += f.scale * (pf(f.a_day / DPY) / pf(f.b_day / DPY) - 1.0) * cd.df_bumped(f.pay_day / DPY, k, d);
            for (auto& o : book[i].ois) s += o.N * (cd.df_bumped(o.a_day / DPY, k, d) / cd.df_bumped(o.b_day / DPY, k, d) - 1.0) * cd.df_bumped(o.pay_day / DPY, k, d); return s; };
        double e1 = 0, e2 = 0;
        for (int k = 1; k <= Kd; ++k) { double f1 = (pv(k, 2e-3) - pv(k, -2e-3)) / 4e-3, f2 = (pv(k, 1e-3) - pv(k, -1e-3)) / 2e-3; e1 += std::fabs(f1 - sd[k-1]); e2 += std::fabs(f2 - sd[k-1]); }
        if (e2 > 1e-8 * scale) { worst_ratio_lo = std::min(worst_ratio_lo, e1 / e2); worst_ratio_hi = std::max(worst_ratio_hi, e1 / e2); }
    }
    CHECK(worst_adj < 1e-13, "scalar scan vs adjoint: %.2e", worst_adj);
    CHECK(worst_ratio_lo > 3.5 && worst_ratio_hi < 4.5, "finite-difference residual ratio out of [3.5,4.5]: [%.2f, %.2f]", worst_ratio_lo, worst_ratio_hi);
    std::printf("scan vs adjoint max rel %.1e; FD residual ratios in [%.2f, %.2f]\n", worst_adj, worst_ratio_lo, worst_ratio_hi);

    // ---- 2. exact zeros beyond maturity, and OIS lag-0 telescoping
    {
        std::vector<UnitCashflow> u; replicate_ois(1e6, 1.0, 2.0, 2.0, df, u); sort_by_time(u);           // lag 0: pay == b
        std::vector<UnitCashflow> v{{1.0, 1e6 * df(1.0)}, {2.0, -1e6 * df(2.0)}};                           // notional in at a, out at b
        std::vector<double> ru(Kd), rv(Kd); scan_discount(cd.S, u, ru.data()); scan_discount(cd.S, v, rv.data());
        double m = 0; for (int k = 0; k < Kd; ++k) m = std::max(m, std::fabs(ru[k] - rv[k]) / 1e6);
        CHECK(m < 1e-9, "OIS lag-0 telescoping: %.2e", m);
        int nz = 0; for (int k = 0; k < Kd; ++k) if (cd.S.B[k] >= 2.0 && ru[k] != 0.0) ++nz;
        CHECK(nz == 0, "non-zero risk beyond maturity: %d buckets", nz);
    }

    // ---- 3. grouped SIMD scan vs scalar, both store policies
    GroupLayout L = build_layout(book, DPY);
    refresh_table(L, cd.S, cp.S, df, pf);
    std::vector<double> outn(L.groups.size() * L.stride(Kd, Kp)), outs(outn.size());
    double* on = (double*)ladder::allocate_aligned(outn.size() * sizeof(double)); double* os = (double*)ladder::allocate_aligned(outs.size() * sizeof(double));
    scan_grouped<Store::normal>(L, cd.S, cp.S, on); scan_grouped<Store::streaming>(L, cd.S, cp.S, os);
    double worst_g = 0, worst_s = 0; std::vector<double> buf(Kd + Kp), sd(Kd), sp(Kp);
    for (size_t gi = 0; gi < L.groups.size(); ++gi) for (int l = 0; l < L.groups[gi].n_valid; ++l) {
        int i = L.groups[gi].inst[l]; gather(L, Kd, Kp, on, (int)gi, l, buf.data());
        scan_discount(cd.S, ucf[i], sd.data()); scan_projection(cp.S, pterms[i], sp.data());
        double scale = 1e-300; for (double v : sd) scale = std::max(scale, std::fabs(v)); for (double v : sp) scale = std::max(scale, std::fabs(v));
        for (int k = 0; k < Kd; ++k) worst_g = std::max(worst_g, std::fabs(buf[k] - sd[k]) / scale);
        for (int k = 0; k < Kp; ++k) worst_g = std::max(worst_g, std::fabs(buf[Kd + k] - sp[k]) / scale);
        for (size_t k = 0; k < L.stride(Kd, Kp); ++k) worst_s = std::max(worst_s, std::fabs(on[gi * L.stride(Kd, Kp) + k] - os[gi * L.stride(Kd, Kp) + k]));
    }
    CHECK(worst_g < 1e-9, "grouped vs scalar: %.2e", worst_g);
    CHECK(worst_s == 0.0, "streaming vs normal store differ: %.2e", worst_s);

    // Verify in-register L1 cache reduction
    std::vector<double> reduced(Kd + Kp, 0.0);
    scan_grouped_reduce(L, cd.S, cp.S, reduced.data());
    std::vector<double> expected_ladder(Kd + Kp, 0.0);
    for (size_t i = 0; i < N; ++i) {
        scan_discount(cd.S, ucf[i], sd.data());
        scan_projection(cp.S, pterms[i], sp.data());
        for (int k = 0; k < Kd; ++k) expected_ladder[k] += sd[k];
        for (int k = 0; k < Kp; ++k) expected_ladder[Kd + k] += sp[k];
    }
    double worst_red = 0.0;
    for (int k = 0; k < Kd + Kp; ++k) {
        double scale = std::max(1.0, std::fabs(expected_ladder[k]));
        worst_red = std::max(worst_red, std::fabs(reduced[k] - expected_ladder[k]) / scale);
    }
    CHECK(worst_red < 1e-12, "in-register reduction vs scalar sum: %.2e", worst_red);

    { size_t slots = L.groups.size() * LANES, live = 0; for (auto& g : L.groups) live += g.n_valid; std::printf("groups %zu, lane occupancy %.1f%% (mixed seasoning per group)\n", L.groups.size(), 100.0 * live / slots); }
    std::printf("grouped vs scalar max rel %.1e (summation order; FMA contraction); streaming == normal: %s; in-register reduce: %.1e\n", worst_g, worst_s == 0.0 ? "yes" : "NO", worst_red);
    ladder::free_aligned(on); ladder::free_aligned(os);

    // ---- 4. cashflows beyond the last boundary: capped and open last stencil, scalar vs grouped vs direct overlap adjoint
    for (int open = 0; open < 2; ++open) {
        Stencils Sd2 = cd.S; Sd2.open_last = (open == 1); Stencils Sp2 = cp.S; Sp2.open_last = (open == 1);
        std::vector<InstrumentSpec> book2(16); std::vector<std::vector<UnitCashflow>> u2(16); std::vector<std::vector<ProjectionTerm>> p2(16);
        for (int i = 0; i < 16; ++i) {                                        // 14-year bonds and swaps against 10-year stencils
            InstrumentSpec& in = book2[i]; int maturity = 30 + i + (int)std::lround(14 * 365.25); in.signature = (i % 2) * 100000 + maturity;
            auto day = [&](int n, int per_year){ return maturity - (int)std::lround(n * 365.25 / per_year); };
            if (i % 2 == 0) for (int j = 27; j >= 0; --j) in.fixed.push_back({day(j, 2), 1e6 * 0.03 / 2 + (j == 0 ? 1e6 : 0.0)});
            else { for (int j = 13; j >= 0; --j) in.fixed.push_back({day(j, 1), -1e6 * 0.03}); for (int j = 27; j >= 0; --j) in.flt.push_back({day(j, 2), day(j + 1, 2), day(j, 2), 1e6}); }
            for (auto& c : in.fixed) replicate_fixed(c.amount, c.day / DPY, df, u2[i]);
            for (auto& f : in.flt) replicate_ibor(f.scale, f.a_day / DPY, f.b_day / DPY, f.pay_day / DPY, df, pf, u2[i], p2[i]);
            sort_by_time(u2[i]);
        }
        GroupLayout L2 = build_layout(book2, DPY); refresh_table(L2, Sd2, Sp2, df, pf);
        double* og = (double*)ladder::allocate_aligned(L2.groups.size() * L2.stride(Kd, Kp) * sizeof(double));
        scan_grouped<Store::normal>(L2, Sd2, Sp2, og);
        double w_sg = 0, w_sa = 0; std::vector<double> b2(Kd + Kp), sd2(Kd), sp2(Kp);
        for (size_t gi = 0; gi < L2.groups.size(); ++gi) for (int l = 0; l < L2.groups[gi].n_valid; ++l) {
            int i = L2.groups[gi].inst[l]; gather(L2, Kd, Kp, og, (int)gi, l, b2.data());
            scan_discount(Sd2, u2[i], sd2.data()); scan_projection(Sp2, p2[i], sp2.data());
            double scale = 1.0; for (double v : sd2) scale = std::max(scale, std::fabs(v));
            for (int k = 1; k <= Kd; ++k) { double a = 0; for (auto& c : u2[i]) a -= c.x * Sd2.overlap(k, c.t);          // direct adjoint
                w_sa = std::max(w_sa, std::fabs(a - sd2[k-1]) / scale); w_sg = std::max(w_sg, std::fabs(b2[k-1] - sd2[k-1]) / scale); }
            for (int k = 1; k <= Kp; ++k) { double a = 0; for (auto& q : p2[i]) a += q.w * (Sp2.psi(k, q.a) - Sp2.psi(k, q.b));
                w_sa = std::max(w_sa, std::fabs(a - sp2[k-1]) / scale); w_sg = std::max(w_sg, std::fabs(b2[Kd+k-1] - sp2[k-1]) / scale); }
        }
        ladder::free_aligned(og);
        CHECK(w_sa < 1e-13, "beyond-last-boundary (%s): scalar vs direct overlap adjoint %.2e", open ? "open" : "capped", w_sa);
        CHECK(w_sg < 1e-9,  "beyond-last-boundary (%s): grouped vs scalar %.2e", open ? "open" : "capped", w_sg);
        std::printf("cashflows beyond B[K], %s last stencil: scalar vs adjoint %.1e, grouped vs scalar %.1e\n", open ? "open" : "capped", w_sa, w_sg);
    }

    // ---- 5. structural identities (consequences of the perturbation definitions and linearity)
    {
        Stencils So = cd.S; So.open_last = true;
        // (a) exactly on a boundary, just before, just after, and beyond the last: scalar == direct overlap adjoint
        std::vector<double> ts = {cd.S.B[3], std::nextafter(cd.S.B[3], 0.0), std::nextafter(cd.S.B[3], 99.0), cd.S.B[Kd], cd.S.B[Kd] + 0.7};
        double w = 0;
        for (double t : ts) { std::vector<UnitCashflow> one{{t, 123.0}}; std::vector<double> g(Kd); scan_discount(So, one, g.data());
            for (int k = 1; k <= Kd; ++k) w = std::max(w, std::fabs(g[k-1] + 123.0 * So.overlap(k, t))); }
        CHECK(w < 1e-9, "boundary placement: %.2e", w);
        // (b) open last wave: sum of wave derivatives == derivative of a parallel forward shift over [B0, inf)
        double w2 = 0;
        for (int i = 0; i < 40; ++i) { std::vector<double> g(Kd); scan_discount(So, ucf[i], g.data()); double sum = 0; for (double v : g) sum += v;
            double par = 0; for (auto& c : ucf[i]) par -= c.x * std::max(c.t - So.B[0], 0.0); w2 = std::max(w2, std::fabs(sum - par) / std::max(1.0, std::fabs(par))); }
        CHECK(w2 < 1e-12, "parallel-shift identity (open last): %.2e", w2);
        // (c) merging adjacent bounded waves: ladder on the coarse grid == sums of fine-grid entries
        Stencils coarse; coarse.B = {cd.S.B[0], cd.S.B[4], cd.S.B[9], cd.S.B[Kd]};
        double w3 = 0;
        for (int i = 0; i < 40; ++i) { std::vector<double> gf(Kd), gc(coarse.K()); scan_discount(cd.S, ucf[i], gf.data()); scan_discount(coarse, ucf[i], gc.data());
            double s1 = 0, s2 = 0, s3 = 0; for (int k = 1; k <= 4; ++k) s1 += gf[k-1]; for (int k = 5; k <= 9; ++k) s2 += gf[k-1]; for (int k = 10; k <= Kd; ++k) s3 += gf[k-1];
            double sc = std::max(1.0, std::fabs(gc[0]) + std::fabs(gc[1]) + std::fabs(gc[2]));
            w3 = std::max(w3, (std::fabs(s1 - gc[0]) + std::fabs(s2 - gc[1]) + std::fabs(s3 - gc[2])) / sc); }
        CHECK(w3 < 1e-12, "merged-wave identity: %.2e", w3);
        // (d) projection coupon entirely beyond the open last wave's start: risk is w*(b-a) on the last stencil, not zero
        { Stencils Sp_o = cp.S; Sp_o.open_last = true; double a = cp.S.B[Kp] + 0.5, b = a + 0.5;
          std::vector<ProjectionTerm> one{{b, a, b, 1000.0}}; std::vector<double> g(Kp); scan_projection(Sp_o, one, g.data());
          CHECK(std::fabs(g[Kp-1] - 1000.0 * (b - a)) < 1e-9, "projection beyond open last wave: %.6f vs %.6f", g[Kp-1], 1000.0 * (b - a));
          Stencils Sp_c = cp.S; scan_projection(Sp_c, one, g.data());
          CHECK(g[Kp-1] == 0.0, "projection beyond capped last wave should be zero: %.3e", g[Kp-1]); }
        std::printf("structural identities: boundary %.1e, parallel %.1e, merge %.1e; projection beyond last wave handled\n", w, w2, w3);
    }

    // ---- 6. simple zero curves and curve construction
    {
        std::vector<double> B = {0.0, 1.0, 2.0, 5.0, 10.0};
        std::vector<double> z = {0.02, 0.025, 0.030, 0.035, 0.040};
        Curve cz = Curve::from_zero_rates(B, z);
        CHECK(cz.df(0.0) == 1.0, "df(0) == 1.0");
        for (size_t i = 1; i < B.size(); ++i) {
            double expected_df = std::exp(-z[i] * B[i]);
            CHECK(std::fabs(cz.df(B[i]) - expected_df) < 1e-15, "zero curve df at pillar %zu", i);
            CHECK(std::fabs(cz.zero_rate(B[i]) - z[i]) < 1e-15, "zero curve zero_rate at pillar %zu", i);
        }

        // flat curve
        Curve cflat = Curve::flat(B, 0.03);
        for (double t : {0.5, 1.5, 3.0, 7.5}) {
            CHECK(std::fabs(cflat.df(t) - std::exp(-0.03 * t)) < 1e-15, "flat curve df at %.1f", t);
            CHECK(std::fabs(cflat.zero_rate(t) - 0.03) < 1e-14, "flat curve zero_rate at %.1f", t);
        }

        // forward rate
        double fwd = cflat.forward_rate(1.0, 2.0);
        double exp_fwd = (std::exp(-0.03 * 1.0) / std::exp(-0.03 * 2.0) - 1.0) / 1.0;
        CHECK(std::fabs(fwd - exp_fwd) < 1e-15, "forward rate consistency");

        // box bump exactness: zero leakage to the left, single multiplicative factor to the right
        int bump_k = 2; // interval [1.0, 2.0)
        double delta = 0.0010;
        CHECK(cz.df_bumped(0.8, bump_k, delta) == cz.df(0.8), "no leakage left of stencil");
        double right_factor = std::exp(-delta * (B[bump_k] - B[bump_k-1]));
        CHECK(std::fabs(cz.df_bumped(5.0, bump_k, delta) / cz.df(5.0) - right_factor) < 1e-15, "exact right tail shift");

        std::printf("simple zero curve construction: verified (nodes, flat, forward rate, exact no-leakage)\n");
    }

    std::printf(fails ? "FAILED (%d)\n" : "all tests passed\n", fails);
    return fails ? 1 : 0;
}
