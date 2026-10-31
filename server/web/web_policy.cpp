#include "web_policy.h"
#include "server_config.h"
#include <cctype>
#include <random>
#include <sstream>

namespace onair_web {

const std::vector<DecoderInfo>& decoderCatalogue() {
    static const std::vector<DecoderInfo> v = {
        {"host", DecoderClass::Data, "The host receiver's own mode (its telemetry)"},
        {"adsb", DecoderClass::Data, "ADS-B / Mode S aircraft"},
        {"ais", DecoderClass::Data, "AIS ships"},
        {"aprs", DecoderClass::Data, "APRS / AX.25 packet"},
        {"acars", DecoderClass::Data, "ACARS"},
        {"ft8", DecoderClass::Data, "FT8"},
        {"ft4", DecoderClass::Data, "FT4"},
        {"wspr", DecoderClass::Data, "WSPR"},
        {"sstv", DecoderClass::Data, "SSTV images"},
        {"wefax", DecoderClass::Data, "Weather fax"},
        {"apt", DecoderClass::Data, "NOAA APT weather satellite images"},
        {"meteor", DecoderClass::Data, "Meteor-M LRPT images"},
        {"rds", DecoderClass::Data, "RDS"},
        {"cw", DecoderClass::Other, "Morse"},
        {"rtty", DecoderClass::Other, "RTTY"},
        {"dmr", DecoderClass::Voice, "DMR"},
        {"p25", DecoderClass::Voice, "P25"},
        {"nxdn", DecoderClass::Voice, "NXDN"},
        {"tetra", DecoderClass::Voice, "TETRA"},
        {"dpmr", DecoderClass::Voice, "dPMR"},
        {"dstar", DecoderClass::Voice, "D-STAR"},
        {"ysf", DecoderClass::Voice, "Yaesu System Fusion"},
        {"pocsag", DecoderClass::Pager, "POCSAG pagers"},
        {"flex", DecoderClass::Pager, "FLEX pagers"},
    };
    return v;
}

namespace {
// The web-safe allow-list (public broadcasts and open data). Everything else is blocked unless the host turns it on after the legal notice.
const std::set<std::string>& webSafeKinds() {
    static const std::set<std::string> s = {
        // analog audio demodulators and the spectrum views
        "fm", "am", "general", "airband", "specan", "specx", "wb",
        // data modes the owner approved for web listeners
        "adsb", "uat", "ais", "aprs", "packet", "acars", "vdl2", "ft8", "ft4", "wspr", "sstv", "wefax",
        // weather-satellite images, RDS of the FM broadcast
        "apt", "goes", "meteor", "hrpt", "elektro", "rds",
        // the host decoder itself: the policy then checks the host's running mode
        "host"};
    return s;
}
const std::set<std::string>& voiceKinds() {
    static const std::set<std::string> s = {"dmr", "p25", "nxdn", "tetra", "dpmr", "dstar", "d-star", "ysf", "fusion", "yaesu", "m17", "dmr_wide",
                                            "p25_wide", "nxdn_wide", "edacs", "tetrapol", "mpt1327", "ltr", "trunk", "trunking", "p25p2", "marinevhf"};
    return s;
}
const std::set<std::string>& pagerKinds() {
    static const std::set<std::string> s = {"pocsag", "flex", "pager", "pagers", "ermes", "golay"};
    return s;
}
// Every other mode id of the receiver registry, listed so that a new mode that nobody classified is noticed by the test. They are all blocked.
const std::set<std::string>& restrictedKnownKinds() {
    static const std::set<std::string> s = {
    "aero", "ale", "ant", "ardop", "astro", "atsc", "atsc3", "atv", "ble", "bletracker", "cdr", "cell", "codar", "contestia", "cquam",
    "cubesat", "cw", "dab", "datawell", "dect", "dgps", "dji", "dme", "dominoex", "drm", "dsc", "dtmb", "dvb", "dvbc", "dvbs", "eczas",
    "eloran", "flarm", "fpv", "freedv", "fsq", "fst4", "ft2", "globalstar", "gnss", "goesdcs", "gsmr", "halow", "hamdrm", "hdr", "hell",
    "hfbeacon", "hfdig", "hfdl", "ifkp", "ils", "inmc", "iono", "iridium", "isdbt", "ism", "js8", "jt4", "jt65", "jt9", "kaal", "kaft",
    "kait", "kapp", "kavb", "kaws", "klw", "lbj", "loraaprs", "loraraw", "lorawan", "mac", "marine", "mesh", "meshcom", "meshcore",
    "meshtastic", "mfsk", "milhf", "modeac", "mscatter", "msk144", "mt63", "multitone", "navtex", "nbiot", "ndb", "nr", "olivia",
    "orbcomm", "pager", "psk", "q65", "qo100", "rid", "rtty", "same", "sarsat", "satbeacon", "sonde", "t", "t2", "tdcs", "thor", "time",
    "tones", "uplink", "vdes", "vdl4", "vor", "vorils", "wifi", "wisun", "wmbus", "zigbee", "zwave",
    // 3.0 modes: blocked until the owner approves them for web listeners
    "argos", "asterix", "horus", "isdbs", "lorasat", "lorawanwide", "nfc", "sbas", "sigfox",
    // 3.1 modes: blocked likewise
    "pactor", "pskd",
    // 3.2 modes: blocked likewise
    "roshf", "ssdv", "ebicab", "esubghz", "inmsat", "nrf24",
    // 3.4 modes: blocked likewise
    "iptv", "atis",   // (esubghz was never classified: web_safety failed on integ-32)
    };
    return s;
}
} // namespace

DecoderClass decoderClass(const std::string& k) {
    if (voiceKinds().count(k)) return DecoderClass::Voice;
    if (pagerKinds().count(k)) return DecoderClass::Pager;
    if (webSafeKinds().count(k)) return DecoderClass::Data;
    return DecoderClass::Other;   // fail closed: a mode id that is not on the allow-list is blocked
}

bool decoderKindClassified(const std::string& k) {
    return voiceKinds().count(k) || pagerKinds().count(k) || webSafeKinds().count(k) || restrictedKnownKinds().count(k) || [&] {
        for (const auto& d : decoderCatalogue()) if (k == d.kind) return true;
        return false;
    }();
}

const char* className(DecoderClass c) { return c == DecoderClass::Voice ? "voice" : c == DecoderClass::Pager ? "pager" : c == DecoderClass::Other ? "restricted" : "data"; }

const char* legalNoticeText() {
    return "Before you let web listeners decode digital voice, pager, mesh or other message traffic, check the law where your receiver is and where your listeners are. "
           "In many countries it is an offence to disclose, publish or pass on the content of radio messages that are not meant for the general "
           "public (for example the EU member states' telecommunications laws, the UK Wireless Telegraphy Act 2006 s.48, the US 47 U.S.C. 605 and "
           "the ECPA). Pager messages often carry personal and medical data. Turning a decoder on here makes its output available to everyone who "
           "can open your OnAir Web page. You, the host, are responsible for what your server makes available. OnAir never decodes encrypted "
           "traffic and never gives web listeners keys. By accepting you confirm that you have checked this and take that responsibility.";
}

static std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == ';') { if (!cur.empty()) v.push_back(cur); cur.clear(); }
        else cur += (char)std::tolower((unsigned char)c);
    }
    if (!cur.empty()) v.push_back(cur);
    return v;
}
static std::string joinSet(const std::set<std::string>& s) {
    std::string o;
    for (const auto& x : s) { if (!o.empty()) o += ','; o += x; }
    return o;
}

void WebPolicy::load(const onair_server::ConfigFile& c) {
    if (c.has("web_demods")) {
        demods_.clear();
        for (const auto& n : splitList(c.get("web_demods"))) { Demod d; if (demodFromName(n, d)) demods_.insert(d); }
    }
    dataOff_.clear(); restrictedOn_.clear();
    for (const auto& n : splitList(c.get("web_decoders_off"))) if (decoderClass(n) == DecoderClass::Data) dataOff_.insert(n);
    for (const auto& n : splitList(c.get("web_decoders_on"))) if (decoderClass(n) != DecoderClass::Data) restrictedOn_.insert(n);
    legalAck_ = c.get("web_legal_ack");
    { const std::string t = c.get("web_allow_tx"); allowTx_ = t == "1" || t == "true" || t == "on"; }
}

void WebPolicy::save(onair_server::ConfigFile& c) const {
    std::string d;
    for (Demod x : demods_) { if (!d.empty()) d += ','; d += demodName(x); }
    c.set("web_demods", d);
    c.set("web_decoders_off", joinSet(dataOff_));
    c.set("web_decoders_on", joinSet(restrictedOn_));
    c.set("web_legal_ack", legalAck_);
    c.set("web_allow_tx", allowTx_ ? "1" : "0");
}

bool WebPolicy::decoderAllowed(const std::string& kind) const {
    if (decoderClass(kind) == DecoderClass::Data) return !dataOff_.count(kind);
    return legalAccepted() && restrictedOn_.count(kind) > 0;   // off by default; on only with the notice accepted
}

bool WebPolicy::setDecoder(const std::string& kind, bool on, const std::string& ack, std::string& err) {
    if (kind.empty() || kind.size() > 24) { err = "bad decoder"; return false; }
    if (decoderClass(kind) == DecoderClass::Data) {
        if (on) dataOff_.erase(kind); else dataOff_.insert(kind);
        return true;
    }
    if (!on) { restrictedOn_.erase(kind); return true; }
    if (ack != kLegalNoticeVersion) { err = std::string("accept the legal notice first (accept_legal_notice: \"") + kLegalNoticeVersion + "\")"; return false; }
    legalAck_ = ack;
    restrictedOn_.insert(kind);
    return true;
}

bool WebPolicy::setDemod(Demod d, bool on) {
    if ((int)d < 0 || d >= Demod::Count) return false;
    if (on) demods_.insert(d); else demods_.erase(d);
    return true;
}

std::string WebPolicy::json() const {
    std::ostringstream o;
    o << "{\"demods\":[";
    bool first = true;
    for (Demod d : demods_) { o << (first ? "" : ",") << '"' << demodName(d) << '"'; first = false; }
    o << "],\"decoders\":{";
    first = true;
    for (const auto& d : decoderCatalogue()) { o << (first ? "" : ",") << '"' << d.kind << "\":{\"on\":" << (decoderAllowed(d.kind) ? "true" : "false") << ",\"class\":\"" << className(d.cls) << "\"}"; first = false; }
    o << "},\"allow_tx\":" << (allowTx_ ? "true" : "false") << ",\"legal_ack\":" << (legalAccepted() ? "true" : "false") << ",\"legal_version\":\"" << kLegalNoticeVersion << "\"}";
    return o.str();
}

// ---------------------------------------------------------------- access
const char* userClassName(UserClass c) { return c == UserClass::Member ? "member" : c == UserClass::Invited ? "invited" : "public"; }
bool authModeFromName(const std::string& s, AuthMode& m) {
    if (s == "open") m = AuthMode::Open; else if (s == "password") m = AuthMode::Password; else if (s == "invite") m = AuthMode::Invite; else return false;
    return true;
}
const char* authModeName(AuthMode m) { return m == AuthMode::Password ? "password" : m == AuthMode::Invite ? "invite" : "open"; }

AuthResult authenticate(const AuthConfig& a, const std::string& password, const std::string& invite) {
    AuthResult r;
    if (!invite.empty()) {
        // every token is compared (constant time each), so the time does not tell which one matched or how many there are
        const InviteToken* hit = nullptr;
        for (const auto& t : a.invites) if (t.token.size() >= 16 && ctEq(invite, t.token)) hit = &t;
        if (hit) { r.ok = true; r.cls = UserClass::Invited; r.label = hit->label; r.maxMinutes = hit->maxMinutes; return r; }
        r.why = "invalid invite";
        return r;
    }
    switch (a.mode) {
    case AuthMode::Open: r.ok = true; r.cls = UserClass::Public; return r;
    case AuthMode::Password:
        if (a.password.size() >= 8 && ctEq(password, a.password)) { r.ok = true; r.cls = UserClass::Member; return r; }
        r.why = password.empty() ? "password required" : "wrong password";
        return r;
    case AuthMode::Invite: r.why = "invite required"; return r;
    }
    return r;
}

std::string newToken(int bytes) {
    std::random_device rd;
    static const char* hx = "0123456789abcdef";
    std::string s;
    for (int i = 0; i < bytes; i++) { const unsigned v = rd() & 0xFF; s += hx[v >> 4]; s += hx[v & 15]; }
    return s;
}

bool FailLimiter::blocked(const std::string& ip, double now) {
    auto it = t_.find(ip);
    return it != t_.end() && it->second.until > now;
}
void FailLimiter::fail(const std::string& ip, double now) {
    if (t_.size() > 4096) {   // bounded: drop the entries that are no longer blocking
        for (auto it = t_.begin(); it != t_.end();) { if (it->second.until <= now && now - it->second.first > 60) it = t_.erase(it); else ++it; }
        if (t_.size() > 4096) t_.clear();
    }
    E& e = t_[ip];
    if (now - e.first > 60) { e.n = 0; e.first = now; }
    if (++e.n >= 5) { e.until = now + 60; e.n = 0; e.first = now; }
}
void FailLimiter::clear(const std::string& ip) { t_.erase(ip); }

} // namespace onair_web
