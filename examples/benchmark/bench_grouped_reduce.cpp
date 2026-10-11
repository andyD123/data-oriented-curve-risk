// First-order portfolio-only scan reduction, adapted from PR #2 by damian123.
// Same seeded mixed bond/IBOR/OIS workload and curve fixtures as bench.cpp.
// No change to first-order maths; measures the savings from not writing the
// entire per-instrument risk matrix to DRAM.
#include "ladder/aligned_memory.hpp"
#include "ladder/scan_grouped_reduce.hpp"
#include "ladder/replicate.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <string_view>
#include <vector>
using namespace ladder;
constexpr double DPY=365.0;
struct Curve {
    Stencils S; std::vector<double> logD;
    double df(double t)const{
        int k=S.bucket(t);double a=(t-S.B[k-1])/S.len(k);
        return std::exp(logD[k-1]+a*(logD[k]-logD[k-1]));
    }
};
static void load_curves(const char* path,Curve& d,Curve& p){
    std::ifstream in(path);int nc;
    if(!in||!(in>>nc)||nc!=2)throw std::runtime_error("cannot read two curves");
    for(Curve* c:{&d,&p}){
        std::string name;int n;
        if(!(in>>name>>n)||n<2)throw std::runtime_error("bad curve header");
        c->S.B.resize(n);c->logD.resize(n);
        for(int i=0;i<n;++i){
            double t,D;if(!(in>>t>>D)||!(D>0))throw std::runtime_error("bad discount factor");
            c->S.B[i]=t;c->logD[i]=std::log(D);
        }
        c->S.validate();
    }
}
static double now_ms(){
    return std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static size_t parse(const char* s,size_t lo,size_t hi){
    size_t v;std::string_view t(s);
    auto [end,error]=std::from_chars(t.data(),t.data()+t.size(),v);
    if(error!=std::errc{}||end!=t.data()+t.size()||v<lo||v>hi)
        throw std::invalid_argument("invalid instrument count or repetitions");
    return v;
}
static double median(std::vector<double> t){
    std::sort(t.begin(),t.end());return t[t.size()/2];
}
int main(int argc,char** argv)try {
    const size_t N=argc>1?parse(argv[1],1,1000000):500000;
    const int reps=static_cast<int>(argc>2?parse(argv[2],1,31):5);
    Curve cd,cp;
    load_curves(argc>3?argv[3]:"examples/benchmark/quantlib_example_curves.txt",cd,cp);
    const int Kd=cd.S.K(),Kp=cp.S.K();
    auto df=[&](double t){return cd.df(t);};
    auto pf=[&](double t){return cp.df(t);};
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> mat_off(2,61),years_d(1,29),type_d(0,2);
    std::uniform_real_distribution<double> rate_d(.01,.06);
    const double notionals[]={1e6,2.5e6,5e6,1e7};
    const double setup_start=now_ms();
    std::vector<InstrumentSpec> book(N);
    // Copy of the production benchmark's draw/order/signature and schedule
    // construction. N=500000, seed=42 produces 64750 AoSoA groups here.
    for(size_t i=0;i<N;++i){
        const int y=years_d(rng),ty=type_d(rng),rem=1+(int)(rng()%y);
        const int maturity=mat_off(rng)+(int)std::lround(y*365.25);
        const double nom=notionals[rng()%4],rate=rate_d(rng);
        const auto day=[&](int j,int periods){
            return maturity-(int)std::lround(j*365.25/periods);
        };
        auto& in=book[i];in.signature=ty*100000+maturity;
        if(ty==0){
            for(int j=2*rem-1;j>=0;--j)
                in.fixed.push_back({day(j,2),nom*rate/2+(j==0?nom:0.)});
        }else{
            for(int j=rem-1;j>=0;--j)
                in.fixed.push_back({day(j,1)+(ty==2?2:0),-nom*rate});
            if(ty==1){
                for(int j=2*rem-1;j>=0;--j)
                    in.flt.push_back({day(j,2),day(j+1,2),day(j,2),nom});
            }else{
                for(int j=rem-1;j>=0;--j)
                    in.ois.push_back({day(j,1)+2,day(j+1,1),day(j,1),nom});
            }
        }
    }
    const double book_ms=now_ms()-setup_start;
    const double layout_start=now_ms();
    GroupLayout L=build_layout(book,DPY);
    const double layout_ms=now_ms()-layout_start;
    refresh_table(L,cd.S,cp.S,df,pf);
    const size_t stride=L.stride(Kd,Kp);
    const size_t entries=L.groups.size()*stride;
    double* full=(double*)allocate_aligned(entries*sizeof(double));
    if (!full) throw std::runtime_error("aligned full risk output allocation failed");
    GroupReduceScratch scratch;
    std::vector<double> reduced(Kd+Kp),from_full(Kd+Kp);
    const auto grouped=[&]{
        scan_grouped<Store::streaming>(L,cd.S,cp.S,full);
    };
    const auto portfolio=[&]{
        scan_grouped_reduce(L,cd.S,cp.S,reduced.data(),scratch);
    };
    const auto sum_full=[&]{
        std::fill(from_full.begin(),from_full.end(),0.0);
        for(size_t gi=0;gi<L.groups.size();++gi)
            for(int k=0;k<Kd+Kp;++k)
                for(int lane=0;lane<L.groups[gi].n_valid;++lane)
                    from_full[k]+=full[gi*stride+(size_t)k*LANES+lane];
    };
    grouped();sum_full();portfolio();
    std::vector<double> time_full,time_portfolio,time_sum,time_full_total,time_refresh;
    for(int rep=0;rep<reps;++rep) {
        double t=now_ms();grouped();time_full.push_back(now_ms()-t);
        t=now_ms();portfolio();time_portfolio.push_back(now_ms()-t);
        t=now_ms();sum_full();time_sum.push_back(now_ms()-t);
        t=now_ms();grouped();sum_full();time_full_total.push_back(now_ms()-t);
        t=now_ms();refresh_table(L,cd.S,cp.S,df,pf);
        time_refresh.push_back(now_ms()-t);
    }
    // Compare against a fresh scalar reduction of the existing materialised
    // grouped output, including every valid lane but not padding.
    sum_full();portfolio();
    double worst=0.0,maxabs=0.0;
    for(int k=0;k<Kd+Kp;++k){
        maxabs=std::max(maxabs,std::abs(from_full[k]));
        const double scale=std::max({1.0,std::abs(from_full[k]),
                                   std::abs(reduced[k])});
        worst=std::max(worst,std::abs(from_full[k]-reduced[k])/scale);
    }
    const double full_ms=median(time_full);
    const double direct_ms=median(time_portfolio);
    const double sum_ms=median(time_sum);
    const double full_total_ms=median(time_full_total);
    const double refresh_ms=median(time_refresh);
    std::printf("reduce_bench N=%zu Kd=%d Kp=%d groups=%zu reps=%d seed=42\n",
                N,Kd,Kp,L.groups.size(),reps);
    std::printf("setup_book_ms=%.3f setup_layout_ms=%.3f refresh_ms=%.6f\n",
                book_ms,layout_ms,refresh_ms);
    std::printf("grouped_write_ms=%.6f grouped_sum_ms=%.6f "
                "grouped_write_plus_sum_ms=%.6f direct_reduce_ms=%.6f\n",
                full_ms,sum_ms,full_total_ms,direct_ms);
    std::printf("materialized_output_MB=%.3f portfolio_output_bytes=%zu "
                "speedup_vs_write=%.3fx speedup_vs_write_plus_sum=%.3fx\n",
                entries*8e-6,static_cast<size_t>(Kd+Kp)*sizeof(double),
                full_ms/direct_ms,full_total_ms/direct_ms);
    std::printf("max_scaled_relative_error=%.9g portfolio_peak_abs=%.9g status=%s\n",
                worst,maxabs,worst<=1e-9?"PASS":"FAIL");
    free_aligned(full);
    return worst<=1e-9?0:1;
}catch(const std::exception& e){
    std::fprintf(stderr,"FAIL: %s\n",e.what());
    return 1;
}
