#include "../example_dir.hpp"
// Adjoint baselines for §9: per-instrument reverse-mode derivative of PV(delta) = sum_i x_i exp(-sum_k delta_k ov_ki)
// at delta = 0, on the paper's benchmark book and curves (bonds + vanilla swaps, Eonia/Euribor example curves).
//   TAPE   generic reverse-mode: forward pass records one node per (cashflow, wave) exp and multiply, with the local
//          derivative; reverse pass walks the tape backwards accumulating adjoints. What an operator-overloading AAD
//          tool does for this valuation.
//   DIRECT tape-free N*K overlap adjoint: d/ddelta_k = -sum_i x_i ov_ki, computed with the geometry known. The
//          optimistic bound for any adjoint engine that still treats each (cashflow, wave) pair.
//   SCAN   the scalar reverse scan (ladder/scan.hpp), N+K.
// Same instruments as examples/benchmark_paper (generator copied), unit cashflows replicated once; timings exclude replication.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>
#include "ladder/stencil.hpp"
#include "ladder/scan.hpp"
#include "ladder/replicate.hpp"
using namespace ladder;
constexpr double DPY = 365.0;
struct Curve { Stencils S; std::vector<double> logD; double df(double t) const { int k = S.bucket(t); double a = (t - S.B[k-1]) / S.len(k); return std::exp(logD[k-1] + a * (logD[k] - logD[k-1])); } };
static Curve read_curve(FILE* in) { char nm[16]; int n; (void)std::fscanf(in, "%15s %d", nm, &n); Curve c; c.S.B.resize(n); c.logD.resize(n); for (int i = 0; i < n; ++i) { double t, D; (void)std::fscanf(in, "%lf %lf", &t, &D); c.S.B[i] = t; c.logD[i] = std::log(D); } return c; }
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct TapeNode { int kind; int a, b; double w; };   // kind 0: y = exp(w * delta_b)  (a: unused)   kind 1: z = x_a * y_b ... simplified below
int main(int argc, char** argv) {
    enter_example_dir();
    size_t N = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 100000; int reps = argc > 2 ? std::atoi(argv[2]) : 3;
    FILE* in = std::fopen("quantlib_example_curves.txt", "r"); int nc; (void)std::fscanf(in, "%d", &nc); Curve cd = read_curve(in), cp = read_curve(in); std::fclose(in);
    auto df = [&](double t){ return cd.df(t); }; auto pf = [&](double t){ return cp.df(t); };
    const int Kd = cd.S.K(), Kp = cp.S.K();
    // ---- same generator as the paper benchmark (bonds + vanilla swaps)
    std::mt19937_64 rng(42); std::uniform_int_distribution<int> start_d(2, 61), years_d(1, 29), type_d(0, 1); std::uniform_real_distribution<double> rate_d(0.01, 0.06);
    const double notionals[] = {1e6, 2.5e6, 5e6, 1e7};
    std::vector<std::vector<UnitCashflow>> ucf(N); std::vector<std::vector<ProjectionTerm>> pt(N);
    for (size_t i = 0; i < N; ++i) {
        int s = start_d(rng), y = years_d(rng), ty = type_d(rng); double notional = notionals[rng() % 4], r = rate_d(rng);
        auto day = [&](int n, int per_year){ return s + (int)std::lround(n * 365.25 / per_year); };
        if (ty == 0) for (int j = 1; j <= 2*y; ++j) replicate_fixed(notional * r / 2 + (j == 2*y ? notional : 0.0), day(j, 2) / DPY, df, ucf[i]);
        else { for (int j = 1; j <= y; ++j) replicate_fixed(-notional * r, day(j, 1) / DPY, df, ucf[i]);
               for (int j = 1; j <= 2*y; ++j) replicate_ibor(notional, day(j-1, 2) / DPY, day(j, 2) / DPY, day(j, 2) / DPY, df, pf, ucf[i], pt[i]); }
        sort_by_time(ucf[i]);
    }
    std::vector<double> out_s(N * (Kd + Kp)), out_d(N * (Kd + Kp)), out_t(N * (Kd + Kp));
    // ---- SCAN
    double best_scan = 1e30;
    for (int r = 0; r < reps; ++r) { double t0 = now_ms(); for (size_t i = 0; i < N; ++i) { scan_discount(cd.S, ucf[i], &out_s[i*(Kd+Kp)]); scan_projection(cp.S, pt[i], &out_s[i*(Kd+Kp)+Kd]); } best_scan = std::min(best_scan, now_ms() - t0); }
    // ---- DIRECT N*K overlap adjoint (no tape): discount waves over unit cashflows; projection waves over coupon terms
    double best_direct = 1e30;
    for (int r = 0; r < reps; ++r) { double t0 = now_ms();
        for (size_t i = 0; i < N; ++i) { double* o = &out_d[i*(Kd+Kp)];
            for (int k = 1; k <= Kd; ++k) { double a = 0; for (auto& c : ucf[i]) a -= c.x * cd.S.overlap(k, c.t); o[k-1] = a; }
            for (int k = 1; k <= Kp; ++k) { double a = 0; for (auto& q : pt[i]) a += q.w * (cp.S.psi(k, q.a) - cp.S.psi(k, q.b)); o[Kd+k-1] = a; } }
        best_direct = std::min(best_direct, now_ms() - t0); }
    // ---- TAPE reverse mode: per instrument, forward pass records nodes, reverse pass accumulates adjoints
    struct Node { int input; double dloc; int parent; };   // value node v = f(parent, delta[input]) with local derivative dloc wrt delta[input]; PV accumulates leaf products
    std::vector<Node> tape; std::vector<double> vals, adj; tape.reserve(1 << 20); vals.reserve(1 << 20); adj.reserve(1 << 20);
    double best_tape = 1e30;
    for (int r = 0; r < reps; ++r) { double t0 = now_ms();
        for (size_t i = 0; i < N; ++i) {
            double* o = &out_t[i*(Kd+Kp)]; for (int k = 0; k < Kd + Kp; ++k) o[k] = 0;
            tape.clear(); vals.clear();
            // forward: for each unit cashflow, chain y_0 = x_i; y_k = y_{k-1} * exp(-delta_k ov_k)  (at delta=0 exp=1); PV += y_Kd
            std::vector<int> leaves; leaves.reserve(ucf[i].size() + pt[i].size());
            for (auto& c : ucf[i]) { int prev = -1; double v = c.x;
                for (int k = 1; k <= Kd; ++k) { double ov = cd.S.overlap(k, c.t); double e = std::exp(-0.0 * ov); v = v * e; tape.push_back({k - 1, -ov * e, prev}); vals.push_back(v); prev = (int)tape.size() - 1; }
                leaves.push_back(prev); }
            for (auto& q : pt[i]) { int prev = -1; double v = q.w;
                for (int k = 1; k <= Kp; ++k) { double ov = -(cp.S.psi(k, q.a) - cp.S.psi(k, q.b)); double e = std::exp(-0.0 * ov); v = v * e; tape.push_back({Kd + k - 1, -ov * e, prev}); vals.push_back(v); prev = (int)tape.size() - 1; }
                leaves.push_back(prev); }
            // reverse: adjoint of PV wrt each node value = 1 at leaves; propagate backwards through the chain
            adj.assign(tape.size(), 0.0); for (int l : leaves) adj[l] = 1.0;
            for (int n = (int)tape.size() - 1; n >= 0; --n) { const Node& nd = tape[n]; double a = adj[n]; if (a == 0.0) continue;
                // v_n = v_parent * e_n  => dv_n/ddelta = v_parent * dloc ; dv_n/dv_parent = e_n (=1 at delta 0)
                double vparent = nd.parent >= 0 ? vals[nd.parent] : vals[n]; // root: v_parent is x itself (vals[n]/e = vals[n])
                o[nd.input] += a * vparent * nd.dloc;
                if (nd.parent >= 0) adj[nd.parent] += a; }
        }
        best_tape = std::min(best_tape, now_ms() - t0); }
    double w1 = 0, w2 = 0; for (size_t j = 0; j < out_s.size(); ++j) { double s = std::max(1.0, std::fabs(out_s[j])); w1 = std::max(w1, std::fabs(out_d[j]-out_s[j])/s); w2 = std::max(w2, std::fabs(out_t[j]-out_s[j])/s); }
    const double nout = (double)N * (Kd + Kp);
    std::printf("N=%zu bonds+vanilla swaps, %d+%d waves; direct vs scan %.1e, tape vs scan %.1e\n", N, Kd, Kp, w1, w2);
    std::printf("%-40s %10.1f ms %9.2f ns/sensitivity\n", "SCAN  scalar reverse scan (N+K)", best_scan, best_scan*1e6/nout);
    std::printf("%-40s %10.1f ms %9.2f ns/sensitivity\n", "DIRECT N*K overlap adjoint, no tape", best_direct, best_direct*1e6/nout);
    std::printf("%-40s %10.1f ms %9.2f ns/sensitivity  (tape nodes/instrument ~%zu)\n", "TAPE  reverse mode with recorded tape", best_tape, best_tape*1e6/nout, tape.size());
}
