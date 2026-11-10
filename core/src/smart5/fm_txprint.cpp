// FM transmitter fingerprint (see fm_txprint.h).
#include "dect2/smart5/fm_txprint.h"
#include <algorithm>
#include <cstdio>
#include <sstream>

namespace dect2 {
namespace smart5 {

bool FmTxPrint::matches(const FmTxSignature& g, const FmPrintSample& s) const {
    if (std::fabs(g.cfoHz - s.cfoHz) > p_.cfoTolHz) return false;
    if (std::fabs(g.pilotPct - s.pilotPct) > p_.pilotTolPct) return false;
    if (g.mpxDbr > -90 && s.mpxDbr > -90 && std::fabs(g.mpxDbr - s.mpxDbr) > p_.mpxTolDb) return false;
    return true;
}

int FmTxPrint::observe(int pi, double f, const FmPrintSample& s) {
    Slot& sl = slots_[key(pi, f)];
    for (size_t i = 0; i < sl.sigs.size(); i++) {
        FmTxSignature& g = sl.sigs[i];
        if (!matches(g, s)) continue;
        const int n = std::min(g.n, 1000) + 1;
        const double dc = s.cfoHz - g.cfoHz, dp = s.pilotPct - g.pilotPct;
        g.cfoHz += dc / n; g.pilotPct += dp / n;
        g.sdCfo = std::sqrt(((n - 1) * g.sdCfo * g.sdCfo + dc * (s.cfoHz - g.cfoHz)) / n);
        g.sdPilot = std::sqrt(((n - 1) * g.sdPilot * g.sdPilot + dp * (s.pilotPct - g.pilotPct)) / n);
        if (s.mpxDbr > -90) g.mpxDbr = g.mpxDbr > -90 ? g.mpxDbr + (s.mpxDbr - g.mpxDbr) / n : s.mpxDbr;
        g.n++;
        sl.cur = (int)i;
        sl.pend.clear();
        return g.id;
    }
    sl.pend.push_back(s);
    if ((int)sl.pend.size() >= p_.learnReports) {
        FmTxSignature g;
        double mc = 0, mp = 0, mm = 0; int nm = 0;
        for (auto& x : sl.pend) { mc += x.cfoHz; mp += x.pilotPct; if (x.mpxDbr > -90) { mm += x.mpxDbr; nm++; } }
        mc /= sl.pend.size(); mp /= sl.pend.size();
        double vc = 0, vp = 0;
        for (auto& x : sl.pend) { vc += (x.cfoHz - mc) * (x.cfoHz - mc); vp += (x.pilotPct - mp) * (x.pilotPct - mp); }
        vc = std::sqrt(vc / sl.pend.size()); vp = std::sqrt(vp / sl.pend.size());
        if (vc <= p_.cfoTolHz / 2 && vp <= p_.pilotTolPct / 2) {   // a steady new transmitter
            g.id = (int)sl.sigs.size() + 1; g.n = (int)sl.pend.size(); g.cfoHz = mc; g.pilotPct = mp; g.mpxDbr = nm ? mm / nm : -99;
            g.sdCfo = vc; g.sdPilot = vp;
            sl.sigs.push_back(g);
            sl.cur = (int)sl.sigs.size() - 1;
            sl.pend.clear();
            return g.id;
        }
        sl.pend.erase(sl.pend.begin());   // not steady yet (fading between two): keep sliding
    }
    return sl.cur >= 0 ? sl.sigs[sl.cur].id : 0;
}

int FmTxPrint::current(int pi, double f) const {
    auto it = slots_.find(key(pi, f));
    return it == slots_.end() || it->second.cur < 0 ? -1 : it->second.sigs[it->second.cur].id;
}
bool FmTxPrint::changed(int pi, double f) const { return current(pi, f) > 1; }
const std::vector<FmTxSignature>* FmTxPrint::signatures(int pi, double f) const {
    auto it = slots_.find(key(pi, f));
    return it == slots_.end() ? nullptr : &it->second.sigs;
}
void FmTxPrint::setLabel(int pi, double f, int id, const std::string& name) {
    auto it = slots_.find(key(pi, f));
    if (it == slots_.end()) return;
    for (auto& g : it->second.sigs) if (g.id == id) g.label = name;
}

std::string FmTxPrint::save() const {
    std::ostringstream o;
    char b[160];
    for (auto& kv : slots_)
        for (auto& g : kv.second.sigs) {
            snprintf(b, sizeof b, "%04X\t%.1f\t%d\t%d\t%.2f\t%.3f\t%.2f\t%.2f\t%.3f\t", kv.first.pi, kv.first.f10 / 10.0, g.id, g.n, g.cfoHz, g.pilotPct, g.mpxDbr, g.sdCfo, g.sdPilot);
            o << b << g.label << "\n";
        }
    return o.str();
}

void FmTxPrint::load(const std::string& text) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        unsigned pi; double f; FmTxSignature g; char lab[128] = {0};
        if (sscanf(line.c_str(), "%x\t%lf\t%d\t%d\t%lf\t%lf\t%lf\t%lf\t%lf\t%127[^\n]", &pi, &f, &g.id, &g.n, &g.cfoHz, &g.pilotPct, &g.mpxDbr, &g.sdCfo, &g.sdPilot, lab) < 9) continue;
        g.label = lab;
        slots_[key((int)pi, f)].sigs.push_back(g);
    }
}

} // namespace smart5
} // namespace dect2
