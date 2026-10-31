#include "webshare_core.h"
#include "web/web_json.h"
#include "web/web_policy.h"
#include "web/web_directory_proto.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace webshare {

namespace {

std::string trimS(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) b--;
    return s.substr(a, b - a);
}
std::string num(double v) { char b[48]; snprintf(b, sizeof b, "%.10g", v); return b; }
std::string joinSet(const std::set<std::string>& s) { std::string o; for (const auto& x : s) { if (!o.empty()) o += ','; o += x; } return o; }
std::set<std::string> splitSet(const std::string& s) {
    std::set<std::string> r;
    std::string cur;
    for (char c : s + ",") { if (c == ',' || c == ' ') { if (!cur.empty()) r.insert(cur); cur.clear(); } else cur += c; }
    return r;
}
bool cleanText(const std::string& s, size_t maxLen) {
    if (s.size() > maxLen) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7F) return false;
    return true;
}
std::string mhz(double hz) {
    char b[48];
    if (hz >= 1e9) snprintf(b, sizeof b, "%.3g GHz", hz / 1e9);
    else if (hz >= 1e6) snprintf(b, sizeof b, "%.4g MHz", hz / 1e6);
    else snprintf(b, sizeof b, "%.4g kHz", hz / 1e3);
    return b;
}

// the decoder table: kind, label, group, cpu 1-5, needs codec, bands (MHz) it works in (empty: anywhere)
struct Dec { const char* kind; const char* label; int cpu; bool codec; std::vector<std::pair<double, double>> bands; };
const std::vector<Dec>& dataDecoders() {
    static const std::vector<Dec> v = {
        {"adsb", "ADS-B aircraft (1090 MHz)", 3, false, {{1089, 1091}}},
        {"ais", "AIS ships", 2, false, {{161.9, 162.1}}},
        {"aprs", "APRS packet", 1, false, {{144.3, 144.9}, {144.99, 145.01}}},
        {"acars", "ACARS / VDL2 aircraft messages", 2, false, {{129, 137}}},
        {"ft8", "FT8", 3, false, {{1.8, 54}}},
        {"ft4", "FT4", 3, false, {{1.8, 54}}},
        {"wspr", "WSPR", 2, false, {{0.1, 54}}},
        {"sstv", "SSTV pictures", 2, false, {{3.5, 30}, {144, 146}, {430, 440}}},
        {"wefax", "Weather fax", 2, false, {{2, 30}}},
        {"apt", "NOAA APT weather satellites", 2, false, {{137, 138}}},
        {"meteor", "Meteor-M LRPT weather satellites", 4, false, {{137, 138}}},
        {"rds", "RDS (FM station names)", 1, false, {{87.5, 108}}},
    };
    return v;
}
const std::vector<Dec>& restrictedDecoders() {
    static const std::vector<Dec> v = {
        {"cw", "Morse (CW) decoder", 1, false, {{0.1, 30}, {144, 146}}},
        {"rtty", "RTTY decoder", 1, false, {{0.1, 30}}},
        {"dmr", "DMR digital voice", 4, true, {{136, 520}}},
        {"p25", "P25 digital voice", 4, true, {{136, 520}, {762, 870}}},
        {"nxdn", "NXDN digital voice", 4, true, {{136, 520}}},
        {"dpmr", "dPMR digital voice", 4, true, {{136, 520}}},
        {"dstar", "D-STAR digital voice", 4, true, {{136, 520}}},
        {"ysf", "Yaesu System Fusion", 4, true, {{136, 520}}},
        {"tetra", "TETRA", 5, true, {{380, 470}}},
        {"pocsag", "POCSAG pagers", 2, false, {{138, 480}}},
        {"flex", "FLEX pagers", 2, false, {{138, 480}}},
    };
    return v;
}
struct Dem { const char* name; const char* label; };
const Dem kDemods[] = {{"am", "AM"}, {"nfm", "Narrow FM"}, {"wfm", "Wide FM (broadcast)"}, {"usb", "Upper sideband"}, {"lsb", "Lower sideband"}, {"cw", "CW (Morse tone)"}};

bool overlaps(const std::vector<std::pair<double, double>>& bands, double loMHz, double hiMHz, std::string& need) {
    if (bands.empty()) return true;
    for (const auto& b : bands) if (b.second >= loMHz && b.first <= hiMHz) return true;
    char t[80];
    snprintf(t, sizeof t, "needs %.5g-%.5g MHz", bands[0].first, bands[0].second);
    need = t;
    if (bands.size() > 1) need += " (or another band)";
    return false;
}

} // namespace

// ---------------------------------------------------------------- radios
RigCaps capsFromDevice(const dect2::DeviceInfo& d) {
    RigCaps c;
    c.name = d.name;
    c.board = d.board;
    c.minHz = d.minFreqHz; c.maxHz = d.maxFreqHz;
    c.minRate = d.minRateHz; c.maxRate = d.maxRateHz; c.steadyRate = d.steadyRateHz;
    c.network = d.isNetwork();
    c.test = d.kind == dect2::DeviceInfo::Synthetic;
    c.isRadio = d.isRadio();
    return c;
}

std::string capsLine(const RigCaps& c) {
    std::string s;
    if (c.test) return "built-in test signal, no radio needed";
    if (c.minHz > 0 || c.maxHz > 0) s = mhz(c.minHz) + " - " + mhz(c.maxHz);
    else s = "range not reported";
    if (c.maxRate > 0) s += ", up to " + num(std::round(c.maxRate / 1e5) / 10) + " Msps";
    if (c.network) s += ", over the network";
    return s;
}

std::vector<RigCaps> shareableRigs(const std::vector<dect2::DeviceInfo>& devices) {
    std::vector<RigCaps> local, net, test;
    for (const auto& d : devices) {
        if (d.kind == dect2::DeviceInfo::Synthetic) test.push_back(capsFromDevice(d));
        else if (d.isRadio() && d.kind != dect2::DeviceInfo::Hamlib) (d.isNetwork() ? net : local).push_back(capsFromDevice(d));
    }
    local.insert(local.end(), net.begin(), net.end());
    local.insert(local.end(), test.begin(), test.end());
    return local;
}

// ---------------------------------------------------------------- presets
const std::vector<RangePreset>& rangePresets() {
    static const std::vector<RangePreset> v = {
        {"fm", "FM broadcast (87.5-108 MHz)", 97.75e6, 20.5e6},
        {"air", "Airband (118-137 MHz)", 127.5e6, 19e6},
        {"marine", "Marine VHF (156-162.5 MHz)", 159.25e6, 6.5e6},
        {"2m", "Ham 2 m (144-148 MHz)", 146e6, 4e6},
        {"70cm", "Ham 70 cm (430-440 MHz)", 435e6, 10e6},
        {"ais", "AIS ships (161.9-162.1 MHz)", 162e6, 0.4e6},
        {"adsb", "ADS-B (1090 MHz)", 1090e6, 2.4e6},
        {"weather", "Weather satellites (137-138 MHz)", 137.5e6, 1.2e6},
        {"hf20", "HF 20 m (14-14.35 MHz)", 14.175e6, 0.35e6},
        {"hf40", "HF 40 m (7-7.2 MHz)", 7.1e6, 0.2e6},
        {"hf", "HF 0-30 MHz (wide)", 15e6, 30e6},
    };
    return v;
}

void applyPreset(ShareSettings& s, const RangePreset& p) {
    s.wholeSpan = false;
    s.centreMHz = p.centreHz / 1e6;
    s.spanMHz = p.spanHz / 1e6;
    s.rateAuto = true;
}

// ---------------------------------------------------------------- settings
bool settingsValid(const ShareSettings& s, std::string& why) {
    if (!cleanText(s.name, 48) || s.name.empty()) { why = "The name must be 1 to 48 characters on one line."; return false; }
    if (!cleanText(s.desc, 280)) { why = "The description is too long (280 characters at most) or has line breaks."; return false; }
    if (!cleanText(s.loc, 40)) { why = "The location must be on one line (latitude,longitude or a locator)."; return false; }
    if (!cleanText(s.contact, 80)) { why = "The contact is too long (80 characters at most)."; return false; }
    if (!cleanText(s.radioLabel, 48)) { why = "The radio name is too long (48 characters at most)."; return false; }
    if (!cleanText(s.password, 64) || (!s.password.empty() && s.password.size() < 8)) { why = "A password needs 8 to 64 characters (or leave it empty for open access)."; return false; }
    if (!(s.centreMHz > 0) || !std::isfinite(s.centreMHz)) { why = "The centre frequency must be a number above 0 MHz."; return false; }
    if (!s.wholeSpan && !(s.spanMHz > 0)) { why = "The span must be above 0 MHz."; return false; }
    if (!s.rateAuto && !(s.rateMsps > 0)) { why = "The sample rate must be above 0 Msps."; return false; }
    if (s.link == "own") {
        std::string out, e;
        if (!onair_web::dirNormalizeUrl(s.ownUrl, out, e)) { why = "Your own address: " + e; return false; }
    }
    if (!s.loc.empty()) {
        std::string out, e;
        if (!onair_web::dirNormalizeLoc(s.loc, s.locDecimals, out, e)) { why = e; return false; }
    }
    return true;
}

std::string settingsToText(const ShareSettings& s) {
    std::ostringstream o;
    auto b = [](bool v) { return v ? "1" : "0"; };
    o << "# OnAir Web setup (written by OnAir; edit in Server > Share on web)\n"
      << "rig=" << s.rig << "\nwhole_span=" << b(s.wholeSpan) << "\ncentre_mhz=" << num(s.centreMHz) << "\nspan_mhz=" << num(s.spanMHz) << "\nrate_auto=" << b(s.rateAuto)
      << "\nrate_msps=" << num(s.rateMsps) << "\ngain_manual=" << b(s.gainManual) << "\ngain_db=" << num(s.gainDb) << "\ndata_off=" << joinSet(s.dataOff)
      << "\nrestricted_on=" << joinSet(s.restrictedOn) << "\nlegal_ack=" << b(s.legalAck) << "\ndemods=" << s.demods << "\nmax_users=" << s.maxUsers << "\nuser_kbps=" << s.userKbps
      << "\naudio_only=" << b(s.audioOnly) << "\nlimit_to_range=" << b(s.limitToRange) << "\npassword=" << s.password << "\nlink=" << s.link << "\nown_url=" << s.ownUrl << "\nlist_public=" << b(s.listPublic)
      << "\nname=" << s.name << "\ndesc=" << s.desc << "\nloc=" << s.loc << "\ncontact=" << s.contact << "\nradio_label=" << s.radioLabel << "\ndir_url=" << s.dirUrl
      << "\nloc_decimals=" << s.locDecimals << "\nreach=" << b(s.reach) << "\n";
    return o.str();
}

ShareSettings settingsFromText(const std::string& text) {
    ShareSettings s;
    std::istringstream in(text);
    std::string l;
    auto flag = [](const std::string& v, bool def) { return v == "1" || v == "true" ? true : (v == "0" || v == "false" ? false : def); };
    auto dbl = [](const std::string& v, double def) { char* e = nullptr; const double d = strtod(v.c_str(), &e); return e && e != v.c_str() && *e == 0 && std::isfinite(d) ? d : def; };
    while (std::getline(in, l)) {
        if (l.empty() || l[0] == '#') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trimS(l.substr(0, eq));
        std::string v = l.substr(eq + 1);
        if (!v.empty() && v.back() == '\r') v.pop_back();
        if (k == "rig") s.rig = v;
        else if (k == "whole_span") s.wholeSpan = flag(v, s.wholeSpan);
        else if (k == "centre_mhz") s.centreMHz = dbl(v, s.centreMHz);
        else if (k == "span_mhz") s.spanMHz = dbl(v, s.spanMHz);
        else if (k == "rate_auto") s.rateAuto = flag(v, s.rateAuto);
        else if (k == "rate_msps") s.rateMsps = dbl(v, s.rateMsps);
        else if (k == "gain_manual") s.gainManual = flag(v, s.gainManual);
        else if (k == "gain_db") s.gainDb = dbl(v, s.gainDb);
        else if (k == "data_off") s.dataOff = splitSet(v);
        else if (k == "restricted_on") s.restrictedOn = splitSet(v);
        else if (k == "legal_ack") s.legalAck = flag(v, false);
        else if (k == "demods") s.demods = v;
        else if (k == "max_users") s.maxUsers = std::clamp(atoi(v.c_str()), 1, 1000);
        else if (k == "user_kbps") s.userKbps = std::clamp(atoi(v.c_str()), 0, 100000);
        else if (k == "audio_only") s.audioOnly = flag(v, false);
        else if (k == "limit_to_range") s.limitToRange = flag(v, false);
        else if (k == "password") s.password = v;
        else if (k == "link") s.link = (v == "own" || v == "lan") ? v : "tunnel";
        else if (k == "own_url") s.ownUrl = v;
        else if (k == "list_public") s.listPublic = flag(v, false);
        else if (k == "name") s.name = v;
        else if (k == "desc") s.desc = v;
        else if (k == "loc") s.loc = v;
        else if (k == "contact") s.contact = v;
        else if (k == "radio_label") s.radioLabel = v;
        else if (k == "dir_url") s.dirUrl = v;
        else if (k == "loc_decimals") s.locDecimals = std::clamp(atoi(v.c_str()), 0, 5);
        else if (k == "reach") s.reach = flag(v, false);
    }
    return s;
}

// ---------------------------------------------------------------- plan
double defaultCentreMHz(const RigCaps& c) {
    if (c.maxHz > 0 && (98e6 < c.minHz || 98e6 > c.maxHz)) return std::round((c.minHz + c.maxHz) / 2e4) / 100.0;
    return 98.0;
}

Plan resolvePlan(const ShareSettings& s, const RigCaps& rig) {
    Plan p;
    p.device = rig.test ? "test" : rig.name;
    p.centreHz = s.centreMHz * 1e6;
    double maxRate = rig.steadyRate > 0 && (rig.maxRate <= 0 || rig.steadyRate < rig.maxRate) ? rig.steadyRate : rig.maxRate;
    if (!s.rateAuto) p.rateHz = s.rateMsps * 1e6;
    else if (s.wholeSpan) p.rateHz = maxRate > 0 ? maxRate : 2.4e6;
    else p.rateHz = std::ceil(s.spanMHz * 1e6 / 0.96 / 1e4) * 1e4;   // the listeners' usable part is 96 % of the capture
    p.loHz = std::max(0.0, p.centreHz - p.rateHz * 0.48);
    p.hiHz = p.centreHz + p.rateHz * 0.48;
    if (!rig.test && rig.maxHz > 0 && (p.centreHz < rig.minHz || p.centreHz > rig.maxHz))
        p.warning = "The centre is outside what this radio is specified for (" + mhz(rig.minHz) + " - " + mhz(rig.maxHz) + "). OnAir will still try; nothing is limited.";
    else if (!rig.test && rig.maxRate > 0 && p.rateHz > rig.maxRate * 1.0001)
        p.warning = "That is above the " + num(std::round(rig.maxRate / 1e5) / 10) + " Msps the radio reports. OnAir will ask the driver anyway; nothing is limited, but it may refuse.";
    return p;
}

std::string rangeSpecMHz(double loHz, double hiHz) {
    char b[64];
    snprintf(b, sizeof b, "%.6g-%.6g", loHz / 1e6, hiHz / 1e6);
    return b;
}
std::string coverageText(const Plan& p) { return rangeSpecMHz(p.loHz, p.hiHz); }

// ---------------------------------------------------------------- decoders
std::vector<DecoderRow> decoderRows(const ShareSettings& s, const Plan& plan) {
    std::vector<DecoderRow> rows;
    const double lo = plan.loHz / 1e6, hi = plan.hiHz / 1e6;
    const std::set<std::string> dem = splitSet(s.demods);
    for (const Dem& d : kDemods) {
        DecoderRow r;
        r.kind = d.name; r.label = d.label; r.group = "Listening modes"; r.isDemod = true; r.cpu = 1;
        r.on = dem.count(d.name) > 0;
        rows.push_back(r);
    }
    for (const Dec& d : dataDecoders()) {
        DecoderRow r;
        r.kind = d.kind; r.label = d.label; r.group = "Data decoders"; r.cpu = d.cpu; r.needsCodec = d.codec;
        r.possible = overlaps(d.bands, lo, hi, r.why);
        r.on = s.dataOff.count(d.kind) == 0;
        rows.push_back(r);
    }
    for (const Dec& d : restrictedDecoders()) {
        DecoderRow r;
        r.kind = d.kind; r.label = d.label; r.group = "Needs your decision (legal notice)"; r.cpu = d.cpu; r.needsCodec = d.codec; r.restricted = true;
        r.possible = overlaps(d.bands, lo, hi, r.why);
        r.on = s.legalAck && s.restrictedOn.count(d.kind) > 0;
        rows.push_back(r);
    }
    return rows;
}

int cpuScore(const std::vector<DecoderRow>& rows) {
    int t = 0;
    for (const auto& r : rows) if (r.on && r.possible && !r.isDemod) t += r.cpu;
    return t;
}

void setGroup(ShareSettings& s, const std::string& group, bool on) {
    if (group == "Listening modes") { s.demods = on ? "am,nfm,wfm,usb,lsb,cw" : ""; return; }
    if (group == "Data decoders") {
        s.dataOff.clear();
        if (!on) for (const Dec& d : dataDecoders()) s.dataOff.insert(d.kind);
        return;
    }
    s.restrictedOn.clear();
    if (on && s.legalAck) for (const Dec& d : restrictedDecoders()) s.restrictedOn.insert(d.kind);
}

// ---------------------------------------------------------------- server settings
std::string buildServerConf(const ShareSettings& s, const Plan& plan, int port, int apiPort, const std::string& publicUrl, const std::string& adminToken) {
    std::ostringstream c;
    c << "# written by OnAir (Server > Share on web); changes made here are overwritten\n"
      << "web=1\nweb_port=" << port << "\nweb_bind=" << (s.link == "lan" ? "0.0.0.0" : "127.0.0.1") << "\nweb_name=" << s.name << "\n"
      << "port=" << apiPort << "\nlan=0\nweb_trust_proxy=1\n";   // the tunnel program is on this computer and ends TLS: without this the browser's Origin (https://...) is refused and every listener looks like 127.0.0.1
    if (!publicUrl.empty()) c << "web_public_url=" << publicUrl << "\n";
    if (!adminToken.empty()) c << "web_admin_token=" << adminToken << "\n";
    c << "device=" << plan.device << "\nmode=fm\nfreq_mhz=" << num(plan.centreHz / 1e6) << "\nrate_msps=" << num(plan.rateHz / 1e6) << "\nautostart=1\n";
    if (s.gainManual) c << "gain_db=" << num(s.gainDb) << "\n";
    c << "web_max_users=" << std::clamp(s.maxUsers, 1, 1000) << "\n";
    if (s.userKbps > 0) c << "web_user_kbps=" << std::max(32, s.userKbps) << "\n";
    c << "web_spectrum=" << (s.audioOnly ? 0 : 1) << "\n";
    if (!s.password.empty()) c << "web_auth=password\nweb_password=" << s.password << "\n";
    else c << "web_auth=open\n";
    c << "web_demods=" << s.demods << "\n";
    std::string off, on;
    {
        std::set<std::string> offs;
        for (const auto& k : s.dataOff) offs.insert(k);
        off = joinSet(offs);
        if (s.legalAck) on = joinSet(s.restrictedOn);
    }
    c << "web_decoders_off=" << off << "\n";
    if (s.legalAck) c << "web_legal_ack=" << onair_web::kLegalNoticeVersion << "\nweb_decoders_on=" << on << "\n";
    if (s.limitToRange) c << "web_allow_range=" << rangeSpecMHz(plan.loHz, plan.hiHz) << "MHz\n";
    return c.str();
}

std::string buildDirectoryConf(const ShareSettings& s, const Plan& plan, int port, const std::string& publicUrl, const std::string& ack, const std::string& keyFile) {
    std::ostringstream c;
    c << "# written by OnAir (the listing program)\nweb=1\nweb_port=" << port << "\nweb_name=" << s.name << "\nweb_max_users=" << std::clamp(s.maxUsers, 1, 1000) << "\n"
      << "web_public_url=" << publicUrl << "\nweb_directory=1\nweb_directory_url=" << s.dirUrl << "\nweb_directory_key=" << keyFile << "\n"
      << "web_directory_loc=" << s.loc << "\nweb_directory_loc_decimals=" << s.locDecimals << "\nweb_directory_freq=" << coverageText(plan) << "\n"
      << "web_directory_radio=" << (s.radioLabel.empty() ? plan.device : s.radioLabel) << "\nweb_directory_contact=" << s.contact << "\nweb_directory_desc=" << s.desc << "\n"
      << "web_directory_reach=" << (s.reach ? 1 : 0) << "\nweb_directory_ack=" << ack << "\n";
    return c.str();
}

// ---------------------------------------------------------------- step 2
const char* step2Name(Step2 s) {
    switch (s) {
        case Step2::Off: return "off";
        case Step2::Previewing: return "previewing";
        case Step2::PreviewReady: return "preview";
        case Step2::PreviewFailed: return "preview-failed";
        case Step2::Listing: return "listing";
        case Step2::Listed: return "listed";
        case Step2::ListFailed: return "failed";
    }
    return "";
}

void step2PreviewResult(Step2State& st, const std::string& out, bool exitedOk) {
    st.problem = Problem{};
    if (exitedOk && out.compare(0, 4, "ack=") == 0) {
        const size_t nl = out.find('\n');
        st.ack = out.substr(4, nl == std::string::npos ? std::string::npos : nl - 4);
        const size_t sep = out.find("\n---\n");
        st.preview = sep == std::string::npos ? std::string() : out.substr(sep + 5);
        if (!st.ack.empty() && !st.preview.empty()) { st.s = Step2::PreviewReady; return; }
    }
    st.s = Step2::PreviewFailed;
    st.problem.code = "preview";
    const size_t e = out.find("error=");
    st.problem.message = e != std::string::npos ? "The listing cannot be made: " + trimS(out.substr(e + 6)) : "The preview could not be made (the server program did not answer).";
    st.problem.advice = "Check the name, location and the public address. The location is 'latitude,longitude' (for example 54.69,25.28) or a locator such as KO24; you can also leave it empty.";
}

void step2Confirm(Step2State& st, long long now) {
    if (st.s != Step2::PreviewReady) return;
    st.s = Step2::Listing;
    st.startedAt = now;
    st.fails = 0; st.lastOk = 0; st.lastTry = 0; st.nextTry = 0;
    st.problem = Problem{};
}

void step2Cancel(Step2State& st) { st = Step2State{}; }

void step2Status(Step2State& st, const std::string& json, bool alive, long long now) {
    if (st.s != Step2::Listing && st.s != Step2::Listed && st.s != Step2::ListFailed) return;
    onair_web::DirStatus ds;
    const bool have = !json.empty() && onair_web::dirStatusParse(json, ds);
    if (!alive && st.s != Step2::ListFailed) {
        st.s = Step2::ListFailed;
        if (have && !ds.last.ok && !ds.last.message.empty()) st.problem = Problem{ds.last.code, ds.last.message, ds.last.advice, ds.last.retrySoon};
        else st.problem = Problem{"stopped", "The listing program stopped.", "Press 'List it' again. If it stops again, copy the log (Help > Report a problem).", false};
        return;
    }
    if (!have) {
        if (now - st.startedAt > 45 && st.s == Step2::Listing) {
            st.s = Step2::ListFailed;
            st.problem = Problem{"silent", "The listing program did not report anything for 45 seconds.", "Press 'Test connection' to see which part is not working, then try again.", false};
        }
        return;
    }
    st.fails = ds.fails; st.lastOk = ds.lastOk; st.lastTry = ds.lastTry; st.nextTry = ds.nextTry;
    if (ds.state == "ok") { st.s = Step2::Listed; st.problem = Problem{}; return; }
    if (ds.state == "error") {
        st.problem = Problem{ds.last.code, ds.last.message, ds.last.advice, ds.last.retrySoon};
        // a failure that fixes itself is retried quietly for a while; any other one is shown at once as the thing to fix
        const bool giveUp = !ds.last.retrySoon || ds.fails >= 8;
        st.s = giveUp ? Step2::ListFailed : Step2::Listing;
        if (st.s == Step2::ListFailed && ds.last.code != "resolving" && ds.last.retrySoon == false) { /* stays visible; the program keeps retrying in the background */ }
    }
}

bool parseTestLine(const std::string& line, TestStep& out) {
    onair_web::Json j;
    std::string e;
    if (!onair_web::jsonParse(line, j, e) || j.kind != onair_web::Json::Obj) return false;
    auto str = [&](const char* k) { const auto* x = j.get(k); return x && x->kind == onair_web::Json::Str ? x->str : std::string(); };
    TestStep t;
    t.step = str("step");
    if (t.step.empty()) return false;
    const auto* ok = j.get("ok");
    t.ok = ok && ok->kind == onair_web::Json::Bool && ok->b;
    const auto* ms = j.get("ms");
    t.ms = ms && ms->kind == onair_web::Json::Num ? (long)ms->num : 0;
    t.code = str("code"); t.message = str("message"); t.advice = str("advice");
    t.done = t.step == "done";
    out = t;
    return true;
}

const char* testStepTitle(const std::string& step) {
    if (step == "directory") return "Directory";
    if (step == "local") return "Web server on this computer";
    if (step == "public") return "Your public address";
    return "";
}

std::string linkQrText(const std::string& url) { return url; }

bool parseStats(const std::string& json, ShareStats& out) {
    onair_web::Json j;
    std::string e;
    if (!onair_web::jsonParse(json.substr(0, onair_web::kJsonMaxInput), j, e) || j.kind != onair_web::Json::Obj) return false;
    auto n = [&](const char* k) { const auto* x = j.get(k); return x && x->kind == onair_web::Json::Num ? x->num : 0.0; };
    ShareStats s;
    s.users = (int)n("users"); s.maxUsers = (int)n("max_users"); s.kbps = n("kbps"); s.maxKbps = n("max_kbps"); s.cpuPct = n("cpu_pct"); s.iqDropped = (long long)n("iq_dropped");
    if (const auto* c = j.get("capture")) {
        if (const auto* a = c->get("centre_hz")) if (a->kind == onair_web::Json::Num) s.centreHz = a->num;
        if (const auto* a = c->get("rate_hz")) if (a->kind == onair_web::Json::Num) s.rateHz = a->num;
    }
    s.ok = true;
    out = s;
    return true;
}

} // namespace webshare
