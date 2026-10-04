#include "../example_dir.hpp"
// Dual-curve bucketed risk benchmark: bonds + vanilla swaps, K=50 pillars per curve.
//
//   BASE  traditional bump-and-reprice: AoS instruments, virtual npv, 2K bumped OIS curves for all
//         instruments plus 2K bumped projection curves for swaps, central differences.
//   SCAN  scalar reverse scan per instrument (the kernel reconciled against QuantLib), own D(t) lookups.
//   GRP   instruments grouped 8-wide by schedule signature, shared D(t) over unique dates,
//         AVX-512 via std::experimental::simd, one column walk per group, normal or streaming stores.
//
// Curve model: log-linear on node discount factors, internal coordinate theta_k = -log(D_k/D_{k-1}).
#include <experimental/simd>
#include <immintrin.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

namespace stdx = std::experimental;
using vec8 = stdx::native_simd<double>;
static_assert(vec8::size() == 8, "expects AVX-512: 8 doubles per vector");

constexpr int LANES = 8;
constexpr double DAYS = 365.0;

// ------------------------------------------------------------------ curve
struct Curve {
    std::vector<double> B, D, logD;                      // B[0] = reference time, D[0] = 1
    int K() const { return (int)B.size() - 1; }
    int bucket(double t) const {
        int k = (int)(std::upper_bound(B.begin(), B.end(), t) - B.begin());
        return std::max(1, std::min(k, K()));
    }
    double alpha(int k, double t) const { return (t - B[k-1]) / (B[k] - B[k-1]); }
    double df(double t) const {
        int k = bucket(t);
        return std::exp(logD[k-1] + alpha(k, t) * (logD[k] - logD[k-1]));
    }
    double psi(int k, double t) const {
        if (t < B[k-1]) return 0.0;
        if (t >= B[k])  return -1.0;
        return -alpha(k, t);
    }
    Curve bumped(int k, double eps) const {           // theta_k += eps  <=>  D_j *= exp(-eps), j>=k
        Curve c = *this;
        for (int j = k; j <= K(); ++j) { c.D[j] *= std::exp(-eps); c.logD[j] = std::log(c.D[j]); }
        return c;
    }
};
// curves from the QuantLib MulticurveBootstrapping example (cashflows2.txt written by ql_examples.cpp)
static void load_curves(const char* path, Curve& ois, Curve& proj) {
    FILE* in = std::fopen(path, "r"); int nc; std::fscanf(in, "%d", &nc);
    for (Curve* c : {&ois, &proj}) { char nm[16]; int n; std::fscanf(in, "%15s %d", nm, &n); c->B.resize(n); c->D.resize(n); c->logD.resize(n);
        for (int i = 0; i < n; ++i) { std::fscanf(in, "%lf %lf", &c->B[i], &c->D[i]); c->logD[i] = std::log(c->D[i]); } }
    std::fclose(in);
}

// ------------------------------------------------------------------ portfolio (AoS, the "authoritative" model)
struct Cashflow { int day; double amount; };
struct FloatCf  { int pay_day, a_day, b_day; double scale; };     // amount = scale * (P(a)/P(b) - 1)

struct Instrument {
    int sig;                                                     // schedule signature (dates + type)
    virtual ~Instrument() = default;
    virtual double npv(const Curve& disc, const Curve& proj) const = 0;
    virtual bool has_float() const = 0;
};
struct Bond : Instrument {
    std::vector<Cashflow> cfs;
    double npv(const Curve& disc, const Curve&) const override {
        double pv = 0; for (auto& c : cfs) pv += c.amount * disc.df(c.day / DAYS); return pv;
    }
    bool has_float() const override { return false; }
};
struct Swap : Instrument {
    std::vector<Cashflow> fixed;
    std::vector<FloatCf>  flt;
    double npv(const Curve& disc, const Curve& proj) const override {
        double pv = 0;
        for (auto& c : fixed) pv += c.amount * disc.df(c.day / DAYS);
        for (auto& f : flt)
            pv += f.scale * (proj.df(f.a_day / DAYS) / proj.df(f.b_day / DAYS) - 1.0) * disc.df(f.pay_day / DAYS);
        return pv;
    }
    bool has_float() const override { return true; }
};

// signature = (type, start offset, maturity years); instruments sharing one share every date
struct Signature { bool swap; int start; int years; };
static int period_day(int start, int n, int per_year) { return start + (int)std::lround(n * 365.25 / per_year); }

static std::vector<std::unique_ptr<Instrument>> make_portfolio(size_t n, std::vector<Signature>& sigs, unsigned seed)
{
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> start_d(2, 61), years_d(1, 29), type_d(0, 1);
    std::unordered_map<long, int> sig_index;
    std::vector<std::unique_ptr<Instrument>> book; book.reserve(n);
    const double notionals[] = {1e6, 2.5e6, 5e6, 1e7};
    for (size_t i = 0; i < n; ++i) {
        Signature s{type_d(rng) == 1, start_d(rng), years_d(rng)};
        long key = (s.swap ? 1000000L : 0L) + s.start * 100L + s.years;
        auto it = sig_index.find(key);
        if (it == sig_index.end()) { it = sig_index.emplace(key, (int)sigs.size()).first; sigs.push_back(s); }
        double notional = notionals[rng() % 4];
        double rate = 0.01 + 0.05 * std::uniform_real_distribution<double>(0, 1)(rng);
        if (!s.swap) {
            auto b = std::make_unique<Bond>(); b->sig = it->second;
            int ncf = 2 * s.years;
            for (int j = 1; j <= ncf; ++j) b->cfs.push_back({period_day(s.start, j, 2), notional * rate * 0.5 + (j == ncf ? notional : 0.0)});
            book.push_back(std::move(b));
        } else {
            auto sw = std::make_unique<Swap>(); sw->sig = it->second;
            for (int j = 1; j <= s.years; ++j) sw->fixed.push_back({period_day(s.start, j, 1), -notional * rate});   // payer
            for (int j = 1; j <= 2 * s.years; ++j) {
                int a = period_day(s.start, j - 1, 2), b = period_day(s.start, j, 2);
                sw->flt.push_back({b, a, b, notional * ((b - a) / 360.0) / ((b - a) / 360.0)});      // Act/360 both
            }
            book.push_back(std::move(sw));
        }
    }
    return book;
}

// ------------------------------------------------------------------ BASE: bump and reprice
static void base_risk(const std::vector<std::unique_ptr<Instrument>>& book, const Curve& ois, const Curve& proj,
                      double eps, double* out_ois, double* out_proj)
{
    const size_t n = book.size(); const int Ko = ois.K(), Kp = proj.K();
    for (int k = 1; k <= Ko; ++k) {
        Curve up = ois.bumped(k, eps), dn = ois.bumped(k, -eps);
        for (size_t i = 0; i < n; ++i)
            out_ois[i * Ko + k - 1] = (book[i]->npv(up, proj) - book[i]->npv(dn, proj)) / (2 * eps);
    }
    for (int k = 1; k <= Kp; ++k) {
        Curve up = proj.bumped(k, eps), dn = proj.bumped(k, -eps);
        for (size_t i = 0; i < n; ++i)
            out_proj[i * Kp + k - 1] = book[i]->has_float() ? (book[i]->npv(ois, up) - book[i]->npv(ois, dn)) / (2 * eps) : 0.0;
    }
}

// ------------------------------------------------------------------ SCAN: scalar reverse scan per instrument
struct ScanInstrument {                     // date-sorted merged discount stream + float coupons
    std::vector<double> t, c;
    std::vector<FloatCf> flt;
};
static std::vector<ScanInstrument> build_scan_layout(const std::vector<std::unique_ptr<Instrument>>& book)
{
    std::vector<ScanInstrument> v(book.size());
    for (size_t i = 0; i < book.size(); ++i) {
        std::vector<Cashflow> merged;
        if (auto* b = dynamic_cast<const Bond*>(book[i].get())) merged = b->cfs;
        else {
            auto* s = static_cast<const Swap*>(book[i].get());
            merged = s->fixed;
            for (auto& f : s->flt) merged.push_back({f.pay_day, 0.0});     // float amounts filled at run time
            std::sort(merged.begin(), merged.end(), [](auto& x, auto& y){ return x.day < y.day; });
            v[i].flt = s->flt;
        }
        for (auto& m : merged) { v[i].t.push_back(m.day / DAYS); v[i].c.push_back(m.amount); }
    }
    return v;
}
static void scan_risk(std::vector<ScanInstrument>& insts, const Curve& ois, const Curve& proj, double* out_ois, double* out_proj)
{
    std::vector<double> x; const int Ko = ois.K(), Kp = proj.K();
    std::vector<double> interior(Ko + 1), suffix(Ko + 1);
    for (size_t i = 0; i < insts.size(); ++i) {
        auto& in = insts[i];
        // projected float amounts into the merged stream (pay dates are sorted; find by day)
        if (!in.flt.empty()) {
            size_t j = 0;
            for (auto& f : in.flt) {
                double amt = f.scale * (proj.df(f.a_day / DAYS) / proj.df(f.b_day / DAYS) - 1.0);
                double tp = f.pay_day / DAYS;
                while (in.t[j] < tp) ++j;
                in.c[j] += amt;                      // may coincide with a fixed date
            }
        }
        const size_t n = in.t.size();
        x.resize(n);
        for (int k = 0; k <= Ko; ++k) interior[k] = 0;
        for (size_t j = 0; j < n; ++j) {
            int k = ois.bucket(in.t[j]);
            x[j] = in.c[j] * ois.df(in.t[j]);
            interior[k] += x[j] * ois.alpha(k, in.t[j]);
        }
        double running = 0; int k = Ko;
        for (size_t j = n; j-- > 0;) {
            while (k >= 1 && in.t[j] < ois.B[k]) { suffix[k] = running; --k; }
            running += x[j];
        }
        while (k >= 1) { suffix[k] = running; --k; }
        double* o = out_ois + i * Ko;
        for (int kk = 1; kk <= Ko; ++kk) o[kk-1] = -(interior[kk] + suffix[kk]);
        double* p = out_proj + i * Kp;
        for (int kk = 0; kk < Kp; ++kk) p[kk] = 0;
        for (auto& f : in.flt) {
            double a = f.a_day / DAYS, b = f.b_day / DAYS;
            double w = f.scale * (proj.df(a) / proj.df(b)) * ois.df(f.pay_day / DAYS);
            int ka = proj.bucket(a), kb = proj.bucket(b);
            for (int q = ka; q <= kb; ++q) p[q-1] += w * (proj.psi(q, a) - proj.psi(q, b));
        }
        // undo the float amounts so the layout can be reused across repetitions
        if (!in.flt.empty()) {
            size_t j = 0;
            for (auto& f : in.flt) {
                double amt = f.scale * (proj.df(f.a_day / DAYS) / proj.df(f.b_day / DAYS) - 1.0);
                double tp = f.pay_day / DAYS;
                while (in.t[j] < tp) ++j;
                in.c[j] -= amt;
            }
        }
    }
}

// ------------------------------------------------------------------ GRP: 8-lane groups by signature
struct alignas(64) Lane8 { double v[LANES]; };
struct Column { int day; int di; };       // discount column (shared across lanes)
struct FloatCol { int pay_day, a_day, b_day; int ip, ia, ib; };
struct Group {
    int sig; int first_inst[LANES]; int n_valid;
    std::vector<Column>   cols;        // union of discount dates, ascending
    std::vector<Lane8>    fixed_amt;   // per column, per lane (fixed/bond amounts)
    std::vector<FloatCol> fcols;       // float coupons (shared dates)
    std::vector<int>      fcol_to_col; // which discount column each float coupon pays into
    std::vector<Lane8>    fscale;      // per float coupon, per lane
};
struct GroupLayout {
    std::vector<Group> groups;
    std::vector<int> unique_days;                     // sorted
    std::unordered_map<int, int> day_index;
};

static GroupLayout build_group_layout(const std::vector<std::unique_ptr<Instrument>>& book, const std::vector<Signature>& sigs)
{
    GroupLayout L;
    std::vector<std::vector<int>> by_sig(sigs.size());
    for (size_t i = 0; i < book.size(); ++i) by_sig[book[i]->sig].push_back((int)i);
    std::vector<int> days;
    for (size_t s = 0; s < sigs.size(); ++s) {
        auto& members = by_sig[s];
        for (size_t g = 0; g < members.size(); g += LANES) {
            Group G; G.sig = (int)s; G.n_valid = (int)std::min<size_t>(LANES, members.size() - g);
            for (int l = 0; l < LANES; ++l) G.first_inst[l] = l < G.n_valid ? members[g + l] : -1;
            // dates from the first member (all share)
            const Instrument* ref = book[members[g]].get();
            std::vector<int> dset;
            if (auto* b = dynamic_cast<const Bond*>(ref)) for (auto& c : b->cfs) dset.push_back(c.day);
            else { auto* sw = static_cast<const Swap*>(ref); for (auto& c : sw->fixed) dset.push_back(c.day); for (auto& f : sw->flt) dset.push_back(f.pay_day); }
            std::sort(dset.begin(), dset.end()); dset.erase(std::unique(dset.begin(), dset.end()), dset.end());
            for (int d : dset) G.cols.push_back({d, -1});
            G.fixed_amt.assign(dset.size(), Lane8{});
            auto col_of = [&](int day){ return (int)(std::lower_bound(dset.begin(), dset.end(), day) - dset.begin()); };
            for (int l = 0; l < G.n_valid; ++l) {
                const Instrument* in = book[members[g + l]].get();
                if (auto* b = dynamic_cast<const Bond*>(in)) for (auto& c : b->cfs) G.fixed_amt[col_of(c.day)].v[l] += c.amount;
                else {
                    auto* sw = static_cast<const Swap*>(in);
                    for (auto& c : sw->fixed) G.fixed_amt[col_of(c.day)].v[l] += c.amount;
                    if (l == 0) for (auto& f : sw->flt) { G.fcols.push_back({f.pay_day, f.a_day, f.b_day, -1, -1, -1}); G.fcol_to_col.push_back(col_of(f.pay_day)); }
                    if (G.fscale.empty()) G.fscale.assign(sw->flt.size(), Lane8{});
                    for (size_t q = 0; q < sw->flt.size(); ++q) G.fscale[q].v[l] = sw->flt[q].scale;
                }
            }
            for (auto& c : G.cols) days.push_back(c.day);
            for (auto& f : G.fcols) { days.push_back(f.a_day); days.push_back(f.b_day); }
            L.groups.push_back(std::move(G));
        }
    }
    std::sort(days.begin(), days.end()); days.erase(std::unique(days.begin(), days.end()), days.end());
    L.unique_days = days;
    for (size_t i = 0; i < days.size(); ++i) L.day_index[days[i]] = (int)i;
    for (auto& G : L.groups) {
        for (auto& c : G.cols) c.di = L.day_index[c.day];
        for (auto& f : G.fcols) { f.ip = L.day_index[f.pay_day]; f.ia = L.day_index[f.a_day]; f.ib = L.day_index[f.b_day]; }
    }
    return L;
}

// shared D(t) and P(t) over unique dates, plus bucket / alpha per date (date-local refresh phase)
struct DateTables { std::vector<double> D, P, alpha, t; std::vector<int> bucket, pbucket; };
static DateTables build_date_tables(const GroupLayout& L, const Curve& ois, const Curve& proj)
{
    DateTables T; size_t n = L.unique_days.size();
    T.D.resize(n); T.P.resize(n); T.alpha.resize(n); T.t.resize(n); T.bucket.resize(n); T.pbucket.resize(n);
    for (size_t i = 0; i < n; ++i) {
        double t = L.unique_days[i] / DAYS; int k = ois.bucket(t);
        T.t[i] = t; T.bucket[i] = k; T.pbucket[i] = proj.bucket(t); T.alpha[i] = ois.alpha(k, t); T.D[i] = ois.df(t); T.P[i] = proj.df(t);
    }
    return T;
}

template <bool STREAM>
static inline void store8(double* p, const vec8& v) {
    if constexpr (STREAM) _mm512_stream_pd(p, (__m512d)v);              // the one intrinsic: store policy only
    else v.copy_to(p, stdx::vector_aligned);
}

// output layout: per group, [K][8] for OIS then [K][8] for projection, contiguous, bucket-major
template <bool STREAM>
static void group_risk(GroupLayout& L, const DateTables& T, const Curve& ois, const Curve& proj, double* out)
{
    const int Ko = ois.K(), Kp = proj.K(); const size_t stride = (size_t)(Ko + Kp) * LANES;
    std::vector<vec8> pacc(Kp + 1);
    for (size_t gi = 0; gi < L.groups.size(); ++gi) {
        Group& G = L.groups[gi];
        double* out_ois  = out + gi * stride;
        double* out_proj = out_ois + Ko * LANES;

        // projected float amounts: ratio broadcast, per-lane scale -> per-lane amount, added to the column
        // (kept in a small scratch so the layout stays reusable)
        vec8 famt[64]; int nf = (int)G.fcols.size();
        for (int q = 0; q < nf; ++q) {
            const FloatCol& f = G.fcols[q];
            double ratio = T.P[f.ia] / T.P[f.ib];
            vec8 s(G.fscale[q].v, stdx::vector_aligned);
            famt[q] = s * (ratio - 1.0);
        }

        // ---- discount pass: backward column walk, pillar snapshots
        // bucket b is accumulated while k == b-1; its suffix (tail >= B_b) was captured on entry
        vec8 running = 0.0, interior = 0.0, suffix_cur = 0.0;
        int k = Ko; int q = nf - 1;
        auto emit = [&](int b) { if (b <= Ko) { vec8 r = -(interior + suffix_cur); store8<STREAM>(out_ois + (b - 1) * LANES, r); } };
        for (int ci = (int)G.cols.size() - 1; ci >= 0; --ci) {
            const int di = G.cols[ci].di;
            const double t = T.t[di];
            while (k >= 1 && t < ois.B[k]) { emit(k + 1); suffix_cur = running; interior = 0.0; --k; }
            vec8 cf(G.fixed_amt[ci].v, stdx::vector_aligned);
            while (q >= 0 && G.fcol_to_col[q] == ci) { cf += famt[q]; --q; }
            vec8 x = cf * T.D[di];
            running += x;
            interior += x * T.alpha[di];
        }
        while (k >= 1) { emit(k + 1); suffix_cur = running; interior = 0.0; --k; }
        emit(1);

        // ---- projection pass: interior only, forward over float coupons
#ifndef NO_PROJ
        for (int kk = 0; kk <= Kp; ++kk) pacc[kk] = 0.0;
        for (int qq = 0; qq < nf; ++qq) {
            const FloatCol& f = G.fcols[qq];
            const int ia = f.ia, ib = f.ib, ip = f.ip;
            const double ta = T.t[ia], tb = T.t[ib];
            const double ratio = T.P[ia] / T.P[ib];
            vec8 s(G.fscale[qq].v, stdx::vector_aligned);
            vec8 w = s * (ratio * T.D[ip]);
            const int ka = T.pbucket[ia], kb = T.pbucket[ib];
            for (int kk = ka; kk <= kb; ++kk) pacc[kk] += w * (proj.psi(kk, ta) - proj.psi(kk, tb));
        }
        for (int kk = 1; kk <= Kp; ++kk) store8<STREAM>(out_proj + (kk - 1) * LANES, pacc[kk]);
#endif
    }
    if constexpr (STREAM) _mm_sfence();
}

// ------------------------------------------------------------------ timing + checks
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static double max_rel(const double* a, const double* b, size_t n, double floor_abs) {
    double m = 0; for (size_t i = 0; i < n; ++i) { double s = std::max(std::fabs(a[i]), std::fabs(b[i])); if (s > floor_abs) m = std::max(m, std::fabs(a[i]-b[i]) / s); } return m;
}

int main(int argc, char** argv)
{
    enter_example_dir();
    size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000;
    int run_base = argc > 2 ? std::atoi(argv[2]) : 1;
    int reps = argc > 3 ? std::atoi(argv[3]) : 3;
    Curve ois, proj; load_curves("quantlib_example_curves.txt", ois, proj);
    const int Ko = ois.K(), Kp = proj.K();

    std::vector<Signature> sigs;
    auto book = make_portfolio(N, sigs, 42);
    size_t ncf = 0; for (auto& b : book) { if (auto* x = dynamic_cast<Bond*>(b.get())) ncf += x->cfs.size(); else { auto* s = static_cast<Swap*>(b.get()); ncf += s->fixed.size() + s->flt.size(); } }
    std::printf("N=%zu instruments, %zu signatures, %.1f cashflows/instrument, K=%d+%d (Eonia+Euribor6M nodes from the QuantLib example)\n", N, sigs.size(), (double)ncf / N, Ko, Kp);

    std::vector<double> out_ois_s(N * Ko), out_proj_s(N * Kp);

    // --- SCAN
    double t0 = now_ms();
    auto scan_layout = build_scan_layout(book);
    double t_scan_layout = now_ms() - t0;
    double best_scan = 1e30;
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); scan_risk(scan_layout, ois, proj, out_ois_s.data(), out_proj_s.data()); best_scan = std::min(best_scan, now_ms() - t0); }

    // --- GRP
    t0 = now_ms();
    auto L = build_group_layout(book, sigs);
    double t_grp_layout = now_ms() - t0;
    const size_t stride = (size_t)(Ko + Kp) * LANES;
    double* out_g = (double*)std::aligned_alloc(64, L.groups.size() * stride * sizeof(double));
    double best_tab = 1e30, best_grp_n = 1e30, best_grp_s = 1e30;
    DateTables T;
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); T = build_date_tables(L, ois, proj); best_tab = std::min(best_tab, now_ms() - t0); }
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); group_risk<false>(L, T, ois, proj, out_g); best_grp_n = std::min(best_grp_n, now_ms() - t0); }
    for (int r = 0; r < reps; ++r) { t0 = now_ms(); group_risk<true>(L, T, ois, proj, out_g); best_grp_s = std::min(best_grp_s, now_ms() - t0); }

    // GRP vs SCAN check (gather per instrument)
    double m_ois = 0, m_proj = 0;
    for (size_t gi = 0; gi < L.groups.size(); ++gi) for (int l = 0; l < L.groups[gi].n_valid; ++l) {
        int i = L.groups[gi].first_inst[l];
        for (int k = 0; k < Ko; ++k) {
            double a = out_g[gi * stride + k * LANES + l], b = out_ois_s[i * Ko + k];
            double s = std::max(std::fabs(a), std::fabs(b)); if (s > 1.0) m_ois = std::max(m_ois, std::fabs(a-b)/s);
        }
        for (int k = 0; k < Kp; ++k) {
            double a = out_g[gi * stride + Ko * LANES + k * LANES + l], b = out_proj_s[i * Kp + k];
            double s = std::max(std::fabs(a), std::fabs(b)); if (s > 1.0) m_proj = std::max(m_proj, std::fabs(a-b)/s);
        }
    }
    std::printf("GRP vs SCAN: max rel diff OIS %.2e, projection %.2e\n", m_ois, m_proj);

    // --- BASE
    double t_base = -1, m_bo = 0, m_bp = 0;
    if (run_base) {
        std::vector<double> bo(N * Ko), bp(N * Kp);
        t0 = now_ms(); base_risk(book, ois, proj, 1e-5, bo.data(), bp.data()); t_base = now_ms() - t0;
        m_bo = max_rel(bo.data(), out_ois_s.data(), N * Ko, 1e4);
        m_bp = max_rel(bp.data(), out_proj_s.data(), N * Kp, 1e4);
        std::printf("BASE vs SCAN (central diff, eps=1e-5, values > 1e4): max rel diff OIS %.2e, projection %.2e\n", m_bo, m_bp);
    }

    const double n_out = (double)N * (Ko + Kp);
    std::printf("\n%-44s %12s %14s %10s\n", "variant", "time (ms)", "ns/output", "speedup");
    if (run_base) std::printf("%-44s %12.1f %14.2f %10s\n", "BASE bump-and-reprice (2Ko+2Kp bumps)", t_base, t_base * 1e6 / n_out, "1x");
    auto row = [&](const char* name, double ms) { std::printf("%-44s %12.2f %14.3f %10s\n", name, ms, ms * 1e6 / n_out, run_base ? (std::string(std::to_string((int)std::lround(t_base / ms))) + "x").c_str() : "-"); };
    row("SCAN scalar reverse scan, own D(t)", best_scan);
    row("GRP  shared D(t)/P(t) tables (per run)", best_tab);
    row("GRP  8-lane AVX-512, normal stores", best_grp_n);
    row("GRP  8-lane AVX-512, streaming stores", best_grp_s);
    row("GRP  tables + streaming kernel", best_tab + best_grp_s);
    std::printf("\none-off layout builds: scan %.1f ms, group %.1f ms; unique dates %zu; groups %zu; output %.0f MB\n",
                t_scan_layout, t_grp_layout, L.unique_days.size(), L.groups.size(), L.groups.size() * stride * 8 / 1e6);
    std::free(out_g);
    return 0;
}
