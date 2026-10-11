// Research-only: 100,000 fixed discounted cashflows, 66 forward-wave buckets.
// Build: g++ -std=c++20 -O3 -march=native -DNDEBUG -I. \
//   examples/benchmark/bench_cross_gamma_research.cpp -o bench_gamma
<<<<<<< HEAD
// Not the 500k-instrument paper benchmark.
=======
// Not a QuantLib test and not the 500k-instrument paper benchmark.
>>>>>>> origin/main
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
<<<<<<< HEAD

// Second-order AAD baseline: a generic scalar tape, forward-over-reverse Hessian (K Hessian-vector products).
// Deliberately generous to the comparator: only non-zero overlaps are recorded, the tape is recorded once and reused
// for every direction, and no allocation happens in the timed loops. It is a plain tape, not a tuned AAD library.
struct AadNode { int op, a, b; double c, v; };       // op: 0 input, 1 scale a by c, 2 add a+b, 3 exp(a)
struct AadTape {
    std::vector<AadNode> n; std::vector<double> dot, adj, adjdot;
    int input(double v){ n.push_back({0,-1,-1,0,v}); return (int)n.size()-1; }
    int scale(int a,double c){ n.push_back({1,a,-1,c,c*n[a].v}); return (int)n.size()-1; }
    int add(int a,int b){ n.push_back({2,a,b,0,n[a].v+n[b].v}); return (int)n.size()-1; }
    int expo(int a){ n.push_back({3,a,-1,0,std::exp(n[a].v)}); return (int)n.size()-1; }
};
// Column j of the Hessian of PV w.r.t. the K inputs (nodes 0..K-1): forward tangent along e_j, then reverse with tangent
static void aad_hvp(AadTape& t,int root,int j,double* column){
    const size_t m=t.n.size();
    t.dot.assign(m,0.0); t.adj.assign(m,0.0); t.adjdot.assign(m,0.0);
    t.dot[j]=1.0;
    for(size_t i=0;i<m;++i){ const AadNode& x=t.n[i];
        switch(x.op){ case 1: t.dot[i]=x.c*t.dot[x.a]; break; case 2: t.dot[i]=t.dot[x.a]+t.dot[x.b]; break;
                      case 3: t.dot[i]=x.v*t.dot[x.a]; break; default: break; } }
    t.adj[root]=1.0;
    for(size_t i=m;i-->0;){ const AadNode& x=t.n[i]; const double a=t.adj[i], ad=t.adjdot[i];
        switch(x.op){
            case 1: t.adj[x.a]+=x.c*a; t.adjdot[x.a]+=x.c*ad; break;
            case 2: t.adj[x.a]+=a; t.adj[x.b]+=a; t.adjdot[x.a]+=ad; t.adjdot[x.b]+=ad; break;
            case 3: t.adj[x.a]+=x.v*a; t.adjdot[x.a]+=t.dot[i]*a+x.v*ad; break;
            default: break; } }
    for(int k=0;k<K;++k) column[k]=t.adjdot[k];
}
=======
>>>>>>> origin/main
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
<<<<<<< HEAD
    // second-order AAD: record once, then K forward-over-reverse passes
    AadTape tape; tape.n.reserve(8000000);
    const auto aad_start=std::chrono::steady_clock::now();
    for(int k=0;k<K;++k) tape.input(0.0);
    int total=-1;
    for(const auto& c:cf){
        int acc=-1;
        for(int k=0;k<K;++k){ const double w=S.overlap(k+1,c.t); if(w==0) continue;
            const int term=tape.scale(k,-w); acc=acc<0?term:tape.add(acc,term); }
        int y=acc<0?tape.input(c.x):tape.scale(tape.expo(acc),c.x);
        total=total<0?y:tape.add(total,y);
    }
    const auto aad_recorded=std::chrono::steady_clock::now();
    std::vector<double> Haad(K*K),col(K);
    for(int j=0;j<K;++j){ aad_hvp(tape,total,j,col.data()); for(int k=0;k<K;++k) Haad[k*K+j]=col[k]; }
    const auto aad_done=std::chrono::steady_clock::now();
    const double aad_record_ms=std::chrono::duration<double,std::milli>(aad_recorded-aad_start).count();
    const double aad_hvp_ms=std::chrono::duration<double,std::milli>(aad_done-aad_recorded).count();
    double aad_rel=0;
    for(int i=0;i<K*K;++i)aad_rel=std::max(aad_rel,std::abs(H[i]-Haad[i])/std::max({1.,std::abs(H[i]),std::abs(Haad[i])}));
    sink=sink+Haad[35*K+39]*1e-11;
=======
>>>>>>> origin/main
    std::printf("N=%d K=%d seed=20261010 repricings=8713\n",N,K);
    std::printf("structured_ms=%.6f direct_analytic_ms=%.6f bump_precompute_ms=%.6f bump_reprice_ms=%.6f\n",
      fast_ms,analytic_ms,pre_ms,fd_ms);
    std::printf("analytic_speedup=%.2fx bump_speedup=%.2fx relative_error_analytic=%.9g relative_error_fd=%.9g witness=%.9g\n",
      analytic_ms/fast_ms,(pre_ms+fd_ms)/fast_ms,max_rel,fd_rel,sink);
<<<<<<< HEAD
    std::printf("aad_tape_nodes=%zu aad_record_ms=%.3f aad_hessian_ms=%.3f (%d forward-over-reverse passes) aad_total_ms=%.3f aad_vs_structured=%.0fx relative_error_aad=%.3g\n",
      tape.n.size(),aad_record_ms,aad_hvp_ms,K,aad_record_ms+aad_hvp_ms,(aad_record_ms+aad_hvp_ms)/fast_ms,aad_rel);
=======
>>>>>>> origin/main
}
