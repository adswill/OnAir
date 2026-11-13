// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham APRS / AX.25 packet: frame and information field builders, the AFSK and G3RUH audio, the message session and the beacon (ham_aprs.h).
#include "dect2/limits.h"
#include "dect2/ham/ham_aprs.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace dect2::ham {

namespace {
constexpr double kPi = 3.14159265358979323846;
std::string up(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string fmtAddr(const ax25::Address& a) { return a.ssid ? a.call + "-" + std::to_string(a.ssid) : a.call; }
}  // namespace

bool ax25ParseAddress(const std::string& sIn, ax25::Address& a) {
    const std::string s = up(trim(sIn));
    const size_t d = s.find('-');
    const std::string c = s.substr(0, d);
    if (c.empty() || c.size() > 6) return false;
    for (char ch : c) if (!isalnum((unsigned char)ch)) return false;
    int ssid = 0;
    if (d != std::string::npos) {
        const std::string n = s.substr(d + 1);
        if (n.empty() || n.size() > 2) return false;
        for (char ch : n) if (!isdigit((unsigned char)ch)) return false;
        ssid = std::stoi(n);
        if (ssid > 15) return false;
    }
    a = ax25::Address();
    a.call = c;
    a.ssid = ssid;
    return true;
}

bool ax25ParsePath(const std::string& s, std::vector<ax25::Address>& path, std::string& err) {
    path.clear();
    err.clear();
    std::string cur;
    auto flush = [&]() -> bool {
        const std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return true;
        ax25::Address a;
        if (!ax25ParseAddress(t, a)) { err = "\"" + t + "\" is not a valid digipeater (CALL or CALL-SSID, SSID 0..15)"; return false; }
        path.push_back(a);
        return true;
    };
    for (char c : s) {
        if (c == ',') { if (!flush()) return false; }
        else cur += c;
    }
    if (!flush()) return false;
    if (path.size() > 8) { err = "at most 8 digipeaters"; return false; }
    return true;
}

bool ax25UiFrame(const std::string& from, const std::string& to, const std::string& path, const std::string& info, ax25::Frame& f, std::string& err) {
    f = ax25::Frame();
    if (!ax25ParseAddress(from, f.from)) { err = "the source callsign \"" + from + "\" is not valid"; return false; }
    if (!ax25ParseAddress(to, f.to)) { err = "the destination \"" + to + "\" is not valid"; return false; }
    if (!ax25ParsePath(path, f.path, err)) return false;
    if (info.size() > 256) { err = "the information field is longer than 256 bytes"; return false; }
    f.control = 0x03;
    f.pid = 0xF0;
    f.info = info;
    err.clear();
    return true;
}

// ---- APRS fields ----
namespace {
// "DDMM.hh" (lat, 2 degree digits) or "DDDMM.hh" (lon), the last `amb` digits blanked
std::string coord(double v, int degDigits, char pos, char neg, int amb) {
    const char h = v >= 0 ? pos : neg;
    v = std::fabs(v);
    int deg = (int)v;
    long hund = std::lround((v - deg) * 6000.0);   // minutes in hundredths
    if (hund >= 6000) { hund -= 6000; deg++; }
    char b[24];
    snprintf(b, sizeof b, degDigits == 2 ? "%02d%02ld.%02ld" : "%03d%02ld.%02ld", deg, hund / 100, hund % 100);
    std::string s = b;
    // blank from the right: hundredths, tenths, minutes units, minutes tens (the '.' stays)
    static const int order[4] = {1, 2, 4, 5};
    for (int k = 0; k < std::min(4, std::max(0, amb)); k++) s[s.size() - (size_t)order[k]] = ' ';
    return s + h;
}
std::string clean(const std::string& t, size_t maxLen, bool msg) {
    std::string o;
    for (char c : t) {
        if ((unsigned char)c < 32 || (unsigned char)c > 126) continue;
        if (msg && (c == '|' || c == '~' || c == '{')) continue;
        o += c;
    }
    if (o.size() > maxLen) o.resize(maxLen);
    return o;
}
std::string addressee(const std::string& to) {
    std::string a = up(trim(to));
    if (a.size() > 9) a.resize(9);
    a.resize(9, ' ');
    return a;
}
}  // namespace

std::string aprsPosition(const AprsPos& p) {
    std::string s;
    s += p.messaging ? '=' : '!';
    s += coord(std::max(-90.0, std::min(90.0, p.lat)), 2, 'N', 'S', p.ambiguity);
    s += p.symTable;
    s += coord(std::max(-180.0, std::min(180.0, p.lon)), 3, 'E', 'W', p.ambiguity);
    s += p.symCode;
    std::string c = clean(p.comment, 43, false);
    if (p.altM > -1e8) {
        char b[16];
        const long ft = std::lround(p.altM / 0.3048);
        snprintf(b, sizeof b, "/A=%06ld", std::max(-99999L, std::min(999999L, ft)));
        c = std::string(b) + c;
        if (c.size() > 43) c.resize(43);
    }
    return s + c;
}
std::string aprsStatus(const std::string& t) { return ">" + clean(t, 62, false); }
std::string aprsMessage(const std::string& to, const std::string& text, const std::string& no) {
    std::string s = ":" + addressee(to) + ":" + clean(text, 67, true);
    if (!no.empty()) s += "{" + clean(no, 5, true);
    return s;
}
std::string aprsAck(const std::string& to, const std::string& no) { return ":" + addressee(to) + ":ack" + clean(no, 5, true); }
std::string aprsRej(const std::string& to, const std::string& no) { return ":" + addressee(to) + ":rej" + clean(no, 5, true); }
std::string aprsBulletin(char id, const std::string& text) {
    id = (char)toupper((unsigned char)id);
    if (!isalnum((unsigned char)id)) id = '0';
    return ":" + addressee(std::string("BLN") + id) + ":" + clean(text, 67, true);
}

bool gridCentre(const std::string& gIn, double& lat, double& lon) {
    const std::string g = up(trim(gIn));
    if (!hamGridValid(g)) return false;
    lon = (g[0] - 'A') * 20.0 - 180 + (g[2] - '0') * 2.0;
    lat = (g[1] - 'A') * 10.0 - 90 + (g[3] - '0') * 1.0;
    if (g.size() >= 6) { lon += (g[4] - 'A') * (2.0 / 24) + 1.0 / 24; lat += (g[5] - 'A') * (1.0 / 24) + 0.5 / 24; }
    else { lon += 1.0; lat += 0.5; }
    return true;
}

// ---- audio ----
double packetBwHz(PacketBaud b) { return b == PacketBaud::B9600 ? 20000 : b == PacketBaud::B1200 ? 16000 : 500; }

std::vector<float> packetAudio(const std::vector<ax25::Frame>& frames, const PacketAudioCfg& c) {
    const double baud = (double)(int)c.baud;
    // the HDLC bits: txDelay flags, every frame (flags between them shared), tail flags
    const int lead = std::max(4, (int)std::lround(c.txDelayMs / 1000.0 * baud / 8));
    const int tail = std::max(2, (int)std::lround(c.txTailMs / 1000.0 * baud / 8));
    std::vector<uint8_t> bits;
    for (size_t i = 0; i < frames.size(); i++) {
        const auto fb = ax25::hdlcBits(ax25::buildFrame(frames[i]), i == 0 ? lead : 0, i + 1 == frames.size() ? tail : 1);
        bits.insert(bits.end(), fb.begin(), fb.end());
    }
    if (bits.empty()) return {};
    std::vector<uint8_t> lev = ax25::nrziEncode(bits);
    const double spb = c.rate / baud;
    std::vector<float> a;
    if (c.baud == PacketBaud::B9600) {
        uint32_t st = 0;
        lev = ax25::scramble(lev, st);
        // NRZ +-1 through a Gaussian filter, BT 0.5
        const size_t n = (size_t)std::ceil((double)lev.size() * spb) + (size_t)(4 * spb);
        std::vector<double> nrz(n, 0.0);
        for (size_t i = 0; i < n; i++) {
            const size_t k = std::min(lev.size() - 1, (size_t)((double)i / spb));
            nrz[i] = lev[k] ? 1.0 : -1.0;
        }
        const double sigma = std::sqrt(std::log(2.0)) / (2 * kPi * 0.5 * baud) * c.rate;
        const int L = (int)std::ceil(3 * sigma);
        std::vector<double> h((size_t)(2 * L + 1));
        double hs = 0;
        for (int i = -L; i <= L; i++) hs += h[(size_t)(i + L)] = std::exp(-0.5 * i * i / (sigma * sigma));
        a.assign(n, 0.f);
        for (size_t i = 0; i < n; i++) {
            double acc = 0;
            for (int k = -L; k <= L; k++) {
                const long j = (long)i + k;
                acc += h[(size_t)(k + L)] * nrz[(size_t)std::max(0L, std::min((long)n - 1, j))];
            }
            a[i] = (float)(acc / hs);
        }
        return a;
    }
    double mark, space, spaceGain = 1;
    if (c.baud == PacketBaud::B1200) { mark = 1200; space = 2200; spaceGain = std::pow(10.0, c.emphasisDb / 20.0); }
    else { mark = c.centreHz - 100; space = c.centreHz + 100; }
    double ph = 0, t = 0;
    for (uint8_t l : lev) {
        t += spb;
        const size_t end = (size_t)std::llround(t);
        const double w = 2 * kPi * (l ? mark : space) / c.rate;
        const double g = l ? 1.0 : spaceGain;
        while (a.size() < end) { a.push_back((float)(g * std::sin(ph))); ph += w; if (ph > 2 * kPi) ph -= 2 * kPi; }
    }
    float pk = 0;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    if (pk > 0) for (auto& v : a) v /= pk;
    return a;
}

bool aprsSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gateIn, const std::vector<ax25::Frame>& frames, const PacketAudioCfg& cfg,
              double level, double nowSec, std::string& err) {
    if (frames.empty()) { err = "nothing to send"; return false; }
    if (std::fabs(cfg.rate - 48000) > 1) { err = "the transmitter takes 48 kHz audio"; return false; }
    std::vector<float> a = packetAudio(frames, cfg);
    if (a.empty()) { err = "nothing to send"; return false; }
    const double dur = (double)a.size() / cfg.rate;
    HamTxCheck g = gateIn;
    const double bw = packetBwHz(cfg.baud);
    if (cfg.baud == PacketBaud::B300) g.audioHz = cfg.centreHz - bw / 2;   // USB: the tones above the dial
    else g.audioHz = -bw / 2;                                                // FM: the channel centred on the dial
    g.bwHz = bw;
    if (!tx.sendAudio(g, std::move(a), bw, level, err)) return false;
    duty.add(nowSec, dur);
    return true;
}

// ---- AFSK receiver ----
AfskRx::AfskRx(double rate, double markHz, double spaceHz, double baud, ax25::HdlcRx::Callback cb)
    : rate_(rate), mark_(markHz), space_(spaceHz), baud_(baud), hdlc_(std::move(cb), true) { design(); }

void AfskRx::design() {
    L_ = std::max(4, (int)std::lround(rate_ / baud_));
    zm_.assign((size_t)L_, {0.f, 0.f});
    zs_ = zm_;
    hpos_ = 0;
    accM_ = accS_ = {0, 0};
}

void AfskRx::reset() { design(); tune_ = 0; phM_ = phS_ = 0; ph_ = -0.5f; lev_ = prev_ = false; eAvgM_ = eAvgS_ = 0; hdlc_.reset(); }

void AfskRx::feed(const float* x, size_t n) {
    const double shift = std::fabs(space_ - mark_);
    const float step = (float)(baud_ / rate_);
    for (size_t i = 0; i < n; i++) {
        const double wm = 2 * kPi * (mark_ + tune_) / rate_, ws = 2 * kPi * (space_ + tune_) / rate_;
        const std::complex<float> m((float)(x[i] * std::cos(phM_)), (float)(-x[i] * std::sin(phM_)));
        const std::complex<float> s((float)(x[i] * std::cos(phS_)), (float)(-x[i] * std::sin(phS_)));
        phM_ += wm; if (phM_ > 2 * kPi) phM_ -= 2 * kPi;
        phS_ += ws; if (phS_ > 2 * kPi) phS_ -= 2 * kPi;
        accM_ += std::complex<double>(m) - std::complex<double>(zm_[hpos_]);
        accS_ += std::complex<double>(s) - std::complex<double>(zs_[hpos_]);
        zm_[hpos_] = m; zs_[hpos_] = s;
        if (++hpos_ == (size_t)L_) {   // once a bit: sum again (no float drift)
            hpos_ = 0;
            accM_ = accS_ = {0, 0};
            for (int k = 0; k < L_; k++) { accM_ += std::complex<double>(zm_[(size_t)k]); accS_ += std::complex<double>(zs_[(size_t)k]); }
        }
        const double em = std::norm(accM_), es = std::norm(accS_);
        lev_ = em > es;
        // tuning: the stronger tone's phase turns by 2 pi (error) / rate per sample (the boxcar sum keeps the input's frequency)
        const std::complex<double> cur = lev_ ? accM_ : accS_;
        const std::complex<float> last = lev_ ? lastM_ : lastS_;
        if (std::abs(cur) > 1e-9 && std::abs(last) > 1e-9) {
            const double err = std::arg(cur * std::conj(std::complex<double>(last))) * rate_ / (2 * kPi);
            if (steady_ > L_ && std::fabs(err) < shift / 2) tune_ += (hdlc_.inFrame() ? 5e-5 : 4e-4) * err;   // only while the window lies on one tone; slower inside a frame
            tune_ = std::max(-shift / 3, std::min(shift / 3, tune_));
        }
        lastM_ = std::complex<float>(accM_); lastS_ = std::complex<float>(accS_);
        steady_ = lev_ != prev_ ? 0 : steady_ + 1;
        if (lev_ != prev_) ph_ *= hdlc_.flagSeen() ? 0.85f : 0.7f;
        prev_ = lev_;
        ph_ += step;
        if (ph_ >= 0.5f) { ph_ -= 1.f; hdlc_.bit(lev_ ? 1 : 0); }
    }
}

// ---- the message session ----
std::string AprsMessenger::send(const std::string& toIn, const std::string& text, double now, std::string& err) {
    ax25::Address a;
    if (!ax25ParseAddress(toIn, a)) { err = "\"" + toIn + "\" is not a valid callsign"; return ""; }
    const std::string body = clean(text, 67, true);
    if (trim(body).empty()) { err = "empty message"; return ""; }
    if (body.size() < text.size()) err = "the message was shortened or had | ~ { removed";
    else err.clear();
    AprsMsgOut m;
    m.to = fmtAddr(a);
    m.text = body;
    m.no = std::to_string(next_);
    next_ = next_ >= 99999 ? 1 : next_ + 1;
    m.nextSec = now;
    out_.push_back(m);
    return m.no;
}

void AprsMessenger::cancel(const std::string& no) {
    for (auto& m : out_) if (m.no == no) m.gaveUp = true;
}

std::vector<std::string> AprsMessenger::due(double now) {
    std::vector<std::string> r;
    for (auto& m : out_) {
        if (m.acked || m.rejected || m.gaveUp || now < m.nextSec) continue;
        if (m.tries >= maxTries) { m.gaveUp = true; continue; }
        r.push_back(aprsMessage(m.to, m.text, m.no));
        m.nextSec = now + firstGapSec * std::pow(2.0, m.tries);
        m.tries++;
    }
    return r;
}

std::vector<std::string> AprsMessenger::onReceive(const std::string& fromIn, const std::string& info, double now) {
    std::vector<std::string> r;
    if (info.size() < 11 || info[0] != ':' || info[10] != ':') return r;
    const std::string to = up(trim(info.substr(1, 9)));
    if (to != up(trim(myCall))) return r;
    const std::string from = up(trim(fromIn));
    std::string body = info.substr(11);
    auto numberAfter = [](const std::string& s) {   // up to a '}' (the reply-ack form "{MM}AA") or the end
        std::string n = s.substr(0, s.find('}'));
        return trim(n);
    };
    if (body.size() > 3 && (body.compare(0, 3, "ack") == 0 || body.compare(0, 3, "rej") == 0)) {
        const bool ack = body[0] == 'a';
        const std::string no = numberAfter(body.substr(3));
        for (auto& m : out_)
            if (m.no == no && up(m.to) == from) { if (ack) m.acked = true; else m.rejected = true; }
        return r;
    }
    std::string no;
    const size_t b = body.find('{');
    if (b != std::string::npos) { no = numberAfter(body.substr(b + 1)); body = body.substr(0, b); }
    if (!no.empty()) r.push_back(aprsAck(from, no));   // every copy is acked: our earlier ack may have been lost
    const std::string key = from + "|" + (no.empty() ? body : no);
    if (std::find(seen_.begin(), seen_.end(), key) == seen_.end()) {
        seen_.push_back(key);
        if (seen_.size() > 5000) seen_.erase(seen_.begin());
        in_.push_back({from, body, no, now});
    }
    return r;
}

// ---- beacon ----
std::string AprsBeacon::due(double now) {
    if (!on_ || now < next_) return "";
    if (sent_ >= cfg.maxTx) { on_ = false; stopped_ = "stopped after " + std::to_string(cfg.maxTx) + " beacons without operator action"; return ""; }
    const double iv = std::max(60.0, cfg.intervalSec);
    next_ = now + iv;
    sent_++;
    const bool st = !cfg.status.empty() && cfg.statusEvery > 0 && (n_++ % cfg.statusEvery) == cfg.statusEvery - 1;
    return st ? aprsStatus(cfg.status) : aprsPosition(cfg.pos);
}

}  // namespace dect2::ham
