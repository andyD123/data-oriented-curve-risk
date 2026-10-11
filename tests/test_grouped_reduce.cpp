// Direct in-register portfolio reduction versus the existing grouped output,
// scalar per-instrument oracle, and empty/stale/irregular-grid conditions.
// The test covers fixed, IBOR, and OIS flows without any QuantLib dependency.
#include "ladder/aligned_memory.hpp"
#include "ladder/scan_grouped_reduce.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <vector>

using namespace ladder;
static void require(bool yes, const char* msg) {
    if (!yes) throw std::runtime_error(msg);
}
static void close_risk(double actual, double reference, double magnitude,
                       const char* msg) {
    const double tolerance = 5e-8 +
        5e-11 * std::max({1.0, std::fabs(actual),
                         std::fabs(reference), magnitude});
    if (!(std::isfinite(actual) && std::isfinite(reference)) ||
        std::fabs(actual-reference) > tolerance) {
        std::fprintf(stderr, "%s: observed %.17g expected %.17g "
                     "scale %.17g tolerance %.4g\n",
                     msg, actual, reference, magnitude, tolerance);
        throw std::runtime_error(msg);
    }
}

static void scenario(bool open, bool large_grid) {
    constexpr double days_per_year = 365.0;
    Stencils Sd{{0.12, 0.25, 0.5, 1., 2., 3., 4., 5., 8.}, open};
    Stencils Sp{{0.10, 0.3, 0.7, 1., 1.5, 2.0, 3., 4., 6., 9.}, open};
    if (large_grid) {
        // Exercise the heap/scratch path beyond the old PR #2 stack limit.
        while (Sp.K() <= 150) Sp.B.push_back(Sp.B.back() + 0.125);
    }
    const auto df = [](double t) {
        return std::exp(-0.025 * t - 0.0003 * t * t);
    };
    const auto pf = [](double t) {
        return std::exp(-0.031 * t - 0.0002 * t * t);
    };
    std::mt19937_64 rng(20261011);
    std::uniform_int_distribution<int> day(0, 3500);
    std::uniform_real_distribution<double> amount(-2e6, 2e6);
    std::vector<InstrumentSpec> book(131);  // 131 is not a multiple of 8.
    for (size_t i = 0; i < book.size(); ++i) {
        InstrumentSpec& inst = book[i];
        inst.signature = static_cast<int>(i % 7);
        for (int n = 0; n < 3; ++n)
            inst.fixed.push_back({day(rng), amount(rng)});
        for (int n = 0; n < 2; ++n) {
            const int a = day(rng), b = a + 95 + n * 63;
            inst.flt.push_back({b + 1, a, b, amount(rng)});
            inst.ois.push_back({b + (n == 0 ? 0 : 5), a, b, amount(rng)});
        }
        if (i % 9 == 0) {
            inst.fixed.push_back({-47, amount(rng)});  // Before B[0].
            inst.fixed.push_back({3900, amount(rng)}); // Beyond B[K].
        }
        if (i % 13 == 0) inst.ois.push_back({4100, 3600, 4050, amount(rng)});
    }
    GroupLayout L = build_layout(book, days_per_year);
    refresh_table(L, Sd, Sp, df, pf);
    const int Kd=Sd.K(), Kp=Sp.K();
    const size_t stride=L.stride(Kd,Kp);
    auto* materialized = static_cast<double*>(
        allocate_aligned(L.groups.size()*stride*sizeof(double)));
    require(materialized != nullptr, "aligned buffer allocation failed");
    scan_grouped<Store::normal>(L,Sd,Sp,materialized);

    // The baseline performs a separately materialised write for every
    // instrument, then aggregates only real SIMD lanes.
    std::vector<long double> materialized_sum(Kd+Kp, 0.0L);
    std::vector<long double> magnitude(Kd+Kp, 0.0L);
    for (size_t gi=0; gi<L.groups.size(); ++gi)
        for (int k=0; k<Kd+Kp; ++k)
            for (int lane=0; lane<L.groups[gi].n_valid; ++lane) {
                const double v=materialized[gi*stride+
                                          static_cast<size_t>(k)*LANES+lane];
                materialized_sum[k]+=v;
                magnitude[k]+=std::fabs(v);
            }
    std::vector<double> reduced(Kd+Kp);
    GroupReduceScratch scratch;
    scan_grouped_reduce(L,Sd,Sp,std::span<double>(reduced),scratch);
    for (int k=0; k<Kd+Kp; ++k)
        close_risk(reduced[k], static_cast<double>(materialized_sum[k]),
                   static_cast<double>(magnitude[k]),
                   "in-register reduce vs grouped output");

    // Independent scalar replication/oracle uses no reduction-specific code.
    std::vector<long double> scalar_sum(Kd+Kp,0.0L);
    std::vector<double> d(Kd), p(Kp);
    for (const auto& inst:book) {
        std::vector<UnitCashflow> cf;
        std::vector<ProjectionTerm> projection;
        for (const auto& f:inst.fixed)
            replicate_fixed(f.amount, f.day/days_per_year, df, cf);
        for (const auto& f:inst.flt)
            replicate_ibor(f.scale, f.a_day/days_per_year,
                           f.b_day/days_per_year,
                           f.pay_day/days_per_year, df, pf, cf, projection);
        for (const auto& o:inst.ois)
            replicate_ois(o.N,o.a_day/days_per_year,o.b_day/days_per_year,
                          o.pay_day/days_per_year,df,cf);
        sort_by_time(cf);
        scan_discount(Sd,cf,d.data());
        scan_projection(Sp,projection,p.data());
        for (int k=0; k<Kd; ++k) scalar_sum[k]+=d[k];
        for (int k=0; k<Kp; ++k) scalar_sum[Kd+k]+=p[k];
    }
    for (int k=0; k<Kd+Kp; ++k)
        close_risk(reduced[k], static_cast<double>(scalar_sum[k]),
                   static_cast<double>(magnitude[k]),
                   "in-register reduce vs scalar oracle");
    const auto once = reduced;
    std::fill(reduced.begin(), reduced.end(),
              std::numeric_limits<double>::quiet_NaN());
    scan_grouped_reduce(L,Sd,Sp,reduced.data(),scratch);
    for (int k=0; k<Kd+Kp; ++k)
        require(reduced[k]==once[k],"scratch reused incorrectly");

    // Typed span catches undersized buffer before the raw-pointer API.
    bool rejects=false;
    try {
        scan_grouped_reduce(L,Sd,Sp,std::span<double>(reduced.data(),
                            reduced.size()-1),scratch);
    } catch (const std::invalid_argument&) { rejects=true; }
    require(rejects,"wrong-sized span accepted");
    rejects=false;
    try { scan_grouped_reduce(L,Sd,Sp,(double*)nullptr,scratch); }
    catch (const std::invalid_argument&) { rejects=true; }
    require(rejects,"null raw output accepted");
    rejects=false;
    Stencils stale=Sd;stale.B[2]+=0.01;
    try {scan_grouped_reduce(L,stale,Sp,reduced.data(),scratch);}
    catch (const std::invalid_argument&) {rejects=true;}
    require(rejects,"stale discount grid accepted");
    rejects=false;
    Stencils stale_projection=Sp;stale_projection.B[2]+=0.01;
    try {scan_grouped_reduce(L,Sd,stale_projection,reduced.data(),scratch);}
    catch (const std::invalid_argument&) {rejects=true;}
    require(rejects,"stale projection grid accepted");
    free_aligned(materialized);
    std::printf("grouped reduce: N=%zu groups=%zu Kd=%d Kp=%d open=%d "
                "PASS vs grouped output and scalar oracle\n",
                book.size(),L.groups.size(),Kd,Kp,open);
}

static void empty_book() {
    const Stencils S{{0.0, 1.0, 2.0},true};
    std::vector<InstrumentSpec> book;
    GroupLayout L=build_layout(book);
    refresh_table(L,S,S,[](double){return 1.0;},
                         [](double){return 1.0;});
    GroupReduceScratch scratch;
    double values[4]={1.0,2.0,3.0,4.0};
    scan_grouped_reduce(L,S,S,values,scratch);
    for (double x:values) require(x==0.0,"empty book not zero");
}
int main() try {
    empty_book();
    scenario(false,false);
    scenario(true,false);
    scenario(true,true);
    std::puts("grouped portfolio reduction PASS");
    return 0;
} catch(const std::exception& e){
    std::fprintf(stderr,"FAIL grouped_reduce: %s\n",e.what());
    return 1;
}
