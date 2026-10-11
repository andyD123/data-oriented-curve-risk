// Recorded QuantLib 1.33 historical five-year OIS, two-business-day payment lag.
// QuantLib archive validates FIRST ORDER wave sensitivities; the second-order
// test reprices the exact exported OIS formula, not a fresh QuantLib engine.
#include "ladder/cross_gamma.hpp"
#include "ladder/scan.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace ladder;
struct Fixed {double t,pv;};
struct Coupon {double a,b,p,A,C;};

static const double boundaries[] = {0.0, 0.0027397260273972603, 0.005479452054794521, 0.00821917808219178, 0.024657534246575342, 0.043835616438356165, 0.06301369863013699, 0.09315068493150686, 0.17534246575342466, 0.25205479452054796, 0.3287671232876712, 0.4054794520547945, 0.5013698630136987, 1.252054794520548, 1.5041095890410958, 1.7616438356164383, 2.010958904109589, 3.0082191780821916, 4.008219178082192, 5.008219178082192, 6.008219178082192, 7.008219178082192, 8.013698630136986, 9.01095890410959, 10.01095890410959, 11.01095890410959, 12.013698630136986, 15.013698630136986, 20.019178082191782, 25.024657534246575, 30.03013698630137};
static const double quantlib_delta[] = {6.068715738365427, 6.068715738365427, 2745.7908810902154, 16474.745268933475, 19220.536145730875, 19220.536145730875, 30203.699663470616, 82373.72634917847, 76882.14459132723, 76882.14459132723, 76882.14459132723, 96102.68073931366, 751149.0408754252, 251332.09460811486, 256795.83579047176, 248600.22400913294, 989332.2251819699, 986974.967755159, 981913.1866272801, -37.922105366305914, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
static const Fixed fixed[] = {
{1.0164383561643835,-5069.25788916147},
{2.0164383561643837,-5093.425987791804},
{3.0136986301369864,-5035.905680174101},
{4.013698630136986,-5013.033025585025},
{5.013698630136986,-4952.708044132392}
};
static const Coupon coupons[] = {
{0.005479452054794521,1.0054794520547945,1.0164383561643835,999998.5901465433,999963.2000537884},
{1.0054794520547945,2.010958904109589,2.0164383561643837,999951.5539691554,999255.2346621354},
{2.010958904109589,3.0082191780821916,3.0136986301369864,999241.9045531526,996113.211463002},
{3.0082191780821916,4.008219178082192,4.013698630136986,996087.6234916969,988872.2680606262},
{4.008219178082192,5.008219178082192,5.013698630136986,988846.0379583295,976972.5456918876}
};

static void check(bool yes,const char* msg){if(!yes)throw std::runtime_error(msg);}
static bool near(double x,double y,double a=1e-5,double r=2e-11){
    return std::abs(x-y)<=a+r*std::max(std::abs(x),std::abs(y));
}
int main() try {
    Stencils S{{std::begin(boundaries),std::end(boundaries)},true};
    const int K=S.K();check(K==30,"expected 30 wave buckets");
    std::vector<UnitCashflow> frozen;
    for(const auto& c:fixed)frozen.push_back({c.t,c.pv});
    for(const auto& c:coupons){
        frozen.push_back({c.a,c.A});
        frozen.push_back({c.b,-c.A});
        frozen.push_back({c.p,c.A-c.C});
    }
    sort_by_time(frozen);
    std::vector<double> g(K),diagonal(K);
    scan_discount(S,frozen,g.data());
    scan_discount_gamma_diagonal(S,frozen,diagonal.data());
    DiscountCrossGammaView H(S,g,diagonal);
    double peak=0,worst=0;
    for(int j=0;j<K;++j){
        peak=std::max(peak,std::abs(g[j]));
        worst=std::max(worst,std::abs(g[j]-quantlib_delta[j]));
    }
    std::printf("archived QuantLib first-order relative error %.9g\n",worst/peak);
    check(worst/peak < 5e-10,"recorded QuantLib delta mismatch");
    auto price=[&](std::span<const double> shock){
        auto exposure=[&](double t){double z=0;for(int j=0;j<K;++j)z+=shock[j]*S.overlap(j+1,t);return z;};
        double pv=0;
        for(const auto& c:fixed)pv+=c.pv*std::exp(-exposure(c.t));
        for(const auto& c:coupons)
            pv+=c.A*std::exp(-exposure(c.p)-exposure(c.a)+exposure(c.b))-c.C*std::exp(-exposure(c.p));
        return pv;
    };
    const std::array<std::array<int,2>,4> pairs{{{{17,18}},{{18,19}},{{19,20}},{{12,13}}}};
    for(const auto& pair:pairs){
        const int j=pair[0]-1,k=pair[1]-1;
        double analytic=H.at(j,k),direct=0;
        for(const auto& c:fixed)direct+=c.pv*S.overlap(j+1,c.t)*S.overlap(k+1,c.t);
        for(const auto& c:coupons){
            analytic+=ois_lag_gamma_correction(S,{c.a,c.b,c.p,c.A},j,k);
            const double qj=S.overlap(j+1,c.p)+S.overlap(j+1,c.a)-S.overlap(j+1,c.b);
            const double qk=S.overlap(k+1,c.p)+S.overlap(k+1,c.a)-S.overlap(k+1,c.b);
            direct+=c.A*qj*qk-c.C*S.overlap(j+1,c.p)*S.overlap(k+1,c.p);
        }
        check(near(analytic,direct,2e-7,3e-12),"reconstructed product gamma mismatch");
        auto fd=[&](double e){
            std::vector<double> sh(K,0);
            sh[j]=e;sh[k]=e;double pp=price(sh);
            sh[k]=-e;double pm=price(sh);
            sh[j]=-e;sh[k]=e;double mp=price(sh);
            sh[k]=-e;double mm=price(sh);
            return (pp-pm-mp+mm)/(4*e*e);
        };
        const double e4=std::abs(fd(.004)-direct),e2=std::abs(fd(.002)-direct);
        check(e2 < .37*e4+1e-5,"central mixed finite difference does not converge");
        std::printf("buckets %d,%d frozen % .4f corrected % .4f\n",j+1,k+1,H.at(j,k),analytic);
    }
    std::vector<double> v(K),hv(K),ref(K);
    for(int j=0;j<K;++j)v[j]=.007*(j%7-3);
    H.multiply(v,hv);
    std::vector<OisLagGammaTerm> terms;
    for(const auto& c:coupons)terms.push_back({c.a,c.b,c.p,c.A});
    add_ois_lag_gamma_product(S,terms,v,hv);
    for(int j=0;j<K;++j){
        for(int k=0;k<K;++k){
            double trueGamma=0;
            for(const auto& c:fixed)
                trueGamma+=c.pv*S.overlap(j+1,c.t)*S.overlap(k+1,c.t);
            for(const auto& c:coupons){
                double qj=S.overlap(j+1,c.p)+S.overlap(j+1,c.a)-S.overlap(j+1,c.b);
                double qk=S.overlap(k+1,c.p)+S.overlap(k+1,c.a)-S.overlap(k+1,c.b);
                trueGamma+=c.A*qj*qk-c.C*S.overlap(j+1,c.p)*S.overlap(k+1,c.p);
            }
            ref[j]+=trueGamma*v[k];
        }
        check(near(hv[j],ref[j],2e-7,3e-12),"archived Hessian-vector product mismatch");
    }
    std::puts("archived QuantLib OIS, reconstructed gamma, Hessian-vector PASS");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL %s\n",e.what());return 1;}
