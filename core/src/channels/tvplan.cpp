// TV channel plans (tvplan.h): the tables. Every raster below is written once as (first centre, step, width) or as explicit lower edges, so the
// numbers can be checked against the sources in docs/TV-CHANNELS.md line by line. Centres are exact (kHz integers; ISDB-T adds 1/7 MHz).
#include "dect2/channels/tvplan.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2::tvplan {

namespace {

constexpr double kIsdbOffsetHz = 1e6 / 7.0;   // ISDB-T: the signal sits 1/7 MHz above the centre of its channel (ARIB STD-B31 / ABNT NBR 15601)

struct Builder {
    Plan p;
    int legacyFromNumber = 0;   // helper for run(): numbers at or above are legacy
    // a run of numbered channels: label = prefix + number [+ suffix], lower edge of the first channel `lowMhz`, `n` channels `stepMhz` apart
    void run(const char* prefix, int first, int n, double lowMhz, double stepMhz, double bwMhz, int legacyFrom = 0, double offsetHz = 0) {
        for (int k = 0; k < n; k++) {
            Channel c;
            c.label = std::string(prefix) + std::to_string(first + k);
            c.bwHz = std::llround(bwMhz * 1e6);
            c.centreHz = std::llround((lowMhz + k * stepMhz + bwMhz / 2) * 1e6) + offsetHz;
            c.legacy = legacyFrom > 0 && first + k >= legacyFrom;
            p.channels.push_back(c);
        }
    }
    // one channel by its lower edge
    void one(const std::string& label, double lowMhz, double bwMhz, bool legacy = false, double offsetHz = 0) {
        Channel c;
        c.label = label;
        c.bwHz = std::llround(bwMhz * 1e6);
        c.centreHz = std::llround((lowMhz + bwMhz / 2) * 1e6) + offsetHz;
        c.legacy = legacy;
        p.channels.push_back(c);
    }
    void finish() {
        std::stable_sort(p.channels.begin(), p.channels.end(), [](const Channel& a, const Channel& b) { return a.centreHz < b.centreHz; });
    }
};

Builder make(const char* id, const char* name, const char* region, unsigned stds, std::vector<std::string> countries, const char* source, const char* note = "") {
    Builder b;
    b.p.id = id; b.p.name = name; b.p.region = region; b.p.stds = stds; b.p.countries = std::move(countries);
    b.p.source = source; b.p.note = note;
    return b;
}
void analog(Builder& b, const char* system, double visionOffMhz, double soundMhz) {
    b.p.system = system; b.p.visionOffHz = visionOffMhz * 1e6; b.p.soundOffHz = soundMhz * 1e6;
}

// ---- shared rasters ----
// CEPT / ITU-R BT.470 (system B and G): band I E2-E4 (7 MHz, 47-68), band III E5-E12 (7 MHz, 174-230), bands IV and V 21-69 (8 MHz, 470-862)
void cept(Builder& b, int legacyUhfFrom) {
    b.run("E", 2, 3, 47.0, 7.0, 7.0, 2);        // band I: analog only in practice; listed as legacy so the default list starts at band III
    b.run("E", 5, 8, 174.0, 7.0, 7.0);
    b.run("", 21, 49, 470.0, 8.0, 8.0, legacyUhfFrom);
}
// the Americas / Korea 6 MHz raster (47 CFR 73.603, ATSC A/65, ITU-R BT.470 system M): 2-4 54-72, 5-6 76-88, 7-13 174-216, 14-69 470-806
void sixMhz(Builder& b, double offsetHz, int lowFrom, int uhfLegacyFrom, bool ch37Legacy) {
    if (lowFrom <= 2) b.run("", 2, 3, 54.0, 6.0, 6.0, 0, offsetHz);
    if (lowFrom <= 5) b.run("", 5, 2, 76.0, 6.0, 6.0, 0, offsetHz);
    b.run("", 7, 7, 174.0, 6.0, 6.0, 0, offsetHz);
    b.run("", 14, 56, 470.0, 6.0, 6.0, uhfLegacyFrom, offsetHz);
    if (ch37Legacy)
        for (Channel& c : b.p.channels) if (c.label == "37") c.legacy = true;   // 608-614: radio astronomy, no TV
}
// China GB 4943 / GY/T 106: DS1-12 (VHF) and DS13-68 (UHF 13-24 at 470-566, 25-68 at 606-958)
void china(Builder& b) {
    b.one("1", 48.5, 8); b.one("2", 56.5, 8); b.one("3", 64.5, 8);
    b.one("4", 76.0, 8); b.one("5", 84.0, 8);
    b.run("", 6, 7, 167.0, 8.0, 8.0);
    b.run("", 13, 12, 470.0, 8.0, 8.0);
    b.run("", 25, 44, 606.0, 8.0, 8.0, 0);
    for (Channel& c : b.p.channels) if (c.centreHz > 862e6) c.legacy = true;
}
// OIRT / CIS: 1-5 (48.5-100) and 6-12 (174-230) at 8 MHz
void oirt(Builder& b, bool lowToo) {
    if (lowToo) { b.one("1", 48.5, 8, true); b.one("2", 58.0, 8, true); b.one("3", 76.0, 8, true); b.one("4", 84.0, 8, true); b.one("5", 92.0, 8, true); }
    b.run("", 6, 7, 174.0, 8.0, 8.0);
}
// Japan (ARIB STD-B31 / MIC): VHF 1-12 (1-3 90-108, 4-12 from 170 with the 7 / 8 overlap), UHF 13-62 at 6 MHz
void japanVhf(Builder& b, double offsetHz, bool legacy) {
    b.one("1", 90, 6, legacy, offsetHz); b.one("2", 96, 6, legacy, offsetHz); b.one("3", 102, 6, legacy, offsetHz);
    b.one("4", 170, 6, legacy, offsetHz); b.one("5", 176, 6, legacy, offsetHz); b.one("6", 182, 6, legacy, offsetHz);
    b.one("7", 188, 6, legacy, offsetHz); b.one("8", 192, 6, legacy, offsetHz); b.one("9", 198, 6, legacy, offsetHz);
    b.one("10", 204, 6, legacy, offsetHz); b.one("11", 210, 6, legacy, offsetHz); b.one("12", 216, 6, legacy, offsetHz);
}
// Australia (ACMA / AS/NZS 1367): VHF 6-12 with 9A at 7 MHz, UHF 28-69 at 7 MHz from 526
void australiaVhf(Builder& b, bool lowToo) {
    if (lowToo) {
        b.one("0", 45, 7, true); b.one("1", 56, 7, true); b.one("2", 63, 7, true); b.one("3", 85, 7, true);
        b.one("4", 94, 7, true); b.one("5", 101, 7, true); b.one("5A", 137, 7, true);
    }
    b.one("6", 174, 7); b.one("7", 181, 7); b.one("8", 188, 7); b.one("9", 195, 7); b.one("9A", 202, 7);
    b.one("10", 209, 7); b.one("11", 216, 7); b.one("12", 223, 7);
}

std::vector<Plan> build() {
    std::vector<Plan> out;
    auto put = [&](Builder& b) { b.finish(); out.push_back(std::move(b.p)); };

    const char* kCept = "ITU-R BT.470-6 and ITU-R BT.1306 (system B/G channel plan); CEPT GE06 (Geneva 2006 agreement); ETSI EN 300 744 annex";
    const unsigned dvb = DvbT | DvbT2;

    // ---------- digital terrestrial ----------
    { Builder b = make("eu", "Europe, Africa, Middle East, South and South-East Asia (CEPT 7/8 MHz)", "Europe", dvb,
          {"LT","LV","EE","PL","DE","FR","GB","IE","NL","BE","LU","ES","PT","IT","CH","AT","SE","NO","FI","DK","IS","CZ","SK","HU","RO","BG","GR","HR","SI","RS","TR","IL","AE","SA","QA","KW","OM","BH","EG","ZA","NG","KE","MA","IN","PK","BD","TH","SG","MY","ID","VN","NZ"},
          kCept, "VHF band III channels E5-E12 are 7 MHz, UHF 21-69 are 8 MHz. UHF 49-69 (694-862 MHz) were given to mobile in most of Europe (legacy). Italy's own VHF channel letters (A-H) are not in this list: type the frequency. New Zealand uses the 8 MHz UHF raster from channel 25.");
      cept(b, 49); put(b); }
    { Builder b = make("ru", "Russia, Belarus, Ukraine (OIRT VHF 8 MHz + UHF)", "Europe", dvb, {"RU","BY","UA"},
          "ITU-R BT.470-6 (systems D/K); GOST 7845; RTRS DVB-T2 network plan", "VHF 6-12 are 8 MHz channels (174-230 MHz), UHF 21-69 as in Europe.");
      oirt(b, false); b.run("", 21, 49, 470.0, 8.0, 8.0, 49); put(b); }
    { Builder b = make("au", "Australia (7 MHz VHF and UHF)", "Australia", dvb, {"AU"},
          "ACMA Australian Radiofrequency Spectrum Plan; AS/NZS 1367 (digital TV channel plan)", "Every channel is 7 MHz. UHF channel 28 is 526-533 MHz (centre 529.5). UHF 52-69 (694-820 MHz) were cleared in the digital dividend (legacy).");
      australiaVhf(b, false); b.run("", 28, 42, 526.0, 7.0, 7.0, 52); put(b); }
    { Builder b = make("us", "United States, Canada, Mexico (6 MHz)", "North America", Atsc | Atsc3, {"US","CA","MX"},
          "ATSC A/65 and 47 CFR 73.603(a) (channels 2-69, 6 MHz); FCC repack: broadcasting 2-36 (and 38-51 for low power)", "Channels 52-69 (698-806 MHz) and 70-83 were reallocated (legacy). Channel 37 is radio astronomy. Channel 1 does not exist.");
      sixMhz(b, 0, 2, 52, true); put(b); }
    { Builder b = make("kr", "South Korea (6 MHz)", "Asia", Atsc | Atsc3, {"KR"},
          "KCC / Korean TTA ATSC channel plan (same 6 MHz raster as the Americas); VHF 7-13 and UHF 14-69", "VHF low channels 2-6 are not in the Korean digital plan. Channels above 51 were reallocated (legacy).");
      b.p.verified = false; sixMhz(b, 0, 7, 52, false); put(b); }
    { Builder b = make("am6-dvb", "Colombia, Panama (DVB-T/T2 on the 6 MHz American raster)", "Americas", dvb, {"CO","PA"},
          "CRC Colombia (DVB-T2, 6 MHz channels) and ITU-R BT.470 system M raster", "The channels are the 6 MHz raster of the Americas without the ISDB-T offset.");
      b.p.verified = false; sixMhz(b, 0, 2, 52, true); put(b); }
    { Builder b = make("br", "Brazil, South America (ISDB-Tb, 6 MHz)", "Americas", IsdbT, {"BR","AR","CL","PE","EC","BO","VE","UY","PY","CR","PH"},
          "ABNT NBR 15601 (SBTVD); ITU-R BT.470 system M raster with the ISDB-T +1/7 MHz offset; ANATEL: UHF 14-51", "Channels 52-69 (698-806 MHz) were cleared for mobile (legacy). The Philippines use the same ISDB-T 6 MHz raster (mapped, not separately checked).");
      sixMhz(b, kIsdbOffsetHz, 2, 52, true); put(b); }
    { Builder b = make("jp", "Japan (ISDB-T, 6 MHz)", "Asia", IsdbT, {"JP"},
          "ARIB STD-B31 (centre frequency offset +1/7 MHz); MIC Japan channel plan: UHF 13-62 (470-770 MHz), VHF 1-12", "Digital TV is on UHF 13-52 (470-710). 53-62 were reallocated; VHF 1-12 are analog-era channels (legacy). Channels 7 and 8 overlap by 2 MHz.");
      b.p.overlaps = true; japanVhf(b, kIsdbOffsetHz, true); b.run("", 13, 50, 470.0, 6.0, 6.0, 53, kIsdbOffsetHz); put(b); }
    { Builder b = make("cn", "China (DTMB, 8 MHz)", "Asia", Dtmb, {"CN","HK","MO"},
          "GB 20600-2006 (DTMB); GB 4943 / GY/T 106 channel plan: DS1-DS12 VHF, DS13-DS68 UHF", "UHF is two blocks: 13-24 (470-566) and 25-68 (606-958 MHz). Above 862 MHz is listed as legacy. Hong Kong and Macau default here but may use other channels: type the frequency.");
      china(b); put(b); }

    // ---------- cable ----------
    { Builder b = make("cable-eu", "Europe cable (CENELEC: E, S1-S41 and UHF)", "Europe", DvbC,
          {"DE","AT","CH","NL","BE","LU","DK","SE","NO","FI","PL","CZ","SK","HU","RO","BG","HR","SI","ES","PT","IT","FR","GB","IE","LT","LV","EE","TR","ZA"},
          "CENELEC EN 50083-1 / ITU-T J.83 annex A cable channel plan: E2-E4, special channels S1-S10 (7 MHz, 104-174), E5-E12, S11-S20 (7 MHz, 230-300), S21-S41 (8 MHz, 302-470), 21-69",
          "S1-S20 are 7 MHz wide on the analog raster; S21-S41 and 21-69 are 8 MHz. Digital cable operators often use their own 8 MHz raster (114 + 8 k MHz): the selector sets the plan channel, the panel's raster list and the frequency box tune any other.");
      b.run("E", 2, 3, 47.0, 7.0, 7.0);
      b.run("S", 1, 10, 104.0, 7.0, 7.0);
      b.run("E", 5, 8, 174.0, 7.0, 7.0);
      b.run("S", 11, 10, 230.0, 7.0, 7.0);
      b.run("S", 21, 21, 302.0, 8.0, 8.0);
      b.run("", 21, 49, 470.0, 8.0, 8.0);
      // UHF 21-69 and S21-S41 both carry plain numbers 21..: keep the labels unique by naming the UHF ones with the band
      for (Channel& c : b.p.channels) if (!c.label.empty() && std::isdigit((unsigned char)c.label[0])) c.label = "U" + c.label;
      put(b); }

    // ---------- analog ----------
    { Builder b = make("an-bg", "Analog PAL / SECAM B/G (Europe, Asia, Africa, Middle East)", "Europe", Analog,
          {"DE","NL","BE","LU","ES","PT","IT","CH","AT","SE","NO","FI","DK","IS","GR","HR","SI","RS","TR","IL","AE","SA","QA","KW","OM","BH","EG","NG","KE","MA","IN","PK","BD","TH","SG","MY","ID","NZ"},
          kCept, "System B on VHF (7 MHz), G on UHF (8 MHz): vision carrier 1.25 MHz above the lower edge, FM sound 5.5 MHz above it. Analog is switched off in most of these countries (legacy).");
      analog(b, "B/G", 1.25, 5.5); cept(b, 0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-i", "Analog PAL I (United Kingdom, Ireland, South Africa)", "Europe", Analog, {"GB","IE","ZA"},
          "ITU-R BT.470-6 system I (UHF 21-69 at 8 MHz); Ofcom UK TV channel plan", "Vision carrier 1.25 MHz above the lower edge, FM sound 6.0 MHz above it, NICAM at 6.552 MHz. The UK switched analog off in 2012.");
      analog(b, "I", 1.25, 6.0); b.run("", 21, 49, 470.0, 8.0, 8.0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-dk", "Analog D/K and SECAM D/K (Russia, Eastern Europe, Central Asia)", "Europe", Analog, {"RU","BY","UA","LT","LV","EE","PL","CZ","SK","HU","RO","BG","VN"},
          "ITU-R BT.470-6 systems D, K: OIRT channels 1-12 (8 MHz) and UHF 21-69", "FM sound 6.5 MHz above the vision carrier. Almost everywhere switched off (legacy).");
      analog(b, "D/K", 1.25, 6.5); oirt(b, true); b.run("", 21, 49, 470.0, 8.0, 8.0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-cn", "Analog PAL D/K (China)", "Asia", Analog, {"CN"},
          "GB 3174 / GB 4943: DS1-DS68 channel plan, system D", "FM sound 6.5 MHz above the vision carrier. Switched off.");
      analog(b, "D/K", 1.25, 6.5); china(b); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-l", "Analog SECAM L (France)", "Europe", Analog, {"FR","MC"},
          "ITU-R BT.470-6 system L, UHF 21-69 (8 MHz); ANFR", "Positive modulation, AM sound 6.5 MHz above the vision carrier. Switched off in 2011.");
      analog(b, "L", 1.25, 6.5); b.run("", 21, 49, 470.0, 8.0, 8.0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-m", "Analog NTSC M / PAL M / PAL N (Americas, Korea, Philippines)", "Americas", Analog, {"US","CA","MX","BR","AR","CL","CO","PE","KR","PH"},
          "ITU-R BT.470-6 systems M and N; 47 CFR 73.603 (channels 2-83)", "Vision carrier 1.25 MHz above the lower edge, FM sound 4.5 MHz above it. Channels 70-83 (806-890 MHz) are listed too. Mostly switched off.");
      analog(b, "M/N", 1.25, 4.5); sixMhz(b, 0, 2, 0, false); b.run("", 70, 14, 806.0, 6.0, 6.0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-j", "Analog NTSC-J (Japan)", "Asia", Analog, {"JP"},
          "ARIB / MIC Japan channel plan: VHF 1-12, UHF 13-62", "Vision carrier 1.25 MHz above the lower edge, FM sound 4.5 MHz above it. Switched off in 2012.");
      b.p.overlaps = true; analog(b, "M", 1.25, 4.5); japanVhf(b, 0, false); b.run("", 13, 50, 470.0, 6.0, 6.0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    { Builder b = make("an-au", "Analog PAL B (Australia)", "Australia", Analog, {"AU"},
          "ACMA / AS/NZS 1367: VHF 0-12 (with 5A, 9A) and UHF 28-69, 7 MHz", "FM sound 5.5 MHz above the vision carrier. Switched off in 2013.");
      analog(b, "B", 1.25, 5.5); australiaVhf(b, true); b.run("", 28, 42, 526.0, 7.0, 7.0); for (Channel& c : b.p.channels) c.legacy = true; put(b); }
    return out;
}

std::string norm(const std::string& s) {
    std::string o;
    for (char ch : s) if (!std::isspace((unsigned char)ch) && ch != '-' && ch != '_') o += (char)std::toupper((unsigned char)ch);
    if (o.rfind("CHANNEL", 0) == 0) o = o.substr(7);
    else if (o.rfind("CH", 0) == 0) o = o.substr(2);
    return o;
}

} // namespace

const std::vector<Plan>& plans() {
    static const std::vector<Plan> p = build();
    return p;
}

const Plan* find(const std::string& id) {
    for (const Plan& p : plans()) if (p.id == id) return &p;
    return nullptr;
}

std::vector<const Plan*> plansFor(unsigned mask) {
    std::vector<const Plan*> o;
    for (const Plan& p : plans()) if (p.stds & mask) o.push_back(&p);
    return o;
}

const Plan* defaultPlan(const std::string& country, unsigned mask) {
    std::string cc;
    for (char ch : country) cc += (char)std::toupper((unsigned char)ch);
    const Plan* first = nullptr;
    for (const Plan& p : plans()) {
        if (!(p.stds & mask)) continue;
        if (!first) first = &p;
        if (!cc.empty() && std::find(p.countries.begin(), p.countries.end(), cc) != p.countries.end()) return &p;
    }
    return first;
}

const Channel* Plan::byLabel(const std::string& label) const {
    const std::string n = norm(label);
    if (n.empty()) return nullptr;
    for (const Channel& c : channels) if (norm(c.label) == n) return &c;
    // "21" in a plan that labels them "U21", "E21" in a plan that labels them "21"
    for (const char* pre : {"U", "E", "S"}) {
        if (n.rfind(pre, 0) == 0) { const std::string rest = n.substr(1); for (const Channel& c : channels) if (norm(c.label) == rest) return &c; }
        else for (const Channel& c : channels) if (norm(c.label) == std::string(pre) + n) { if (std::string(pre) != "S") return &c; }
    }
    return nullptr;
}

const Channel* Plan::at(double hz, double tolHz) const {
    const Channel* best = nullptr;
    double bestD = 1e18;
    for (const Channel& c : channels) {
        const double d = std::fabs(c.centreHz - hz);
        const double tol = tolHz >= 0 ? tolHz : c.bwHz / 2;
        if (d <= tol && d < bestD) { best = &c; bestD = d; }
    }
    return best;
}

const Channel* Plan::step(const Channel* c, int dir, double fromHz) const {
    if (channels.empty() || dir == 0) return nullptr;
    if (c) {
        const int i = indexOf(c) + (dir > 0 ? 1 : -1);
        return i >= 0 && i < (int)channels.size() ? &channels[i] : nullptr;
    }
    if (dir > 0) { for (const Channel& x : channels) if (x.centreHz > fromHz + 1) return &x; }
    else for (size_t i = channels.size(); i-- > 0;) if (channels[i].centreHz < fromHz - 1) return &channels[i];
    return nullptr;
}

std::string describe(const Plan& p, double hz) {
    const Channel* c = p.at(hz);
    if (!c) return std::string();
    char b[64];
    std::snprintf(b, sizeof b, "%s (%.3f MHz)", c->label.c_str(), c->centreHz / 1e6);
    return b;
}

} // namespace dect2::tvplan
