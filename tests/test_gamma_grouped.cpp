// Eight-lane Stage-2 gamma versus scalar and independent product-rule oracles.
// Include OIS payment lag, IBOR projection-held-fixed, open/capped final waves.
#include "ladder/aligned_memory.hpp"
#include "ladder/scan_gamma_grouped.hpp"
#include "ladder/cross_gamma.hpp"
#include "ladder/replicate.hpp"
#include "ladder/scan.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>
using namespace ladder;
static void check(bool b,const char* msg){if(!b)throw std::runtime_error(msg);}
static bool close(double a,double b,double abs=1e-7,double rel=2e-10){
    return std::abs(a-b)<=abs+rel*std::max({1.,std::abs(a),std::abs(b)});
}
static void test(int seed,bool open){
    constexpr double DPY=365.;
    Stencils Sd{{.10,.25,.5,1.,1.5,2.,2.5,3.,3.5,4.,4.5,5.,6.,7.},open};
    Stencils Sp{{0.,.5,1.,1.5,2.,2.5,3.,3.5,4.,4.5,5.,5.5,6.,7.,8.},true};
    auto df=[](double t){return std::exp(-.027*t-.00025*t*t);};
    auto pf=[](double t){return std::exp(-.034*t-.00014*t*t);};
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> days(15,3500),step(4,190);
    std::uniform_real_distribution<double> notional(-2e6,2e6),fixed_amount(-3e5,3e5);
    std::vector<InstrumentSpec> book(263);
    for(size_t i=0;i<book.size();++i){
        auto& x=book[i];x.signature=static_cast<int>(i%11);
        for(int j=0;j<6;++j)x.fixed.push_back({days(rng),fixed_amount(rng)});
        for(int j=0;j<3;++j){
            int a=days(rng),b=a+step(rng);
            x.flt.push_back({b,a,b,notional(rng)});
        }
        for(int j=0;j<3;++j){
            int a=days(rng),b=a+step(rng),p=b+(j==0?0:step(rng));
            x.ois.push_back({p,a,b,notional(rng)});
        }
        if(i%7==0){x.fixed.push_back({5,fixed_amount(rng)});x.ois.push_back({2890,2000,2600,notional(rng)});}
        if(i%9==0){x.fixed.push_back({3900,fixed_amount(rng)});x.ois.push_back({4100,3850,3900,notional(rng)});}
    }
    GroupLayout L=build_layout(book,DPY);
    refresh_table(L,Sd,Sp,df,pf);
    const size_t entries=L.groups.size()*grouped_gamma_stride(Sd.K());
    double* out=(double*)allocate_aligned(entries*sizeof(double));
    double* streaming=(double*)allocate_aligned(entries*sizeof(double));
    GroupGammaScratch scratch;
    for(int repeat=0;repeat<3;++repeat)
        scan_grouped_gamma_diagonal<Store::normal>(L,Sd,out,scratch);
    scan_grouped_gamma_diagonal<Store::streaming>(L,Sd,streaming,scratch);
    std::vector<double> portfolio(Sd.K()),sum(Sd.K(),0.);
    scan_grouped_gamma_portfolio_diagonal(L,Sd,portfolio.data(),scratch);
    GammaDiagonalScratch scalar_scratch; // reused across all instruments
    double worst=0.;int checked=0;
    for(size_t gi=0;gi<L.groups.size();++gi){
        const Group& G=L.groups[gi];
        for(int lane=0;lane<LANES;++lane){
            if(lane>=G.n_valid){
                for(int k=0;k<Sd.K();++k){
                    const size_t slot=gi*grouped_gamma_stride(Sd.K())+(size_t)k*LANES+lane;
                    check(out[slot]==0.,"invalid/padded lane not zero");
                }
                continue;
            }
            const auto& spec=book[G.inst[lane]];
            std::vector<UnitCashflow> cash;
            for(const auto& f:spec.fixed)replicate_fixed(f.amount,f.day/DPY,df,cash);
            for(const auto& f:spec.flt){
                std::vector<ProjectionTerm> projections;
                replicate_ibor(f.scale,f.a_day/DPY,f.b_day/DPY,f.pay_day/DPY,
                               df,pf,cash,projections);
            }
            for(const auto& o:spec.ois)
                replicate_ois(o.N,o.a_day/DPY,o.b_day/DPY,o.pay_day/DPY,df,cash);
            sort_by_time(cash);
            std::vector<double> scalar(Sd.K());
            scan_discount_gamma_diagonal(Sd,cash,scalar_scratch,scalar.data());
            for(int k=0;k<Sd.K();++k){
                const size_t slot=gi*grouped_gamma_stride(Sd.K())+(size_t)k*LANES+lane;
                const double got=out[slot];
                check(got==streaming[slot],"normal vs non-temporal output mismatch");
                double direct=0.,corrected=scalar[k];
                for(const auto& f:spec.fixed){
                    const double t=f.day/DPY,h=Sd.overlap(k+1,t);
                    direct+=f.amount*df(t)*h*h;
                }
                for(const auto& f:spec.flt){
                    const double t=f.pay_day/DPY,h=Sd.overlap(k+1,t);
                    direct+=f.scale*(pf(f.a_day/DPY)/pf(f.b_day/DPY)-1.)*df(t)*h*h;
                }
                for(const auto& o:spec.ois){
                    const double a=o.a_day/DPY,b=o.b_day/DPY,p=o.pay_day/DPY;
                    const double A=o.N*df(p)*df(a)/df(b),C=o.N*df(p);
                    const double ha=Sd.overlap(k+1,a),hb=Sd.overlap(k+1,b),hp=Sd.overlap(k+1,p);
                    const double r=hb-ha,s=hp-hb,q=hp+ha-hb;
                    corrected-=2*A*r*s;
                    direct+=A*q*q-C*hp*hp;
                }
                check(close(got,corrected),"corrected grouped vs scalar diagonal");
                check(close(got,direct),"original OIS/IBOR product-rule Hessian");
                worst=std::max(worst,std::abs(got-direct)/std::max({1.,std::abs(got),std::abs(direct)}));
                sum[k]+=direct;
                ++checked;
            }
        }
    }
    for(int k=0;k<Sd.K();++k)
        check(close(portfolio[k],sum[k],5e-5,5e-10),"portfolio reduced gamma");
    free_aligned(out);free_aligned(streaming);
    Stencils wrong=Sd;wrong.B[2]+=.01;
    bool caught=false;
    try{scan_grouped_gamma_diagonal(L,wrong,(double*)0x1,scratch);}
    catch(const std::invalid_argument&){caught=true;}
    check(caught,"stale discount grid was accepted");
    std::printf("grouped gamma: open=%d instruments=%zu groups=%zu checked=%d worst relative=%.3g PASS\n",
                open,book.size(),L.groups.size(),checked,worst);
}
int main()try{
    test(20261011,false);test(20261011,true);
    std::puts("PASS grouped gamma: PORTABLE, AVX2 or AVX512");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
