// Checked grouped API and borrowing contracts, without QuantLib.
// Compare to unchanged raw-pointer kernel contracts and exercise failures
// before any unsafe loads or writes.
#include "ladder/aligned_memory.hpp"
#include "ladder/scan_grouped_reduce.hpp"
#include "ladder/scan_gamma_grouped.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace ladder;
using View = GroupedDiscountCrossGammaView;
using ConstBuffer = std::span<const double>;
static_assert(std::is_constructible_v<View,const GroupLayout&,const Stencils&,
                                      int,ConstBuffer,ConstBuffer>);
static_assert(!std::is_constructible_v<View,GroupLayout&&,const Stencils&,
                                       int,ConstBuffer,ConstBuffer>);
static_assert(!std::is_constructible_v<View,const GroupLayout&,Stencils&&,
                                       int,ConstBuffer,ConstBuffer>);

static void check(bool pass,const char* name) {
    if (!pass) throw std::runtime_error(name);
}
template<class F> static void reject(F&& f,const char* name) {
    bool thrown=false;
    try { f(); } catch(const std::invalid_argument&) { thrown=true; }
    check(thrown,name);
}
template<class F> static void reject_index(F&& f,const char* name) {
    bool thrown=false;
    try { f(); } catch(const std::out_of_range&) { thrown=true; }
    check(thrown,name);
}

static void exercise(bool open) {
    const Stencils Sd{{0.,.4,.9,1.7,3.2},open};
    const Stencils Sp{{0.,.25,1.,2.,4.},true};
    const auto df=[](double t){return std::exp(-.025*t);};
    const auto pf=[](double t){return std::exp(-.031*t);};
    std::vector<InstrumentSpec> book(11); // partially populated groups
    for(int i=0;i<static_cast<int>(book.size());++i) {
        auto& inst=book[i];
        inst.signature=i%2;
        inst.fixed.push_back({100+i*15, 5000.0+i*200});
        inst.fixed.push_back({900+i*5, -2300.0});
        if(i%3==0)inst.fixed.push_back({-50,-130.0});
        inst.flt.push_back({425+i*4,75+i*3,350+i*4,170000.0+i*500});
        inst.ois.push_back({620+i*10,120+i*4,610+i*10,
                            230000.0+i*650});
    }
    GroupLayout L=build_layout(book);
    refresh_table(L,Sd,Sp,df,pf);
    const int Kd=Sd.K(),Kp=Sp.K();
    const size_t n=detail::grouped_output_entries(L,Kd,Kp);
    check(L.groups.size()==2,"unexpected group population");
    const size_t alloc=(n+16)*sizeof(double);
    double* raw=static_cast<double*>(allocate_aligned(alloc));
    double* typed=static_cast<double*>(allocate_aligned(alloc));
    double* streaming=static_cast<double*>(allocate_aligned(alloc));
    check(raw && typed && streaming,"aligned allocation failure");
    scan_grouped<Store::normal>(L,Sd,Sp,raw);
    scan_grouped<Store::normal>(L,Sd,Sp,std::span<double>(typed,n));
    scan_grouped<Store::streaming>(L,Sd,Sp,std::span<double>(streaming,n));
    for(size_t k=0;k<n;++k) {
        check(raw[k]==typed[k],"typed grouped output differs from raw");
        check(raw[k]==streaming[k],"streaming output differs from normal");
    }

    reject([&]{scan_grouped(L,Sd,Sp,std::span<double>(typed,n-1));},
           "undersized grouped output was accepted");
    reject([&]{scan_grouped(L,Sd,Sp,std::span<double>(typed,n+1));},
           "oversized grouped output was accepted");
    reject([&]{scan_grouped(L,Sd,Sp,std::span<double>(typed+1,n));},
           "unaligned grouped output was accepted");

    std::vector<double> slot(Kd+Kp),other(Kd+Kp);
    for(size_t gi=0;gi<L.groups.size();++gi) {
        for(int lane=0;lane<L.groups[gi].n_valid;++lane) {
            gather(L,Kd,Kp,raw,static_cast<int>(gi),lane,slot.data());
            gather(L,Kd,Kp,ConstBuffer(typed,n),static_cast<int>(gi),
                   lane,std::span<double>(other));
            check(slot==other,"checked gather differs from raw");
        }
    }
    reject([&]{gather(L,Kd,Kp,ConstBuffer(typed,n-1),0,0,
                      std::span<double>(slot));},
           "short gather input accepted");
    reject([&]{gather(L,Kd,Kp,ConstBuffer(typed,n),0,0,
                      std::span<double>(slot.data(),slot.size()-1));},
           "short gather output accepted");
    reject_index([&]{gather(L,Kd,Kp,ConstBuffer(typed,n),0,
                            L.groups[0].n_valid,std::span<double>(slot));},
                 "padded gather lane accepted");
    reject_index([&]{gather(L,Kd,Kp,ConstBuffer(typed,n),
                            static_cast<int>(L.groups.size()),0,
                            std::span<double>(slot));},
                 "invalid group accepted");

    GroupReduceScratch reducer;
    std::vector<double> totals(Kd+Kp),same(Kd+Kp);
    scan_grouped_reduce(L,Sd,Sp,std::span<double>(totals),reducer);
    scan_grouped_reduce(L,Sd,Sp,std::span<double>(same));
    check(totals==same,"typed reduction scratch convenience differs");
    reject([&]{scan_grouped_reduce(L,Sd,Sp,
           std::span<double>(totals.data(),totals.size()-1),reducer);},
           "short portfolio output accepted");

    const size_t ng=detail::checked_grouped_product(L.groups.size(),
                grouped_gamma_stride(Kd));
    double* gamma=static_cast<double*>(allocate_aligned((ng+16)*sizeof(double)));
    double* gamma_checked=static_cast<double*>(
        allocate_aligned((ng+16)*sizeof(double)));
    check(gamma && gamma_checked,"gamma allocation failure");
    GroupGammaScratch scratch;
    scan_grouped_gamma_diagonal(L,Sd,gamma,scratch);
    scan_grouped_gamma_diagonal(L,Sd,std::span<double>(gamma_checked,ng),
                                scratch);
    for(size_t k=0;k<ng;++k)
        check(gamma[k]==gamma_checked[k],"checked gamma differs from raw");
    reject([&]{scan_grouped_gamma_diagonal(
                L,Sd,std::span<double>(gamma_checked,ng-1),scratch);},
           "short grouped gamma accepted");
    reject([&]{scan_grouped_gamma_diagonal(
                L,Sd,std::span<double>(gamma_checked,ng+1),scratch);},
           "oversized grouped gamma accepted");
    reject([&]{scan_grouped_gamma_diagonal(
                L,Sd,std::span<double>(gamma_checked+1,ng),scratch);},
           "misaligned gamma accepted");

    std::vector<double> gamma_sum(Kd),gamma_sum_check(Kd);
    scan_grouped_gamma_portfolio_diagonal(L,Sd,gamma_sum.data(),scratch);
    scan_grouped_gamma_portfolio_diagonal(
        L,Sd,std::span<double>(gamma_sum_check),scratch);
    check(gamma_sum==gamma_sum_check,"portfolio gamma difference");
    reject([&]{scan_grouped_gamma_portfolio_diagonal(
               L,Sd,std::span<double>(gamma_sum_check.data(),Kd-1),scratch);},
           "wrong gamma portfolio size accepted");

    View v(L,Sd,Kp,ConstBuffer(typed,n),ConstBuffer(gamma_checked,ng));
    View raw_view(L,Sd,Kp,typed,gamma_checked);
    for(int i=0;i<Kd;++i)for(int j=0;j<Kd;++j)
        check(v.at(0,0,i,j)==raw_view.at(0,0,i,j),
              "checked view differs from legacy view");
    reject([&]{View invalid(L,Sd,Kp,ConstBuffer(typed,n-1),
                             ConstBuffer(gamma_checked,ng));},
           "short first-order gamma-view input accepted");
    reject([&]{View invalid(L,Sd,Kp,ConstBuffer(typed,n),
                             ConstBuffer(gamma_checked,ng-1));},
           "short diagonal gamma-view input accepted");
    reject([&]{View invalid(L,Sd,Kp+1,ConstBuffer(typed,n),
                             ConstBuffer(gamma_checked,ng));},
           "wrong projection-wave count accepted");

    Stencils moved=Sd;
    moved.B[1]+=.02;
    reject([&]{scan_grouped(L,moved,Sp,std::span<double>(typed,n));},
           "stale discount grid accepted");
    GroupLayout invalid=L;
    invalid.refreshed=false;
    reject([&]{scan_grouped(invalid,Sd,Sp,std::span<double>(typed,n));},
           "unrefreshed layout accepted");

    free_aligned(gamma);
    free_aligned(gamma_checked);
    free_aligned(raw);
    free_aligned(typed);
    free_aligned(streaming);
    std::printf("checked grouped API: open=%d groups=%zu PASS\n",
                open,L.groups.size());
}
static void empty_book() {
    const Stencils waves{{0.0,1.0,2.0},true};
    GroupLayout L=build_layout(std::vector<InstrumentSpec>{});
    refresh_table(L,waves,waves,[](double){return 1.0;},
                                [](double){return 1.0;});
    scan_grouped(L,waves,waves,std::span<double>{});
    GroupGammaScratch scratch;
    scan_grouped_gamma_diagonal(L,waves,std::span<double>{},scratch);
    std::vector<double> totals(4,1.0);
    scan_grouped_reduce(L,waves,waves,std::span<double>(totals));
    check(std::all_of(totals.begin(),totals.end(),
                      [](double x){return x==0.0;}),"empty reduction not zero");
}
int main() try {
    empty_book();
    exercise(false);
    exercise(true);
    std::puts("PASS checked grouped spans, gamma views, reduction and lane bounds");
    return 0;
} catch(const std::exception& e) {
    std::fprintf(stderr,"FAIL grouped API: %s\n",e.what());
    return 1;
}
