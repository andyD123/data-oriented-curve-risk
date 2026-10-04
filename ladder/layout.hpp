#pragma once
// Grouped (AoSoA) layout: instruments sharing a schedule signature and curves are packed eight per group;
// each group walks its payment columns with per-lane amounts and per-column broadcasts from a shared date table.
#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_map>
#include <vector>
#include "stencil.hpp"

namespace ladder {

constexpr int LANES = 8;
struct alignas(64) Lane8 { double v[LANES]; };

// ---- instrument description (integer days from the reference date; amounts in currency)
struct FixedFlow { int day; double amount; };
struct FloatFlow { int pay_day, a_day, b_day; double scale; };   // IBOR: amount = scale*(P(a)/P(b)-1)
struct OisFlow   { int pay_day, a_day, b_day; double N; };       // compounded ON: N*(D(a)/D(b)-1) paid at pay_day
struct InstrumentSpec {
    int signature;                     // grouping key: conventions + last regular payment date (same key => shared regular grid)
    std::vector<FixedFlow> fixed;
    std::vector<FloatFlow> flt;
    std::vector<OisFlow>   ois;
};

// ---- shared date table: geometry fixed by the book, values refreshed per curve update
struct DateTable {
    std::vector<int>    day;           // unique days, ascending
    std::vector<double> t, D, P;       // time, discount factor, projection pseudo-discount
    std::vector<int>    bo, bp;        // stencil interval on the discount / projection stencils
};

struct Group {
    int n_valid = 0; std::array<int, LANES> inst{};
    std::vector<int>   col_di;         // discount columns, ascending by date: index into the table
    std::vector<Lane8> col_amt;        // per column, per lane fixed amount
    std::vector<std::array<int,3>> f_di; std::vector<int> f_col; std::vector<Lane8> f_scale;          // IBOR {ip, ia, ib}
    std::vector<std::array<int,3>> o_di; std::vector<std::array<int,3>> o_col; std::vector<Lane8> o_N; // OIS {ip, ia, ib}
    std::vector<std::pair<int,int>> o_at;  // (column, q*3 + kind) for every OIS entry, sorted by column
};

struct GroupLayout {
    std::vector<Group> groups;
    DateTable table;
    double days_per_year = 365.0;
    size_t stride(int K_disc, int K_proj) const { return (size_t)(K_disc + K_proj) * LANES; }
};

inline GroupLayout build_layout(const std::vector<InstrumentSpec>& book, double days_per_year = 365.0)
{
    GroupLayout L; L.days_per_year = days_per_year;
    std::unordered_map<int, std::vector<int>> by_sig;
    for (size_t i = 0; i < book.size(); ++i) by_sig[book[i].signature].push_back((int)i);
    std::vector<int> sigs; for (auto& kv : by_sig) sigs.push_back(kv.first); std::sort(sigs.begin(), sigs.end());
    std::vector<int> days; std::vector<std::vector<int>> col_day;                 // per group, column days
    for (int s : sigs) {
        auto& members = by_sig[s];
        for (size_t g = 0; g < members.size(); g += LANES) {
            Group G; G.n_valid = (int)std::min<size_t>(LANES, members.size() - g);
            for (int l = 0; l < LANES; ++l) G.inst[l] = l < G.n_valid ? members[g + l] : -1;
            // columns are the union of the members' dates; a lane holds zero on a column where its instrument has no
            // cashflow. Members with the same conventions and last regular date share every regular column whatever
            // their start dates (the longest-history member supplies the grid); unrelated members still work, at lower
            // lane utilisation.
            std::vector<int> dset;
            for (int l = 0; l < G.n_valid; ++l) {
                const InstrumentSpec& in = book[members[g + l]];
                for (auto& c : in.fixed) dset.push_back(c.day);
                for (auto& f : in.flt)   dset.push_back(f.pay_day);
                for (auto& o : in.ois)   { dset.push_back(o.pay_day); dset.push_back(o.a_day); dset.push_back(o.b_day); }
            }
            std::sort(dset.begin(), dset.end()); dset.erase(std::unique(dset.begin(), dset.end()), dset.end());
            auto col_of = [&](int d){ return (int)(std::lower_bound(dset.begin(), dset.end(), d) - dset.begin()); };
            G.col_amt.assign(dset.size(), Lane8{});
            // float / OIS coupons are keyed by their (pay, a, b) dates across the group: union, then per-lane scale
            std::vector<std::array<int,3>> fkeys, okeys;
            for (int l = 0; l < G.n_valid; ++l) { const InstrumentSpec& in = book[members[g + l]];
                for (auto& f : in.flt) fkeys.push_back({f.pay_day, f.a_day, f.b_day});
                for (auto& o : in.ois) okeys.push_back({o.pay_day, o.a_day, o.b_day}); }
            std::sort(fkeys.begin(), fkeys.end()); fkeys.erase(std::unique(fkeys.begin(), fkeys.end()), fkeys.end());
            std::sort(okeys.begin(), okeys.end()); okeys.erase(std::unique(okeys.begin(), okeys.end()), okeys.end());
            for (auto& k : fkeys) { G.f_di.push_back(k); G.f_col.push_back(col_of(k[0])); }
            for (auto& k : okeys) { G.o_di.push_back(k); G.o_col.push_back({col_of(k[0]), col_of(k[1]), col_of(k[2])}); }
            G.f_scale.assign(fkeys.size(), Lane8{}); G.o_N.assign(okeys.size(), Lane8{});
            auto fidx = [&](const std::array<int,3>& k){ return (int)(std::lower_bound(fkeys.begin(), fkeys.end(), k) - fkeys.begin()); };
            auto oidx = [&](const std::array<int,3>& k){ return (int)(std::lower_bound(okeys.begin(), okeys.end(), k) - okeys.begin()); };
            for (int l = 0; l < G.n_valid; ++l) {
                const InstrumentSpec& in = book[members[g + l]];
                for (auto& c : in.fixed) G.col_amt[col_of(c.day)].v[l] += c.amount;
                for (auto& f : in.flt) G.f_scale[fidx({f.pay_day, f.a_day, f.b_day})].v[l] += f.scale;
                for (auto& o : in.ois) G.o_N[oidx({o.pay_day, o.a_day, o.b_day})].v[l] += o.N;
            }
            for (size_t q = 0; q < G.o_col.size(); ++q) for (int kind = 0; kind < 3; ++kind) G.o_at.push_back({G.o_col[q][kind], (int)q * 3 + kind});
            std::sort(G.o_at.begin(), G.o_at.end());
            for (int d : dset) days.push_back(d);
            for (auto& f : G.f_di) { days.push_back(f[1]); days.push_back(f[2]); }
            col_day.push_back(dset);
            L.groups.push_back(std::move(G));
        }
    }
    std::sort(days.begin(), days.end()); days.erase(std::unique(days.begin(), days.end()), days.end());
    L.table.day = days;
    std::unordered_map<int,int> index; for (size_t i = 0; i < days.size(); ++i) index[days[i]] = (int)i;
    for (size_t gi = 0; gi < L.groups.size(); ++gi) {                              // day -> table index, once
        Group& G = L.groups[gi];
        for (int d : col_day[gi]) G.col_di.push_back(index[d]);
        for (auto& f : G.f_di) for (int& d : f) d = index[d];
        for (auto& o : G.o_di) for (int& d : o) d = index[d];
    }
    size_t n = days.size(); L.table.t.resize(n); L.table.D.resize(n); L.table.P.resize(n); L.table.bo.resize(n); L.table.bp.resize(n);
    for (size_t i = 0; i < n; ++i) L.table.t[i] = days[i] / days_per_year;
    return L;
}

// per curve update: discount factors and stencil intervals for every unique date (the only curve evaluation)
template <class DF, class PF>
inline void refresh_table(GroupLayout& L, const Stencils& Sd, const Stencils& Sp, DF df, PF pf)
{
    DateTable& T = L.table;
    for (size_t i = 0; i < T.day.size(); ++i) {
        double t = T.t[i]; T.D[i] = df(t); T.P[i] = pf(t); T.bo[i] = Sd.bucket(t); T.bp[i] = Sp.bucket(t);
    }
}

} // namespace ladder
