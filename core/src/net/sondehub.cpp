// SondeHub radiosonde upload (sondehub.h): the telemetry and listener JSON, gzip, the batch and the upload thread.
// The HTTPS requests go through the app's HTTP client (dect2/tv/iptv.h: OpenSSL on Linux, the system TLS on macOS and Windows).
#include "dect2/net/sondehub.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/timecompat.h"
#include "dect2/tv/iptv.h"
#include "tcp_client.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#include "../../../third_party/stb/stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace dect2 {
namespace sondehub {

const char* softwareVersion() { return ONAIR_VERSION; }
std::string userAgent() { return std::string("OnAir-") + ONAIR_VERSION; }

namespace {
std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
    return s.substr(i);
}
bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
std::string afterDash(const std::string& s) { const size_t d = s.find('-'); return d == std::string::npos ? s : s.substr(d + 1); }
bool serialChars(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    for (char c : s) if (!std::isalnum((unsigned char)c) && c != '-' && c != '_') return false;
    return true;
}
bool finite(double v) { return std::isfinite(v); }
std::string num(double v, int dec) { char b[48]; snprintf(b, sizeof b, "%.*f", dec, v); return b; }
}  // namespace

bool identify(const SondeInfo& s, Ident& id, std::string* why) {
    auto no = [&](const char* r) { if (why) *why = r; return false; };
    id = Ident();
    const std::string ser = trimmed(s.serial);
    if (ser.empty() || ser.find('?') != std::string::npos) return no("serial number not known yet");
    switch (s.kind) {
    case SondeRs41:
        id.manufacturer = "Vaisala"; id.type = "RS41"; id.serial = ser;
        if (startsWith(s.subtype, "RS41-")) id.subtype = s.subtype;
        break;
    case SondeRs92:
        id.manufacturer = "Vaisala"; id.type = "RS92"; id.serial = ser;
        if (startsWith(s.subtype, "RS92-")) id.subtype = s.subtype;
        break;
    case SondeDfm: {   // "DFM-17" -> "DFM17", the spelling of the format page
        id.manufacturer = "Graw"; id.type = "DFM"; id.serial = ser; id.slow = true;
        std::string st;
        for (char c : s.subtype) if (c != '-') st += c;
        if (startsWith(st, "DFM") && st.size() > 3) id.subtype = st;
        break;
    }
    case SondeM10: id.manufacturer = "Meteomodem"; id.type = "M10"; id.serial = ser; break;
    case SondeM20: id.manufacturer = "Meteomodem"; id.type = "M20"; id.serial = ser; break;
    case SondeLms6: {   // the band says which one; without a frequency the decoder's subtype
        id.manufacturer = "Lockheed Martin"; id.serial = afterDash(ser);
        const bool l = s.freqHz > 1e9 || (s.freqHz <= 0 && s.subtype.find("1680") != std::string::npos);
        id.type = l ? "LMS6-1680" : "LMS6-400";
        break;
    }
    case SondeImet54: id.manufacturer = "Intermet Systems"; id.type = "iMet-54"; id.serial = afterDash(ser); break;
    case SondeMrz:    // "MRZ-5667-39155" -> "5667-39155"
        id.manufacturer = "Meteo-Radiy"; id.type = "MRZ"; id.serial = startsWith(ser, "MRZ-") ? ser.substr(4) : ser;
        if (startsWith(s.subtype, "MRZ-N1")) id.subtype = "MRZ-N1";
        break;
    case SondeMeisei: id.manufacturer = "Meisei"; id.type = "iMS-100"; id.serial = afterDash(ser); id.slow = true; break;
    case SondeImet: return no("iMet-1/4 send no serial number: not uploaded (auto_rx makes one up from data OnAir does not have)");
    default: return no("sonde type not known to SondeHub");
    }
    if (!serialChars(id.serial)) return no("serial number has unexpected characters");
    // frame: the sonde's counter; M10, M20 and DFM have no unique one, the format page says to use the sonde time as a Unix timestamp
    if (s.kind == SondeDfm || s.kind == SondeM10 || s.kind == SondeM20 || s.frame < 0) id.frame = s.hasTime ? (long long)std::floor(s.unixTime) : -1;
    else id.frame = s.frame;
    if (id.frame < 0) return no("no frame number or time yet");
    return true;
}

std::string isoTime(double t) {
    const double f = std::floor(t);
    struct tm g; gmTime((time_t)f, &g);
    long us = std::lround((t - f) * 1e6); if (us > 999999) us = 999999;
    char b[48];
    snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, us);
    return b;
}

std::string httpDate(double t) {
    static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    struct tm g; gmTime((time_t)std::floor(t), &g);
    char b[48];
    snprintf(b, sizeof b, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[g.tm_wday % 7], g.tm_mday, mo[g.tm_mon % 12], g.tm_year + 1900, g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

bool validUploader(const std::string& call) {
    const std::string c = trimmed(call);
    if (c.size() < 3 || c.size() > 64) return false;
    for (unsigned char ch : c) if (ch < 0x20 || ch == 0x7f) return false;
    return true;
}

namespace {
std::string positionJson(const SondeHubConfig& c, const SondeHubStation& st) {
    if (!c.sharePos || !st.set || !(std::fabs(st.lat) <= 90) || !(std::fabs(st.lon) <= 180)) return "null";
    return "[" + num(st.lat, 5) + "," + num(st.lon, 5) + "," + num(finite(st.altM) ? st.altM : 0, 1) + "]";
}
}  // namespace

std::string telemetryJson(const SondeHubConfig& c, const SondeHubStation& st, const SondeInfo& s, double wallUtc, std::string* why, Ident* idOut) {
    auto no = [&](const std::string& r) { if (why) *why = r; return std::string(); };
    if (!validUploader(c.call)) return no("needs a callsign");
    if (!s.hasPos || !(std::fabs(s.lat) <= 90) || !(std::fabs(s.lon) <= 180) || (s.lat == 0 && s.lon == 0)) return no("no GPS position");
    if (!(s.altM > -500 && s.altM < 50000)) return no("altitude out of range");
    if (s.sats >= 0 && s.sats < 4) return no("no 3D GPS fix (" + std::to_string(s.sats) + " satellites)");
    if (!s.hasTime || !(s.unixTime > 946684800)) return no("no date and time from the sonde yet");
    if (std::fabs(s.unixTime - wallUtc) > 3600) return no("sonde time more than an hour from the clock (an old recording?): not uploaded");
    Ident id;
    std::string w;
    if (!identify(s, id, &w)) return no(w);
    if (idOut) *idOut = id;
    std::string o = "{";
    o += "\"software_name\":\"" + std::string(kSoftwareName) + "\"";
    o += ",\"software_version\":\"" + jsonEscape(softwareVersion()) + "\"";
    o += ",\"uploader_callsign\":\"" + jsonEscape(trimmed(c.call)) + "\"";
    o += ",\"time_received\":\"" + isoTime(wallUtc) + "\"";
    o += ",\"manufacturer\":\"" + id.manufacturer + "\"";
    o += ",\"type\":\"" + id.type + "\"";
    if (!id.subtype.empty()) o += ",\"subtype\":\"" + jsonEscape(id.subtype) + "\"";
    o += ",\"serial\":\"" + jsonEscape(id.serial) + "\"";
    o += ",\"frame\":" + std::to_string(id.frame);
    o += ",\"datetime\":\"" + isoTime(s.unixTime) + "\"";
    o += ",\"lat\":" + num(s.lat, 6) + ",\"lon\":" + num(s.lon, 6) + ",\"alt\":" + num(s.altM, 1);
    if (s.hasTemp && finite(s.tempC) && s.tempC > -273) o += ",\"temp\":" + num(s.tempC, 1);
    if (s.hasHumidity && finite(s.humidity) && s.humidity >= 0) o += ",\"humidity\":" + num(s.humidity, 1);
    if (s.hasPressure && finite(s.pressureHpa) && s.pressureHpa >= 0) o += ",\"pressure\":" + num(s.pressureHpa, 2);
    if (s.hasVel && finite(s.vSpeed) && finite(s.hSpeed) && finite(s.headingDeg)) {
        double hd = std::fmod(s.headingDeg, 360.0); if (hd < 0) hd += 360;
        o += ",\"vel_v\":" + num(s.vSpeed, 2) + ",\"vel_h\":" + num(s.hSpeed, 2) + ",\"heading\":" + num(hd, 1);
    }
    if (s.sats >= 0) o += ",\"sats\":" + std::to_string(s.sats);
    if (s.batteryV >= 0 && finite(s.batteryV)) o += ",\"batt\":" + num(s.batteryV, 2);
    if (s.kind == SondeRs41 && s.burstKillS >= 0 && s.burstKillS < 65535) o += ",\"burst_timer\":" + std::to_string(s.burstKillS);
    if (s.freqHz > 0 && finite(s.freqHz)) o += ",\"frequency\":" + num(s.freqHz / 1e6, 4);
    if (finite(s.snrDb) && s.snrDb != 0) o += ",\"snr\":" + num(s.snrDb, 1);
    const std::string pos = positionJson(c, st);
    if (pos != "null") o += ",\"uploader_position\":" + pos;
    if (!trimmed(c.antenna).empty()) o += ",\"uploader_antenna\":\"" + jsonEscape(trimmed(c.antenna)) + "\"";
    o += "}";
    return o;
}

std::string listenerJson(const SondeHubConfig& c, const SondeHubStation& st) {
    std::string o = "{";
    o += "\"software_name\":\"" + std::string(kSoftwareName) + "\"";
    o += ",\"software_version\":\"" + jsonEscape(softwareVersion()) + "\"";
    o += ",\"uploader_callsign\":\"" + jsonEscape(trimmed(c.call)) + "\"";
    o += ",\"uploader_position\":" + positionJson(c, st);
    if (!trimmed(c.antenna).empty()) o += ",\"uploader_antenna\":\"" + jsonEscape(trimmed(c.antenna)) + "\"";
    if (!trimmed(c.email).empty()) o += ",\"uploader_contact_email\":\"" + jsonEscape(trimmed(c.email)) + "\"";
    o += std::string(",\"mobile\":") + (c.mobile && c.sharePos ? "true" : "false");
    o += "}";
    return o;
}

std::string jsonArray(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); i++) { if (i) o += ','; o += v[i]; }
    return o + "]";
}

uint32_t crc32(const void* data, size_t n) {
    static uint32_t tab[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

std::string gzip(const std::string& in) {
    std::string o("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10);   // magic, deflate, no flags, no time, no extra flags, OS unknown
    std::vector<unsigned char> d(in.begin(), in.end());
    int n = 0;
    unsigned char* z = d.empty() ? nullptr : stbi_zlib_compress(d.data(), (int)d.size(), &n, 8);
    if (z && n >= 6) o.append((const char*)z + 2, (size_t)n - 6);   // the zlib stream without its 2 byte header and Adler-32
    else o.append("\x03\x00", 2);                                     // an empty final fixed block
    if (z) STBIW_FREE(z);
    const uint32_t c = crc32(in.data(), in.size()), sz = (uint32_t)in.size();
    for (int i = 0; i < 4; i++) o += (char)((c >> (8 * i)) & 0xFF);
    for (int i = 0; i < 4; i++) o += (char)((sz >> (8 * i)) & 0xFF);
    return o;
}

namespace {
// every string value of the key in a JSON text (enough for SondeHub's {"message", "errors":[{"error_message"}], "warnings":[...]})
std::vector<std::string> stringsOf(const std::string& j, const std::string& key) {
    std::vector<std::string> v;
    const std::string k = "\"" + key + "\"";
    size_t p = 0;
    while ((p = j.find(k, p)) != std::string::npos) {
        p += k.size();
        while (p < j.size() && (j[p] == ' ' || j[p] == ':')) p++;
        if (p >= j.size() || j[p] != '"') continue;
        std::string s;
        for (p++; p < j.size() && j[p] != '"'; p++) {
            if (j[p] == '\\' && p + 1 < j.size()) { p++; s += j[p] == 'n' ? ' ' : j[p]; }
            else s += j[p];
        }
        v.push_back(s);
    }
    return v;
}
}  // namespace

std::string answerText(const std::string& body) {
    std::string o;
    std::set<std::string> seen;
    for (const char* k : {"message", "error_message", "warning_message", "description"})
        for (const std::string& s : stringsOf(body, k)) if (!s.empty() && seen.insert(s).second) { if (!o.empty()) o += "; "; o += s; }
    if (o.empty()) {
        for (char c : body) o += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
        o = trimmed(o);
    }
    if (o.size() > 400) { o.resize(400); o += "..."; }
    return o;
}

bool Batch::add(const std::string& serial, long long frame, const std::string& json, bool slow, double t) {
    auto it = last_.find(serial);
    if (it != last_.end() && it->second == frame) { duplicates++; return false; }
    if (q_.size() >= kMax) { overflow++; return false; }
    if (last_.size() > 2000) last_.clear();
    last_[serial] = frame;
    if (q_.empty() && since_ < 0) since_ = t;
    q_.push_back(json);
    slow_ = slow_ || slow;
    return true;
}
double Batch::intervalS(int uploadS) const {
    const double base = std::max(1, uploadS);
    return slow_ ? std::max(base, std::min(30.0, std::floor(base * 1.5))) : base;
}
bool Batch::due(double t, int uploadS) const { return !q_.empty() && since_ >= 0 && t - since_ >= intervalS(uploadS); }
std::vector<std::string> Batch::take(double t) {
    std::vector<std::string> v;
    v.swap(q_);
    slow_ = false;
    since_ = t;
    return v;
}

}  // namespace sondehub

// ------------------------------------------------------------------------------------------------------------------ the uploader
struct SondeHub::Impl {
    mutable std::mutex mu;
    std::condition_variable cv;
    SondeHubConfig cfg;
    SondeHubStation st;
    SondeHubTiming tm;
    SondeHubStatus stat;
    std::string lastJson, lastListener;
    sondehub::Batch batch;
    std::set<std::string> sondesInBatch;
    std::thread th;
    std::atomic<bool> quit{false};
    uint64_t gen = 0;                         // grows with every configure: a running upload of an older setting stops retrying
    // the thread's own
    double lastListenerT = -1e18; bool listenerSent = false; std::string lastListenerPos;

    bool sendOnce(const std::string& url, const std::string& body, bool gz, const std::string& date, int& code, std::string& answer, std::string& err) {
        iptv::HttpOptions o;
        o.userAgent = sondehub::userAgent();
        o.method = "PUT";
        o.requestBody = body;
        o.keepErrorBody = true;
        o.connectTimeoutMs = 20000; o.readTimeoutMs = 20000; o.maxRedirects = 0;
        o.headers.push_back({"Content-Type", "application/json"});
        if (gz) o.headers.push_back({"Content-Encoding", "gzip"});
        o.headers.push_back({"Date", date});
        iptv::HttpResult r;
        const bool ok = iptv::httpRequestAll(url, o, answer, r);
        code = r.status;
        if (!ok || r.status == 0) { err = r.error.empty() ? "no answer from SondeHub" : r.error; return false; }
        return true;
    }
    // one PUT with the retries of the API page: 5xx and no answer up to 5 tries, anything else is final. The answer code (0 none).
    int put(const std::string& path, const std::string& body, bool gz, std::string& answer, std::string& err, uint64_t myGen) {
        std::string base;
        double wait;
        { std::lock_guard<std::mutex> lk(mu); base = cfg.server; wait = tm.retryWaitS; }
        while (!base.empty() && base.back() == '/') base.pop_back();
        if (base.compare(0, 8, "https://") == 0 && !iptv::httpsAvailable()) { err = "this build of OnAir has no HTTPS support (on Linux: OpenSSL 3), so SondeHub cannot be reached"; return 0; }
        int code = 0;
        for (int tryNo = 0; tryNo < 5; tryNo++) {
            if (tryNo) {
                { std::lock_guard<std::mutex> lk(mu); stat.reconnects++; }
                for (double w = 0; w < wait && !quit.load(); w += 0.05) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                std::lock_guard<std::mutex> lk(mu);
                if (quit.load() || gen != myGen || !cfg.enabled) { err = "stopped"; return 0; }
            }
            err.clear(); answer.clear();
            const bool got = sendOnce(base + path, body, gz, sondehub::httpDate((double)unixNow()), code, answer, err);
            if (got && !(code >= 500 && code <= 599)) return code;
            if (got) err = "SondeHub is busy (HTTP " + std::to_string(code) + ")" + (answer.empty() ? "" : ": " + sondehub::answerText(answer));
        }
        return 0;   // gave up: err says why
    }

    void doListener(const SondeHubConfig& c, const SondeHubStation& s, uint64_t myGen) {
        const std::string js = sondehub::listenerJson(c, s);
        std::string ans, err;
        const int code = put("/listeners", js, false, ans, err, myGen);
        std::lock_guard<std::mutex> lk(mu);
        lastListener = js;
        if (code == 200 || code == 201 || code == 202) {
            stat.beacons++;
            if (stat.lastError.compare(0, 18, "station position: ") == 0) stat.lastError.clear();
        } else stat.lastError = "station position: " + (code ? "HTTP " + std::to_string(code) + (ans.empty() ? "" : ": " + sondehub::answerText(ans)) : err);
    }

    void doTelemetry(std::vector<std::string> objs, size_t sondes, uint64_t myGen) {
        const std::string js = sondehub::jsonArray(objs);
        const std::string gz = sondehub::gzip(js);
        std::string ans, err;
        { std::lock_guard<std::mutex> lk(mu); stat.state = "uploading " + std::to_string(objs.size()) + " frames"; }
        const int code = put("/sondes/telemetry", gz, true, ans, err, myGen);
        std::lock_guard<std::mutex> lk(mu);
        lastJson = js;
        char b[200];
        snprintf(b, sizeof b, "PUT /sondes/telemetry: %zu frames of %zu sonde%s, %zu bytes JSON, %zu bytes gzip, ", objs.size(), sondes, sondes == 1 ? "" : "s", js.size(), gz.size());
        stat.lastLine = b + (code ? "HTTP " + std::to_string(code) : std::string("no answer"));
        if (code == 200) { stat.sent += objs.size(); stat.uploads++; stat.connected = true; stat.lastError.clear(); }
        else if (code == 201 || code == 202) {   // taken, with notes about some frames (z-check and others)
            stat.sent += objs.size(); stat.uploads++; stat.connected = true;
            stat.lastError = "SondeHub took the upload with notes: " + sondehub::answerText(ans);
        } else {
            stat.dropped += objs.size(); stat.connected = false;
            stat.lastError = code ? "SondeHub refused the upload (HTTP " + std::to_string(code) + "): " + sondehub::answerText(ans) : "upload failed after 5 tries: " + err;
        }
    }

    void loop() {
        while (!quit.load()) {
            SondeHubConfig c; SondeHubStation s; SondeHubTiming t; uint64_t myGen;
            std::vector<std::string> objs; size_t sondes = 0;
            bool listener = false;
            {
                std::unique_lock<std::mutex> lk(mu);
                cv.wait_for(lk, std::chrono::milliseconds(200));
                if (quit.load()) break;
                c = cfg; s = st; t = tm; myGen = gen;
                if (!c.enabled) continue;
                if (!sondehub::validUploader(c.call)) { stat.state = "needs a callsign (Uploader callsign)"; continue; }
                const double now = steadyNowSec();
                const int up = t.uploadS > 0 ? (int)t.uploadS : std::max(10, std::min(60, c.uploadS));
                if (batch.due(now, up)) { objs = batch.take(now); sondes = sondesInBatch.size(); sondesInBatch.clear(); }
                const bool mobile = c.mobile && c.sharePos;
                const std::string pos = sondehub::listenerJson(c, s);
                if (mobile) listener = s.set && s.live && now - lastListenerT >= t.chaseEveryS;
                else listener = !listenerSent || now - lastListenerT >= t.listenerEveryS || (pos != lastListenerPos && now - lastListenerT >= 60);
                if (listener) { lastListenerT = now; listenerSent = true; lastListenerPos = pos; }
                if (objs.empty() && !listener)
                    stat.state = batch.size() ? "collecting: " + std::to_string(batch.size()) + " frames for the next upload" : "on, waiting for a sonde with a GPS fix";
            }
            if (listener) doListener(c, s, myGen);
            if (!objs.empty()) doTelemetry(std::move(objs), sondes, myGen);
        }
    }
};

SondeHub& SondeHub::get() { static SondeHub* h = new SondeHub(); return *h; }   // never destroyed: the receiver may still report at exit
SondeHub::SondeHub() : impl_(new Impl) {}

void SondeHub::configure(const SondeHubConfig& c) {
    Impl& I = *impl_;
    {
        std::lock_guard<std::mutex> lk(I.mu);
        const bool idChanged = c.call != I.cfg.call || c.server != I.cfg.server || c.mobile != I.cfg.mobile || c.sharePos != I.cfg.sharePos || c.antenna != I.cfg.antenna || c.email != I.cfg.email;
        I.cfg = c; I.gen++;
        if (idChanged) I.listenerSent = false;   // the station record again, with the new details
        if (!c.enabled) { I.batch.take(steadyNowSec()); I.sondesInBatch.clear(); I.stat = SondeHubStatus(); I.stat.state = "off"; }
        else if (I.stat.state.empty() || I.stat.state == "off") I.stat.state = "starting";
    }
    want_.store(c.enabled);
    if (c.enabled && !I.th.joinable()) { netInit(); I.quit = false; I.th = namedThread("onair-sondehub", [&I] { I.loop(); }); }
    I.cv.notify_one();
}
SondeHubConfig SondeHub::config() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->cfg; }
SondeHubStatus SondeHub::status() const {
    std::lock_guard<std::mutex> lk(impl_->mu);
    if (!impl_->cfg.enabled) { SondeHubStatus s; s.state = "off"; return s; }
    return impl_->stat;
}
std::string SondeHub::lastJson() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->lastJson; }
std::string SondeHub::lastListenerJson() const { std::lock_guard<std::mutex> lk(impl_->mu); return impl_->lastListener; }
void SondeHub::setStation(const SondeHubStation& st) { std::lock_guard<std::mutex> lk(impl_->mu); impl_->st = st; }
void SondeHub::setTiming(const SondeHubTiming& t) { std::lock_guard<std::mutex> lk(impl_->mu); impl_->tm = t; }

void SondeHub::frame(const SondeInfo& s, double wallUtc) {
    if (!wanted()) return;
    Impl& I = *impl_;
    std::lock_guard<std::mutex> lk(I.mu);
    if (!I.cfg.enabled) return;
    std::string why;
    sondehub::Ident id;
    const std::string js = sondehub::telemetryJson(I.cfg, I.st, s, wallUtc, &why, &id);
    if (js.empty()) { I.stat.skipped++; I.stat.skipWhy = (s.serial.empty() ? std::string(sondeKindName(s.kind)) : s.serial) + ": " + why; return; }
    if (I.batch.add(id.serial, id.frame, js, id.slow, steadyNowSec())) I.sondesInBatch.insert(id.type + ":" + id.serial);
    else if (I.batch.size() >= sondehub::Batch::kMax) I.stat.dropped++;
}

void SondeHub::stop() {
    Impl& I = *impl_;
    want_.store(false);
    I.quit = true;
    I.cv.notify_one();
    if (I.th.joinable()) I.th.join();
    std::lock_guard<std::mutex> lk(I.mu);
    I.cfg.enabled = false;
    I.batch.take(0); I.sondesInBatch.clear();
    I.stat = SondeHubStatus();
    I.listenerSent = false; I.lastListenerT = -1e18;
}

}  // namespace dect2
