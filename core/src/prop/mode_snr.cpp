// Measured decode thresholds of OnAir modes (see mode_snr.h). Numbers copied from docs/SENSITIVITY.md (the committed-harness table,
// improved values after "->"), bandwidths from tools/sens/sens_modes*.h; FT8 from docs/SENSITIVITY-4.0-B2.md.
#include "dect2/prop/mode_snr.h"

namespace dect2::prop {
namespace {
constexpr double N = ModeSnr::kNoValue;
const char* kS = "docs/SENSITIVITY.md";
}

const std::vector<ModeSnr>& modeThresholds() {
    static const std::vector<ModeSnr> v = {
        {"dvbt2", "DVB-T2", 7.61e6, 14, 18, 20, kS},
        {"dvbt", "DVB-T", 7.61e6, 15, 16, 22, kS},
        {"atsc", "ATSC 1.0", 5.38e6, 15, 17, N, kS},
        {"dab", "DAB / DAB+", 1.536e6, 3, 9, 5, kS},
        {"isdbt", "ISDB-T", 5.57e6, 18, 20, N, kS},
        {"fm", "FM broadcast (audio)", 200e3, 5, 3, N, kS},
        {"fm-rds", "FM broadcast (RDS)", 200e3, 16, 14, 26, kS},
        {"hdr-fm", "HD Radio FM", 400e3, 16, N, N, kS},
        {"hdr-am", "HD Radio AM", 30e3, 37, N, N, kS},
        {"am", "AM (envelope)", 9e3, 27, 29, N, kS},
        {"am-sync", "AM (synchronous)", 9e3, 27, N, N, kS},
        {"ssb", "SSB voice", 2.7e3, 20, N, N, kS},
        {"drm", "DRM", 10e3, 16, N, 24, kS},
        {"ft8", "FT8", 2500, -21, N, N, "docs/SENSITIVITY-4.0-B2.md"},
        {"dvbs2", "DVB-S2", 5e6, 3, N, N, kS},
        {"dtmb", "DTMB", 7.56e6, 18, 19, 23, kS},
        {"atv", "Analog TV (PAL B/G)", 7e6, -4, N, N, kS},
        {"dmr", "DMR", 12.5e3, 5, 3, 9, kS},
        {"p25", "P25 phase 1", 12.5e3, 6, 7, 13, kS},
        {"nxdn", "NXDN", 6.25e3, 9, 9, 14, kS},
        {"dpmr", "dPMR", 6.25e3, 7, 9, 12, kS},
        {"dstar", "D-STAR", 6.25e3, 8, 8, 21, kS},
        {"m17", "M17", 9e3, 5, 5, 5, kS},
        {"tetrapol", "TETRAPOL", 12.5e3, 10, 10, 13, kS},
        {"adsb", "ADS-B", 2e6, 14, 12, 22, kS},
        {"ais", "AIS", 25e3, 16, 18, 21, kS},
        {"acars", "ACARS", 25e3, 7, 5, 19, kS},
        {"vdl2", "VDL Mode 2", 25e3, 12, N, 15, kS},
        {"aero", "Inmarsat Aero", 12.5e3, 11, N, N, kS},
        {"inmc", "Inmarsat-C", 1.2e3, 0, N, N, kS},
        {"iridium", "Iridium", 35e3, 12, 15, 14, kS},
        {"globalstar", "Globalstar", 1.23e6, 0, 0, -3, kS},
        {"orbcomm", "Orbcomm", 10e3, 8, 6, 17, kS},
        {"argos", "Argos", 5e3, 17, 10, 17, kS},
        {"sarsat", "COSPAS-SARSAT", 3e3, 0, 0, 3, kS},
        {"sonde", "Radiosonde", 10e3, 12, 9, N, kS},
        {"pager", "POCSAG / FLEX", 25e3, 1, 1, 13, kS},
        {"packet", "APRS / AX.25", 25e3, 5, 5, 21, kS},
        {"wisun", "Wi-SUN FAN", 200e3, 10, 15, 17, kS},
        {"halow", "Wi-Fi HaLow", 1e6, 7, 8, 10, kS},
        {"zigbee", "Zigbee", 2e6, 2, 7, 13, kS},
        {"zwave", "Z-Wave", 100e3, 10, 13, 13, kS},
        {"sigfox", "Sigfox", 600, 17, N, N, kS},
        {"ble", "Bluetooth LE", 1e6, 8, 17, N, kS},
        {"meteor", "Meteor-M LRPT", 72e3, 8, N, N, kS},
        {"hrpt", "MetOp AHRPT", 2.33e6, 8, 16, N, kS},
        {"goes", "GOES HRIT", 927e3, -1, 1, N, kS},
    };
    return v;
}

const ModeSnr* findModeThreshold(const std::string& id) {
    for (const ModeSnr& m : modeThresholds()) if (id == m.id) return &m;
    return nullptr;
}

double modeRequiredSnrDb(const ModeSnr& m, int condition) {
    const double c = m.combinedDb != ModeSnr::kNoValue ? m.combinedDb : m.awgnDb;
    if (condition <= 0) return m.awgnDb;
    if (condition == 1) return c;
    return m.fadingDb != ModeSnr::kNoValue ? m.fadingDb : c;
}

} // namespace dect2::prop
