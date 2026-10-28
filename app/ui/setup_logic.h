// The setup wizard v2, pure part (no ImGui, no App): places (cities and countries), the mapping from a place to the country, the ITU region,
// the band plan, the FM step, DAB band III, the TV channel plan and the time zone; the interests and what they set (the modes listed first,
// the starting mode, a starter preset list); and the rule for who gets the wizard at all. Drawn by wizard.cpp; tested by tests/app/test_setup.cpp.
// Texts are N_() and go through tr() when shown. House style: plain words, no exclamation marks.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "learn_logic.h"
#include "dect2/channels/tvplan.h"

#ifndef N_
#define N_(s) s
#define SETUP_UNDEF_N_
#endif

namespace setup {

// ---- countries ----
struct Country {
    const char* code;      // ISO 3166 alpha-2
    const char* name;      // N_()
    int itu;               // ITU / IARU region 1, 2 or 3
    int region;            // index in learn::regions(): the TV standard, FM band and de-emphasis
    const char* bandPlan;  // data/bandplans/builtin.json id, "" when the file has no plan for it
    const char* tz;        // IANA time zone of the main part of the country
    double lat, lon;       // a point in the middle of the populated part (the capital)
    int tvMhz;             // width of a UHF TV channel: 6, 7 or 8
    bool dab3;             // DAB radio is on air in band III (174 to 240 MHz)
};
inline const Country* countries(int* n) {
    // learn region indexes: 0 Europe, 1 UK and Ireland, 2 North America, 3 South and Central America, 4 Middle East, 5 Africa,
    // 6 Australia and New Zealand, 7 Japan, 8 South Korea, 9 China, 10 India and South Asia, 11 Elsewhere
    static const Country k[] = {
        {"LT", N_("Lithuania"), 1, 0, "lt", "Europe/Vilnius", 54.69, 25.28, 8, true},
        {"LV", N_("Latvia"), 1, 0, "lv", "Europe/Riga", 56.95, 24.11, 8, true},
        {"EE", N_("Estonia"), 1, 0, "ee", "Europe/Tallinn", 59.44, 24.75, 8, true},
        {"PL", N_("Poland"), 1, 0, "pl", "Europe/Warsaw", 52.23, 21.01, 8, true},
        {"DE", N_("Germany"), 1, 0, "de", "Europe/Berlin", 52.52, 13.40, 8, true},
        {"FR", N_("France"), 1, 0, "fr", "Europe/Paris", 48.86, 2.35, 8, true},
        {"GB", N_("United Kingdom"), 1, 1, "gb", "Europe/London", 51.51, -0.13, 8, true},
        {"IE", N_("Ireland"), 1, 1, "ie", "Europe/Dublin", 53.35, -6.26, 8, true},
        {"NL", N_("Netherlands"), 1, 0, "nl", "Europe/Amsterdam", 52.37, 4.90, 8, true},
        {"BE", N_("Belgium"), 1, 0, "eu", "Europe/Brussels", 50.85, 4.35, 8, true},
        {"LU", N_("Luxembourg"), 1, 0, "eu", "Europe/Luxembourg", 49.61, 6.13, 8, true},
        {"ES", N_("Spain"), 1, 0, "es", "Europe/Madrid", 40.42, -3.70, 8, true},
        {"PT", N_("Portugal"), 1, 0, "eu", "Europe/Lisbon", 38.72, -9.14, 8, true},
        {"IT", N_("Italy"), 1, 0, "it", "Europe/Rome", 41.90, 12.50, 8, true},
        {"CH", N_("Switzerland"), 1, 0, "ch", "Europe/Zurich", 46.95, 7.45, 8, true},
        {"AT", N_("Austria"), 1, 0, "at", "Europe/Vienna", 48.21, 16.37, 8, true},
        {"SE", N_("Sweden"), 1, 0, "se", "Europe/Stockholm", 59.33, 18.07, 8, true},
        {"NO", N_("Norway"), 1, 0, "no", "Europe/Oslo", 59.91, 10.75, 8, true},
        {"FI", N_("Finland"), 1, 0, "fi", "Europe/Helsinki", 60.17, 24.94, 8, true},
        {"DK", N_("Denmark"), 1, 0, "dk", "Europe/Copenhagen", 55.68, 12.57, 8, true},
        {"IS", N_("Iceland"), 1, 0, "eu", "Atlantic/Reykjavik", 64.15, -21.94, 8, false},
        {"CZ", N_("Czechia"), 1, 0, "eu", "Europe/Prague", 50.08, 14.44, 8, true},
        {"SK", N_("Slovakia"), 1, 0, "eu", "Europe/Bratislava", 48.15, 17.11, 8, true},
        {"HU", N_("Hungary"), 1, 0, "eu", "Europe/Budapest", 47.50, 19.04, 8, true},
        {"RO", N_("Romania"), 1, 0, "eu", "Europe/Bucharest", 44.43, 26.10, 8, false},
        {"BG", N_("Bulgaria"), 1, 0, "eu", "Europe/Sofia", 42.70, 23.32, 8, false},
        {"GR", N_("Greece"), 1, 0, "eu", "Europe/Athens", 37.98, 23.73, 8, false},
        {"HR", N_("Croatia"), 1, 0, "eu", "Europe/Zagreb", 45.81, 15.98, 8, true},
        {"SI", N_("Slovenia"), 1, 0, "eu", "Europe/Ljubljana", 46.06, 14.51, 8, true},
        {"RS", N_("Serbia"), 1, 0, "eu", "Europe/Belgrade", 44.79, 20.45, 8, false},
        {"UA", N_("Ukraine"), 1, 0, "eu", "Europe/Kyiv", 50.45, 30.52, 8, false},
        {"BY", N_("Belarus"), 1, 0, "eu", "Europe/Minsk", 53.90, 27.57, 8, false},
        {"TR", N_("Turkey"), 1, 0, "eu", "Europe/Istanbul", 39.93, 32.86, 8, false},
        {"RU", N_("Russia"), 1, 0, "eu", "Europe/Moscow", 55.76, 37.62, 8, false},
        {"US", N_("United States"), 2, 2, "us", "America/New_York", 38.90, -77.04, 6, false},
        {"CA", N_("Canada"), 2, 2, "ca", "America/Toronto", 45.42, -75.70, 6, false},
        {"MX", N_("Mexico"), 2, 2, "us", "America/Mexico_City", 19.43, -99.13, 6, false},
        {"BR", N_("Brazil"), 2, 3, "br", "America/Sao_Paulo", -15.79, -47.88, 6, false},
        {"AR", N_("Argentina"), 2, 3, "br", "America/Argentina/Buenos_Aires", -34.60, -58.38, 6, false},
        {"CL", N_("Chile"), 2, 3, "br", "America/Santiago", -33.45, -70.67, 6, false},
        {"CO", N_("Colombia"), 2, 3, "br", "America/Bogota", 4.71, -74.07, 6, false},
        {"PE", N_("Peru"), 2, 3, "br", "America/Lima", -12.05, -77.04, 6, false},
        {"AU", N_("Australia"), 3, 6, "au", "Australia/Sydney", -35.28, 149.13, 7, true},
        {"NZ", N_("New Zealand"), 3, 6, "nz", "Pacific/Auckland", -41.29, 174.78, 8, false},
        {"JP", N_("Japan"), 3, 7, "jp", "Asia/Tokyo", 35.68, 139.69, 6, false},
        {"KR", N_("South Korea"), 3, 8, "us", "Asia/Seoul", 37.57, 126.98, 6, true},
        {"CN", N_("China"), 3, 9, "cn", "Asia/Shanghai", 39.90, 116.40, 8, false},
        {"IN", N_("India"), 3, 10, "in", "Asia/Kolkata", 28.61, 77.21, 8, false},
        {"PK", N_("Pakistan"), 3, 10, "in", "Asia/Karachi", 33.69, 73.05, 8, false},
        {"BD", N_("Bangladesh"), 3, 10, "in", "Asia/Dhaka", 23.81, 90.41, 8, false},
        {"TH", N_("Thailand"), 3, 11, "", "Asia/Bangkok", 13.76, 100.50, 8, false},
        {"SG", N_("Singapore"), 3, 11, "", "Asia/Singapore", 1.35, 103.82, 8, true},
        {"MY", N_("Malaysia"), 3, 11, "", "Asia/Kuala_Lumpur", 3.14, 101.69, 8, false},
        {"ID", N_("Indonesia"), 3, 11, "", "Asia/Jakarta", -6.21, 106.85, 8, false},
        {"PH", N_("Philippines"), 3, 11, "", "Asia/Manila", 14.60, 120.98, 6, false},
        {"VN", N_("Vietnam"), 3, 11, "", "Asia/Ho_Chi_Minh", 21.03, 105.85, 8, false},
        {"AE", N_("United Arab Emirates"), 1, 4, "ae", "Asia/Dubai", 25.20, 55.27, 8, false},
        {"SA", N_("Saudi Arabia"), 1, 4, "ae", "Asia/Riyadh", 24.71, 46.68, 8, false},
        {"QA", N_("Qatar"), 1, 4, "ae", "Asia/Qatar", 25.29, 51.53, 8, false},
        {"KW", N_("Kuwait"), 1, 4, "ae", "Asia/Kuwait", 29.38, 47.99, 8, false},
        {"OM", N_("Oman"), 1, 4, "ae", "Asia/Muscat", 23.59, 58.41, 8, false},
        {"BH", N_("Bahrain"), 1, 4, "ae", "Asia/Bahrain", 26.23, 50.59, 8, false},
        {"IL", N_("Israel"), 1, 4, "eu", "Asia/Jerusalem", 31.77, 35.22, 8, false},
        {"EG", N_("Egypt"), 1, 5, "za", "Africa/Cairo", 30.04, 31.24, 8, false},
        {"ZA", N_("South Africa"), 1, 5, "za", "Africa/Johannesburg", -25.75, 28.19, 8, false},
        {"NG", N_("Nigeria"), 1, 5, "za", "Africa/Lagos", 9.08, 7.40, 8, false},
        {"KE", N_("Kenya"), 1, 5, "za", "Africa/Nairobi", -1.29, 36.82, 8, false},
        {"MA", N_("Morocco"), 1, 5, "za", "Africa/Casablanca", 34.02, -6.84, 8, false},
    };
    if (n) *n = (int)(sizeof k / sizeof *k);
    return k;
}
inline const Country* countryByCode(const std::string& code) {
    int n = 0;
    const Country* c = countries(&n);
    for (int i = 0; i < n; i++) if (code == c[i].code) return &c[i];
    return nullptr;
}

// ---- places: a city, or a country as a whole ----
struct City { const char* name; const char* country; double lat, lon; };
inline const City* cities(int* n) {
    static const City k[] = {
        {"Vilnius", "LT", 54.69, 25.28}, {"Kaunas", "LT", 54.90, 23.89}, {"Klaipeda", "LT", 55.71, 21.14}, {"Siauliai", "LT", 55.93, 23.31},
        {"Riga", "LV", 56.95, 24.11}, {"Tallinn", "EE", 59.44, 24.75}, {"Warsaw", "PL", 52.23, 21.01}, {"Krakow", "PL", 50.06, 19.94},
        {"Gdansk", "PL", 54.35, 18.65}, {"Berlin", "DE", 52.52, 13.40}, {"Munich", "DE", 48.14, 11.58}, {"Hamburg", "DE", 53.55, 9.99},
        {"Frankfurt", "DE", 50.11, 8.68}, {"Paris", "FR", 48.86, 2.35}, {"Lyon", "FR", 45.76, 4.84}, {"Marseille", "FR", 43.30, 5.37},
        {"London", "GB", 51.51, -0.13}, {"Manchester", "GB", 53.48, -2.24}, {"Edinburgh", "GB", 55.95, -3.19}, {"Birmingham", "GB", 52.49, -1.89},
        {"Dublin", "IE", 53.35, -6.26}, {"Amsterdam", "NL", 52.37, 4.90}, {"Brussels", "BE", 50.85, 4.35}, {"Madrid", "ES", 40.42, -3.70},
        {"Barcelona", "ES", 41.39, 2.17}, {"Lisbon", "PT", 38.72, -9.14}, {"Rome", "IT", 41.90, 12.50}, {"Milan", "IT", 45.46, 9.19},
        {"Zurich", "CH", 47.38, 8.54}, {"Vienna", "AT", 48.21, 16.37}, {"Stockholm", "SE", 59.33, 18.07}, {"Gothenburg", "SE", 57.71, 11.97},
        {"Oslo", "NO", 59.91, 10.75}, {"Helsinki", "FI", 60.17, 24.94}, {"Copenhagen", "DK", 55.68, 12.57}, {"Reykjavik", "IS", 64.15, -21.94},
        {"Prague", "CZ", 50.08, 14.44}, {"Budapest", "HU", 47.50, 19.04}, {"Bucharest", "RO", 44.43, 26.10}, {"Sofia", "BG", 42.70, 23.32},
        {"Athens", "GR", 37.98, 23.73}, {"Zagreb", "HR", 45.81, 15.98}, {"Belgrade", "RS", 44.79, 20.45}, {"Kyiv", "UA", 50.45, 30.52},
        {"Istanbul", "TR", 41.01, 28.98}, {"Ankara", "TR", 39.93, 32.86}, {"Moscow", "RU", 55.76, 37.62}, {"Saint Petersburg", "RU", 59.93, 30.34},
        {"New York", "US", 40.71, -74.01}, {"Washington", "US", 38.90, -77.04}, {"Chicago", "US", 41.88, -87.63}, {"Dallas", "US", 32.78, -96.80},
        {"Denver", "US", 39.74, -104.99}, {"Los Angeles", "US", 34.05, -118.24}, {"San Francisco", "US", 37.77, -122.42}, {"Seattle", "US", 47.61, -122.33},
        {"Miami", "US", 25.76, -80.19}, {"Toronto", "CA", 43.65, -79.38}, {"Vancouver", "CA", 49.28, -123.12}, {"Montreal", "CA", 45.50, -73.57},
        {"Mexico City", "MX", 19.43, -99.13}, {"Sao Paulo", "BR", -23.55, -46.63}, {"Rio de Janeiro", "BR", -22.91, -43.17}, {"Buenos Aires", "AR", -34.60, -58.38},
        {"Santiago", "CL", -33.45, -70.67}, {"Bogota", "CO", 4.71, -74.07}, {"Lima", "PE", -12.05, -77.04}, {"Sydney", "AU", -33.87, 151.21},
        {"Melbourne", "AU", -37.81, 144.96}, {"Perth", "AU", -31.95, 115.86}, {"Brisbane", "AU", -27.47, 153.03}, {"Auckland", "NZ", -36.85, 174.76},
        {"Wellington", "NZ", -41.29, 174.78}, {"Tokyo", "JP", 35.68, 139.69}, {"Osaka", "JP", 34.69, 135.50}, {"Seoul", "KR", 37.57, 126.98},
        {"Beijing", "CN", 39.90, 116.40}, {"Shanghai", "CN", 31.23, 121.47}, {"Shenzhen", "CN", 22.54, 114.06}, {"Delhi", "IN", 28.61, 77.21},
        {"Mumbai", "IN", 19.08, 72.88}, {"Bangalore", "IN", 12.97, 77.59}, {"Karachi", "PK", 24.86, 67.01}, {"Dhaka", "BD", 23.81, 90.41},
        {"Bangkok", "TH", 13.76, 100.50}, {"Singapore", "SG", 1.35, 103.82}, {"Kuala Lumpur", "MY", 3.14, 101.69}, {"Jakarta", "ID", -6.21, 106.85},
        {"Manila", "PH", 14.60, 120.98}, {"Hanoi", "VN", 21.03, 105.85}, {"Dubai", "AE", 25.20, 55.27}, {"Abu Dhabi", "AE", 24.45, 54.38},
        {"Riyadh", "SA", 24.71, 46.68}, {"Doha", "QA", 25.29, 51.53}, {"Tel Aviv", "IL", 32.09, 34.78}, {"Cairo", "EG", 30.04, 31.24},
        {"Johannesburg", "ZA", -26.20, 28.05}, {"Cape Town", "ZA", -33.92, 18.42}, {"Lagos", "NG", 6.52, 3.38}, {"Nairobi", "KE", -1.29, 36.82},
        {"Casablanca", "MA", 33.57, -7.59},
    };
    if (n) *n = (int)(sizeof k / sizeof *k);
    return k;
}

struct Place { std::string name; std::string country; double lat = 0, lon = 0; bool isCity = false; };

inline std::string fold(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (c < 0x80) o += (char)std::tolower(c);   // ASCII only: names in the tables are ASCII
    return o;
}
// cities first (names starting with the text, then containing it), then countries; at most `limit` places
inline std::vector<Place> search(const std::string& query, size_t limit = 8) {
    std::string q = fold(query);
    while (!q.empty() && q.back() == ' ') q.pop_back();
    while (!q.empty() && q.front() == ' ') q.erase(q.begin());
    std::vector<Place> out;
    if (q.empty()) return out;
    int nc = 0, nk = 0;
    const City* C = cities(&nc);
    const Country* K = countries(&nk);
    auto add = [&](const Place& p) { for (const Place& o : out) if (o.name == p.name && o.country == p.country) return; if (out.size() < limit) out.push_back(p); };
    if (q.size() == 2) for (int i = 0; i < nk; i++) if (fold(K[i].code) == q) add({K[i].name, K[i].code, K[i].lat, K[i].lon, false});   // an exact country code first
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nc; i++) {
            const std::string n = fold(C[i].name);
            const size_t at = n.find(q);
            if (at == std::string::npos || (pass == 0) != (at == 0)) continue;
            add({C[i].name, C[i].country, C[i].lat, C[i].lon, true});
        }
    }
    for (int i = 0; i < nk; i++) {
        const std::string n = fold(K[i].name);
        if (n.find(q) != std::string::npos || (q.size() == 2 && fold(K[i].code) == q)) add({K[i].name, K[i].code, K[i].lat, K[i].lon, false});
    }
    return out;
}

// ---- from a position to a country ----
inline double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    const double r = 3.14159265358979323846 / 180.0;
    const double a = std::sin((lat2 - lat1) * r / 2), b = std::sin((lon2 - lon1) * r / 2);
    const double h = a * a + std::cos(lat1 * r) * std::cos(lat2 * r) * b * b;
    return 12742.0 * std::asin(std::sqrt(std::min(1.0, h)));
}
// the country a position is in, as well as a short table can tell: the nearest city within 300 km, else the nearest country point within
// 700 km (the countries are big in the Americas and Russia, so the cities are used first); nullptr when nothing is near
inline const Country* countryAt(double lat, double lon) {
    int nc = 0, nk = 0;
    const City* C = cities(&nc);
    const Country* K = countries(&nk);
    const City* bc = nullptr;
    double bd = 300;
    for (int i = 0; i < nc; i++) { const double d = distanceKm(lat, lon, C[i].lat, C[i].lon); if (d < bd) { bd = d; bc = &C[i]; } }
    if (bc) return countryByCode(bc->country);
    const Country* best = nullptr;
    bd = 700;
    for (int i = 0; i < nk; i++) { const double d = distanceKm(lat, lon, K[i].lat, K[i].lon); if (d < bd) { bd = d; best = &K[i]; } }
    return best;
}
// the ITU / IARU region of a point with no country: by longitude (a coarse rule, only for places the table does not know)
inline int ituRegionAt(double lat, double lon) {
    (void)lat;
    if (lon < -30) return 2;
    if (lon <= 60) return 1;
    return 3;
}

// ---- what a place sets ----
struct Derived {
    bool known = false;               // a country was found; otherwise only the ITU region (by longitude) is set
    std::string country, countryName;
    int itu = 1;
    int region = 11;                  // learn::regions() index
    std::string bandPlan;             // "" keeps the automatic plan
    std::string tz;                   // IANA name, "" when unknown
    int fmStepKhz = 100;              // the tuning step on the FM band: 200 kHz in the Americas, 100 kHz elsewhere
    bool dab3 = false;                // DAB radio in band III
    int tvMhz = 8;                    // UHF channel width
    std::string tv;                   // "DVB-T2" ... from the region table
    std::string tvPlan;               // the TV channel plan of the country for that standard (core/include/dect2/channels/tvplan.h), "" when none
    double lat = 0, lon = 0;          // the receiver position (ADS-B, maps, satellite passes)
    bool havePosition = false;
};
inline Derived derive(const Country* c, double lat, double lon, bool havePosition) {
    Derived d;
    d.havePosition = havePosition;
    d.lat = lat; d.lon = lon;
    if (c) {
        d.known = true;
        d.country = c->code; d.countryName = c->name;
        d.itu = c->itu; d.region = c->region;
        d.bandPlan = c->bandPlan; d.tz = c->tz;
        d.dab3 = c->dab3; d.tvMhz = c->tvMhz;
    } else {
        d.itu = havePosition ? ituRegionAt(lat, lon) : 1;
        d.region = d.itu == 2 ? 2 : 11;
        d.tvMhz = d.itu == 2 ? 6 : 8;
    }
    d.fmStepKhz = d.itu == 2 ? 200 : 100;
    const learn::Region& r = learn::regions(nullptr)[learn::clampRegion(d.region)];
    d.tv = r.tvId ? std::string("DTMB") : r.tvFamily == 1 ? "ATSC" : r.tvFamily == 4 ? "ISDB-T" : r.tvStd == 2 ? "DVB-T" : "DVB-T2";
    {   // the numbered channels: the country's plan for its standard (DTMB: China's)
        const unsigned m = d.tv == "ATSC" ? dect2::tvplan::Atsc | dect2::tvplan::Atsc3 : d.tv == "ISDB-T" ? dect2::tvplan::IsdbT : d.tv == "DTMB" ? dect2::tvplan::Dtmb : dect2::tvplan::DvbAny;
        if (const dect2::tvplan::Plan* tp = dect2::tvplan::defaultPlan(d.country, m)) d.tvPlan = tp->id;
    }
    return d;
}
inline Derived deriveFromPlace(const Place& p) { return derive(countryByCode(p.country), p.lat, p.lon, true); }
inline Derived deriveFromPosition(double lat, double lon) { return derive(countryAt(lat, lon), lat, lon, true); }
inline Derived deriveSkipped() { return derive(nullptr, 0, 0, false); }

// ---- interests ----
enum Interest { IntFm, IntTv, IntAircraft, IntShips, IntHam, IntSats, IntWeather, IntVoice, IntEverything, IntCount };
struct StarterPreset { const char* name; const char* mode; double mhz; };   // mhz 0: the region's FM frequency
struct InterestDef {
    const char* label;                 // N_()
    const char* blurb;                 // N_()
    const char* startMode;             // the mode OnAir opens in when this is the first ticked interest ("fm" and "tv" are resolved by region)
    const char* modes[6];              // mode ids listed first (starred); unknown ids are skipped
    StarterPreset presets[4];
};
inline const InterestDef* interests() {
    static const InterestDef k[IntCount] = {
        {N_("FM and DAB radio"), N_("Music and news from local stations."), "fm", {"fm", "dab", "hdr", "cdr", nullptr},
         {{"FM: local station", "fm", 0}, {"FM: low end of the band", "fm", 88.5}, {nullptr, nullptr, 0}}},
        {N_("TV"), N_("Free digital TV from the nearest transmitter."), "tv", {"tv", "dvbs", "atv", nullptr},
         {{"TV: UHF channel 30", "tv", 546}, {nullptr, nullptr, 0}}},
        {N_("Aircraft"), N_("Planes on a map, and pilots talking to the towers."), "adsb", {"adsb", "airband", "acars", "vdl2", "hfdl", nullptr},
         {{"ADS-B: aircraft positions", "adsb", 1090}, {"Airband: emergency guard", "airband", 121.5}, {"ACARS: aircraft messages", "acars", 131.55}, {nullptr, nullptr, 0}}},
        {N_("Ships"), N_("Ships on a map (AIS) and marine radio."), "ais", {"ais", "marine", "dsc", "navtex", nullptr},
         {{"AIS: ships, channel A", "ais", 161.975}, {"AIS: ships, channel B", "ais", 162.025}, {"Marine channel 16", "marine", 156.8}, {nullptr, nullptr, 0}}},
        {N_("Ham radio"), N_("FT8 and other amateur data modes on the short-wave bands."), "ft8", {"ft8", "ft4", "wspr", "js8", "packet", "cw"},
         {{"FT8 20 m", "ft8", 14.074}, {"WSPR 20 m", "wspr", 14.0956}, {"APRS (Europe)", "packet", 144.8}, {nullptr, nullptr, 0}}},
        {N_("Satellites"), N_("Satellite passes, amateur and weather satellites."), "cubesat", {"cubesat", "meteor", "hrpt", "goes", "qo100", nullptr},
         {{"Weather satellites (METEOR)", "meteor", 137.9}, {"APT 137 MHz", "meteor", 137.62}, {nullptr, nullptr, 0}}},
        {N_("Weather pictures"), N_("Cloud pictures from weather satellites and weather balloons."), "meteor", {"meteor", "hrpt", "goes", "sonde", "wefax", nullptr},
         {{"METEOR-M LRPT", "meteor", 137.1}, {"Radiosondes 403 MHz", "sonde", 403.0}, {"GOES HRIT", "goes", 1694.1}, {nullptr, nullptr, 0}}},
        {N_("Trunking and voice"), N_("Digital voice and the control channels of radio systems (listening to open traffic only)."), "dmr", {"dmr", "p25", "tetra", "nxdn", "dstar", "dpmr"},
         {{nullptr, nullptr, 0}, {nullptr, nullptr, 0}, {nullptr, nullptr, 0}, {nullptr, nullptr, 0}}},
        {N_("Everything"), N_("The spectrum scanner and the general receiver. Nothing is hidden."), "specan", {"specan", "general", nullptr},
         {{nullptr, nullptr, 0}, {nullptr, nullptr, 0}, {nullptr, nullptr, 0}, {nullptr, nullptr, 0}}},
    };
    return k;
}
// the ticked interests in table order -> the starting mode (the first ticked; nothing ticked: the spectrum scanner)
inline const char* startModeFor(unsigned bits) {
    for (int i = 0; i < IntCount; i++) if (bits & (1u << i)) return interests()[i].startMode;
    return "specan";
}
// the mode ids to list first: the ticked interests' modes in table order, without repeats. "Everything" lists none first (all are shown anyway).
inline std::vector<std::string> modesFirst(unsigned bits) {
    std::vector<std::string> v;
    for (int i = 0; i < IntCount; i++) {
        if (!(bits & (1u << i)) || i == IntEverything) continue;
        for (const char* m : interests()[i].modes) if (m && std::find(v.begin(), v.end(), m) == v.end()) v.push_back(m);
    }
    return v;
}
// the starter presets of the ticked interests, with the FM entry resolved to the region's frequency; the FM band start is moved to the
// region's band (87.5 or 76 MHz) and the ham APRS entry to the region's frequency
struct Starter { std::string name, mode; double mhz; };
inline std::vector<Starter> starterPresets(unsigned bits, const Derived& d) {
    std::vector<Starter> v;
    const learn::Region& r = learn::regions(nullptr)[learn::clampRegion(d.region)];
    for (int i = 0; i < IntCount; i++) {
        if (!(bits & (1u << i))) continue;
        for (const StarterPreset& p : interests()[i].presets) {
            if (!p.name) continue;
            double mhz = p.mhz;
            std::string name = p.name;
            if (!std::strcmp(p.mode, "fm")) {
                if (mhz == 0) mhz = r.fmMhz;
                else { mhz = r.fmLoMhz + 1.0; name = "FM: low end of the band"; }
            } else if (!std::strcmp(p.mode, "packet")) {
                mhz = d.itu == 2 ? 144.39 : d.itu == 3 ? 144.64 : 144.8;
                name = d.itu == 2 ? "APRS (Americas)" : d.itu == 3 ? "APRS (Asia, Pacific)" : "APRS (Europe)";
            } else if (!std::strcmp(p.mode, "tv")) {
                continue;   // a TV channel frequency depends on the band plan; the TV mode starts its own scan instead
            }
            bool dup = false;
            for (const Starter& s : v) if (s.mode == p.mode && std::fabs(s.mhz - mhz) < 1e-6) dup = true;
            if (!dup) v.push_back({name, p.mode, mhz});
        }
    }
    return v;
}
// the Interest bits that match the old learn::Goal bits, so the hands-on lessons follow the interests
inline unsigned goalBitsFor(unsigned bits) {
    unsigned g = 0;
    if (bits & (1u << IntFm)) g |= 1u << learn::GoalListen;
    if (bits & (1u << IntTv)) g |= 1u << learn::GoalTv;
    if (bits & (1u << IntAircraft)) g |= (1u << learn::GoalAirband) | (1u << learn::GoalTrack);
    if (bits & (1u << IntShips)) g |= 1u << learn::GoalTrack;
    if (bits & (1u << IntHam)) g |= 1u << learn::GoalHam;
    if (bits & ((1u << IntSats) | (1u << IntWeather))) g |= 1u << learn::GoalSat;
    if (bits & (1u << IntEverything)) g |= (1u << learn::GoalScan) | (1u << learn::GoalExplore);
    return g;
}

// ---- the help page of a radio: the maker's or the project's page for the kind; the OnAir list of radios when the kind is not known ----
inline const char* radioHelpUrl(const std::string& board, const std::string& name) {
    const std::string b = learn::lower(board + " " + name);
    struct R { const char* key; const char* url; };
    static const R k[] = {
        {"hackrf", "https://hackrf.readthedocs.io/en/latest/getting_started_with_hackrf_and_gnu_radio.html"},
        {"rtl", "https://www.rtl-sdr.com/rtl-sdr-quick-start-guide/"},
        {"airspy", "https://airspy.com/quickstart/"},
        {"sdrplay", "https://www.sdrplay.com/start/"},
        {"rsp", "https://www.sdrplay.com/start/"},
        {"pluto", "https://wiki.analog.com/university/tools/pluto"},
        {"rx888", "https://www.rtl-sdr.com/rx888-mkii-sdr-quick-start-guide/"},
        {"lime", "https://wiki.myriadrf.org/Lime_Suite"},
        {"blade", "https://www.nuand.com/bladerf-1/"},
        {"kiwi", "http://kiwisdr.com/quickstart/"},
    };
    for (const R& r : k) if (b.find(r.key) != std::string::npos) return r.url;
    return "https://github.com/adswill/OnAir/blob/main/DEVICES.md";
}

// ---- who gets the wizard ----
// Settings keys that do not count as "the user has used OnAir before": the wizard's own, and the operating system's (macOS adds its own keys
// to an application's defaults).
inline bool isOwnOrSystemKey(const std::string& k) {
    static const char* const pre[] = {"setup", "tour", "Apple", "NS", "AK", "com.apple"};
    for (const char* p : pre) if (k.rfind(p, 0) == 0) return true;
    return false;
}
// The wizard shows on a start with no arguments, when the setup was never finished and there is no prior settings file (no key but the
// wizard's own): an update never shows it to somebody who already uses OnAir. A setup left unfinished (startedHere: its step key is saved) resumes.
inline bool wizardPending(bool noArguments, bool setupDone, const std::vector<std::string>& keys, bool settingsSetAside, bool startedHere = false) {
    if (!noArguments || setupDone || settingsSetAside) return false;
    if (startedHere) return true;   // the wizard was opened by this start-up before and not finished (the app may have saved its settings at exit)
    for (const std::string& k : keys) if (!isOwnOrSystemKey(k)) return false;
    return true;
}

}  // namespace setup

#ifdef SETUP_UNDEF_N_
#undef N_
#undef SETUP_UNDEF_N_
#endif
