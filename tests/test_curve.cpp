// Standalone curve construction, interpolation, tails and box-shock checks.
// The curve does not depend on QuantLib, calendars or the SIMD backend.
#include "ladder/curve.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using ladder::Curve;
using ladder::Stencils;
using ladder::UnitCashflow;

static void check(bool success, const char* name) {
    if (!success) throw std::runtime_error(name);
}
static void close(double actual, double expected, double rel, const char* name) {
    const double scale = std::max({1.0, std::abs(actual), std::abs(expected)});
    if (!std::isfinite(actual) || !std::isfinite(expected) ||
        std::abs(actual-expected) > rel*scale) {
        std::fprintf(stderr, "%s: actual %.17g expected %.17g scale %.17g\n",
                     name, actual, expected, scale);
        throw std::runtime_error(name);
    }
}
template <class Fn>
static void reject(Fn&& fn, const char* name) {
    bool thrown=false;
    try { fn(); } catch (const std::exception&) { thrown=true; }
    check(thrown,name);
}

static void flat_and_reference() {
    const auto curve = Curve::flat({0.0, 0.25, 1.0, 2.0}, 0.035);
    check(curve.stencils().K()==3, "flat stencil count");
    check(curve.stencils().open_last, "flat final wave defaults open");
    for (double t : {-0.4, 0.0, 0.001, 0.25, 0.6, 1.0, 2.0, 5.0}) {
        const double expected=std::exp(-0.035*t);
        close(curve.df(t), expected, 6e-15, "flat discount");
        close(curve.zero_rate(t), 0.035, 6e-15, "flat continuously compounded rate");
    }
    for (const auto& [a,b] : std::vector<std::pair<double,double>>{
         {0.0,0.0000001},{0.0,0.25},{0.4,1.7},{2.0,4.0}}) {
        close(curve.forward_rate(a,b), std::expm1(0.035*(b-a))/(b-a),
              5e-15, "flat simple forward");
    }
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    reject([&]{(void)curve.df(nan);}, "df must reject nan time");
    reject([&]{(void)curve.zero_rate(inf);}, "rate must reject infinity");
    reject([&]{(void)curve.forward_rate(1.0,1.0);}, "zero tenor accepted");
    reject([&]{(void)curve.forward_rate(1.0,0.5);}, "reversed tenor accepted");
    reject([&]{(void)curve.forward_rate(nan,0.5);}, "nan tenor accepted");
    reject([&]{(void)curve.df_bumped(1.0,0,0.1);}, "zero-index wave accepted");
    reject([&]{(void)curve.df_bumped(1.0,4,0.1);}, "past last wave accepted");
    reject([&]{(void)curve.df_bumped(1.0,1,nan);}, "nan shock accepted");
    reject([&]{(void)curve.df_bumped(inf,1,0.1);}, "infinite maturity accepted");
}

static void shifted_first_boundary() {
    const auto curve=Curve::flat({0.15,0.6,1.2,2.3},0.04);
    // PR #2 returned 1 whenever t<=B[0], despite constructing
    // D(B[0]) = exp(-r*B[0]). This regression forbids that jump.
    for (double t : {-0.2,0.0,0.03,0.1499999999,0.15,0.1500000001,
                     0.6,1.0,2.3,4.0})
        close(curve.df(t),std::exp(-0.04*t),7e-15,
              "shifted first boundary discontinuity");
    close(curve.zero_rate(0.0),0.04,7e-15,
          "shifted origin zero-rate");
    const std::vector<double> B{0.15,0.6,1.2,2.3};
    const std::vector<double> z{0.028,0.032,0.038,0.041};
    const auto from_rates=Curve::from_zero_rates(B,z);
    for (size_t k=0;k<B.size();++k)
        close(from_rates.df(B[k]),std::exp(-z[k]*B[k]),6e-15,
              "zero-rate node fidelity");
    close(from_rates.df(0.0),1.0,0.0,"explicit valuation origin");
    close(from_rates.zero_rate(0.0),z[0],6e-15,
          "zero-rate limit at shifted origin");

    const std::vector<double> D{
        std::exp(-0.025*B[0]),std::exp(-0.030*B[1]),
        std::exp(-0.037*B[2]),std::exp(-0.042*B[3])};
    const auto from_discounts=Curve::from_dfs(B,D);
    for (size_t k=0;k<B.size();++k)
        close(from_discounts.df(B[k]),D[k],6e-15,"discount node fidelity");
    close(from_discounts.df(0.0),1.0,0.0,
          "nonunit first node must not erase origin");
    close(from_discounts.df(B[0]/2),std::sqrt(D[0]),6e-15,
          "implicit origin log interpolation");
    close(from_discounts.zero_rate(0.0),-std::log(D[0])/B[0],
          6e-15,"discount-implied short zero rate");
    close(from_discounts.log_df(B[0]),std::log(D[0]),6e-15,
          "log DF readback");
    check(from_discounts.log_discount_nodes().size()==B.size(),
          "node representation shape");
}

static void wave_bump_and_derivative() {
    const std::vector<double> B{0.0,0.3,0.75,1.5,2.0};
    const auto open=Curve::flat(B,0.03,true);
    const auto bounded=Curve::flat(B,0.03,false);
    const int K=open.stencils().K();
    const double shock=2e-4, eps=1e-5;
    const double late=4.25;
    close(open.df_bumped(late,K,shock)/open.df(late),
          std::exp(-shock*(late-B[K-1])),2e-15,
          "open final wave extends indefinitely");
    close(bounded.df_bumped(late,K,shock)/bounded.df(late),
          std::exp(-shock*(B[K]-B[K-1])),2e-15,
          "bounded final wave saturates");
    close(open.df_bumped(-0.1,1,shock),open.df(-0.1),0.0,
          "no pre-first-wave shock");
    for (const auto* c : {&open,&bounded}) {
        for (int k=1;k<=K;++k) {
            for (double t : {-0.1,0.0,0.1,0.3,0.74,0.75,1.5,2.0,2.9,4.0}) {
                const double implied=(c->df_bumped(t,k,eps) -
                                      c->df_bumped(t,k,-eps))/(2*eps);
                const double expected=-c->df(t)*c->stencils().overlap(k,t);
                close(implied,expected,3e-8,
                      "bump derivative disagrees with Hagan overlap");
            }
        }
    }
}

static void risk_reconciliation() {
    // Independent repricing through the first-class curve versus the
    // existing scalar scan of discounted signed fixed cashflows.
    const auto C=Curve::from_zero_rates({0.,0.4,0.9,1.8,3.0},
                                      {0.025,0.029,0.032,0.036,0.041});
    const std::vector<std::pair<double,double>> book{
        {-0.2,75.0},{0.0,1200.0},{0.2,-350.0},{0.4,8500.0},
        {0.91,-12000.0},{1.8,15000.0},{2.95,25000.0},{4.2,-13000.0}};
    std::vector<UnitCashflow> pv;
    for (const auto& [t,amount] : book)
        pv.push_back({t,amount*C.df(t)});
    ladder::sort_by_time(pv);
    std::vector<double> analytical(C.stencils().K());
    ladder::scan_discount(C.stencils(),pv,analytical.data());
    const auto value=[&](int k,double delta){
        double result=0.0;
        for(const auto& [t,amount]:book)
            result+=amount*C.df_bumped(t,k,delta);
        return result;
    };
    constexpr double step=1e-5;
    for(int k=1;k<=C.stencils().K();++k)
        close((value(k,step)-value(k,-step))/(2*step),analytical[k-1],
              2e-8,"first-class curve wave bump versus scalar scan");

    // A nonlinear but fully forecast OIS uses exactly the same discount
    // curves and first-order frozen signed-weight replication as ladder.
    std::vector<UnitCashflow> ois;
    const double a=0.35,b=1.0,p=1.05,N=1e6;
    ladder::replicate_ois(N,a,b,p,[&](double t){return C.df(t);},ois);
    ladder::sort_by_time(ois);
    std::vector<double> ois_scan(C.stencils().K());
    ladder::scan_discount(C.stencils(),ois,ois_scan.data());
    const auto coupon_pv=[&](int k,double delta){
        const auto D=[&](double t){return C.df_bumped(t,k,delta);};
        return N*(D(a)/D(b)-1.0)*D(p);
    };
    for(int k=1;k<=C.stencils().K();++k)
        close((coupon_pv(k,step)-coupon_pv(k,-step))/(2*step),
              ois_scan[k-1],1e-7,
              "fully forecast OIS first order through curve bumps");
}

static void bad_inputs() {
    const double nan=std::numeric_limits<double>::quiet_NaN();
    const double inf=std::numeric_limits<double>::infinity();
    reject([&]{(void)Curve::flat({0.0},0.04);},
           "single node accepted");
    reject([&]{(void)Curve::flat({-0.1,1.0},0.04);},
           "negative reference boundary accepted");
    reject([&]{(void)Curve::flat({0.0,0.0},0.04);},
           "duplicate boundaries accepted");
    reject([&]{(void)Curve::flat({0.0,inf},0.04);},
           "infinite boundary accepted");
    reject([&]{(void)Curve::flat({0.0,1.0},nan);},
           "nonfinite rate accepted");
    reject([&]{(void)Curve::from_zero_rates({0.0,1.0},{0.05});},
           "wrong zero-rate count accepted");
    reject([&]{(void)Curve::from_zero_rates({0.0,1.0},{0.05,inf});},
           "infinite zero rate accepted");
    reject([&]{(void)Curve::from_dfs({0.0,1.0},{0.999,0.96});},
           "nonunit valuation-origin DF accepted");
    reject([&]{(void)Curve::from_dfs({0.0,1.0},{1.0,0.0});},
           "zero DF accepted");
    reject([&]{(void)Curve::from_dfs({0.0,1.0},{1.0,nan});},
           "nan DF accepted");
    reject([&]{(void)Curve::from_dfs({0.2,1.0},{0.99});},
           "wrong DF count accepted");
    // Negative rates and increasing D(t) are legitimate, not invalid.
    const auto negative=Curve::flat({0.0,0.7,1.9},-0.01);
    close(negative.df(1.2),std::exp(0.012),6e-15,
          "negative rates must work");
}
int main() try {
    flat_and_reference();
    shifted_first_boundary();
    wave_bump_and_derivative();
    risk_reconciliation();
    bad_inputs();
    std::puts("first-class ladder::Curve PASS");
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr,"FAIL test_curve: %s\n",e.what());
    return 1;
}
