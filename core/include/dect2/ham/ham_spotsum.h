// Spot-based propagation summary (4.0): from the stations the user's own receiver decoded (FT8, FT4, WSPR, JS8) per band, in the last window:
// how many, the median and best SNR, the farthest, and the directions open (eight 45 degree sectors with a count), so "20 m: 41 spots, open to the
// south east, 7500 km" is one line. Pure code. Complements band_open.h, which counts by hour of day over weeks; this is the last half hour.
#pragma once
#include "dect2/track/track_geo.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct HamSpot { std::string band; double timeS = 0; double snrDb = 0; double lat = 0, lon = 0; bool hasPos = false; };

struct BandSummary {
    std::string band;
    int spots = 0;
    double medianSnrDb = 0, bestSnrDb = -99, farthestKm = 0;
    int sector[8] = {0};          // 0 = N .. 7 = NW
    int openSectors = 0;          // sectors with at least minPerSector spots
    std::string line;             // "20m: 41 spots, farthest 7500 km, open to SE, S"
};

class HamSpotSummary {
public:
    void setStation(double lat, double lon) { lat_ = lat; lon_ = lon; has_ = true; }
    void add(const HamSpot& s) { v_.push_back(s); if (v_.size() > 20000) v_.erase(v_.begin(), v_.begin() + 5000); }
    std::vector<BandSummary> summary(double nowS, double windowS, int minPerSector = 2) const {
        std::map<std::string, std::vector<const HamSpot*>> by;
        for (const auto& s : v_) if (s.timeS >= nowS - windowS && s.timeS <= nowS) by[s.band].push_back(&s);
        static const char* dir[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        std::vector<BandSummary> out;
        for (auto& kv : by) {
            BandSummary b; b.band = kv.first; b.spots = (int)kv.second.size();
            std::vector<double> snr;
            for (const HamSpot* s : kv.second) {
                snr.push_back(s->snrDb); b.bestSnrDb = std::max(b.bestSnrDb, s->snrDb);
                if (has_ && s->hasPos) {
                    b.farthestKm = std::max(b.farthestKm, track::gcDistM(lat_, lon_, s->lat, s->lon) / 1000.0);
                    b.sector[((int)((track::gcBearingDeg(lat_, lon_, s->lat, s->lon) + 22.5) / 45.0)) & 7]++;
                }
            }
            std::sort(snr.begin(), snr.end()); b.medianSnrDb = snr[snr.size() / 2];
            std::string open;
            for (int i = 0; i < 8; i++) if (b.sector[i] >= minPerSector) { b.openSectors++; open += (open.empty() ? "" : ", "); open += dir[i]; }
            char t[200];
            std::snprintf(t, sizeof t, "%s: %d spots, median %.0f dB", b.band.c_str(), b.spots, b.medianSnrDb);
            b.line = t;
            if (b.farthestKm > 0) { std::snprintf(t, sizeof t, ", farthest %.0f km", b.farthestKm); b.line += t; }
            if (!open.empty()) b.line += ", open to " + open;
            out.push_back(b);
        }
        std::sort(out.begin(), out.end(), [](const BandSummary& a, const BandSummary& c) { return a.spots > c.spots; });
        return out;
    }
private:
    std::vector<HamSpot> v_; double lat_ = 0, lon_ = 0; bool has_ = false;
};

} // namespace dect2
