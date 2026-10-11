#include "../example_dir.hpp"
// Replay first-order risk weights. This is an immutable reference run.
#include "ladder/scan.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
using namespace ladder;
namespace {
template<class T> T read(std::istream& in, const char* what) {
    T value{};
    if (!(in >> value)) throw std::runtime_error(std::string("missing or malformed ") + what);
    return value;
}
int count(std::istream& in, const char* what) {
    int n=read<int>(in,what);
    if (n<0 || n>1000000) throw std::runtime_error(std::string("invalid count: ")+what);
    return n;
}
Stencils row(std::istream& in) {
    std::string line;
    if(!std::getline(in,line)) throw std::runtime_error("missing wave boundary row");
    std::istringstream values(line); Stencils s;
    for(double x; values >> x;) s.B.push_back(x);
    if(!values.eof()) throw std::runtime_error("malformed wave boundary row");
    s.open_last=true; s.validate(); return s;
}
void end(std::istream& in) {
    std::string token;
    if(in >> token) throw std::runtime_error("unexpected trailing input");
}
}
int main(int argc, char** argv) try {
    if(argc>2) throw std::runtime_error("usage: scan_wave [data-and-output-directory]");
    enter_example_dir(argc==2 ? argv[1] : nullptr);
    std::ifstream buckets("wave_buckets.txt");
    if(!buckets) throw std::runtime_error("cannot open wave_buckets.txt");
    auto sd=row(buckets), sp=row(buckets); end(buckets);
    std::ifstream in("unit_cashflows_wave.txt");
    if(!in) throw std::runtime_error("cannot open unit_cashflows_wave.txt");
    const int n=count(in,"instrument count");
    std::vector<std::vector<double>> results;
    for(int i=0;i<n;++i) {
        const auto name=read<std::string>(in,"instrument name");
        const int ne=count(in,"discount count"), np=count(in,"projection count");
        std::vector<UnitCashflow> cash(ne);
        for(auto& c:cash) { c.t=read<double>(in,"cashflow time"); c.x=read<double>(in,"cashflow weight"); }
        std::vector<ProjectionTerm> proj(np);
        for(auto& p:proj) {
            p.t_pay=read<double>(in,"payment time"); p.a=read<double>(in,"fixing start");
            p.b=read<double>(in,"fixing end"); p.w=read<double>(in,"projection weight");
        }
        sort_by_time(cash);
        std::vector<double> risk(sd.K()+sp.K());
        scan_discount(sd,cash,risk.data()); scan_projection(sp,proj,risk.data()+sd.K());
        for(double v:risk) if(!std::isfinite(v)) throw std::runtime_error("non-finite risk for "+name);
        results.push_back(std::move(risk));
    }
    end(in); // validate the complete input before opening the output
    std::ofstream out("scan_wave_risk.txt");
    if(!out) throw std::runtime_error("cannot write scan_wave_risk.txt");
    out << std::setprecision(17);
    for(const auto& risk:results) { for(double v:risk) out << v << ' '; out << '\n'; }
    out.close();
    if(!out) throw std::runtime_error("failed writing scan_wave_risk.txt");
    std::cout << "scan_wave: " << n << " instruments, " << sd.K() << '+' << sp.K() << " waves\n";
    return 0;
} catch(const std::exception& e) {
    std::cerr << "scan_wave: " << e.what() << '\n'; return 1;
}
