#include "../example_dir.hpp"
// Bucket ladders for the QuantLib-example instruments via the library: replication from the dumped terms,
// log-linear curves on the example's bootstrapped nodes, scalar scan. Input cashflows2.txt (from ql_examples).
#include <cmath>
#include <cstdio>
#include <vector>
#include "ladder/stencil.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
using namespace ladder;
struct Curve { Stencils S; std::vector<double> logD;
    double df(double t) const { int k = S.bucket(t); double a = (t - S.B[k-1]) / S.len(k); return std::exp(logD[k-1] + a * (logD[k] - logD[k-1])); } };
static Curve read_curve(FILE* in) { char nm[16]; int n; (void)std::fscanf(in, "%15s %d", nm, &n); Curve c; c.S.B.resize(n); c.logD.resize(n);
    for (int i = 0; i < n; ++i) { double t, D; (void)std::fscanf(in, "%lf %lf", &t, &D); c.S.B[i] = t; c.logD[i] = std::log(D); } return c; }
int main() {
    enter_example_dir();
    FILE* in = std::fopen("./cashflows2.txt", "r"); int nc; (void)std::fscanf(in, "%d", &nc);
    Curve cd = read_curve(in), cp = read_curve(in);
    auto df = [&](double t){ return cd.df(t); }; auto pf = [&](double t){ return cp.df(t); };
    int n; (void)std::fscanf(in, "%d", &n);
    FILE* out = std::fopen("./scan_risk.txt", "w");
    std::vector<double> dd(cd.S.K()), dp(cp.S.K());
    for (int i = 0; i < n; ++i) {
        char name[64]; int nterms, nproj; (void)std::fscanf(in, "%63s %d %d", name, &nterms, &nproj);
        std::vector<UnitCashflow> u; std::vector<ProjectionTerm> p; double pv = 0;
        for (int j = 0; j < nterms; ++j) { char kind; double v[4]; (void)std::fscanf(in, " %c %lf %lf %lf %lf", &kind, &v[0], &v[1], &v[2], &v[3]);
            size_t before = u.size();
            if (kind == 'C') replicate_fixed(v[0], v[1], df, u);
            else if (kind == 'F') replicate_ibor(v[0], v[1], v[2], v[3], df, pf, u, p);
            else if (kind == 'O') replicate_ois(v[0], v[1], v[2], v[3], df, u);
            for (size_t q = before; q < u.size(); ++q) pv += u[q].x; }
        for (int j = 0; j < nproj; ++j) { char kind; double v[5]; (void)std::fscanf(in, " %c %lf %lf %lf %lf %lf", &kind, &v[0], &v[1], &v[2], &v[3], &v[4]); }   // P rows: already in p
        // the scan here is per unit theta_k (= per unit delta_k times len); convert to the harness's theta convention
        sort_by_time(u); scan_discount(cd.S, u, dd.data()); scan_projection(cp.S, p, dp.data());
        for (int k = 1; k <= cd.S.K(); ++k) dd[k-1] /= cd.S.len(k);
        for (int k = 1; k <= cp.S.K(); ++k) dp[k-1] /= cp.S.len(k);
        std::fprintf(out, "%.17g\n", pv); for (double v : dd) std::fprintf(out, "%.17g ", v); std::fprintf(out, "\n"); for (double v : dp) std::fprintf(out, "%.17g ", v); std::fprintf(out, "\n");
    }
    std::fclose(out); std::printf("scan_reconcile: %d instruments\n", n);
}
