// Independent direct-overlap oracle: do not call Stencils::overlap/psi here.
#include "ladder/aligned_memory.hpp"
#include "ladder/scan.hpp"
#include "ladder/scan_simd.hpp"
#include <cstdio>
#include <limits>
#include <numeric>
#include <string>
using namespace ladder;
namespace {
int checks = 0, failures = 0;
void check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
void near(double actual, long double expected, const char* message, long double scale = 1) {
    check(std::isfinite(actual) && std::isfinite(expected) &&
          std::fabs(static_cast<long double>(actual) - expected) <= 2e-11L * std::max(scale, std::fabs(expected)), message);
}
template<class F> void rejects(F f, const char* message) {
    bool caught = false;
    try { f(); } catch (const std::invalid_argument&) { caught = true; }
    check(caught, message);
}
long double ov(const Stencils& s, int k, long double t) {
    const long double lo = s.B[k], hi = s.B[k+1];
    if (t <= lo) return 0;
    if (s.open_last && k == s.K()-1) return t-lo;
    return t >= hi ? hi-lo : t-lo;
}
long double discount(const Stencils& s, int k, const std::vector<UnitCashflow>& cf) {
    long double v=0;
    for (auto c : cf) v -= static_cast<long double>(c.x)*ov(s,k,c.t);
    return v;
}
void one_record(const Stencils& s, double t, double x) {
    const int K=s.K();
    std::vector<UnitCashflow> cf{{t,x}};
    std::vector<double> scalar(K); scan_discount(s,cf,scalar.data());
    InstrumentSpec in{}; in.fixed={{0,x}};
    auto l=build_layout({in}); l.table.t[0]=t;
    refresh_table(l,s,s,[](double){return 1.;},[](double){return 1.;});
    alignas(64) double normal[64]{}, streaming[64]{};
    scan_grouped(l,s,s,normal); scan_grouped<Store::streaming>(l,s,s,streaming);
    for (int k=0;k<K;++k) {
        const auto expected=discount(s,k,cf);
        near(scalar[k],expected,"single-record scalar/direct");
        near(normal[8*k],expected,"single-record grouped/direct");
    }
    for (int k=0;k<2*K;++k) for (int lane=0;lane<8;++lane) {
        check(normal[8*k+lane]==streaming[8*k+lane],"normal/streaming equality");
        if (lane || k>=K) check(normal[8*k+lane]==0.,"padding/projection zero");
    }
}
void mixed_book(bool open) {
    Stencils sd{{0.,.6,1.7,3.},open}, sp{{.25,.9,2.,3.5},open};
    auto df=[](double t){return std::exp(-.021*t-.0007*t*t);};
    auto pf=[](double t){return std::exp(-.027*t-.0009*t*t);};
    std::vector<InstrumentSpec> book(19);
    for (int i=0;i<19;++i) {
        auto& b=book[i]; b.signature=7; // deliberately unrelated/partially shared schedules
        const double sign=(i%2 ? -1. : 1.);
        b.fixed={{-30-i,100.},{0,30.},{300+i*17,sign*123.},{2000-i*3,50.},{300+i*17,-sign*23.}};
        b.flt={{500,10+i*3,450+i,sign*1000.},{1500,1000+i,1400+i,300.}};
        b.ois={{800+i,-10+i,790+i,sign*500.},{1800,1400+i,1700+i,-200.}};
    }
    auto l=build_layout(book); refresh_table(l,sd,sp,df,pf);
    const auto n=l.groups.size()*l.stride(sd.K(),sp.K());
    auto* storage=static_cast<double*>(allocate_aligned((n+16)*sizeof(double)));
    auto* other=static_cast<double*>(allocate_aligned(n*sizeof(double)));
    std::fill(storage,storage+n+16,1234567.); double* out=storage+8;
    scan_grouped(l,sd,sp,out); scan_grouped<Store::streaming>(l,sd,sp,other);
    check(l.groups.size()==3 && l.groups.back().n_valid==3,"partial union groups");
    for (size_t j=0;j<n;++j) check(out[j]==other[j],"mixed store equality");
    for (int j=0;j<8;++j) check(storage[j]==1234567. && storage[n+8+j]==1234567.,"output canary");
    for (size_t g=0;g<l.groups.size();++g) for (int lane=0;lane<8;++lane) {
        if (lane>=l.groups[g].n_valid) {
            for (int k=0;k<sd.K()+sp.K();++k) check(out[g*l.stride(sd.K(),sp.K())+8*k+lane]==0.,"partial lane zero");
            continue;
        }
        const auto& b=book[l.groups[g].inst[lane]];
        std::vector<UnitCashflow> cash;
        std::vector<ProjectionTerm> proj;
        for (auto c:b.fixed) cash.push_back({c.day/365.,c.amount*df(c.day/365.)});
        for (auto c:b.flt) {
            double t=c.pay_day/365.,a=c.a_day/365.,z=c.b_day/365.;
            double w=c.scale*df(t)*pf(a)/pf(z);
            cash.push_back({t,c.scale*df(t)*(pf(a)/pf(z)-1.)}); proj.push_back({t,a,z,w});
        }
        for (auto c:b.ois) {
            double t=c.pay_day/365.,a=c.a_day/365.,z=c.b_day/365.;
            double w=c.N*df(t)*df(a)/df(z);
            cash.push_back({t,w-c.N*df(t)}); cash.push_back({a,w}); cash.push_back({z,-w});
        }
        sort_by_time(cash);
        std::vector<double> scalar(sd.K()), projection(sp.K());
        scan_discount(sd,cash,scalar.data()); scan_projection(sp,proj,projection.data());
        const double* row=out+g*l.stride(sd.K(),sp.K())+lane;
        for (int k=0;k<sd.K();++k) {
            const auto expected=discount(sd,k,cash);
            near(scalar[k],expected,"mixed scalar/direct",1e4);
            near(row[8*k],expected,"mixed grouped/direct",1e4);
        }
        for (int k=0;k<sp.K();++k) {
            long double expected=0;
            for (auto p:proj) expected+=static_cast<long double>(p.w)*(ov(sp,k,p.b)-ov(sp,k,p.a));
            near(projection[k],expected,"projection scalar/direct",1e4);
            near(row[8*(sd.K()+k)],expected,"projection grouped/direct",1e4);
        }
        if(open) {
            long double parallel=0;
            for(auto c:cash) parallel-=static_cast<long double>(c.x)*std::max(c.t-sd.B[0],0.);
            near(std::accumulate(scalar.begin(),scalar.end(),0.),parallel,"open parallel identity",1e4);
        }
    }
    auto changed=sp; changed.B[1]=1.;
    rejects([&]{scan_grouped(l,sd,changed,out);},"stale projection grid");
    rejects([&]{scan_grouped(l,sd,sp,out+1);},"misaligned output");
    rejects([&]{scan_grouped(l,sd,sp,nullptr);},"null grouped output");
    free_aligned(storage); free_aligned(other);
}
}
int main() try {
    for (const auto& grid:std::vector<std::vector<double>>{{0,1,2},{.5,1,2},{-.5,.25,2},{1,2}})
        for (bool open:{false,true}) {
            Stencils s{grid,open};
            std::vector<double> times{grid.front()-1,grid.back()+.5};
            for(double b:grid) {
                times.push_back(std::nextafter(b,-std::numeric_limits<double>::infinity()));
                times.push_back(b);
                times.push_back(std::nextafter(b,std::numeric_limits<double>::infinity()));
            }
            for(double t:times) for(double x:{100.,-100.,0.}) one_record(s,t,x);
            std::vector<double> out(s.K(),99.);
            scan_discount(s,{},out.data());
            for(double x:out) check(x==0.,"empty cashflow stream");
            scan_projection(s,{},out.data());
            for(double x:out) check(x==0.,"empty projection stream");
        }
    for (bool open:{false,true}) {
        Stencils s{{0,1,2},open}; double out[2];
        scan_discount(s,std::vector<UnitCashflow>{{2.,100.}},out); near(out[1],-100.,"t=2 terminal regression");
        scan_discount(s,std::vector<UnitCashflow>{{2.5,100.}},out); near(out[1],open?-150.:-100.,"t=2.5 terminal regression");
        scan_discount(s,std::vector<UnitCashflow>{{-.5,100.},{1.5,100.},{1.5,-100.}},out);
        near(out[0],0.,"signed cancellation first wave"); near(out[1],0.,"signed cancellation last wave");
        scan_projection(s,std::vector<ProjectionTerm>{{3,2.25,2.75,100}},out);
        near(out[1],open?50.:0.,"projection open tail");
        mixed_book(open);
    }
    const double nan=std::numeric_limits<double>::quiet_NaN(), inf=std::numeric_limits<double>::infinity();
    Stencils good{{0,1,2}}; double out[2];
    for(auto grid:std::vector<std::vector<double>>{{},{0},{0,0},{1,0},{0,nan},{0,inf},{-1e308,1e308}}) {
        Stencils bad{grid};
        rejects([&]{scan_discount(bad,{},out);},"invalid discount stencil");
        rejects([&]{scan_projection(bad,{},out);},"invalid projection stencil");
    }
    for(auto cf:std::vector<std::vector<UnitCashflow>>{{{nan,1}},{{1,inf}},{{2,1},{1,1}}})
        rejects([&]{scan_discount(good,cf,out);},"invalid cashflow input");
    std::vector<UnitCashflow> bad_time{{nan,1}};
    rejects([&]{sort_by_time(bad_time);},"NaN cannot be sorted");
    for(auto term:std::vector<ProjectionTerm>{{3,2,1,1},{3,1,2,nan},{inf,1,2,1}})
        rejects([&]{scan_projection(good,std::vector<ProjectionTerm>{term},out);},"invalid projection input");
    rejects([&]{scan_discount(good,{},nullptr);},"null scalar output");
    rejects([&]{scan_projection(good,{},nullptr);},"null projection output");
    for(double dpy:{0.,-1.,nan,inf}) rejects([&]{build_layout({},dpy);},"invalid day scale");
    InstrumentSpec bad{}; bad.fixed={{1,nan}};
    rejects([&]{build_layout({bad});},"non-finite fixed amount");
    bad.fixed.clear(); bad.flt={{3,2,1,100}};
    rejects([&]{build_layout({bad});},"reversed IBOR dates");
    bad.flt.clear(); bad.ois={{3,1,2,inf}};
    rejects([&]{build_layout({bad});},"non-finite OIS notional");
    auto empty=build_layout({});
    refresh_table(empty,good,good,[](double){return 1.;},[](double){return 1.;});
    scan_grouped(empty,good,good,nullptr);
    InstrumentSpec single{}; single.fixed={{100,1}}; auto l=build_layout({single});
    alignas(64) double aligned[32];
    rejects([&]{scan_grouped(l,good,good,aligned);},"unrefreshed table");
    for(double factor:{0.,-1.,nan,inf}) {
        rejects([&]{refresh_table(l,good,good,[&](double){return factor;},[](double){return 1.;});},"invalid curve factor");
        check(!l.refreshed,"failed refresh stays invalid");
    }
    // Empty instruments still emit complete zero rows, including padding.
    l=build_layout(std::vector<InstrumentSpec>(9,InstrumentSpec{}));
    refresh_table(l,good,good,[](double){return 1.;},[](double){return 1.;});
    alignas(64) double zeros[64]; std::fill(std::begin(zeros),std::end(zeros),99.);
    scan_grouped(l,good,good,zeros);
    for(double v:zeros) check(v==0.,"empty instrument output");
    std::printf("boundary/oracle/input checks: %d; failures: %d\n",checks,failures);
    return failures?1:0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"unexpected exception: %s\n",e.what()); return 1;
}
