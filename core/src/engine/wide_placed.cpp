// Several modes at once: see wide_placed.h.
#include "dect2/engine/wide_placed.h"
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace dect2 {

std::string placedToText(const std::vector<Placed>& v) {
    std::ostringstream o;
    o.precision(15);
    for (const Placed& p : v) o << p.absHz << ' ' << p.widthHz << ' ' << (int)p.kind << '\n';
    return o.str();
}

std::vector<Placed> placedFromText(const std::string& s) {
    std::vector<Placed> v;
    std::istringstream in(s);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        Placed p;
        int k = 0;
        if (!(l >> p.absHz >> p.widthHz >> k) || p.absHz <= 0 || p.widthHz <= 0 || k < 0 || k >= (int)WsKind::Count) continue;
        p.kind = (WsKind)k;
        v.push_back(p);
    }
    return v;
}

bool placedInCapture(const Placed& p, double centerHz, double rateHz) {
    return rateHz > 0 && std::fabs(p.absHz - centerHz) + p.widthHz / 2 <= rateHz * 0.45;
}

void PlacedSync::apply(WideSession& ws, const std::vector<Placed>& v, double centerHz, double rateHz) {
    const std::string text = placedToText(v);
    if (text == lastText_ && centerHz == lastCenter_ && rateHz == lastRate_) return;
    const bool moved = centerHz != lastCenter_ || rateHz != lastRate_;
    lastText_ = text; lastCenter_ = centerHz; lastRate_ = rateHz;
    if (moved) {   // the entries are placed by their offset from the centre
        for (const auto& kv : ids_) ws.remove(kv.second);
        ids_.clear();
    }
    std::map<double, int> keep;
    for (const Placed& p : v) {
        if (!placedInCapture(p, centerHz, rateHz)) continue;
        auto it = ids_.find(p.absHz);
        if (it != ids_.end()) { keep[p.absHz] = it->second; ids_.erase(it); ws.setKind(keep[p.absHz], p.kind); continue; }
        keep[p.absHz] = ws.addManual(p.absHz - centerHz, p.widthHz, p.kind);
    }
    for (const auto& kv : ids_) ws.remove(kv.second);   // taken off the list
    ids_ = std::move(keep);
}

void PlacedSync::clear(WideSession& ws) {
    for (const auto& kv : ids_) ws.remove(kv.second);
    forget();
}

bool placedIsTap(double dxPx, double dyPx, double slopPx) { return std::fabs(dxPx) < slopPx && std::fabs(dyPx) < slopPx; }

int placedNearest(const std::vector<Placed>& v, double absHz, double tolHz) {
    int best = -1;
    double bd = 0;
    for (size_t i = 0; i < v.size(); i++) {
        const double d = std::fabs(v[i].absHz - absHz);
        if (d > std::max(tolHz, v[i].widthHz * 0.5)) continue;
        if (best < 0 || d < bd) { best = (int)i; bd = d; }
    }
    return best;
}

Placed placedAtClick(double absHz, double widthHz, WsKind kind) {
    Placed p;
    p.absHz = std::round(absHz / 100.0) * 100.0;
    p.widthHz = std::max(1000.0, widthHz);
    p.kind = kind;
    return p;
}

} // namespace dect2
