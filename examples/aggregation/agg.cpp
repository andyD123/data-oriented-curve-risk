// Risk aggregation: contiguous ordered ladders vs per-instrument maps keyed by tenor label.
#include <chrono>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>
#include <random>
#include <algorithm>
static double now_ms(){ return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main(){
    const size_t N = 100000; const int K = 66;
    std::vector<std::string> tenor(K); for (int k = 0; k < K; ++k) tenor[k] = (k < 30 ? "EONIA_" : "EUR6M_") + std::to_string(k) + "M";
    std::mt19937_64 rng(1); std::uniform_real_distribution<double> u(-1e3, 1e3);
    std::vector<double> flat(N * K); for (auto& x : flat) x = u(rng);                       // contiguous, stencil-major per instrument
    std::vector<std::map<std::string,double>> maps(N); std::vector<std::unordered_map<std::string,double>> umaps(N);
    for (size_t i = 0; i < N; ++i) for (int k = 0; k < K; ++k) { maps[i][tenor[k]] = flat[i*K+k]; umaps[i][tenor[k]] = flat[i*K+k]; }
    // 1. contiguous: book ladder = sum over instruments
    double best1 = 1e30; std::vector<double> book(K);
    for (int r = 0; r < 5; ++r) { double t0 = now_ms(); std::fill(book.begin(), book.end(), 0.0);
        for (size_t i = 0; i < N; ++i) { const double* p = &flat[i*K]; for (int k = 0; k < K; ++k) book[k] += p[k]; }
        best1 = std::min(best1, now_ms() - t0); }
    // 2. std::map per instrument, accumulate into a std::map
    double best2 = 1e30; std::map<std::string,double> bookm;
    for (int r = 0; r < 3; ++r) { double t0 = now_ms(); bookm.clear();
        for (size_t i = 0; i < N; ++i) for (auto& kv : maps[i]) bookm[kv.first] += kv.second;
        best2 = std::min(best2, now_ms() - t0); }
    // 3. unordered_map per instrument, accumulate into an unordered_map
    double best3 = 1e30; std::unordered_map<std::string,double> booku;
    for (int r = 0; r < 3; ++r) { double t0 = now_ms(); booku.clear();
        for (size_t i = 0; i < N; ++i) for (auto& kv : umaps[i]) booku[kv.first] += kv.second;
        best3 = std::min(best3, now_ms() - t0); }
    // 4. contiguous, hierarchical: 1000 books of 500 instruments, then desks of 10 books -> same pass, two levels
    double best4 = 1e30; std::vector<double> books(200 * K), desks(20 * K);
    for (int r = 0; r < 5; ++r) { double t0 = now_ms(); std::fill(books.begin(), books.end(), 0.0); std::fill(desks.begin(), desks.end(), 0.0);
        for (size_t i = 0; i < N; ++i) { double* b = &books[(i / 500) * K]; const double* p = &flat[i*K]; for (int k = 0; k < K; ++k) b[k] += p[k]; }
        for (int b = 0; b < 200; ++b) { double* d = &desks[(b / 10) * K]; for (int k = 0; k < K; ++k) d[k] += books[b*K+k]; }
        best4 = std::min(best4, now_ms() - t0); }
    double chk = 0; for (int k = 0; k < K; ++k) chk += book[k] - bookm[tenor[k]]; 
    std::printf("100k instruments x 66 sensitivities, book-level aggregation (one core):\n");
    std::printf("  contiguous ordered ladders, vector add        %8.2f ms   (%.2f ns/value)\n", best1, best1*1e6/(N*K));
    std::printf("  contiguous, two-level hierarchy (book, desk)  %8.2f ms\n", best4);
    std::printf("  std::unordered_map<tenor,double> per instrument %7.1f ms   (%.1f ns/value)  %.0fx slower\n", best3, best3*1e6/(N*K), best3/best1);
    std::printf("  std::map<tenor,double> per instrument         %8.1f ms   (%.1f ns/value)  %.0fx slower\n", best2, best2*1e6/(N*K), best2/best1);
    std::printf("  (check %.1e)  memory: flat %.0f MB vs maps ~%.0f MB\n", chk, N*K*8/1e6, N*K*(48.0+32+8)/1e6);
}
