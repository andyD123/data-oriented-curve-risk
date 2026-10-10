// Research-only: 100,000 fixed discounted cashflows, 66 forward-wave buckets.
// Build: g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//   examples/benchmark/bench_cross_gamma_research.cpp -o bench_gamma
// Not a QuantLib test and not the 500k-instrument paper benchmark.
#include "ladder/cross_gamma.hpp"
#include "ladder/scan.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>
using namespace ladder;
constexpr int N=100000,K=66;
static volatile double sink=0;
static double median(int repeat,std::function<void()> fn){
    std::vector<double> t;
    for(int i=0;i<repeat;++i){auto a=std::chrono::steady_clock::now();fn();
        auto b=std::chrono::steady_clock::now();
        t.push_back(std::chrono::duration<double,std::milli>(b-a).count());}
    std::sort(t.begin(),t.end());return t[t.size()/2];
}
static void direct(const Stencils& S,const std::vector<UnitCashflow>& cf,
                   std::vector<double>& H){
    std::fill(H.begin(),H.end(),0);
    std::array<double,K> w{};
    for(const auto& c:cf){
        for(int j=0;j<K;++j)w[j]=S.overlap(j+1,c.t);
        for(int j=0;j<K;++j){
            if(w[j]==0)continue;
            const double xw=c.x*w[j];
            for(int k=j;k<K;++k)H[j*K+k]+=xw*w[k];
        }
    }
    for(int j=0;j<K;++j)for(int k=j+1;k<K;++k)H[k*K+j]=H[j*K+k];
}
int main(){
    std::vector<double> B(K+1);
    for(int j=0;j<=K;++j)B[j]=j;
    const Stencils S{B,true};
    std::mt19937_64 rng(20261010);
    std::uniform_real_distribution<double> times(-1,68),amounts(-1000,1000);
    std::vector<UnitCashflow> cf;cf.reserve(N);
    for(int i=0;i<N;++i)cf.push_back({times(rng),amounts(rng)});
    std::sort(cf.begin(),cf.end(),[](auto a,auto b){return a.t<b.t;});
    std::vector<double> g(K),d(K),H(K*K),reference(K*K),fd(K*K);
    const auto fast=[&]{
        scan_discount(S,cf,g.data());
        scan_discount_gamma_diagonal(S,cf,d.data());
        const DiscountCrossGammaView view(S,g,d);
        for(int j=0;j<K;++j)for(int k=0;k<K;++k)H[j*K+k]=view.at(j,k);
        sink=sink+H[35*K+39]*1e-11;
    };
    const auto analytic=[&]{direct(S,cf,reference);sink=sink+reference[35*K+39]*1e-11;};
    fast();analytic();
    double max_rel=0;
    for(int i=0;i<K*K;++i)max_rel=std::max(max_rel,
      std::abs(H[i]-reference[i])/std::max({1.,std::abs(H[i]),std::abs(reference[i])}));
    const double fast_ms=median(17,fast);
    const double analytic_ms=median(3,analytic);
    const auto start=std::chrono::steady_clock::now();
    std::vector<double> overlap((size_t)K*N);
    for(int j=0;j<K;++j)for(int i=0;i<N;++i)
        overlap[(size_t)j*N+i]=S.overlap(j+1,cf[i].t);
    const auto precomputed=std::chrono::steady_clock::now();
    const auto pv=[&](int j,int k,double dj,double dk){
        const double* a=overlap.data()+(size_t)j*N;
        const double* b=overlap.data()+(size_t)k*N;
        double result=0;
        for(int i=0;i<N;++i)result+=cf[i].x*std::exp(-dj*a[i]-dk*b[i]);
        return result;
    };
    constexpr double eps=.001;
    const double base=pv(0,0,0,0);
    for(int j=0;j<K;++j)
        fd[j*K+j]=(pv(j,j,eps,0)+pv(j,j,-eps,0)-2*base)/(eps*eps);
    for(int j=0;j<K;++j)for(int k=j+1;k<K;++k){
        const double pp=pv(j,k,eps,eps),pm=pv(j,k,eps,-eps);
        const double mp=pv(j,k,-eps,eps),mm=pv(j,k,-eps,-eps);
        fd[j*K+k]=fd[k*K+j]=(pp-pm-mp+mm)/(4*eps*eps);
    }
    const auto finished=std::chrono::steady_clock::now();
    const double pre_ms=std::chrono::duration<double,std::milli>(precomputed-start).count();
    const double fd_ms=std::chrono::duration<double,std::milli>(finished-precomputed).count();
    double fd_rel=0;
    for(int i=0;i<K*K;++i)fd_rel=std::max(fd_rel,
      std::abs(H[i]-fd[i])/std::max({1.,std::abs(H[i]),std::abs(fd[i])}));
    sink=sink+fd[35*K+39]*1e-11;
    std::printf("N=%d K=%d seed=20261010 repricings=8713\n",N,K);
    std::printf("structured_ms=%.6f direct_analytic_ms=%.6f bump_precompute_ms=%.6f bump_reprice_ms=%.6f\n",
      fast_ms,analytic_ms,pre_ms,fd_ms);
    std::printf("analytic_speedup=%.2fx bump_speedup=%.2fx relative_error_analytic=%.9g relative_error_fd=%.9g witness=%.9g\n",
      analytic_ms/fast_ms,(pre_ms+fd_ms)/fast_ms,max_rel,fd_rel,sink);
}
