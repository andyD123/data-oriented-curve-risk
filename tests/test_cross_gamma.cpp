#include "ladder/cross_gamma.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
using namespace ladder;

static void require(bool yes, const char* message) {
    if (!yes) throw std::runtime_error(message);
}
static bool close(double x, double y, double rtol = 3e-12) {
    return std::abs(x-y) <= rtol * std::max({1.0, std::abs(x), std::abs(y)});
}
static std::vector<double> first_order_oracle(const Stencils& S, const std::vector<UnitCashflow>& cf) {
    std::vector<double> g(S.K());
    for (const auto& c: cf) for (int k=1;k<=S.K();++k) g[k-1]-=c.x*S.overlap(k,c.t);
    return g;
}
static void fixed_case(const Stencils& S, std::vector<UnitCashflow> cash) {
    std::sort(cash.begin(), cash.end(), [](const auto& a, const auto& b){return a.t < b.t;});
    auto g = first_order_oracle(S,cash);
    std::vector<double> diag(S.K());
    scan_discount_gamma_diagonal(S,cash,diag.data());
    DiscountCrossGammaView H(S,g,diag);
    std::vector<double> v(S.K()), out(S.K());
    for(int i=0;i<S.K();++i)v[i]=0.15-0.02*i;
    H.multiply(v,out);
    for(int j=0;j<S.K();++j){
        double directv=0;
        for(int k=0;k<S.K();++k){
            double expected=0;
            for(auto c:cash)expected+=c.x*S.overlap(j+1,c.t)*S.overlap(k+1,c.t);
            require(close(H.at(j,k),expected),"fixed gamma mismatch");
            directv+=expected*v[k];
        }
        require(close(out[j],directv),"fixed HVP mismatch");
    }
}
static void ois_case() {
    Stencils S{{0,1,2,3,4,6}, true};
    const double a=1.2,b=2.8,p=3.25,A=1.14e6,C=1.11e6;
    OisLagGammaTerm term{a,b,p,A};
    // Frozen first-order signed records: +A@a, -A@b, +(A-C)@p.
    std::vector<UnitCashflow> frozen{{a,A},{b,-A},{p,A-C}};
    auto g=first_order_oracle(S,frozen);
    std::vector<double> diag(S.K()); scan_discount_gamma_diagonal(S,frozen,diag.data());
    DiscountCrossGammaView H(S,g,diag);
    auto exposure=[&](int j,double t){return S.overlap(j+1,t);};
    for(int j=0;j<S.K();++j)for(int k=0;k<S.K();++k){
        const double qj=exposure(j,p)+exposure(j,a)-exposure(j,b);
        const double qk=exposure(k,p)+exposure(k,a)-exposure(k,b);
        const double hj=exposure(j,p), hk=exposure(k,p);
        const double expected=A*qj*qk-C*hj*hk;
        const double actual=H.at(j,k)+ois_lag_gamma_correction(S,term,j,k);
        require(close(actual,expected),"lagged OIS gamma mismatch");
    }
    const std::vector<double> v{.3,-.2,.1,.4,.05};
    std::vector<double> hv(S.K());H.multiply(v,hv);
    add_ois_lag_gamma_product(S,std::span<const OisLagGammaTerm>(&term,1),v,hv);
    for(int j=0;j<S.K();++j){
        double direct=0;
        for(int k=0;k<S.K();++k){
            const double qj=exposure(j,p)+exposure(j,a)-exposure(j,b);
            const double qk=exposure(k,p)+exposure(k,a)-exposure(k,b);
            direct+=(A*qj*qk-C*exposure(j,p)*exposure(k,p))*v[k];
        }
        require(close(hv[j],direct),"OIS HVP mismatch");
    }
    const OisLagGammaTerm zero{a,b,b,A};
    for(int j=0;j<S.K();++j)for(int k=0;k<S.K();++k)
        require(ois_lag_gamma_correction(S,zero,j,k)==0,"zero-lag correction nonzero");
}
int main(){
    try {
        fixed_case(Stencils{{1,2,3},false},{{0.5,100},{1.5,220},{2.5,-60},{3.5,90}});
        fixed_case(Stencils{{1,2,3},true}, {{0.5,100},{1.5,220},{2.5,-60},{3.5,90}});
        fixed_case(Stencils{{0,2},false}, {{-1,100},{0,20},{2,40},{5,-10}});
        fixed_case(Stencils{{0,2},true},  {{-1,100},{0,20},{2,40},{5,-10}});
        std::mt19937_64 rng(20261010);
        std::uniform_real_distribution<double> time(-1.,35.), amount(-200.,200.);
        std::vector<UnitCashflow> many;
        for(int i=0;i<4000;++i)many.push_back({time(rng),amount(rng)});
        std::vector<double> B;
        for(int i=0;i<=15;++i)B.push_back(0.5*i);
        fixed_case(Stencils{B,false},many);
        fixed_case(Stencils{B,true},many);
        ois_case();
        std::puts("cross-gamma: fixed + open/capped + lagged OIS + HVP PASS");
        return 0;
    }catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
