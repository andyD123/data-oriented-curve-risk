// Same seeded 500k bonds/IBOR/OIS book generator and two curve fixtures as
// bench.cpp. Measures first-order-only, extra grouped diagonal gamma, and a
// K-value portfolio-gamma reduction. Not a second-order bump/reprice baseline.
#include "ladder/aligned_memory.hpp"
#include "ladder/scan_gamma_grouped.hpp"
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
    const size_t delta_entries=L.groups.size()*L.stride(Kd,Kp);
    const size_t gamma_entries=L.groups.size()*grouped_gamma_stride(Kd);
    double* delta=(double*)allocate_aligned(delta_entries*sizeof(double));
    double* gamma=(double*)allocate_aligned(gamma_entries*sizeof(double));
    GroupGammaScratch scratch;
    std::vector<double> portfolio_gamma(Kd);
    const auto first=[&]{scan_grouped<Store::streaming>(L,cd.S,cp.S,delta);};
    const auto second=[&]{scan_grouped_gamma_diagonal<Store::streaming>(L,cd.S,gamma,scratch);};
    const auto portfolio=[&]{scan_grouped_gamma_portfolio_diagonal(L,cd.S,portfolio_gamma.data(),scratch);};
    first();second();portfolio(); // all repeated-work scratch is warm
    std::vector<double> first_times,gamma_times,portfolio_times,both_times,refresh_times;
    for(int r=0;r<reps;++r){
        double t=now_ms();first();first_times.push_back(now_ms()-t);
        t=now_ms();second();gamma_times.push_back(now_ms()-t);
        t=now_ms();portfolio();portfolio_times.push_back(now_ms()-t);
        t=now_ms();first();second();both_times.push_back(now_ms()-t);
        t=now_ms();refresh_table(L,cd.S,cp.S,df,pf);refresh_times.push_back(now_ms()-t);
    }
    const double first_ms=median(first_times),gamma_ms=median(gamma_times);
    const double portfolio_ms=median(portfolio_times),both_ms=median(both_times);
    const double refresh_ms=median(refresh_times);

    // Independent portfolio aggregate of the materialised results.
    double portfolio_error=0.0;
    for(int k=0;k<Kd;++k){
        long double reference=0,mag=0;
        for(size_t gi=0;gi<L.groups.size();++gi)
            for(int lane=0;lane<L.groups[gi].n_valid;++lane){
                const double x=gamma[gi*grouped_gamma_stride(Kd)+(size_t)k*LANES+lane];
                reference+=x;mag+=std::abs(x);
            }
        const double scale=std::max({1.,std::abs(portfolio_gamma[k]),
                                   std::abs((double)reference),(double)mag*1e-12});
        portfolio_error=std::max(portfolio_error,
                                std::abs(portfolio_gamma[k]-(double)reference)/scale);
    }
    // Check actual fixed/IBOR/OIS formula second derivatives, not only
    // frozen signed first-order records. Sample ~256 groups across full book.
    double worst=0.;size_t checks=0;
    const size_t sample_step=std::max<size_t>(1,L.groups.size()/256);
    for(size_t gi=0;gi<L.groups.size();gi+=sample_step){
        const Group& G=L.groups[gi];
        for(int lane=0;lane<G.n_valid;++lane){
            const auto& spec=book[G.inst[lane]];
            for(int kk=0;kk<Kd;++kk){
                double direct=0.;
                for(const auto& f:spec.fixed){
                    double t=f.day/DPY,h=cd.S.overlap(kk+1,t);
                    direct+=f.amount*df(t)*h*h;
                }
                for(const auto& f:spec.flt){
                    double t=f.pay_day/DPY,h=cd.S.overlap(kk+1,t);
                    direct+=f.scale*(pf(f.a_day/DPY)/pf(f.b_day/DPY)-1.)*df(t)*h*h;
                }
                for(const auto& o:spec.ois){
                    const double a=o.a_day/DPY,b=o.b_day/DPY,p=o.pay_day/DPY;
                    const double A=o.N*df(p)*df(a)/df(b),C=o.N*df(p);
                    const double ha=cd.S.overlap(kk+1,a),hb=cd.S.overlap(kk+1,b);
                    const double hp=cd.S.overlap(kk+1,p),q=hp+ha-hb;
                    direct+=A*q*q-C*hp*hp;
                }
                const double x=gamma[gi*grouped_gamma_stride(Kd)+(size_t)kk*LANES+lane];
                worst=std::max(worst,std::abs(x-direct)/
                               std::max({1.,std::abs(x),std::abs(direct)}));
                ++checks;
            }
        }
    }
    std::printf("gamma_stage2 N=%zu Kd=%d Kp=%d groups=%zu reps=%d\n",
                N,Kd,Kp,L.groups.size(),reps);
    std::printf("setup_book_ms=%.3f setup_layout_ms=%.3f\n",book_ms,layout_ms);
    std::printf("refresh_ms=%.6f first_order_kernel_ms=%.6f gamma_diagonal_kernel_ms=%.6f gamma_portfolio_ms=%.6f combined_kernel_ms=%.6f\n",
                refresh_ms,first_ms,gamma_ms,portfolio_ms,both_ms);
    std::printf("first_order_output_MB=%.3f gamma_output_MB=%.3f extra_gamma_ratio=%.3f combined_ratio=%.3f\n",
                delta_entries*8e-6,gamma_entries*8e-6,gamma_ms/first_ms,both_ms/first_ms);
    std::printf("oracle_checks=%zu max_rel=%.3g portfolio_max_rel=%.3g status=%s\n",
                checks,worst,portfolio_error,
                worst<1e-9&&portfolio_error<1e-9?"PASS":"FAIL");
    free_aligned(delta);free_aligned(gamma);
    return worst<1e-9&&portfolio_error<1e-9?0:1;
}catch(const std::exception& e){
    std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;
}
