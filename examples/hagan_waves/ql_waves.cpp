#include "../example_dir.hpp"
// Reference built on QuantLib's own Examples (v1.33): MulticurveBootstrapping (Eonia + Euribor6M quotes,
// 11 Dec 2012, from Ametrano & Bianchetti figs 25/31) and Bonds (4.5% 2007-2017 bond, ZC Aug 2013).
// Change from the example: PiecewiseYieldCurve<Discount, Cubic> instead of <Discount, Cubic>.
// After bootstrap the node discount factors are frozen into InterpolatedDiscountCurve<Cubic>;
// risk is a central difference in theta_k = -log(D_k/D_{k-1}) per curve (nodes j >= k scaled by exp(-eps)).
//
// Output cashflows2.txt — generalised stream for scan2.cpp:
//   2 curves: name nnodes t0 D0 t1 D1 ...   (times Act/365F from today; first node is the curve reference)
//   ninst
//   name nterms nproj
//     C amount t                      fixed cashflow amount*D(t)
//     F scale a b p                   ibor coupon  scale*(P(a)/P(b)-1)*D(p)
//     O N a b p                       ois coupon   N*(D(a)/D(b)-1)*D(p)
//     P t a b scale amt_ql            projection-pass row (same as before)
#include <ql/quantlib.hpp>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace QuantLib;

struct Term { char kind; double v[4]; };
// Hagan box wave: instantaneous forward bumped by delta on [t0,t1) (times from the base curve's reference date)
class BoxShifted : public YieldTermStructure {
    Handle<YieldTermStructure> base_; Time t0_, t1_; Real delta_;
public:
    BoxShifted(Handle<YieldTermStructure> b, Time t0, Time t1, Real d) : YieldTermStructure(b->dayCounter()), base_(std::move(b)), t0_(t0), t1_(t1), delta_(d) { registerWith(base_); enableExtrapolation(); }
    Date maxDate() const override { return base_->maxDate(); }
    const Date& referenceDate() const override { return base_->referenceDate(); }
    Calendar calendar() const override { return base_->calendar(); }
    Natural settlementDays() const override { return base_->settlementDays(); }
protected:
    DiscountFactor discountImpl(Time t) const override { Real s = std::min(std::max(t - t0_, 0.0), t1_ - t0_); return base_->discount(t, true) * std::exp(-delta_ * s); }
};
struct Inst { std::string name; std::vector<Leg> legs; std::vector<int> legkind; /*0 fixed/bond,1 ibor,2 ois*/ std::vector<double> sign; };

int main()
{
    enter_example_dir();
    Calendar calendar = TARGET();
    Date todaysDate(11, December, 2012);
    Settings::instance().evaluationDate() = todaysDate;
    Integer fixingDays = 2;
    Date settlementDate = calendar.advance(todaysDate, fixingDays, Days);
    DayCounter tsDC = Actual365Fixed();
    DayCounter depositDayCounter = Actual360();

    // ================= EONIA CURVE (quotes verbatim from the example)
    std::vector<ext::shared_ptr<RateHelper>> eoniaInstruments; std::vector<ext::shared_ptr<SimpleQuote>> eoniaQ;
    auto eoniaBare = ext::make_shared<Eonia>();
    auto Q = [](std::vector<ext::shared_ptr<SimpleQuote>>& v, double q) { v.push_back(ext::make_shared<SimpleQuote>(q)); return Handle<Quote>(v.back()); };
    for (auto [sd, q] : std::vector<std::pair<Natural,double>>{{0,0.0004},{1,0.0004},{2,0.0004}})
        eoniaInstruments.push_back(ext::make_shared<DepositRateHelper>(Q(eoniaQ, q), 1*Days, sd, calendar, Following, false, depositDayCounter));
    for (auto [p, q] : std::vector<std::pair<Period,double>>{{1*Weeks,0.00070},{2*Weeks,0.00069},{3*Weeks,0.00078},{1*Months,0.00074}})
        eoniaInstruments.push_back(ext::make_shared<OISRateHelper>(2, p, Q(eoniaQ, q), eoniaBare));
    {
        std::vector<std::tuple<Date,Date,double>> dated = {
            {Date(16,January,2013),Date(13,February,2013),0.000460},{Date(13,February,2013),Date(13,March,2013),0.000160},
            {Date(13,March,2013),Date(10,April,2013),-0.000070},{Date(10,April,2013),Date(8,May,2013),-0.000130},
            {Date(8,May,2013),Date(12,June,2013),-0.000140}};
        for (auto& [a,b,q] : dated) eoniaInstruments.push_back(ext::make_shared<DatedOISRateHelper>(a, b, Q(eoniaQ, q), eoniaBare));
    }
    for (auto [p, q] : std::vector<std::pair<Period,double>>{{15*Months,0.00002},{18*Months,0.00008},{21*Months,0.00021},{2*Years,0.00036},{3*Years,0.00127},{4*Years,0.00274},{5*Years,0.00456},{6*Years,0.00647},{7*Years,0.00827},{8*Years,0.00996},{9*Years,0.01147},{10*Years,0.0128},{11*Years,0.01404},{12*Years,0.01516},{15*Years,0.01764},{20*Years,0.01939},{25*Years,0.02003},{30*Years,0.02038}})
        eoniaInstruments.push_back(ext::make_shared<OISRateHelper>(2, p, Q(eoniaQ, q), eoniaBare));

    auto eoniaBoot = ext::make_shared<PiecewiseYieldCurve<Discount, Cubic>>(todaysDate, eoniaInstruments, tsDC);
    eoniaBoot->enableExtrapolation();
    RelinkableHandle<YieldTermStructure> discH; discH.linkTo(eoniaBoot);

    // ================= EURIBOR 6M CURVE (quotes verbatim from the example)
    std::vector<ext::shared_ptr<RateHelper>> eurInstruments; std::vector<ext::shared_ptr<SimpleQuote>> eurQ;
    auto euribor6MBare = ext::make_shared<Euribor6M>();
    eurInstruments.push_back(ext::make_shared<DepositRateHelper>(Q(eurQ, 0.00312), 6*Months, 3, calendar, Following, false, depositDayCounter));
    {
        double fra[] = {0.002930,0.002720,0.002600,0.002560,0.002520,0.002480,0.002540,0.002610,0.002670,0.002790,0.002910,0.003030,0.003180,0.003350,0.003520,0.003710,0.003890,0.004090};
        for (int m = 1; m <= 18; ++m) eurInstruments.push_back(ext::make_shared<FraRateHelper>(Q(eurQ, fra[m-1]), m, euribor6MBare));
    }
    for (auto [p, q] : std::vector<std::pair<Period,double>>{{3*Years,0.004240},{4*Years,0.005760},{5*Years,0.007620},{6*Years,0.009540},{7*Years,0.011350},{8*Years,0.013030},{9*Years,0.014520},{10*Years,0.015840},{12*Years,0.018090},{15*Years,0.020370},{20*Years,0.021870},{25*Years,0.022340},{30*Years,0.022560},{35*Years,0.022950},{40*Years,0.023480},{50*Years,0.024210},{60*Years,0.024630}})
        eurInstruments.push_back(ext::make_shared<SwapRateHelper>(Q(eurQ, q), p, calendar, Annual, Unadjusted, Thirty360(Thirty360::European), euribor6MBare, Handle<Quote>(), 0*Days, discH));
    auto eurBoot = ext::make_shared<PiecewiseYieldCurve<Discount, Cubic>>(settlementDate, eurInstruments, tsDC, PiecewiseYieldCurve<Discount, Cubic>::bootstrap_type(1.0e-15));
    eurBoot->enableExtrapolation();

    // ================= freeze nodes (internal coordinates); handles now point at frozen curves
    auto nodes_of = [](const auto& boot) { std::vector<Date> d; std::vector<DiscountFactor> v; for (auto& n : boot->nodes()) { d.push_back(n.first); v.push_back(n.second); } return std::make_pair(d, v); };
    auto [eoniaDates, eoniaDfs] = nodes_of(eoniaBoot);
    auto [eurDates, eurDfs] = nodes_of(eurBoot);
    auto frozen = [&](const std::vector<Date>& d, const std::vector<DiscountFactor>& v) { auto c = ext::make_shared<InterpolatedDiscountCurve<Cubic>>(d, v, tsDC, calendar); c->enableExtrapolation(); return c; };
    auto bumped = [](const std::vector<DiscountFactor>& v, int k, double eps) { auto b = v; double f = std::exp(-eps); for (size_t j = k; j < b.size(); ++j) b[j] *= f; return b; };
    discH.linkTo(frozen(eoniaDates, eoniaDfs));
    RelinkableHandle<YieldTermStructure> projH(frozen(eurDates, eurDfs));
    {   // frozen == bootstrapped?
        double m = 0; for (int d = 1; d < 365*20; d += 37) { Date x = todaysDate + d; m = std::max(m, std::fabs(discH->discount(x)/eoniaBoot->discount(x) - 1)); if (x >= settlementDate) m = std::max(m, std::fabs(projH->discount(x)/eurBoot->discount(x) - 1)); }
        std::printf("frozen vs bootstrapped curves, max rel diff: %.2e   (Eonia %zu nodes, Euribor6M %zu nodes)\n", m, eoniaDates.size(), eurDates.size());
    }
    auto euribor = ext::make_shared<Euribor6M>(projH);
    auto eonia   = ext::make_shared<Eonia>(discH);
    const bool at_par = IborCoupon::Settings::instance().usingAtParCoupons();

    // ================= instruments
    std::vector<Inst> insts;
    std::vector<ext::shared_ptr<Instrument>> keep;
    Real nominal = 1000000.0;
    auto add_swap = [&](const std::string& name, const ext::shared_ptr<Swap>& s, int floatKind) {
        keep.push_back(s); insts.push_back({name, {s->leg(0), s->leg(1)}, {0, floatKind}, {s->payer(0) ? -1.0 : 1.0, s->payer(1) ? -1.0 : 1.0}});
    };
    // (1) the example's 5y spot payer swap and (2) its 5y, 1y-forward swap
    for (auto [name, start] : std::vector<std::pair<std::string,Date>>{{"swap_5y_example", settlementDate}, {"swap_1y5y_fwd_example", calendar.advance(settlementDate, 1, Years)}}) {
        Date maturity = calendar.advance(start, 5, Years);
        Schedule fs(start, maturity, Period(Annual), calendar, Unadjusted, Unadjusted, DateGeneration::Forward, false);
        Schedule fl(start, maturity, Period(Semiannual), calendar, ModifiedFollowing, ModifiedFollowing, DateGeneration::Forward, false);
        add_swap(name, ext::make_shared<VanillaSwap>(Swap::Payer, nominal, fs, 0.007, Thirty360(Thirty360::European), fl, euribor, 0.0, Actual360()), 1);
    }
    // (3) Bonds example: 4.5% semi-annual 15 May 2007 - 15 May 2017; (4) ZC 15 Aug 2013
    {
        Schedule bs(Date(15,May,2007), Date(15,May,2017), Period(Semiannual), UnitedStates(UnitedStates::GovernmentBond), Unadjusted, Unadjusted, DateGeneration::Backward, false);
        auto b = ext::make_shared<FixedRateBond>(3, 100.0, bs, std::vector<Rate>(1, 0.045), ActualActual(ActualActual::Bond), ModifiedFollowing, 100.0, Date(15,May,2007));
        keep.push_back(b); insts.push_back({"bond_fixed_4.5_example", {b->cashflows()}, {0}, {1.0}});
        auto z = ext::make_shared<ZeroCouponBond>(3, UnitedStates(UnitedStates::GovernmentBond), 100.0, Date(15,August,2013), Following, 100.0, Date(15,August,2003));
        keep.push_back(z); insts.push_back({"bond_zero_example", {z->cashflows()}, {0}, {1.0}});
    }
    // (4b) a 35-year 4% bond: cashflows beyond the Eonia curve's last pillar, exercising the last wave
    {
        Schedule bs(settlementDate, calendar.advance(settlementDate, 35, Years), Period(Annual), calendar, Unadjusted, Unadjusted, DateGeneration::Backward, false);
        auto b = ext::make_shared<FixedRateBond>(0, 1000000.0, bs, std::vector<Rate>(1, 0.04), Thirty360(Thirty360::European), Unadjusted, 100.0);
        keep.push_back(b); insts.push_back({"bond_35y_beyond_last_pillar", {b->cashflows()}, {0}, {1.0}});
    }
    // (5) OIS 5y on Eonia, annual, payment lag 2 days
    {
        auto ois = MakeOIS(5*Years, eonia, 0.005).withSettlementDays(2).withPaymentLag(2).withNominal(nominal).withType(Swap::Payer);
        ext::shared_ptr<OvernightIndexedSwap> s = ois;
        add_swap("ois_5y_lag2", s, 2);
    }
    // (6) amortising 7y: notional steps down linearly each year
    {
        Date start = settlementDate, maturity = calendar.advance(start, 7, Years);
        Schedule fs(start, maturity, Period(Annual), calendar, Unadjusted, Unadjusted, DateGeneration::Forward, false);
        Schedule fl(start, maturity, Period(Semiannual), calendar, ModifiedFollowing, ModifiedFollowing, DateGeneration::Forward, false);
        std::vector<Real> nf, nl;
        for (int i = 0; i < 7; ++i) nf.push_back(nominal * (1.0 - i/7.0));
        for (int j = 0; j < 14; ++j) nl.push_back(nominal * (1.0 - (j/2)/7.0));
        Leg fixed = FixedRateLeg(fs).withNotionals(nf).withCouponRates(0.012, Thirty360(Thirty360::European)).withPaymentAdjustment(Unadjusted);
        Leg flt   = IborLeg(fl, euribor).withNotionals(nl).withPaymentDayCounter(Actual360()).withPaymentAdjustment(ModifiedFollowing);
        add_swap("swap_amort_7y", ext::make_shared<Swap>(fixed, flt), 1);
    }
    // (7) 6y with a 2-month front stub and a 25bp floating spread
    {
        Date start = settlementDate, maturity = calendar.advance(start, 6, Years), first = calendar.advance(start, 2, Months);
        Schedule fs(start, maturity, Period(Annual), calendar, Unadjusted, Unadjusted, DateGeneration::Forward, false, first);
        Schedule fl(start, maturity, Period(Semiannual), calendar, ModifiedFollowing, ModifiedFollowing, DateGeneration::Forward, false, first);
        Leg fixed = FixedRateLeg(fs).withNotionals(nominal).withCouponRates(0.011, Thirty360(Thirty360::European)).withPaymentAdjustment(Unadjusted);
        Leg flt   = IborLeg(fl, euribor).withNotionals(nominal).withPaymentDayCounter(Actual360()).withSpreads(0.0025).withPaymentAdjustment(ModifiedFollowing);
        add_swap("swap_stub_spread_6y", ext::make_shared<Swap>(fixed, flt), 1);
    }

        auto leg_npv = [&](const Leg& leg, const YieldTermStructure& c) { return CashFlows::npv(leg, c, false, todaysDate, todaysDate); };
    auto pv_all  = [&](const Inst& in, const YieldTermStructure& c) { double s = 0; for (size_t l = 0; l < in.legs.size(); ++l) s += in.sign[l] * leg_npv(in.legs[l], c); return s; };

    // ================= dump generalised cashflows
    {
        FILE* f = std::fopen("./cashflows_wave.txt", "w");
        auto dump_curve = [&](const char* nm, const std::vector<Date>& d, const std::vector<DiscountFactor>& v) {
            std::fprintf(f, "%s %zu", nm, d.size()); for (size_t i = 0; i < d.size(); ++i) std::fprintf(f, " %.17g %.17g", tsDC.yearFraction(todaysDate, d[i]), v[i]); std::fprintf(f, "\n"); };
        std::fprintf(f, "2\n"); dump_curve("ois", eoniaDates, eoniaDfs); dump_curve("proj", eurDates, eurDfs);
        std::fprintf(f, "%zu\n", insts.size());
        int on_node = 0;
        for (auto& in : insts) {
            std::vector<Term> terms; std::vector<std::array<double,5>> prows;
            for (size_t l = 0; l < in.legs.size(); ++l) for (auto& cf : in.legs[l]) {
                if (cf->date() <= todaysDate) continue;
                double t = tsDC.yearFraction(todaysDate, cf->date());
                for (auto& nd : eoniaDates) if (nd == cf->date()) ++on_node;
                const double sg = in.sign[l];
                if (in.legkind[l] == 0) terms.push_back({'C', {sg * cf->amount(), t, 0, 0}});
                else if (in.legkind[l] == 1) {
                    auto c = ext::dynamic_pointer_cast<IborCoupon>(cf);
                    Date a = at_par ? c->accrualStartDate() : euribor->valueDate(c->fixingDate());
                    Date b = at_par ? c->accrualEndDate()   : euribor->maturityDate(a);
                    double tau_idx = euribor->dayCounter().yearFraction(a, b);
                    double scale = c->nominal() * c->accrualPeriod() / tau_idx;
                    double ta = tsDC.yearFraction(todaysDate, a), tb = tsDC.yearFraction(todaysDate, b);
                    terms.push_back({'F', {sg * scale, ta, tb, t}});
                    if (c->spread() != 0.0) terms.push_back({'C', {sg * c->nominal() * c->accrualPeriod() * c->spread(), t, 0, 0}});
                    prows.push_back({t, ta, tb, sg * scale, sg * (cf->amount() - c->nominal() * c->accrualPeriod() * c->spread())});
                } else {
                    auto c = ext::dynamic_pointer_cast<OvernightIndexedCoupon>(cf);
                    double ta = tsDC.yearFraction(todaysDate, c->valueDates().front()), tb = tsDC.yearFraction(todaysDate, c->valueDates().back());
                    terms.push_back({'O', {sg * c->nominal() * c->accrualPeriod() / c->dayCounter().yearFraction(c->valueDates().front(), c->valueDates().back()), ta, tb, t}});
                    // amount check: N*(D(a)/D(b)-1)*accrual/tau_idx vs QL
                    double amt = c->nominal() * c->accrualPeriod() / c->dayCounter().yearFraction(c->valueDates().front(), c->valueDates().back()) * (discH->discount(c->valueDates().front())/discH->discount(c->valueDates().back()) - 1.0);
                    static double maxo = 0; maxo = std::max(maxo, std::fabs(amt/cf->amount() - 1)); if (&cf == &in.legs[l].back()) std::printf("ois coupon amount vs QL compounding, max rel diff %.2e\n", maxo);
                }
            }
            std::fprintf(f, "%s %zu %zu\n", in.name.c_str(), terms.size(), prows.size());
            for (auto& tm : terms) std::fprintf(f, "%c %.17g %.17g %.17g %.17g\n", tm.kind, tm.v[0], tm.v[1], tm.v[2], tm.v[3]);
            for (auto& r : prows) std::fprintf(f, "P %.17g %.17g %.17g %.17g %.17g\n", r[0], r[1], r[2], r[3], r[4]);
        }
        std::fclose(f);
        std::printf("payment dates coinciding with an Eonia node: %d\n", on_node);
    }

    // ================= box-wave risk: QuantLib central difference on the shipped cubic curves
    // wave k on a curve = forward bump on [node_{k-1}, node_k), times from that curve's reference date
    auto times_from_ref = [&](const std::vector<Date>& d) { std::vector<Time> t; for (auto& x : d) t.push_back(tsDC.yearFraction(d[0], x)); return t; };
    auto tO = times_from_ref(eoniaDates), tP = times_from_ref(eurDates);
    // optional non-pillar reporting grid (Hagan: the risk grid is independent of the calibration grid):
    // risk_grid.txt holds boundary times in years from today, first entry 0; applied to both curves
    if (FILE* rg = std::fopen("./risk_grid.txt", "r")) {
        std::vector<Time> g; double v; while (std::fscanf(rg, "%lf", &v) == 1) g.push_back(v); std::fclose(rg);
        tO = g; tP.clear(); for (double x : g) tP.push_back(std::max(0.0, x - tsDC.yearFraction(todaysDate, settlementDate)));   // Euribor times from its own reference
        std::printf("risk grid from risk_grid.txt: %zu boundaries (non-pillar)\n", g.size());
    }
    const int Ko = (int)tO.size() - 1, Kp = (int)tP.size() - 1;
    auto baseO = ext::shared_ptr<YieldTermStructure>(discH.currentLink()), baseP = ext::shared_ptr<YieldTermStructure>(projH.currentLink());
    Handle<YieldTermStructure> hO(baseO), hP(baseP);
    auto wave_risk = [&](const std::vector<const Inst*>& items, double eps, std::vector<std::vector<double>>& out) {
        out.assign(items.size(), std::vector<double>(Ko + Kp, 0.0));
        const Time INF = 1e9;   // Hagan 2.2b: the last wave extends flat beyond the final maturity
        for (int k = 1; k <= Ko; ++k) {
            discH.linkTo(ext::make_shared<BoxShifted>(hO, tO[k-1], k == Ko ? INF : tO[k], +eps)); std::vector<double> up; for (auto* in : items) up.push_back(pv_all(*in, **discH));
            discH.linkTo(ext::make_shared<BoxShifted>(hO, tO[k-1], k == Ko ? INF : tO[k], -eps)); for (size_t i = 0; i < items.size(); ++i) out[i][k-1] = (up[i] - pv_all(*items[i], **discH)) / (2*eps);
        }
        discH.linkTo(baseO);
        for (int k = 1; k <= Kp; ++k) {
            projH.linkTo(ext::make_shared<BoxShifted>(hP, tP[k-1], k == Kp ? INF : tP[k], +eps)); std::vector<double> up; for (auto* in : items) up.push_back(pv_all(*in, **discH));
            projH.linkTo(ext::make_shared<BoxShifted>(hP, tP[k-1], k == Kp ? INF : tP[k], -eps)); for (size_t i = 0; i < items.size(); ++i) out[i][Ko + k-1] = (up[i] - pv_all(*items[i], **discH)) / (2*eps);
        }
        projH.linkTo(baseP);
    };
    std::vector<const Inst*> book; for (auto& in : insts) book.push_back(&in);
    std::vector<std::vector<double>> R4, R2; wave_risk(book, 1e-4, R4); wave_risk(book, 5e-5, R2);
    auto dump = [&](const char* path, const std::vector<std::vector<double>>& R) { FILE* f = std::fopen(path, "w"); for (auto& r : R) { for (double v : r) std::fprintf(f, "%.17g ", v); std::fprintf(f, "\n"); } std::fclose(f); };
    dump("./wave_risk_0.0001.txt", R4); dump("./wave_risk_5e-05.txt", R2);
    for (double e : {4e-4, 2e-4, 2.5e-5}) { std::vector<std::vector<double>> R; wave_risk(book, e, R); char nm[64]; std::snprintf(nm, sizeof nm, "./wave_risk_%g.txt", e); dump(nm, R); }

    // ================= hedge instruments: the example's par OIS (Eonia waves) and par Euribor swaps (Euribor waves), 1-10y
    std::vector<Inst> hedges; std::vector<std::string> hnames;
    for (auto [p, q] : std::vector<std::pair<Period,double>>{{2*Years,0.00036},{3*Years,0.00127},{4*Years,0.00274},{5*Years,0.00456},{6*Years,0.00647},{7*Years,0.00827},{8*Years,0.00996},{9*Years,0.01147},{10*Years,0.0128}}) {
        ext::shared_ptr<OvernightIndexedSwap> s = MakeOIS(p, eonia, q).withSettlementDays(2).withNominal(1e6).withType(Swap::Receiver);
        keep.push_back(s); hedges.push_back({"OIS " + std::to_string(p.length()) + "y", {s->leg(0), s->leg(1)}, {0, 2}, {s->payer(0) ? -1.0 : 1.0, s->payer(1) ? -1.0 : 1.0}}); hnames.push_back(hedges.back().name);
    }
    for (auto [p, q] : std::vector<std::pair<Period,double>>{{3*Years,0.004240},{4*Years,0.005760},{5*Years,0.007620},{6*Years,0.009540},{7*Years,0.011350},{8*Years,0.013030},{9*Years,0.014520},{10*Years,0.015840}}) {
        ext::shared_ptr<VanillaSwap> s = MakeVanillaSwap(p, euribor, q).withSettlementDays(2).withNominal(1e6).withType(Swap::Receiver).withFixedLegDayCount(Thirty360(Thirty360::European)).withFixedLegTenor(1*Years).withFixedLegConvention(Unadjusted).withDiscountingTermStructure(discH);
        keep.push_back(s); hedges.push_back({"IRS " + std::to_string(p.length()) + "y", {s->leg(0), s->leg(1)}, {0, 1}, {s->payer(0) ? -1.0 : 1.0, s->payer(1) ? -1.0 : 1.0}}); hnames.push_back(hedges.back().name);
    }
    std::vector<const Inst*> hp; for (auto& h : hedges) hp.push_back(&h);
    std::vector<std::vector<double>> HR; wave_risk(hp, 1e-4, HR);
    { FILE* f = std::fopen("./wave_hedges.txt", "w"); for (size_t i = 0; i < hedges.size(); ++i) { std::fprintf(f, "%s", hnames[i].c_str()); for (double v : HR[i]) std::fprintf(f, " %.17g", v); std::fprintf(f, "\n"); } std::fclose(f); }
    { FILE* f = std::fopen("./wave_buckets.txt", "w"); const double ts = tsDC.yearFraction(todaysDate, settlementDate);
      for (double t : tO) std::fprintf(f, "%.17g ", t); std::fprintf(f, "\n");                 // Eonia reference = today
      for (double t : tP) std::fprintf(f, "%.17g ", t + ts); std::fprintf(f, "\n"); std::fclose(f); }   // Euribor: back to today's basis

    // ================= unit cashflows priced on the shipped cubic curves (input to scan_wave.cpp)
    {
        FILE* f = std::fopen("./unit_cashflows_wave.txt", "w");
        std::fprintf(f, "%zu\n", insts.size());
        for (auto& in : insts) {
            std::vector<std::pair<double,double>> ent; std::vector<std::array<double,4>> prow;
            for (size_t l = 0; l < in.legs.size(); ++l) for (auto& cf : in.legs[l]) {
                if (cf->date() <= todaysDate) continue;
                const double sg = in.sign[l]; double t = tsDC.yearFraction(todaysDate, cf->date()); double Dp = discH->discount(cf->date());
                if (in.legkind[l] == 0) ent.push_back({t, sg * cf->amount() * Dp});
                else if (in.legkind[l] == 1) {
                    auto c = ext::dynamic_pointer_cast<IborCoupon>(cf);
                    Date a = at_par ? c->accrualStartDate() : euribor->valueDate(c->fixingDate()), b = at_par ? c->accrualEndDate() : euribor->maturityDate(a);
                    double scale = c->nominal() * c->accrualPeriod() / euribor->dayCounter().yearFraction(a, b);
                    ent.push_back({t, sg * cf->amount() * Dp});                                         // discount-curve unit cashflow of the projected coupon
                    prow.push_back({t, tsDC.yearFraction(todaysDate, a), tsDC.yearFraction(todaysDate, b), sg * scale * Dp * projH->discount(a) / projH->discount(b)});
                } else {
                    auto c = ext::dynamic_pointer_cast<OvernightIndexedCoupon>(cf);
                    Date a = c->valueDates().front(), b = c->valueDates().back();
                    double N = sg * c->nominal() * c->accrualPeriod() / c->dayCounter().yearFraction(a, b);
                    double x = N * Dp * discH->discount(a) / discH->discount(b);
                    ent.push_back({t, x}); ent.push_back({tsDC.yearFraction(todaysDate, a), x}); ent.push_back({tsDC.yearFraction(todaysDate, b), -x}); ent.push_back({t, -N * Dp});
                }
            }
            std::fprintf(f, "%s %zu %zu\n", in.name.c_str(), ent.size(), prow.size());
            for (auto& e : ent) std::fprintf(f, "%.17g %.17g\n", e.first, e.second);
            for (auto& r : prow) std::fprintf(f, "%.17g %.17g %.17g %.17g\n", r[0], r[1], r[2], r[3]);
        }
        std::fclose(f);
    }
    std::printf("box-wave reference on cubic curves: %zu book instruments, %zu hedges, %d+%d waves\n", insts.size(), hedges.size(), Ko, Kp);
    for (auto& in : insts) std::printf("  %-26s PV %14.4f\n", in.name.c_str(), pv_all(in, **discH));
    return 0;
}
