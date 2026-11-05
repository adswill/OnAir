// KiwiSDR (a network HF receiver, 0 to 30 MHz) as an IQ source: the Kiwi's "IQ" mode over its WebSocket (port 8073), 12 kHz of spectrum around the
// tuned frequency. The frames are interpolated to a rate the receivers accept (kiwisdr_proto.h: 21 x 12 kHz = 252 kHz, or 84 x for the general
// receiver's 250 kHz tune offset), and the Kiwi is tuned to the DIAL frequency: the engine's tune offset (a narrow-channel mode sits that far above
// the dial in a normal radio's samples) is put back by a mixer here, so the same receivers work on a radio with a 12 kHz window.
// Protocol: see the facts and their VERIFIED / ASSUMED marks in kiwisdr_proto.h. Public Kiwis are somebody's hobby: one connection per radio, a
// keepalive every second (what the official page does), SET ident_user=OnAir so the owner sees who listens, and a growing pause (5 s .. 2 min,
// a minute at least after "busy" or a time limit) before every new connection after one was lost. A first connection that fails is not repeated.
// Not tested against a real KiwiSDR: only against a fake one in tests/source/test_kiwisdr.cpp.
#include "../net/tcp_client.h"
#include "../net/ws_client.h"
#include "kiwisdr_proto.h"
#include "native_common.h"
#include "dect2/net/kiwisdr.h"
#include "dect2/net/netradio.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <cmath>
#include <ctime>
#include <map>
#include <mutex>

namespace dect2 {

namespace {
std::mutex gKiwiMu;
std::map<std::string, KiwiStatus> gStatus;
std::map<std::string, std::string> gPass;
std::atomic<int> gBackMin{5000}, gBackMax{120000}, gStallMs{8000};

void noteStatus(const std::string& a, const std::function<void(KiwiStatus&)>& f) { std::lock_guard<std::mutex> lk(gKiwiMu); f(gStatus[a]); }
std::string passFor(const std::string& a) { std::lock_guard<std::mutex> lk(gKiwiMu); auto it = gPass.find(a); return it == gPass.end() ? std::string() : it->second; }
}  // namespace

KiwiStatus kiwiStatus(const std::string& hp) { std::lock_guard<std::mutex> lk(gKiwiMu); auto it = gStatus.find(hp); return it == gStatus.end() ? KiwiStatus() : it->second; }
void kiwiSetPassword(const std::string& hp, const std::string& pw) { std::lock_guard<std::mutex> lk(gKiwiMu); if (pw.empty()) gPass.erase(hp); else gPass[hp] = pw; }

double kiwiGpsToUnix(uint32_t gpsSec, uint32_t gpsNsec, double nowUnix, int leap) {
    constexpr double kGpsEpoch = 315964800.0, kWeek = 604800.0;
    if (gpsSec >= (uint32_t)kWeek) return -1;
    const double gpsNow = nowUnix - kGpsEpoch + leap;
    double week = std::floor(gpsNow / kWeek);
    double t = week * kWeek + gpsSec;
    if (t - gpsNow > kWeek / 2) t -= kWeek;
    else if (gpsNow - t > kWeek / 2) t += kWeek;
    return t + gpsNsec * 1e-9 + kGpsEpoch - leap;
}

namespace native {

void kiwiSetBackoffForTest(int minMs, int maxMs) { gBackMin = minMs; gBackMax = maxMs; }
void kiwiSetStallForTest(int ms) { gStallMs = ms; }

namespace {

constexpr int kConnectMs = 4000, kAnswerMs = 2000, kKeepaliveMs = 1000;
constexpr size_t kMaxMessage = 1u << 16;     // an SND frame is about 2 KiB; a W/F line about 1 KiB
constexpr size_t kMaxFrameSamples = 8192;
constexpr double kMinFreq = 10e3, kMaxFreq = 30e6;
constexpr int kStableMs = 30000;

enum class Why { None, Net, Busy, BadPassword, Down, Multi, TimeLimit, Kicked, Redirect, Stalled, Closed, Other };

bool isHold(Why w) { return w == Why::Busy || w == Why::TimeLimit || w == Why::Multi || w == Why::Kicked || w == Why::Down; }

// What a MSG name=value means for us (the names are kiwiclient's; the time-limit and kick names are ASSUMED from the Kiwi's own page)
Why classify(const std::string& name, const std::string& value, std::string& text) {
    if (name == "too_busy") {
        text = "all of this KiwiSDR's " + (value.empty() || value == "0" ? std::string("receiver") : value) + " listener slots are in use: pick another station or try later";
        return Why::Busy;
    }
    if (name == "badp") {   // badp=0 is the "password accepted" answer every Kiwi sends after sample_rate; the others are refusals (KiwiSDR source, rx_server)
        if (value == "0") return Why::None;
        if (value == "1") text = "the KiwiSDR asks for a password (or the one given is wrong): set it in the station's password field";
        else if (value == "2" || value == "3") { text = "the KiwiSDR could not yet tell whether this address is on its local network: try again in a moment"; return Why::Busy; }
        else if (value == "4") text = "this KiwiSDR does not allow admin connections from this address";
        else if (value == "5") { text = "this KiwiSDR allows only one connection from an address (another one from this network is open)"; return Why::Multi; }
        else if (value == "6") { text = "the KiwiSDR is updating its database: try again in about a minute"; return Why::Busy; }
        else if (value == "7") { text = "another admin session is open on this KiwiSDR"; return Why::Busy; }
        else text = "the KiwiSDR refused the connection (badp=" + value + ")";
        return Why::BadPassword;
    }
    if (name == "down") { text = "the KiwiSDR is down (maintenance or restarting)"; return Why::Down; }
    if (name == "redirect") { text = "the KiwiSDR asks to use another address"; return Why::Redirect; }
    if (name == "inactivity_timeout" || name == "ip_limit_exceeded" || name == "tlimit" || name == "time_limit") {
        text = "the KiwiSDR's time limit for one listener ran out";
        return Why::TimeLimit;
    }
    if (name == "kick_by_admin" || name == "kicked") { text = "the owner of the KiwiSDR disconnected this session"; return Why::Kicked; }
    return Why::None;
}

bool splitHostPort(const std::string& a, std::string& host, int& port) {
    const size_t c = a.rfind(':');
    if (c == std::string::npos || c == 0) return false;
    host = a.substr(0, c);
    if (host.size() > 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    port = atoi(a.c_str() + c + 1);
    return port > 0 && port < 65536;
}

// the whole SND/MSG session set-up on an attached, handshaken socket; what the server said about itself goes to info
struct KiwiInfo { std::string version; double maxHz = kMaxFreq; double sampleRate = 0; bool gotRate = false, gotAuth = false; };

// connection, upgrade, auth, then the server's first messages up to sample_rate. why/err say what went wrong.
bool connectAndAuth(WsClient& ws, const std::string& host, int port, const std::string& pw, int timeoutMs, KiwiInfo& info, Why& why, std::string& err) {
    const std::string addr = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    why = Why::Net;
    const sock_t s = tcpConnect(host, port, timeoutMs, err);
    if (s == kBadSock) { err = "cannot connect to the KiwiSDR at " + addr + ": " + err; return false; }
    ws.attach(s);
    sockRecvTimeout(s, kAnswerMs);
    std::string e;
    const std::string path = "/kiwi/" + std::to_string((long long)std::time(nullptr)) + "/SND";
    if (!ws.handshake(host, port, path, e)) { err = addr + ": " + e + " (is it a KiwiSDR? the default port is 8073)"; return false; }
    if (!ws.sendText(kiwi::authCmd(pw))) { err = "the connection to " + addr + " dropped"; return false; }
    std::vector<unsigned char> m;
    std::vector<std::pair<std::string, std::string>> kv;
    const auto t0 = std::chrono::steady_clock::now();
    auto tAuth = std::chrono::steady_clock::time_point{};
    while (!(info.gotRate && info.gotAuth && (!info.version.empty() || (tAuth.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() - tAuth > std::chrono::milliseconds(1500))))) {   // a real Kiwi says sample_rate first and badp=0 (accepted) / badp=N / too_busy after it
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(std::max(timeoutMs, 3000) * 2)) { err = addr + (info.gotRate ? " did not answer the password step (no badp): the station may be overloaded or not a KiwiSDR" : " did not finish the KiwiSDR hand-shake (no sample_rate): too slow, or not a KiwiSDR"); why = Why::Other; return false; }
        const auto kind = ws.readMessage(m, kMaxMessage);
        if (kind == WsClient::Kind::Timeout) continue;
        if (kind == WsClient::Kind::Closed || kind == WsClient::Kind::Bad) { err = addr + " closed the connection or sent bad data during the hand-shake"; why = Why::Closed; return false; }
        if (kind != WsClient::Kind::Binary || !kiwi::parseMsg(m.data(), m.size(), kv)) continue;
        for (const auto& p : kv) {
            std::string text;
            const Why w = classify(p.first, p.second, text);
            if (w != Why::None) { why = w; err = text; return false; }
            if (p.first == "badp") { info.gotAuth = true; tAuth = std::chrono::steady_clock::now(); }
            else if (p.first == "sample_rate") { info.sampleRate = atof(p.second.c_str()); info.gotRate = true; }
            else if (p.first == "audio_rate") ws.sendText("SET AR OK in=" + std::to_string(atoi(p.second.c_str())) + " out=44100");
            else if (p.first == "version_maj") info.version = p.second + info.version;
            else if (p.first == "version_min") info.version += "." + p.second;
            else if (p.first == "bandwidth") { const double b = atof(p.second.c_str()); if (b > 1e6 && b < 1e9) info.maxHz = b; }
        }
    }
    if (info.sampleRate < 8000 || info.sampleRate > 16000) { err = addr + " reports an IQ rate of " + std::to_string((int)info.sampleRate) + " Hz, not the 12 kHz of a KiwiSDR"; why = Why::Other; return false; }
    why = Why::None;
    return true;
}

void closeWs(WsClient& ws) {
    if (ws.sock() != kBadSock) { ws.sendClose(); sockShutdown(ws.sock()); sockClose(ws.sock()); }
    ws.attach(kBadSock);
}

}  // namespace

bool kiwiParseMsgForTest(const unsigned char* p, size_t n, std::vector<std::pair<std::string, std::string>>& kv) { return kiwi::parseMsg(p, n, kv); }

bool kiwiProbe(const std::string& host, int port, int timeoutMs, std::string& err) {
    WsClient ws;
    KiwiInfo info;
    Why why;
    const std::string addr = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    const bool ok = connectAndAuth(ws, host, port, passFor(addr), timeoutMs, info, why, err);
    closeWs(ws);   // the slot is given back at once; nothing is streamed
    if (ok) noteStatus(addr, [&](KiwiStatus& s) { s.version = info.version; });
    return ok;
}

class KiwiSource : public NativeSource {
public:
    explicit KiwiSource(std::string addr) : addr_(std::move(addr)) {}
    ~KiwiSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        std::string host;
        int port = 0;
        if (!splitHostPort(addr_, host, port)) { err = "KiwiSDR: bad address " + addr_ + " (host:port)"; openFailure_ = OpenFailure::Final; return false; }
        const auto now = std::chrono::steady_clock::now();
        if (established_ && now < nextTry_) {   // after a loss: not before the pause is over (no hammering of somebody's receiver)
            err = lastReason_;
            openFailure_ = OpenFailure::Retry;
            return false;
        }
        const int off = (int)std::llround(offsetOf(s));
        const int L = std::abs(off) > 100000 ? 84 : 21;
        KiwiInfo info;
        Why why = Why::None;
        std::string e;
        if (!connectAndAuth(ws_, host, port, passFor(addr_), kConnectMs, info, why, e)) {
            closeWs(ws_);
            err = "KiwiSDR: " + e;
            lastReason_ = err;
            noteStatus(addr_, [&](KiwiStatus& st) { st.connected = false; st.message = err; });
            if (established_) schedule(why);
            else openFailure_ = OpenFailure::Final;   // the first try is the user's: it is told at once and not repeated
            return false;
        }
        if (ups_.factor() != L) ups_.configure(L, (double)off);
        else ups_.setShift((double)off);
        curShift_ = pendShift_ = (double)off;
        rate_ = ups_.outRate();
        sentDial_ = -1; sentCut_ = -1; sentAgc_ = -1; sentGain_ = -1;
        expectSeq_ = 0; haveSeq_ = false;
        maxHz_ = info.maxHz;
        // set-up commands that need no tuning
        bool ok = ws_.sendText("SET ident_user=OnAir") && ws_.sendText("SET compression=0") && ws_.sendText("SET squelch=0 max=0") && ws_.sendText("SET genattn=0") && ws_.sendText("SET gen=0 mix=-1");
        if (!ok) { closeWs(ws_); err = "KiwiSDR: the connection to " + addr_ + " dropped"; if (established_) schedule(Why::Closed); else openFailure_ = OpenFailure::Final; return false; }
        sockRecvTimeout(ws_.sock(), kKeepaliveMs);
        if (!configure(s, err, false)) { closeWs(ws_); if (established_) schedule(Why::Closed); else openFailure_ = OpenFailure::Final; return false; }
        ws_.sendText("SET keepalive");
        lastKeep_ = now;
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(now - lostAt_).count();
            fprintf(stderr, "KiwiSDR: reconnected after %.1f s\n", sec);
            fflush(stderr);
            lostAt_ = {};
        }
        fprintf(stderr, "KiwiSDR: connected to %s%s%s\n", addr_.c_str(), info.version.empty() ? "" : ", version ", info.version.c_str());
        fflush(stderr);
        noteStatus(addr_, [&](KiwiStatus& st) { st.connected = true; st.message.clear(); st.version = info.version; });
        lastReason_.clear();
        streamStart_ = now;
        established_ = true;
        return true;
    }

    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        if (ws_.sock() == kBadSock) return false;
        const double off = offsetOf(s);
        double dial = s.centerHz - off;
        std::string warn;
        if (dial < kMinFreq || dial > std::min(maxHz_, kMaxFreq)) {
            warn = freqSpecNote("KiwiSDR", dial, kMinFreq, std::min(maxHz_, kMaxFreq));
            dial = std::min(std::max(dial, kMinFreq), std::min(maxHz_, kMaxFreq));
        }
        pendShift_ = off;
        int cut = atoi(radioOption(s, "passband", "5000").c_str());
        if (cut < 500 || cut > 6000) cut = 5000;
        const bool agcOn = radioOption(s, "agc", "auto") != "manual";
        const int gain = (int)std::lround(std::min(std::max(s.gainDb, 0.0), 120.0));
        swapIq_ = radioFlag(s, "swapiq", false);
        bool ok = true;
        const long long dialHz = std::llround(dial);
        if (dialHz != sentDial_ || cut != sentCut_) { ok = ok && ws_.sendText(kiwi::tuneCmd((double)dialHz, cut)); sentDial_ = dialHz; sentCut_ = cut; }
        if ((int)agcOn != sentAgc_ || (!agcOn && gain != sentGain_)) { ok = ok && ws_.sendText(kiwi::agcCmd(agcOn, gain)); sentAgc_ = agcOn; sentGain_ = gain; }
        if (!ok) { err = "KiwiSDR: the connection to " + addr_ + " dropped"; return false; }
        requested_ = s.sampleRate;
        err = warn;   // no rate warning: a Kiwi always runs at its one rate
        return true;
    }

    void closeDevice() override {
        closeWs(ws_);
        noteStatus(addr_, [](KiwiStatus& st) { st.connected = false; });
    }
    bool restartForRate() const override { return false; }
    void interrupt() override { if (ws_.sock() != kBadSock) sockShutdown(ws_.sock()); }

    void streamLoop() override {
        std::vector<unsigned char> m;
        std::vector<std::pair<std::string, std::string>> kv;
        std::vector<float> fl;
        std::vector<cf32> in, out;
        Why end = Why::Closed;
        std::string endText;
        // the stall timer runs on IQ samples, not on any message: a Kiwi that goes on sending MSG lines, audio or waterfall frames but no IQ
        // used to keep the connection "alive" while the engine reported "radio stopped sending samples" for good (issue #257)
        auto last = std::chrono::steady_clock::now(), lastIq = last;
        uint64_t bad = 0, notIq = 0;
        const int stallMs = gStallMs.load();
        while (run_) {
            const auto now = std::chrono::steady_clock::now();
            if (now - lastKeep_ >= std::chrono::milliseconds(kKeepaliveMs)) {
                if (!ws_.sendText("SET keepalive")) break;
                lastKeep_ = now;
            }
            if (now - lastIq > std::chrono::milliseconds(stallMs)) {
                end = Why::Stalled;
                const std::string secs = std::to_string(std::max(1, stallMs / 1000)) + " s";
                if (now - last > std::chrono::milliseconds(stallMs)) endText = "no data from the KiwiSDR for " + secs;
                else if (notIq) endText = "the KiwiSDR sent audio frames but no IQ for " + secs + " (it did not take SET mod=iq)";
                else endText = "the KiwiSDR sent messages but no IQ samples for " + secs;
                break;
            }
            const auto kind = ws_.readMessage(m, kMaxMessage);
            if (kind == WsClient::Kind::Timeout) continue;
            if (kind == WsClient::Kind::Closed) { end = Why::Closed; endText = "the KiwiSDR closed the connection (many public Kiwis end a session after about 30 minutes)"; break; }
            if (kind == WsClient::Kind::Bad) { end = Why::Other; endText = "bad data from the KiwiSDR (a frame over the size limit or a malformed one)"; break; }
            last = now;
            if (kind != WsClient::Kind::Binary) continue;
            if (m.size() >= 3 && memcmp(m.data(), "MSG", 3) == 0) {
                if (!kiwi::parseMsg(m.data(), m.size(), kv)) continue;
                bool stop = false;
                for (const auto& p : kv) {
                    std::string text;
                    const Why w = classify(p.first, p.second, text);
                    if (w != Why::None) { end = w; endText = text; stop = true; break; }
                    if (p.first == "audio_rate") ws_.sendText("SET AR OK in=" + std::to_string(atoi(p.second.c_str())) + " out=44100");
                }
                if (stop) break;
                continue;
            }
            kiwi::Snd snd;
            const kiwi::SndResult r = kiwi::parseSnd(m.data(), m.size(), snd);
            if (r == kiwi::SndResult::NotSnd) continue;   // W/F, EXT: not asked for here
            if (r == kiwi::SndResult::NotIq || r == kiwi::SndResult::Compressed) notIq++;
            if ((r != kiwi::SndResult::Ok && r != kiwi::SndResult::OddSize) || snd.samples == 0 || snd.samples > kMaxFrameSamples) { bad++; noteStatus(addr_, [](KiwiStatus& st) { st.badFrames++; }); continue; }
            if (r == kiwi::SndResult::OddSize) bad++;
            // sequence: a gap is filled with silence (the time base of FT8 and WSPR slots goes on), a repeat of the last frame is dropped.
            // TCP keeps the order, so any other step backwards is the server counting from a new start (an audio restart on its side):
            // follow it, as kiwiclient does. Dropping those frames instead kept every later frame out, and the stream went silent (#257).
            uint32_t gap = 0;
            if (haveSeq_) {
                const uint32_t d = snd.seq - expectSeq_;
                if (d == 0xFFFFFFFFu) { bad++; continue; }   // the same frame again
                gap = d >= 0x80000000u ? 0 : d;
            }
            haveSeq_ = true;
            expectSeq_ = snd.seq + 1;
            if (gap > 0) {
                const uint64_t miss = (uint64_t)gap * snd.samples;
                if (ring_) ring_->noteLost(miss * (uint64_t)ups_.factor());
                const size_t fill = (size_t)std::min<uint64_t>(miss, 24000);   // at most 2 s of silence
                in.assign(fill, cf32(0, 0));
                push2(in, out);
            }
            fl.resize(snd.samples * 2);
            kiwi::decodeIq(m.data(), snd, fl.data(), swapIq_);
            in.resize(snd.samples);
            for (size_t k = 0; k < snd.samples; k++) in[k] = cf32(fl[2 * k], fl[2 * k + 1]);
            push2(in, out);
            lastIq = now;
            notIq = 0;   // the reason of a stall is what came after the last IQ
            const bool gpsOk = snd.gpsStatus != 254 && snd.gpsStatus != 255;
            noteStatus(addr_, [&](KiwiStatus& st) {
                st.frames++; st.gapFrames += gap; st.rssiDbm = snd.rssiDbm; st.adcOverload = snd.flags & kiwi::kOverflow;
                st.gpsValid = gpsOk; if (gpsOk) { st.gpsSec = snd.gpsSec; st.gpsNsec = snd.gpsNsec; }
            });
        }
        if (bad) { fprintf(stderr, "KiwiSDR: %llu malformed frames ignored\n", (unsigned long long)bad); fflush(stderr); }
        if (run_) {
            lastReason_ = endText.empty() ? "KiwiSDR: the connection was lost" : "KiwiSDR: " + endText;
            const bool stable = std::chrono::steady_clock::now() - streamStart_ > std::chrono::milliseconds(kStableMs);
            if (stable) backoff_ = 0;
            fprintf(stderr, "%s\n", lastReason_.c_str());
            fflush(stderr);
            noteStatus(addr_, [&](KiwiStatus& st) { st.connected = false; st.message = lastReason_; });
            lostAt_ = std::chrono::steady_clock::now();
            schedule(end);
        }
    }

private:
    static double offsetOf(const TuneSettings& s) {
        auto it = s.radio.find("tune_offset_hz");
        return it == s.radio.end() ? 0.0 : atof(it->second.c_str());
    }
    void push2(const std::vector<cf32>& in, std::vector<cf32>& out) {
        if (pendShift_ != curShift_) { ups_.setShift(pendShift_); curShift_ = pendShift_; }
        out.clear();
        ups_.process(in.data(), in.size(), out);
        push(out.data(), out.size());
    }
    // the pause before the next connection: doubling from the minimum to the maximum, and a minute at least after a busy server or a time limit
    void schedule(Why w) {
        const int mn = gBackMin.load(), mx = gBackMax.load();
        backoff_ = backoff_ <= 0 ? mn : std::min(backoff_ * 2, mx);
        long long ms = backoff_;
        if (isHold(w)) ms = std::max<long long>(ms, std::min<long long>(12LL * mn, mx));
        nextTry_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    }

    std::string addr_;
    WsClient ws_;
    kiwi::Upsampler ups_;
    double maxHz_ = kMaxFreq;
    std::atomic<double> pendShift_{0};
    double curShift_ = 0;
    bool swapIq_ = false;
    long long sentDial_ = -1;
    int sentCut_ = -1, sentAgc_ = -1, sentGain_ = -1;
    uint32_t expectSeq_ = 0;
    bool haveSeq_ = false;
    bool established_ = false;
    int backoff_ = 0;
    std::string lastReason_;
    std::chrono::steady_clock::time_point nextTry_{}, lastKeep_{}, streamStart_{}, lostAt_{};
};

static void setCaps(DeviceInfo& d) {
    d.minRateHz = 12000.0 * 21;   // the interpolated rates (kiwisdr_proto.h); the source picks one by the mode's tune offset
    d.maxRateHz = 12000.0 * 84;
    d.minFreqHz = kMinFreq;
    d.maxFreqHz = kMaxFreq;
    d.gainMinDb = 0;
    d.gainMaxDb = 120;
    d.settings.clear();
    d.settings.push_back(choiceSetting("passband", "Passband", "The half-width of the Kiwi's IQ filter around the tuned frequency. The Kiwi sends 12 kHz of spectrum; the widest setting uses nearly all of it.",
                                       {"3000", "5000", "5900"}, {"+-3.0 kHz", "+-5.0 kHz", "+-5.9 kHz"}, "5000"));
    d.settings.push_back(choiceSetting("agc", "Gain control", "Automatic gain (the Kiwi's AGC), or the manual gain set by the gain slider (0 to 120 dB in the Kiwi's own scale).",
                                       {"auto", "manual"}, {"AGC", "Manual (gain slider)"}, "auto"));
    d.settings.push_back(boolSetting("swapiq", "Swap I and Q", "Mirrors the spectrum. Turn on if signals that should be on the upper side of the frequency appear on the lower side."));
}

DeviceInfo kiwiSdrDevice(const std::string& host, int port) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "kiwisdr";
    d.nativeArgs = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    d.serial = d.nativeArgs;
    d.name = "KiwiSDR " + d.nativeArgs + " (network, HF 12 kHz)";
    setCaps(d);
    return d;
}

void kiwiSdrCaps(DeviceInfo& d) { setCaps(d); }
std::unique_ptr<IqSource> makeKiwiSdr(const DeviceInfo& d) { return std::make_unique<KiwiSource>(d.nativeArgs); }

}  // namespace native
}  // namespace dect2
