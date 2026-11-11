// ham.decode events: see ham_events.h.
#include "dect2/ham/ham_events.h"
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2::ham {

namespace {
bool gridLatLon(const std::string& g0, double& lat, double& lon) {   // Maidenhead, 4 or 6 characters, the middle of the square
    if (g0.size() != 4 && g0.size() != 6) return false;
    std::string g = g0;
    for (size_t i = 0; i < g.size(); i++) g[i] = (char)(i < 2 || i >= 4 ? std::toupper((unsigned char)g[i]) : g[i]);
    if (g[0] < 'A' || g[0] > 'R' || g[1] < 'A' || g[1] > 'R' || g[2] < '0' || g[2] > '9' || g[3] < '0' || g[3] > '9') return false;
    if (g == "RR73") return false;   // a message, not a square
    lon = (g[0] - 'A') * 20.0 - 180 + (g[2] - '0') * 2.0;
    lat = (g[1] - 'A') * 10.0 - 90 + (g[3] - '0') * 1.0;
    if (g.size() == 6) {
        if (g[4] < 'A' || g[4] > 'X' || g[5] < 'A' || g[5] > 'X') return false;
        lon += (g[4] - 'A') * (2.0 / 24) + 1.0 / 24;
        lat += (g[5] - 'A') * (1.0 / 24) + 0.5 / 24;
    } else { lon += 1.0; lat += 0.5; }
    return true;
}
}  // namespace

automation::Event hamDecodeEvent(const HamHeard& h, const DecodeMarks& m) {
    automation::Event e("ham.decode");
    e.at(h.utc, h.mode, h.freqHz);
    e.str("from", h.call).str("call", h.call).str("grid", h.grid).str("text", h.msg).str("mode", h.mode).str("band", h.band).str("country", h.country);
    e.num("snr_db", h.snrDb).num("dt", h.dt).num("audio_hz", h.audioHz).num("dial_mhz", h.freqHz / 1e6).flag("cq", h.cq);
    if (h.dbm >= 0) e.num("dbm", h.dbm);
    double la, lo;
    if (gridLatLon(h.grid, la, lo)) e.num("lat", la).num("lon", lo);
    e.flag("new_dxcc", m.newDxcc).flag("new_dxcc_band", m.newDxccBand).flag("new_grid", m.newGrid).flag("new_grid_band", m.newGridBand)
     .flag("worked", m.workedCall).flag("worked_band", m.workedCallBand);
    return e;
}

bool HamDecodeOnce::first(const std::string& call, const std::string& mode, const std::string& msg, double slotUtc) {
    char b[32];
    std::snprintf(b, sizeof b, "%.0f", std::floor(slotUtc));
    const std::string k = call + "|" + mode + "|" + msg + "|" + b;
    if (!seen_.insert(k).second) return false;
    order_.push_back(k);
    if (order_.size() > 5000) { seen_.erase(order_.front()); order_.pop_front(); }
    return true;
}

bool HamCallOnce::first(const std::string& key, double now, double holdSec) {
    while (!recent_.empty() && now - recent_.front().second > holdSec) recent_.pop_front();
    for (const auto& r : recent_) if (r.first == key) return false;
    recent_.push_back({key, now});
    if (recent_.size() > 2000) recent_.pop_front();
    return true;
}

}  // namespace dect2::ham
