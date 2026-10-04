// Box-wave ladders from unit cashflows priced on whatever curve QuantLib used (here the shipped cubic curves),
// using the library scan. Input: unit_cashflows_wave.txt, wave_buckets.txt (written by ql_waves).
#include <cstdio>
#include <vector>
#include "ladder/stencil.hpp"
#include "ladder/unit_cashflow.hpp"
#include "ladder/scan.hpp"
using namespace ladder;
static std::vector<double> read_row(FILE* f) { std::vector<double> v; char line[8192]; if (!std::fgets(line, sizeof line, f)) return v; char* p = line; char* e; for (;;) { double d = std::strtod(p, &e); if (e == p) break; v.push_back(d); p = e; } return v; }
int main() {
    FILE* bf = std::fopen("./wave_buckets.txt", "r"); Stencils Sd, Sp; Sd.B = read_row(bf); Sp.B = read_row(bf); std::fclose(bf);
    Sd.open_last = Sp.open_last = true;   // Hagan 2015 eq. 2.2b: last wave open-ended, matching ql_waves' BoxShifted
    FILE* in = std::fopen("./unit_cashflows_wave.txt", "r"); int n; (void)std::fscanf(in, "%d", &n);
    FILE* out = std::fopen("./scan_wave_risk.txt", "w");
    std::vector<double> dd(Sd.K()), dp(Sp.K());
    for (int i = 0; i < n; ++i) {
        char name[64]; int ne, np; (void)std::fscanf(in, "%63s %d %d", name, &ne, &np);
        std::vector<UnitCashflow> e(ne); for (auto& x : e) (void)std::fscanf(in, "%lf %lf", &x.t, &x.x);
        std::vector<ProjectionTerm> p(np); for (auto& r : p) (void)std::fscanf(in, "%lf %lf %lf %lf", &r.t_pay, &r.a, &r.b, &r.w);
        sort_by_time(e);
        scan_discount(Sd, e, dd.data()); scan_projection(Sp, p, dp.data());
        for (double v : dd) std::fprintf(out, "%.17g ", v); for (double v : dp) std::fprintf(out, "%.17g ", v); std::fprintf(out, "\n");
    }
    std::fclose(out); std::printf("scan_wave: %d instruments, %d+%d waves\n", n, Sd.K(), Sp.K());
}
