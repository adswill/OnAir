// Wi-Fi client view (wifi_clients.h). Frame control and address layout: IEEE 802.11-2020, clause 9.2-9.3.
#include "dect2/wifi/wifi_clients.h"
#include <algorithm>
#include <cstdio>

namespace dect2 {

std::string WifiClientTracker::tagOf(const uint8_t* m) const {
    uint32_t h = 2166136261u ^ salt_;
    for (int i = 0; i < 6; i++) { h ^= m[i]; h *= 16777619u; }
    char b[12]; snprintf(b, sizeof b, "c-%06x", h & 0xFFFFFF);
    return b;
}

void WifiClientTracker::clear() { cl_.clear(); al_.clear(); deauths_ = eapol_ = 0; win_.clear(); }

WifiClientInfo& WifiClientTracker::client(const std::string& tag, int64_t wall) {
    auto it = std::find_if(cl_.begin(), cl_.end(), [&](const WifiClientInfo& c) { return c.tag == tag; });
    if (it == cl_.end()) {
        if (cl_.size() >= 256) { auto oldest = std::min_element(cl_.begin(), cl_.end(), [](const auto& a, const auto& b) { return a.lastWall < b.lastWall; }); *oldest = WifiClientInfo(); it = oldest; }
        else { cl_.push_back(WifiClientInfo()); it = cl_.end() - 1; }
        it->tag = tag;
    }
    it->lastWall = wall;
    return *it;
}

void WifiClientTracker::observe(const uint8_t* p, size_t n, const std::string& bssid, int64_t wall, double sec) {
    if (n < 24) return;
    const int fc0 = p[0], fc1 = p[1];
    const int type = (fc0 >> 2) & 3, subtype = (fc0 >> 4) & 15;
    const uint8_t* a1 = p + 4, *a2 = p + 10;
    auto broadcast = [](const uint8_t* m) { return (m[0] & 1) != 0; };
    if (type == 0) {
        if (subtype == 4) {                            // probe request
            if (broadcast(a2)) return;
            auto& c = client(tagOf(a2), wall);
            c.probeFrames++;
            if (n >= 26) {                              // a wildcard probe has an empty SSID element: it names nothing
                size_t i = 24;
                while (i + 2 <= n) {
                    const int id = p[i], len = p[i + 1];
                    if (i + 2 + (size_t)len > n) break;
                    if (id == 0 && len > 0 && len <= 32) {
                        std::string ssid((const char*)p + i + 2, (size_t)len);
                        auto& v = c.probes;
                        if (std::find(v.begin(), v.end(), ssid) == v.end()) { v.push_back(ssid); if (v.size() > 12) v.erase(v.begin()); }
                    }
                    i += 2 + (size_t)len;
                }
            }
        } else if (subtype == 12 || subtype == 10) {  // deauthentication, disassociation
            deauths_++;
            const std::string key = bssid.empty() ? std::string("broadcast") : bssid;
            auto it = std::find_if(win_.begin(), win_.end(), [&](const Win& w) { return w.key == key; });
            if (it == win_.end()) { win_.push_back(Win{key, sec, 0, -1e9}); it = win_.end() - 1; }
            if (sec - it->t0 > 10.0) { it->t0 = sec; it->count = 0; }
            it->count++;
            if (it->count >= 15 && sec - it->lastAlarm > 60.0) {
                WifiFloodAlarm a; a.wallTime = wall; a.bssid = key; a.frames = it->count;
                al_.insert(al_.begin(), a);
                if (al_.size() > 50) al_.pop_back();
                it->lastAlarm = sec;
            }
        }
        return;
    }
    if (type != 2) return;
    const bool toDs = fc1 & 1, fromDs = fc1 & 2;
    if (toDs == fromDs) return;                         // ad hoc and WDS frames: no client role
    const uint8_t* sta = toDs ? a2 : a1;
    if (!broadcast(sta)) {
        auto& c = client(tagOf(sta), wall);
        c.dataFrames++;
        if (!bssid.empty()) c.withAp = bssid;
    }
    // LLC SNAP with ethertype 0x888E (EAPOL), type 3 = EAPOL-Key: counted only
    const size_t hdr = 24 + ((subtype & 8) ? 2u : 0u) + ((fc1 & 0x80) ? 4u : 0u);
    if (n >= hdr + 8 + 2) {
        const uint8_t* l = p + hdr;
        if (!(fc1 & 0x40) && l[0] == 0xAA && l[1] == 0xAA && l[2] == 3 && l[6] == 0x88 && l[7] == 0x8E && l[9] == 3) eapol_++;
    }
}

} // namespace dect2
