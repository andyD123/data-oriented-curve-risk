// Reproducible 16-bucket Hagan-wave cross-gamma example.
// All values are currency PV derivatives per unit instantaneous-forward shift.
// This is an explanatory example, not a speed/QuantLib benchmark.
#include "ladder/cross_gamma.hpp"
#include "ladder/scan.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace ladder;
struct OisCoupon { double a,b,p,A,C; };
constexpr double flat_rate=0.0325;

double discount(double t) { return std::exp(-flat_rate*t); }
OisCoupon make_ois(double a,double b,double p,double signed_notional) {
    return {a,b,p,signed_notional*discount(p)*discount(a)/discount(b),
                  signed_notional*discount(p)};
}

double independent_gamma(const Stencils& S,
                         const std::vector<UnitCashflow>& fixed,
                         const std::vector<OisCoupon>& ois,
                         int j,int k) {
    const auto h=[&](int wave,double t){return S.overlap(wave+1,t);};
    double result=0.0;
    for(const auto& c:fixed) result+=c.x*h(j,c.t)*h(k,c.t);
    for(const auto& c:ois) {
        const double qj=h(j,c.p)+h(j,c.a)-h(j,c.b);
        const double qk=h(k,c.p)+h(k,c.a)-h(k,c.b);
        result+=c.A*qj*qk-c.C*h(j,c.p)*h(k,c.p);
    }
    return result;
}

double reprice(const Stencils& S,
               const std::vector<UnitCashflow>& fixed,
               const std::vector<OisCoupon>& ois,
               const std::vector<double>& shock) {
    const auto exposure=[&](double t){
        double z=0.0;
        for(int k=0;k<S.K();++k) z+=shock[k]*S.overlap(k+1,t);
        return z;
    };
    double pv=0.0;
    for(const auto& c:fixed)pv+=c.x*std::exp(-exposure(c.t));
    for(const auto& c:ois)
        pv+=c.A*std::exp(-exposure(c.p)-exposure(c.a)+exposure(c.b))
           -c.C*std::exp(-exposure(c.p));
    return pv;
}

bool approximately(double actual,double expected,double relative_tolerance) {
    return std::abs(actual-expected)<=relative_tolerance*
        std::max({1.0,std::abs(actual),std::abs(expected)});
}
} // namespace

int main() try {
    // Irregular maturities, deliberately more than a toy 3-bucket example.
    const Stencils S{{0,.25,.5,1,2,3,4,5,7,10,12,15,20,25,30,35,40},true};
    S.validate();
    const int K=S.K();

    // Small coupon ladder plus a long-dated principal beyond B[K]=40y.
    // x = known amount * D(t), including the sign of the payment.
    std::vector<UnitCashflow> fixed;
    for(const auto& [t,amount] : std::vector<std::pair<double,double>>{
        {.9,12500},{2.25,19000},{5.1,26000},{8.75,30000},
        {11.75,35000},{14.8,40000},{17.25,45000},
        {25.1,50000},{38.0,55000},{43.5,1150000}})
        fixed.push_back({t,amount*discount(t)});

    // One fully forecast OIS coupon: 12.25->14.90y, paid at 15.25y.
    // Its payment LAG crosses the 15y wave boundary: an off-diagonal
    // gamma correction appears between wave 11 [12,15) and 12 [15,20).
    // A second, zero-lag OIS coupon shows the correction vanishes for p=b.
    const std::vector<OisCoupon> ois{
        make_ois(12.25,14.90,15.25,1000000.0),
        make_ois(6.4,7.6,7.6,700000.0)
    };

    // The existing first-order scan sees frozen signed dated PV weights.
    std::vector<UnitCashflow> frozen=fixed;
    std::vector<OisLagGammaTerm> lag_terms;
    for(const auto& c:ois) {
        frozen.push_back({c.a,c.A});
        frozen.push_back({c.b,-c.A});
        frozen.push_back({c.p,c.A-c.C});
        lag_terms.push_back({c.a,c.b,c.p,c.A});
    }
    sort_by_time(frozen);
    std::vector<double> g(K),diag(K);
    scan_discount(S,frozen,g.data());
    // The OPTIONAL second-order pass computes only the K diagonal moments.
    scan_discount_gamma_diagonal(S,frozen,diag.data());
    const DiscountCrossGammaView frozen_gamma(S,g,diag);
    const auto true_gamma=[&](int j,int k){
        double v=frozen_gamma.at(j,k);
        for(const auto& c:lag_terms)v+=ois_lag_gamma_correction(S,c,j,k);
        return v;
    };

    std::printf("16-bucket Hagan cross-gamma example; two OIS coupons; open final wave\n");
    std::printf("bucket   interval(years)  first dPV/dRate    diagonal gamma (corrected)\n");
    for(int k=0;k<K;++k){
        std::printf("%3d  [%6.2f,%6.2f)%15.3f%22.3f%s\n",
            k+1,S.B[k],S.B[k+1],g[k],true_gamma(k,k),
            (k==K-1?"  [open beyond 40y]":""));
    }

    std::puts("\nSelected CROSS-gamma pairs (different buckets):");
    std::puts("waves     frozen Hjk     lag correction      correct Hjk");
    for(const auto& [a,b]:std::array<std::pair<int,int>,6>{{{2,8},{7,11},{10,11},{11,12},{11,16},{12,16}}}) {
        const int j=a-1,k=b-1;
        std::printf("%2d,%2d %16.3f %17.3f %17.3f\n",
                    a,b,frozen_gamma.at(j,k),true_gamma(j,k)-frozen_gamma.at(j,k),true_gamma(j,k));
    }

    // Full 16x16 direct differentiation is an independent correctness oracle.
    double max_relative=0.0;
    for(int j=0;j<K;++j)for(int k=0;k<K;++k){
        const double expected=independent_gamma(S,fixed,ois,j,k);
        const double actual=true_gamma(j,k);
        max_relative=std::max(max_relative,
            std::abs(actual-expected)/std::max({1.0,std::abs(actual),std::abs(expected)}));
    }

    std::vector<double> v(K),hv(K),direct_hv(K,0.0);
    for(int k=0;k<K;++k)v[k]=(k%3==0?-.002:.003);
    frozen_gamma.multiply(v,hv);
    add_ois_lag_gamma_product(S,lag_terms,v,hv);
    double max_hvp_relative=0.0,quadratic=0.0;
    for(int j=0;j<K;++j){
        for(int k=0;k<K;++k)direct_hv[j]+=independent_gamma(S,fixed,ois,j,k)*v[k];
        max_hvp_relative=std::max(max_hvp_relative,
            std::abs(hv[j]-direct_hv[j])/
                std::max({1.0,std::abs(hv[j]),std::abs(direct_hv[j])}));
        quadratic+=.5*v[j]*hv[j];
    }

    // The monetary significance of cross gamma: isolate a two-wave simultaneous
    // +50bp shift and subtract the two individual single-wave effects.
    std::vector<double> shock(K,0.0);
    const int j=10,k=11; // 0-index waves 11 and 12, across the payment boundary
    const double bump=.005;
    const double pv0=reprice(S,fixed,ois,shock);
    shock[j]=bump;const double pvj=reprice(S,fixed,ois,shock);
    shock[j]=0;shock[k]=bump;const double pvk=reprice(S,fixed,ois,shock);
    shock[j]=bump;const double pvjoint=reprice(S,fixed,ois,shock);
    const double observed_cross=pvjoint-pvj-pvk+pv0;
    const double quadratic_cross=true_gamma(j,k)*bump*bump;
    std::printf("\nWave 11+12 simultaneous +50bp scenario:\n");
    std::printf("  pure cross-PV effect (repriced): % .8f\n",observed_cross);
    std::printf("  H[11,12]*0.005^2 approximation: % .8f\n",quadratic_cross);
    std::printf("  omitted-lag gamma would predict: % .8f\n",frozen_gamma.at(j,k)*bump*bump);
    std::printf("\nSecond-order portfolio scenario 0.5*v^T*H*v: % .8f\n",quadratic);
    std::printf("Worst relative direct Hessian error: %.3g\n",max_relative);
    std::printf("Worst relative matrix-free HVP error: %.3g\n",max_hvp_relative);

    if(!approximately(max_relative,0,1e-10)||
       !approximately(max_hvp_relative,0,1e-10)||
       !approximately(observed_cross,quadratic_cross,0.06))
        throw std::runtime_error("cross-gamma demonstration correctness failure");
    std::puts("PASS: reduced-form cross-gamma, diagonal, lag and HVP");
    return 0;
}catch(const std::exception& e){
    std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;
}
