#pragma once
// Experimental variable-width fixed-cashflow schedule groups.
// Hardware SIMD remains eight doubles wide; a logical schedule group may contain
// many vec8 blocks so date/bucket control is amortised across a longer inner loop.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "layout.hpp"
#include "scan_simd.hpp"

namespace ladder {

struct WideFixedGroup {
    int n_valid = 0;
    int blocks = 0;
    std::vector<int> inst;
    std::vector<int> col_di;
    std::vector<Lane8> col_amt; // column-major: [column * blocks + block]
    std::size_t out_offset = 0;  // doubles in the layout-wide output
};

struct WideFixedLayout {
    std::vector<WideFixedGroup> groups;
    DateTable table;
    double days_per_year = 365.0;
    bool refreshed = false;
    std::vector<double> discount_grid;
    std::size_t output_values = 0;
};

inline WideFixedLayout build_wide_fixed_layout(const std::vector<InstrumentSpec>& book,
                                                int max_group_width,
                                                double days_per_year = 365.0)
{
    if (max_group_width < LANES || max_group_width % LANES != 0)
        throw std::invalid_argument("build_wide_fixed_layout: width must be a positive multiple of 8");
    if (!(days_per_year > 0.0) || !std::isfinite(days_per_year))
        throw std::invalid_argument("build_wide_fixed_layout: days_per_year must be finite and positive");
    if (book.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::length_error("build_wide_fixed_layout: too many instruments");

    WideFixedLayout L;
    L.days_per_year = days_per_year;
    std::unordered_map<int, std::vector<int>> by_sig;
    for (std::size_t i = 0; i < book.size(); ++i) {
        const auto& in = book[i];
        if (!in.flt.empty() || !in.ois.empty())
            throw std::invalid_argument("build_wide_fixed_layout: experiment accepts fixed cashflows only");
        for (const auto& c : in.fixed)
            if (!std::isfinite(c.amount))
                throw std::invalid_argument("build_wide_fixed_layout: non-finite fixed amount");
        by_sig[in.signature].push_back(static_cast<int>(i));
    }

    std::vector<int> sigs;
    sigs.reserve(by_sig.size());
    for (const auto& kv : by_sig) sigs.push_back(kv.first);
    std::sort(sigs.begin(), sigs.end());

    std::vector<int> all_days;
    std::vector<std::vector<int>> group_days;
    for (int sig : sigs) {
        const auto& members = by_sig[sig];
        for (std::size_t first = 0; first < members.size(); first += static_cast<std::size_t>(max_group_width)) {
            const int count = static_cast<int>(std::min<std::size_t>(max_group_width, members.size() - first));
            WideFixedGroup G;
            G.n_valid = count;
            G.blocks = (count + LANES - 1) / LANES;
            G.inst.reserve(count);
            for (int i = 0; i < count; ++i) G.inst.push_back(members[first + static_cast<std::size_t>(i)]);

            std::vector<int> dset;
            for (int idx : G.inst)
                for (const auto& c : book[static_cast<std::size_t>(idx)].fixed)
                    dset.push_back(c.day);
            std::sort(dset.begin(), dset.end());
            dset.erase(std::unique(dset.begin(), dset.end()), dset.end());

            G.col_amt.assign(dset.size() * static_cast<std::size_t>(G.blocks), Lane8{});
            auto col_of = [&](int d) {
                return static_cast<int>(std::lower_bound(dset.begin(), dset.end(), d) - dset.begin());
            };
            for (int local = 0; local < count; ++local) {
                const int block = local / LANES, lane = local % LANES;
                const auto& in = book[static_cast<std::size_t>(G.inst[static_cast<std::size_t>(local)])];
                for (const auto& c : in.fixed) {
                    const int ci = col_of(c.day);
                    G.col_amt[static_cast<std::size_t>(ci) * G.blocks + block].v[lane] += c.amount;
                }
            }

            all_days.insert(all_days.end(), dset.begin(), dset.end());
            group_days.push_back(std::move(dset));
            L.groups.push_back(std::move(G));
        }
    }

    std::sort(all_days.begin(), all_days.end());
    all_days.erase(std::unique(all_days.begin(), all_days.end()), all_days.end());
    L.table.day = all_days;
    std::unordered_map<int,int> index;
    index.reserve(all_days.size());
    for (std::size_t i = 0; i < all_days.size(); ++i) index[all_days[i]] = static_cast<int>(i);
    for (std::size_t gi = 0; gi < L.groups.size(); ++gi) {
        auto& G = L.groups[gi];
        G.col_di.reserve(group_days[gi].size());
        for (int d : group_days[gi]) G.col_di.push_back(index[d]);
    }

    const std::size_t n = all_days.size();
    L.table.t.resize(n); L.table.D.resize(n); L.table.P.resize(n);
    L.table.bo.resize(n); L.table.bp.resize(n);
    for (std::size_t i = 0; i < n; ++i) L.table.t[i] = all_days[i] / days_per_year;
    return L;
}

template <class DF>
inline void refresh_wide_fixed(WideFixedLayout& L, const Stencils& S, DF df)
{
    L.refreshed = false;
    S.validate();
    for (std::size_t i = 0; i < L.table.day.size(); ++i) {
        const double t = L.table.t[i];
        const double D = df(t);
        if (!(D > 0.0) || !std::isfinite(D))
            throw std::invalid_argument("refresh_wide_fixed: discount factor must be finite and positive");
        L.table.D[i] = D;
        L.table.bo[i] = S.bucket(t);
    }
    L.discount_grid = S.B;
    L.output_values = 0;
    for (auto& G : L.groups) {
        G.out_offset = L.output_values;
        L.output_values += static_cast<std::size_t>(S.K()) * G.blocks * LANES;
    }
    L.refreshed = true;
}

template <Store policy = Store::normal>
inline void scan_wide_fixed(const WideFixedLayout& L, const Stencils& S, double* out)
{
    S.validate();
    if (!L.refreshed || L.discount_grid != S.B)
        throw std::invalid_argument("scan_wide_fixed: refresh for this stencil first");
    if (L.output_values && (!out || reinterpret_cast<std::uintptr_t>(out) % 64 != 0))
        throw std::invalid_argument("scan_wide_fixed: output must be non-null and 64-byte aligned");

    int max_blocks = 0;
    for (const auto& G : L.groups) max_blocks = std::max(max_blocks, G.blocks);
    std::vector<vec8> running(max_blocks), interior(max_blocks), suffix(max_blocks), beyond(max_blocks);

    const int K = S.K();
    const double BK = S.B[K], BK1 = S.B[K - 1];
    for (const auto& G : L.groups) {
        const int B = G.blocks;
        for (int b = 0; b < B; ++b)
            running[b] = interior[b] = suffix[b] = beyond[b] = vzero();

        int k = K;
        auto emit = [&](int bucket) {
            if (bucket > K) return;
            const std::size_t base = G.out_offset + static_cast<std::size_t>(bucket - 1) * B * LANES;
            const vec8 len = vbroadcast(S.len(bucket));
            for (int b = 0; b < B; ++b) {
                vec8 r = (bucket == K && S.open_last)
                    ? vneg(vadd(interior[b], beyond[b]))
                    : vneg(vfma(len, suffix[b], interior[b]));
                store8<policy>(out + base + static_cast<std::size_t>(b) * LANES, r);
            }
        };

        for (int ci = static_cast<int>(G.col_di.size()) - 1; ci >= 0; --ci) {
            const int di = G.col_di[static_cast<std::size_t>(ci)];
            const double t = L.table.t[static_cast<std::size_t>(di)];
            while (k >= 1 && t < S.B[k]) {
                emit(k + 1);
                for (int b = 0; b < B; ++b) { suffix[b] = running[b]; interior[b] = vzero(); }
                --k;
            }
            const vec8 D = vbroadcast(L.table.D[static_cast<std::size_t>(di)]);
            const bool in_interior = t < BK && t >= S.B.front();
            const bool in_beyond = t >= BK && S.open_last;
            const vec8 w = in_interior ? vbroadcast(t - S.B[k]) : vzero();
            const vec8 wb = in_beyond ? vbroadcast(t - BK1) : vzero();
            const std::size_t col = static_cast<std::size_t>(ci) * B;
            for (int b = 0; b < B; ++b) {
                const vec8 x = vmul(vload(G.col_amt[col + b].v), D);
                running[b] = vadd(running[b], x);
                if (in_beyond) beyond[b] = vfma(x, wb, beyond[b]);
                else if (in_interior) interior[b] = vfma(x, w, interior[b]);
            }
        }
        while (k >= 1) {
            emit(k + 1);
            for (int b = 0; b < B; ++b) { suffix[b] = running[b]; interior[b] = vzero(); }
            --k;
        }
        emit(1);
    }
    if constexpr (policy == Store::streaming) vfence();
}

} // namespace ladder
