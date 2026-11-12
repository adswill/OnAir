// Radiosonde launch sites over time (SondeHub site pages, regular chasers): the site each sonde came from (its first fix, low and climbing),
// the sites this station hears, and where each site's sondes came down. docs/SONDE-SITES-3.9.md.
#pragma once
#include "dect2/sonde/sonde_sites_known.h"
#include "dect2/sonde/sonde_tel.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct SondeSite {
    int id = 0;
    std::string name;                  // the nearest known launch site within 15 km (sonde_sites_known.h), else "Site 1" until the user names it
    double lat = 0, lon = 0;           // the mean of the launches
    int launches = 0;
    int64_t lastUtc = 0;
    std::vector<int64_t> launchTimes;  // unix times of the launches heard (last 80): feeds the launch clock (sonde_launch_clock.h)
    std::vector<std::pair<float, float>> landings;   // lat, lon of the last positions after a burst
};

class SondeSites {
public:
    // one look at the sondes (seconds of wall clock): new launches and landings are taken from their tracks
    void update(const std::vector<SondeInfo>& sondes, int64_t utc);
    const std::vector<SondeSite>& sites() const { return sites_; }
    void rename(int id, const std::string& name);
    // 4.0: name the sites from a list of known launch sites. A new site takes the name of the nearest known one within 15 km; sites that
    // still have their placeholder name ("Site <id>", also from a 3.9 file) are renamed now. A name the user gave is kept. Returns the renamed.
    int setKnown(const KnownSondeSites* known);
    static bool placeholderName(const SondeSite& s) { return s.name == "Site " + std::to_string(s.id); }
    std::string serialize() const;
    void parse(const std::string& text);
    bool dirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }
    static constexpr double kSiteKm = 5;        // launches this close are one site
    static constexpr double kLaunchMaxM = 2500; // a first fix higher than this is not a launch seen from the ground
private:
    struct Seen { int site = -1; bool launch = false, landed = false; };
    std::map<std::string, Seen> seen_;          // by serial, this session
    std::vector<SondeSite> sites_;
    bool dirty_ = false;
    int nextId_ = 1;
    const KnownSondeSites* known_ = nullptr;
};

}  // namespace dect2
