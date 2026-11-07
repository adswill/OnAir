// SDR++ server client: a radio served by SDR++'s headless mode (`sdrpp --server`, TCP port 5259). Written from the SDR++ sources
// (core/src/server_protocol.h, core/src/server.cpp, core/src/gui/smgui.cpp) and tried against a real server with an Airspy R2.
//
// The wire: little-endian packets of a 32-bit type and a 32-bit size that counts the 8 header bytes. The client sends COMMAND packets (a
// 32-bit command and its argument): GET_UI, UI_ACTION, START, STOP, SET_FREQUENCY (f64 Hz), SET_SAMPLE_TYPE (u8: 0 int8, 1 int16, 2 float),
// SET_COMPRESSION (u8). Only GET_UI, SET_FREQUENCY and a UI_ACTION that asks for it are answered (COMMAND_ACK with the same command). The
// server sends its sample rate unprompted (COMMAND SET_SAMPLERATE, f64), at connect and whenever it changes, and turns a second client away
// with COMMAND DISCONNECT: it serves one client at a time. Samples come as BASEBAND packets: u16 compression type, u16 sample type, f32
// scale (the packet's peak), then interleaved I/Q; int16 and int8 samples are value * scale / 32768 (/ 128).
//
// There is no command for the sample rate or the gain: they live in the source module's own menu, which the server sends as a serialised
// draw list (SmGui) and which a client drives with UI_ACTION (a widget's id and its new value). OnAir reads that list generically: the
// combo whose id says "sr" and whose entries read as rates is the sample rate, the gain slider (or the LNA/mixer/VGA stage sliders) is the
// gain, and every other control of the server's radio (gain mode, bias tee, AGC, antenna, the server's source, ...) is offered as a radio
// setting under its own name. So any source module SDR++ has works, not only the ones OnAir knows. A change made this way is a change to
// the server's own settings: SDR++ keeps it after OnAir disconnects.
#include "../net/tcp_client.h"
#include "native_common.h"
#include "dect2/net/netradio.h"
#include "dect2/net/net_proto.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <functional>
#ifndef _WIN32
#include <sys/select.h>
#endif

namespace dect2 {
namespace native {

namespace {

enum : uint32_t { kPktCommand = 0, kPktAck = 1, kPktBaseband = 2, kPktBasebandZstd = 3, kPktVfo = 4, kPktFft = 5, kPktError = 6 };
enum : uint32_t {
    kCmdGetUi = 0, kCmdUiAction = 1, kCmdStart = 2, kCmdStop = 3, kCmdSetFrequency = 4, kCmdGetSampleRate = 5, kCmdSetSampleType = 6,
    kCmdSetCompression = 7, kCmdSetSampleRate = 0x80, kCmdDisconnect = 0x81,
};
enum : uint8_t { kElStep = 0, kElBool = 1, kElInt = 2, kElFloat = 3, kElString = 4 };
enum : uint8_t {
    kStFillWidth = 0x00, kStSameLine, kStBeginDisabled, kStEndDisabled,
    kStCombo = 0x80, kStButton, kStColumns, kStNextColumn, kStRadio, kStBeginGroup, kStEndGroup, kStLeftLabel, kStSliderInt,
    kStSliderFloatSteps, kStInputInt, kStCheckbox, kStSliderFloat, kStInputText, kStText, kStTextColored,
};
constexpr int kConnectMs = 3000, kStallMs = 3000, kAckMs = 4000, kMaxAckMs = 30000;
constexpr int kPipelinePackets = 2;   // SDR++'s dsp::stream between the source and the server: a write buffer and a read buffer
constexpr const char* kServerSourceId = "##sdrpp_server_src_sel";

uint32_t le32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const unsigned char* p) { return (uint16_t)(p[0] | p[1] << 8); }
float lef32(const unsigned char* p) { float f; uint32_t u = le32(p); memcpy(&f, &u, 4); return f; }
double lef64(const unsigned char* p) { uint64_t u = 0; for (int k = 7; k >= 0; k--) u = u << 8 | p[k]; double d; memcpy(&d, &u, 8); return d; }

std::string lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
std::string hiddenPart(const std::string& id) { const size_t p = id.find("##"); return p == std::string::npos ? std::string() : id.substr(p + 2); }
std::string visiblePart(const std::string& id) { return id.substr(0, id.find("##")); }
std::string labelFromId(const std::string& id) { return netp::sdrppLabelFromId(id); }

} // namespace

// ---- the server's menu --------------------------------------------------------------------------------------------------------------------

// the menu from the draw list: net_proto.h reads it (C++ or the Rust crate onair_net)
bool sdrppParseUi(const unsigned char* p, size_t n, std::vector<SdrppWidget>& out) {
    out.clear();
    std::vector<netp::UiWidget> u;
    if (!netp::sdrppParseUi(p, n, u)) return false;
    for (auto& x : u) {
        SdrppWidget w;
        w.kind = (SdrppWidget::Kind)x.kind;
        w.id = std::move(x.id); w.label = std::move(x.label); w.items = std::move(x.items);
        w.i = x.i; w.b = x.b; w.value = x.value; w.minV = x.minV; w.maxV = x.maxV; w.step = x.step; w.disabled = x.disabled;
        out.push_back(std::move(w));
    }
    return true;
}

double sdrppParseRate(const std::string& text) { return netp::sdrppRateText(text); }

int sdrppRateCombo(const std::vector<SdrppWidget>& ui) {
    int fallback = -1;
    for (size_t k = 0; k < ui.size(); k++) {
        const auto& w = ui[k];
        if (w.kind != SdrppWidget::Combo || w.id == kServerSourceId || w.items.empty()) continue;
        const bool allRates = std::all_of(w.items.begin(), w.items.end(), [](const std::string& s) { return netp::sdrppRateText(s) > 0; });
        if (!allRates) continue;
        const unsigned words = netp::sdrppIdFlags(w.id);
        const std::string h = lower(hiddenPart(w.id));
        if ((words & netp::kWordSr) || h.find("samplerate") != std::string::npos || h.find("sample_rate") != std::string::npos || h.find("_sr") != std::string::npos) return (int)k;
        if (fallback < 0 && !(words & netp::kWordBw) && h.find("bandwidth") == std::string::npos && h.find("_bw") == std::string::npos) fallback = (int)k;
    }
    return fallback;
}

SdrppGainPlan sdrppGainPlan(const std::vector<SdrppWidget>& ui) {
    SdrppGainPlan g;
    // one gain slider: the linear or sensitive gain of an Airspy / HydraSDR, else any slider called gain that is not a stage of it
    int best = -1, bestScore = 0;
    std::vector<int> stages;
    for (size_t k = 0; k < ui.size(); k++) {
        const auto& w = ui[k];
        if (w.kind != SdrppWidget::SliderInt && w.kind != SdrppWidget::SliderFloat) continue;
        if (w.minV == w.maxV) continue;
        const unsigned words = netp::sdrppIdFlags(w.id);
        const std::string h = lower(hiddenPart(w.id));
        const bool stage = (words & (netp::kWordLna | netp::kWordVga | netp::kWordMix | netp::kWordMixer | netp::kWordIf)) != 0;
        const bool gain = h.find("gain") != std::string::npos || lower(w.label).find("gain") != std::string::npos;
        if (stage) { stages.push_back((int)k); continue; }
        if (!gain) continue;
        const int score = (words & netp::kWordLin) ? 4 : (words & netp::kWordSens) ? 3 : 2;
        if (score > bestScore) { best = (int)k; bestScore = score; }
    }
    if (best >= 0) g.sliders.push_back(best);
    else g.sliders = stages;
    for (int k : g.sliders) {
        g.span += std::fabs(ui[(size_t)k].maxV - ui[(size_t)k].minV);
        if (!ui[(size_t)k].disabled) g.drivable = true;
    }
    if (g.sliders.size() == 1) {   // one slider: the gain is its own scale (dB, an index into a gain table, ...)
        const auto& w = ui[(size_t)g.sliders[0]];
        g.lo = std::min(w.minV, w.maxV);
        g.hi = std::max(w.minV, w.maxV);
    } else { g.lo = 0; g.hi = g.span; }   // stages: the sum of their ranges, each moved by the same fraction
    return g;
}

double sdrppSliderTarget(const SdrppWidget& w, const SdrppGainPlan& g, double gain) {
    const double frac = g.hi > g.lo ? std::min(1.0, std::max(0.0, (gain - g.lo) / (g.hi - g.lo))) : 0;
    double v = w.minV + frac * (w.maxV - w.minV);
    if (w.kind == SdrppWidget::SliderInt) v = std::round(v);
    else if (w.step > 0) v = w.minV + std::round((v - w.minV) / w.step) * w.step;
    return v;
}

namespace {

// ---- what was seen of each server (per "host:port"), for the radio list ------------------------------------------------------------------

struct ServerState {
    bool known = false;
    double rate = 0;                       // what the server last said it runs at
    std::vector<SdrppWidget> ui;           // its menu as last seen
    std::set<std::string> lockedRunning;   // controls the server greys out while it streams (sample rate, device, ...)
};
std::mutex gSeenMu;
std::map<std::string, ServerState> gSeen;
ServerState seen(const std::string& addr) { std::lock_guard<std::mutex> lk(gSeenMu); auto it = gSeen.find(addr); return it == gSeen.end() ? ServerState{} : it->second; }
void noteSeen(const std::string& addr, const ServerState& s) { std::lock_guard<std::mutex> lk(gSeenMu); gSeen[addr] = s; }

std::string sourceOf(const std::vector<SdrppWidget>& ui) {
    for (const auto& w : ui)
        if (w.kind == SdrppWidget::Combo && w.id == kServerSourceId && w.i >= 0 && w.i < (int)w.items.size()) return w.items[(size_t)w.i];
    return "";
}
std::vector<double> ratesOf(const std::vector<SdrppWidget>& ui) {
    std::vector<double> r;
    const int c = sdrppRateCombo(ui);
    if (c >= 0) for (const auto& s : ui[(size_t)c].items) r.push_back(netp::sdrppRateText(s));
    return r;
}
// the key of a server control in TuneSettings::radio: "srv:" and its id; radio buttons share one key per group ("srv:##_airspy_gm_...")
std::string settingKey(const SdrppWidget& w) { return "srv:" + (w.kind == SdrppWidget::Radio ? "##" + hiddenPart(w.id) : w.id); }
std::string numText(double v) { char b[48]; snprintf(b, sizeof b, "%.6g", v); return b; }

// The radio settings of a server as last seen: OnAir's own (wire format, gain left to the server, frequency correction), then every
// control of the server's menu that OnAir does not drive itself, with the server's current value as the default (so an untouched setting
// is never sent).
std::vector<RadioSetting> settingsOf(const ServerState& st) {
    std::vector<RadioSetting> v;
    v.push_back(choiceSetting("format", "Wire format",
                              "The sample format the server sends. 16-bit (the default) keeps weak signals next to strong ones apart; 8-bit needs\n"
                              "half the network (2 bytes a sample) and lets a slow link carry a higher rate; float is exact but takes 8 bytes a sample.",
                              {"8", "16", "32"}, {"8-bit", "16-bit", "32-bit float"}, "16"));
    v.push_back(boolSetting("keepgain", "Leave the gain to the server",
                            "OnAir does not move the server's gain slider: the gain stays as it is set in SDR++ (or by the server's AGC)."));
    v.push_back(ppmSetting());
    const int rateCombo = sdrppRateCombo(st.ui);
    const SdrppGainPlan gp = sdrppGainPlan(st.ui);
    const std::string src = sourceOf(st.ui);
    const std::string help = "A control of the SDR++ server" + (src.empty() ? std::string() : " (" + src + ")") +
                             ", changed on the server itself: SDR++ keeps it after OnAir disconnects. Untouched, OnAir leaves it as the server has it.";
    std::set<std::string> done;
    for (size_t k = 0; k < st.ui.size(); k++) {
        const auto& w = st.ui[k];
        if ((int)k == rateCombo || std::find(gp.sliders.begin(), gp.sliders.end(), (int)k) != gp.sliders.end()) continue;
        const std::string key = settingKey(w);
        if (w.kind == SdrppWidget::Text || !done.insert(key).second) continue;
        RadioSetting r;
        r.key = key;
        r.label = "SDR++: " + w.label;
        r.help = help;
        switch (w.kind) {
        case SdrppWidget::Combo: {
            if (w.items.empty()) continue;
            r.type = RadioSetting::Choice;
            for (size_t i = 0; i < w.items.size(); i++) { r.values.push_back(std::to_string(i)); r.names.push_back(w.items[i]); }
            r.def = std::to_string(std::max(0, std::min(w.i, (int)w.items.size() - 1)));
            break;
        }
        case SdrppWidget::Checkbox:
            r.type = RadioSetting::Bool; r.def = w.b ? "1" : "0";
            if (r.label == "SDR++: " + labelFromId(w.id) && !visiblePart(w.id).empty()) r.label = "SDR++: " + visiblePart(w.id);
            break;
        case SdrppWidget::Radio: {
            r.type = RadioSetting::Choice;
            r.label = "SDR++: " + labelFromId(w.id);
            for (const auto& o : st.ui) {
                if (o.kind != SdrppWidget::Radio || settingKey(o) != key) continue;
                r.values.push_back(visiblePart(o.id));
                r.names.push_back(visiblePart(o.id));
                if (o.b) r.def = visiblePart(o.id);
            }
            if (r.def.empty() && !r.values.empty()) r.def = r.values[0];
            break;
        }
        case SdrppWidget::SliderInt: case SdrppWidget::SliderFloat: case SdrppWidget::InputInt:
            r.type = RadioSetting::Number;
            r.minV = std::min(w.minV, w.maxV); r.maxV = std::max(w.minV, w.maxV);
            r.step = w.step > 0 ? w.step : (r.maxV - r.minV) / 100;
            r.def = numText(w.value);
            break;
        default: continue;
        }
        v.push_back(std::move(r));
    }
    return v;
}

void setCaps(DeviceInfo& d, const ServerState& st) {
    d.rateRanges.clear();
    std::vector<double> r = ratesOf(st.ui);
    r.erase(std::remove_if(r.begin(), r.end(), [](double x) { return !(x > 0); }), r.end());
    if (r.empty() && st.rate > 0) r.push_back(st.rate);
    std::sort(r.begin(), r.end());
    for (double x : r) d.rateRanges.push_back({x, x});
    d.minRateHz = r.empty() ? 0 : r.front();
    d.maxRateHz = r.empty() ? 0 : r.back();
    const SdrppGainPlan g = sdrppGainPlan(st.ui);
    if (g.hi > g.lo) { d.gainMinDb = g.lo; d.gainMaxDb = g.hi; }
    else { d.gainMinDb = 0; d.gainMaxDb = 21; }   // not seen yet: an Airspy's 21 steps, the most common radio behind a server
    d.settings = settingsOf(st);
}

bool splitHostPort(const std::string& a, std::string& host, int& port) { return netp::splitHostPort(a, host, port); }

// ---- one connection to a server -----------------------------------------------------------------------------------------------------------
// Every packet is read by readOne(), whoever calls it: the stream thread while the radio runs, the caller of waitAck() otherwise (it reads
// itself when no one else holds readMu_, and waits for the reader's notice when someone does). So a command sent while streaming gets its
// answer through the stream thread.
class Link {
public:
    explicit Link(std::string addr) : addr_(std::move(addr)) {}
    ~Link() { close(); }

    bool connect(int timeoutMs, std::string& err) {
        std::string host, e;
        int port = 0;
        if (!splitHostPort(addr_, host, port)) { err = "bad address " + addr_ + " (host:port)"; return false; }
        const sock_t s = tcpConnect(host, port, timeoutMs, e);
        if (s == kBadSock) { err = e; return false; }
        sockRecvTimeout(s, timeoutMs);
        std::lock_guard<std::mutex> lk(stMu_);
        sock_ = s;
        dead_ = false; busy_ = false; badPacket_ = false;
        for (auto& a : acks_) a = 0;
        flushing_ = false; tail_ = 0; goodPackets_ = 0;
        lastRx_ = std::chrono::steady_clock::now();
        return true;
    }
    void setRecvTimeout(int ms) { if (sock_ != kBadSock) sockRecvTimeout(sock_, ms); }
    void close() {
        std::lock_guard<std::mutex> lk(sendMu_);
        if (sock_ != kBadSock) { sockShutdown(sock_); sockClose(sock_); }
        sock_ = kBadSock;
    }
    void shutdown() { if (sock_ != kBadSock) sockShutdown(sock_); }
    // only our sending side: the peer reads what is queued (a STOP just sent) and then sees the end of the stream
    void shutdownSend() {
        if (sock_ == kBadSock) return;
#ifdef _WIN32
        ::shutdown(sock_, SD_SEND);
#else
        ::shutdown(sock_, SHUT_WR);
#endif
    }

    bool command(uint32_t cmd, const unsigned char* arg = nullptr, size_t n = 0) {
        const std::string b = netp::sdrppCommand(cmd, arg, n);
        std::lock_guard<std::mutex> lk(sendMu_);
        return sock_ != kBadSock && sockSendAll(sock_, b.data(), b.size());
    }
    bool commandU8(uint32_t cmd, uint8_t v) { return command(cmd, &v, 1); }
    // one control of the server's menu set to a value (an element as SmGui stores it); sync: the server answers with its redrawn menu
    bool uiAction(const std::string& id, uint8_t type, int32_t i, float f, bool b, bool sync) {
        const std::string a = netp::sdrppUiAction(id, type, i, f, b, sync);
        return command(kCmdUiAction, (const unsigned char*)a.data(), a.size());
    }
    uint64_t acks(uint32_t cmd) { std::lock_guard<std::mutex> lk(stMu_); return cmd < 8 ? acks_[cmd] : 0; }
    // waits for one more answer to cmd than `before` (reading the socket itself when the stream thread does not). The answer comes after every
    // sample the server queued before it, so on a slow link it can take a while: the wait gives up after ms without any packet arriving, or
    // after kMaxAckMs in all.
    bool waitAck(uint32_t cmd, uint64_t before, int ms) {
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            std::chrono::steady_clock::time_point last;
            {
                std::lock_guard<std::mutex> lk(stMu_);
                if (acks_[cmd] != before) return true;
                if (dead_) return false;
                last = std::max(lastRx_, t0);
            }
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(ms) || now - t0 > std::chrono::milliseconds(kMaxAckMs)) return false;
            if (readMu_.try_lock()) {
                const int r = readOne(50);
                readMu_.unlock();
                if (r < 0) return false;
            } else {
                std::unique_lock<std::mutex> lk(stMu_);
                cv_.wait_for(lk, std::chrono::milliseconds(20), [&] { return acks_[cmd] != before || dead_; });
            }
        }
    }
    // The sample rate the server sends when it takes a client, at the end of its connect handler. Nothing may be asked before it: SDR++'s
    // server builds that packet and its answers in one buffer without a lock, and its connect handler first stops a running source (slow,
    // for a server started with --autostart), so a GET_UI answered meanwhile goes out with the other packet's header and the stream is out
    // of step from there on. false: none came within ms (an older server: go on anyway) or the server turned this client away.
    bool waitGreeting(int ms) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (rate() <= 0 && !isDead() && std::chrono::steady_clock::now() < end) {
            std::lock_guard<std::mutex> lk(readMu_);
            if (readOne(100) < 0) return false;
        }
        return rate() > 0;
    }
    // GET_UI and its answer (the menu); the sample rate the server sends at connect usually comes first
    bool fetchUi(int ms) {
        const uint64_t n = acks(kCmdGetUi);
        if (!command(kCmdGetUi)) return false;
        if (!waitAck(kCmdGetUi, n, ms)) return false;
        if (rate() <= 0) {   // not yet: give it a moment (it is sent right after the connection is accepted)
            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            while (rate() <= 0 && std::chrono::steady_clock::now() < end) {
                if (!readMu_.try_lock()) break;
                const int r = readOne(100);
                readMu_.unlock();
                if (r < 0) return false;
            }
        }
        return true;
    }

    // waits up to waitMs for a packet, then reads all of it and handles it: 1 one was handled, 0 none came, -1 the connection failed or the
    // server sent nonsense. The wait is a select(), so a quiet server never leaves half a packet read.
    int readOne(int waitMs) {
        if (sock_ == kBadSock) return fail(-1);
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock_, &rd);
        timeval tv{waitMs / 1000, (waitMs % 1000) * 1000};
        const int sel = select((int)sock_ + 1, &rd, nullptr, nullptr, &tv);
        if (sel == 0) return 0;
        if (sel < 0) return errno == EINTR ? 0 : fail(-1);
        unsigned char h[8];
        if (!sockRecvAll(sock_, h, sizeof h)) return fail(-1);
        uint32_t type = 0, bodySize = 0;
        if (!netp::sdrppPacketHeader(h, type, bodySize)) { { std::lock_guard<std::mutex> lk(stMu_); badPacket_ = true; } return fail(-1); }
        body_.resize(bodySize);
        if (!body_.empty() && !sockRecvAll(sock_, body_.data(), body_.size())) return fail(-1);
        { std::lock_guard<std::mutex> lk(stMu_); lastRx_ = std::chrono::steady_clock::now(); goodPackets_++; }
        handle(type, body_.data(), body_.size());
        return isDead() ? -1 : 1;
    }

    double rate() { std::lock_guard<std::mutex> lk(stMu_); return rate_; }
    std::vector<SdrppWidget> ui() { std::lock_guard<std::mutex> lk(stMu_); return ui_; }
    bool isDead() { std::lock_guard<std::mutex> lk(stMu_); return dead_; }
    bool dropping() { std::lock_guard<std::mutex> lk(stMu_); return discard_ || flushing_ || tail_ > 0; }
    bool busy() { std::lock_guard<std::mutex> lk(stMu_); return busy_; }
    bool badPacket() { std::lock_guard<std::mutex> lk(stMu_); return badPacket_; }
    uint64_t goodPackets() { std::lock_guard<std::mutex> lk(stMu_); return goodPackets_; }
    bool sawZstd() { std::lock_guard<std::mutex> lk(stMu_); return zstd_; }
    void expectStream(bool on) { std::lock_guard<std::mutex> lk(stMu_); expectStream_ = on; }
    bool expectingStream() { std::lock_guard<std::mutex> lk(stMu_); return expectStream_; }
    void discard(bool on) { std::lock_guard<std::mutex> lk(stMu_); discard_ = on; }
    // drops the samples until the server answers the next cmd (call it before sending that command): what the server queued before it
    // was taken with the old frequency or gain, and on a slow link that is seconds of it
    void flushUntilAck(uint32_t cmd) {
        std::lock_guard<std::mutex> lk(stMu_);
        flushing_ = true; flushCmd_ = cmd; flushAt_ = acks_[cmd] + 1;
    }
    std::string lastError() { std::lock_guard<std::mutex> lk(stMu_); return lastError_; }

    std::function<void(const cf32*, size_t)> onSamples;
    std::function<void(double)> onRate;

private:
    int fail(int r) {
        { std::lock_guard<std::mutex> lk(stMu_); dead_ = true; }
        cv_.notify_all();
        return r;
    }
    void handle(uint32_t type, const unsigned char* b, size_t n) {
        if (type == kPktBaseband) {
            if (n < 8) return;
            {
                std::lock_guard<std::mutex> lk(stMu_);
                if (discard_ || flushing_) return;
                if (tail_ > 0) { tail_--; return; }
            }
            const uint16_t st = le16(b + 2);
            const float scale = lef32(b + 4);
            const unsigned char* d = b + 8;
            const size_t bytes = n - 8;
            if (st == 2) {
                conv_.resize(bytes / 8);
                for (size_t k = 0; k < conv_.size(); k++) conv_[k] = cf32(lef32(d + 8 * k), lef32(d + 8 * k + 4));
            } else if (st == 1) {
                const float m = scale / 32768.f;
                conv_.resize(bytes / 4);
                for (size_t k = 0; k < conv_.size(); k++) conv_[k] = cf32((int16_t)le16(d + 4 * k) * m, (int16_t)le16(d + 4 * k + 2) * m);
            } else if (st == 0) {
                const float m = scale / 128.f;
                conv_.resize(bytes / 2);
                for (size_t k = 0; k < conv_.size(); k++) conv_[k] = cf32((int8_t)d[2 * k] * m, (int8_t)d[2 * k + 1] * m);
            } else return;
            scrubNonFinite(conv_.data(), conv_.size());   // a garbled float or scale must not put a NaN into the receiver
            if (onSamples && !conv_.empty()) onSamples(conv_.data(), conv_.size());
            return;
        }
        if (type == kPktBasebandZstd) {   // never asked for (SET_COMPRESSION 0 is sent): say so once and drop it
            std::lock_guard<std::mutex> lk(stMu_);
            if (!zstd_) { fprintf(stderr, "SDR++ server: %s sends compressed samples, which OnAir does not read\n", addr_.c_str()); fflush(stderr); }
            zstd_ = true;
            return;
        }
        if (type == kPktError) {
            const uint8_t c = n ? b[0] : 0;
            std::string what;
            std::lock_guard<std::mutex> lk(stMu_);
            lastError_ = netp::sdrppErrorText(c, what) ? what : "error " + std::to_string(c);
            fprintf(stderr, "SDR++ server: %s reports an error: %s\n", addr_.c_str(), lastError_.c_str());
            fflush(stderr);
            return;
        }
        if (n < 4 || (type != kPktCommand && type != kPktAck)) return;   // VFO and FFT streams are never asked for
        const uint32_t cmd = le32(b);
        if (type == kPktCommand) {
            if (cmd == kCmdSetSampleRate && n >= 12) {
                const double r = lef64(b + 4);
                { std::lock_guard<std::mutex> lk(stMu_); rate_ = r; }
                if (onRate) onRate(r);
            } else if (cmd == kCmdDisconnect) {
                { std::lock_guard<std::mutex> lk(stMu_); busy_ = true; }
                fail(-1);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lk(stMu_);
            if (cmd == kCmdGetUi || cmd == kCmdUiAction) {
                std::vector<SdrppWidget> w;
                if (sdrppParseUi(b + 4, n - 4, w)) ui_ = std::move(w);
                else { fprintf(stderr, "SDR++ server: %s sent a menu OnAir cannot read\n", addr_.c_str()); fflush(stderr); }
            }
            if (cmd < 8) acks_[cmd]++;
            // what the server had in its own pipeline when it answered (the stream's two buffers) was taken before the change too
            if (flushing_ && cmd == flushCmd_ && acks_[cmd] >= flushAt_) { flushing_ = false; tail_ = kPipelinePackets; }
        }
        cv_.notify_all();
    }

    std::string addr_;
    sock_t sock_ = kBadSock;
    std::mutex sendMu_, stMu_;
public:
    std::mutex readMu_;
private:
    std::condition_variable cv_;
    uint64_t acks_[8] = {};
    double rate_ = 0;
    std::vector<SdrppWidget> ui_;
    bool dead_ = false, busy_ = false, badPacket_ = false, zstd_ = false, expectStream_ = false, discard_ = false, flushing_ = false;
    uint32_t flushCmd_ = 0;
    int tail_ = 0;
    uint64_t goodPackets_ = 0;
    uint64_t flushAt_ = 0;
    std::chrono::steady_clock::time_point lastRx_{};
    std::string lastError_;
    std::vector<unsigned char> body_;
    std::vector<cf32> conv_;
};

std::string busyText(const std::string& addr) {
    return "the SDR++ server at " + addr + " is busy: it serves one client at a time and another one (SDR++, or another OnAir) is connected";
}
std::string silentText(const std::string& addr) { return addr + " accepted the connection but did not answer as an SDR++ server (is this its port, 5259 by default?)"; }

// connect and read the menu: the common start of a probe and of opening the radio
bool hello(Link& l, const std::string& addr, int timeoutMs, std::string& err, bool& retry) {
    retry = false;
    std::string e;
    if (!l.connect(timeoutMs, e)) { err = e; retry = true; return false; }
    if (!l.waitGreeting(timeoutMs) && l.busy()) { err = busyText(addr); retry = true; return false; }
    if (!l.fetchUi(timeoutMs)) {
        if (l.busy()) { err = busyText(addr); retry = true; }
        else if (l.badPacket() && l.goodPackets()) { err = addr + " sent a garbled reply (SDR++'s server can mix up two of its packets)"; retry = true; }
        else if (l.badPacket()) err = addr + " is not an SDR++ server";
        else err = l.isDead() ? "the connection to " + addr + " dropped" : silentText(addr);
        return false;
    }
    return true;
}

} // namespace

bool sdrppProbe(const std::string& host, int port, int timeoutMs, std::string& err) {
    const std::string addr = (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
    Link l(addr);
    bool retry = false;
    if (!hello(l, addr, timeoutMs, err, retry)) return false;
    ServerState st = seen(addr);
    st.known = true;
    st.rate = l.rate();
    st.ui = l.ui();
    noteSeen(addr, st);
    if (sourceOf(st.ui).empty()) { err = addr + " is an SDR++ server, but it has no source module loaded"; return false; }
    return true;
}

class SdrppSource : public NativeSource {
public:
    explicit SdrppSource(std::string addr) : addr_(std::move(addr)), link_(addr_) {
        link_.onSamples = [this](const cf32* p, size_t n) { got_ += n; push(p, n); };
    }
    ~SdrppSource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        bool retry = false;
        std::string e;
        streaming_ = false;
        link_.expectStream(false);
        link_.discard(false);
        if (!hello(link_, addr_, kConnectMs, e, retry)) { err = "SDR++ server: " + e; openFailure_ = retry ? OpenFailure::Retry : OpenFailure::Final; return false; }
        link_.setRecvTimeout(kStallMs);
        ServerState st = seen(addr_);
        st.known = true; st.rate = link_.rate(); st.ui = link_.ui();
        noteSeen(addr_, st);
        const std::string src = sourceOf(st.ui);
        fprintf(stderr, "SDR++ server: connected to %s (source %s, %.3f Msps)\n", addr_.c_str(), src.empty() ? "none" : src.c_str(), st.rate / 1e6);
        fflush(stderr);
        if (src.empty()) { err = "SDR++ server: " + addr_ + " has no source module loaded"; openFailure_ = OpenFailure::Final; return false; }
        if (lostAt_.time_since_epoch().count() != 0) {
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - lostAt_).count();
            const uint64_t n = (uint64_t)std::llround(sec * (rate_ > 0 ? rate_ : 0));
            if (ring_) ring_->noteLost(n);
            fprintf(stderr, "SDR++ server: reconnected after %.1f s, about %llu samples lost\n", sec, (unsigned long long)n);
            fflush(stderr);
            lostAt_ = {};
        }
        sentFreq_ = -1; sentFmt_ = -1;
        sentGain_.clear();
        if (!link_.commandU8(kCmdSetCompression, 0)) { err = "SDR++ server: the connection to " + addr_ + " dropped"; return false; }
        if (!configure(s, err, false)) return false;
        if (!link_.command(kCmdStart)) { err = "SDR++ server: the connection to " + addr_ + " dropped"; return false; }
        streaming_ = true;
        link_.expectStream(true);
        // which controls the server greys out while it streams (the sample rate, the device, the source): those are changed between a STOP
        // and a START. The menu read before START shows them enabled.
        if (link_.fetchUi(kAckMs)) {
            std::set<std::string> was, locked;
            for (const auto& w : st.ui) if (!w.disabled) was.insert(w.id);
            for (const auto& w : link_.ui()) if (w.disabled && was.count(w.id)) locked.insert(w.id);
            locked.insert(kServerSourceId);
            ServerState now = seen(addr_);
            now.lockedRunning = locked;
            now.ui = link_.ui();
            noteSeen(addr_, now);
        }
        return true;
    }

    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        (void)live;
        const std::string lost = "SDR++ server: the connection to " + addr_ + " dropped";
        const std::set<std::string> locked = seen(addr_).lockedRunning;
        bool stopped = false;
        // a control the server only takes while stopped: STOP first (the samples still on their way are dropped), START when all is set
        auto hold = [&](const std::string& id) {
            if (!streaming_ || stopped || !(locked.count(id) || id == kServerSourceId)) return true;
            link_.discard(true);
            link_.expectStream(false);
            stopped = true;
            return link_.command(kCmdStop);
        };
        auto act = [&](const SdrppWidget& w, uint8_t type, int32_t i, float f, bool b) {
            if (!hold(w.id)) return false;
            const uint64_t n = link_.acks(kCmdUiAction);
            return link_.uiAction(w.id, type, i, f, b, true) && link_.waitAck(kCmdUiAction, n, kAckMs);
        };
        std::string warn;
        bool ok = true;

        // 1. the server's own controls the user set in OnAir (a gain mode can bring other sliders, so the menu is read again after each)
        for (int pass = 0; ok && pass < 24; pass++) {
            const auto ui = link_.ui();
            bool changed = false;
            for (const auto& w : ui) {
                const std::string key = settingKey(w);
                const auto it = s.radio.find(key);
                if (it == s.radio.end() || w.kind == SdrppWidget::Text) continue;
                const std::string& v = it->second;
                if (w.kind == SdrppWidget::Combo) {
                    const int want = atoi(v.c_str());
                    if (want != w.i && want >= 0 && want < (int)w.items.size()) { ok = act(w, kElInt, want, 0, false); changed = true; }
                } else if (w.kind == SdrppWidget::Checkbox) {
                    const bool want = v == "1" || v == "true";
                    if (want != w.b) { ok = act(w, kElBool, 0, 0, want); changed = true; }
                } else if (w.kind == SdrppWidget::Radio) {
                    if (visiblePart(w.id) == v && !w.b) { ok = act(w, kElBool, 0, 0, true); changed = true; }
                } else {
                    double want = atof(v.c_str());
                    if (w.maxV != w.minV) want = std::min(std::max(w.minV, w.maxV), std::max(std::min(w.minV, w.maxV), want));
                    const double tol = w.kind == SdrppWidget::SliderFloat ? std::max(1e-4, w.step / 2) : 0.5;
                    if (std::fabs(want - w.value) >= tol) {
                        ok = w.kind == SdrppWidget::SliderFloat ? act(w, kElFloat, 0, (float)want, false) : act(w, kElInt, (int32_t)std::lround(want), 0, false);
                        changed = true;
                    }
                }
                if (changed) break;
            }
            if (!changed) break;
        }

        // 2. the sample rate: the slowest the server offers at or above what is asked for (the fastest when none is)
        if (ok) {
            const auto ui = link_.ui();
            const int c = sdrppRateCombo(ui);
            if (c >= 0) {
                const auto rates = ratesOf(ui);
                const double want = s.sampleRate > 0 ? s.sampleRate : 0;
                int pick = -1;
                for (size_t k = 0; k < rates.size(); k++)
                    if (rates[k] >= want * 0.9999 && (pick < 0 || rates[k] < rates[(size_t)pick])) pick = (int)k;
                if (pick < 0) for (size_t k = 0; k < rates.size(); k++) if (pick < 0 || rates[k] > rates[(size_t)pick]) pick = (int)k;
                if (pick >= 0 && pick != ui[(size_t)c].i) ok = act(ui[(size_t)c], kElInt, pick, 0, false);
            }
        }

        // a change while the server streams is not waited for (that would hold the caller until the link has drained): the samples are
        // dropped until the server has answered it, and its menu is taken from that answer
        const bool async = streaming_ && !stopped;
        auto slide = [&](const SdrppWidget& w, double t) {
            const bool isInt = w.kind == SdrppWidget::SliderInt;
            if (!async) return isInt ? act(w, kElInt, (int32_t)std::lround(t), 0, false) : act(w, kElFloat, 0, (float)t, false);
            link_.flushUntilAck(kCmdUiAction);
            return link_.uiAction(w.id, isInt ? kElInt : kElFloat, (int32_t)std::lround(t), (float)t, false, true);
        };

        // 3. the gain, on the server's gain slider (or its LNA / mixer / VGA stages together)
        if (ok && !radioFlag(s, "keepgain")) {
            const auto ui = link_.ui();
            const SdrppGainPlan g = sdrppGainPlan(ui);
            if (g.sliders.empty()) addNote(warn, "the server's radio has no gain control OnAir can find: set the gain in SDR++");
            else if (!g.drivable) addNote(warn, "the gain is set by the server's AGC (switch it off under Radio settings to set it here)");
            for (int k : g.sliders) {
                const auto& w = ui[(size_t)k];
                if (w.disabled) continue;
                const double t = sdrppSliderTarget(w, g, s.gainDb);
                const auto sent = sentGain_.find(w.id);   // the menu may not have caught up with a change still on its way
                if (std::fabs(t - (sent != sentGain_.end() && async ? sent->second : w.value)) < (w.kind == SdrppWidget::SliderInt ? 0.5 : std::max(1e-4, w.step / 2)))
                    continue;
                ok = ok && slide(w, t);
                sentGain_[w.id] = t;
            }
        }

        // 4. the wire format and the frequency
        const std::string f = radioOption(s, "format", "16");
        const int fmt = f == "8" ? 0 : f == "32" ? 2 : 1;
        if (ok && fmt != sentFmt_) { ok = link_.commandU8(kCmdSetSampleType, (uint8_t)fmt); sentFmt_ = fmt; }
        const double hz = ppmCorrectedHz(std::max(s.centerHz, 0.0), radioPpm(s));
        if (ok && hz != sentFreq_) {
            const uint64_t n = link_.acks(kCmdSetFrequency);
            unsigned char b[8];
            uint64_t u;
            memcpy(&u, &hz, 8);
            for (int k = 0; k < 8; k++) b[k] = (unsigned char)(u >> (8 * k));
            if (async) { link_.flushUntilAck(kCmdSetFrequency); ok = link_.command(kCmdSetFrequency, b, 8); }
            else ok = link_.command(kCmdSetFrequency, b, 8) && link_.waitAck(kCmdSetFrequency, n, kAckMs);
            sentFreq_ = hz;
        }

        if (stopped) {
            link_.discard(false);
            if (ok) ok = link_.command(kCmdStart);
            link_.expectStream(ok);
        }
        if (!ok) { err = link_.busy() ? "SDR++ server: " + busyText(addr_) : lost; return false; }

        // what the server runs at now (it says so itself after a change of rate)
        const double r = link_.rate();
        if (r > 0) rate_ = r;
        requested_ = s.sampleRate;
        ServerState st = seen(addr_);
        st.known = true; st.rate = r; st.ui = link_.ui();
        noteSeen(addr_, st);
        if (!warn.empty()) err = warn;
        else checkRate(s.sampleRate, err);
        return true;
    }

    void closeDevice() override {
        if (streaming_.exchange(false)) link_.command(kCmdStop);   // the server keeps its radio running for the next client otherwise
        link_.expectStream(false);
        link_.close();
    }
    bool restartForRate() const override { return false; }   // configure() stops and starts the server's stream itself
    void streamLoop() override {
        // no packet for kStallMs while the server is meant to stream: its radio stopped (unplugged at the server) or the network is gone.
        // Quiet is fine while configure() holds the stream stopped.
        auto quietSince = std::chrono::steady_clock::now();
        // the samples that never arrive: SDR++ numbers nothing, so a link slower than the stream shows only as fewer samples than the rate the
        // server announced (the server's radio drops the rest). Counted per 2 s window in which nothing was dropped on purpose.
        auto winStart = quietSince;
        got_ = 0;
        bool clean = true, warned = false;
        while (run_) {
            int r;
            {
                std::lock_guard<std::mutex> lk(link_.readMu_);
                r = link_.readOne(200);
            }
            if (r < 0) break;
            const auto now = std::chrono::steady_clock::now();
            if (link_.dropping() || !link_.expectingStream()) clean = false;
            if (now - winStart >= std::chrono::seconds(2)) {
                const double sec = std::chrono::duration<double>(now - winStart).count(), want = rate_ * sec, have = (double)got_.exchange(0);
                if (clean && rate_ > 0 && have < want * 0.9) {
                    if (ring_) ring_->noteLost((uint64_t)(want - have));
                    if (!warned) {
                        fprintf(stderr, "SDR++ server: the network from %s carries only %.2f of the %.2f Msps the server sends: the rest is lost. "
                                        "Use a lower sample rate or the 8-bit wire format (Radio settings), or a faster link\n",
                                addr_.c_str(), have / sec / 1e6, rate_ / 1e6);
                        fflush(stderr);
                        warned = true;
                    }
                }
                winStart = now;
                clean = true;
            }
            if (r > 0 || !link_.expectingStream()) { quietSince = now; continue; }
            if (now - quietSince > std::chrono::milliseconds(kStallMs)) {
                fprintf(stderr, "SDR++ server: %s sends no samples (is its radio connected?)\n", addr_.c_str());
                fflush(stderr);
                break;
            }
        }
        if (run_) {
            fprintf(stderr, "SDR++ server: lost the connection to %s%s\n", addr_.c_str(), link_.busy() ? " (another client took it over)" : "");
            fflush(stderr);
            lostAt_ = std::chrono::steady_clock::now();
        }
    }
    void interrupt() override {   // stop(): STOP before the socket goes, or the server's radio keeps running for nobody
#ifdef _WIN32
        // Windows: closing a socket that still has unread stream data in its receive queue resets the connection, and the reset throws away the
        // STOP the server has received but not read yet. So end our sending side only; the stream thread then ends on the server's close or
        // on this short receive timeout (stop() closes the socket after the thread is gone).
        if (streaming_.exchange(false)) {
            link_.command(kCmdStop);
            link_.shutdownSend();
            link_.setRecvTimeout(200);
        } else {
            link_.shutdown();
        }
#else
        if (streaming_.exchange(false)) link_.command(kCmdStop);
        link_.shutdown();
#endif
    }

private:
    std::string addr_;
    Link link_;
    std::atomic<bool> streaming_{false};
    std::atomic<uint64_t> got_{0};
    int sentFmt_ = -1;
    double sentFreq_ = -1;
    std::map<std::string, double> sentGain_;   // what each gain slider was last sent while streaming
    std::chrono::steady_clock::time_point lostAt_{};
};

DeviceInfo sdrppDevice(const std::string& host, int port) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "sdrpp";
    const bool v6 = host.find(':') != std::string::npos;
    d.nativeArgs = (v6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    d.serial = d.nativeArgs;   // the radio's settings are kept per server, whatever source it runs
    const ServerState st = seen(d.nativeArgs);
    const std::string src = sourceOf(st.ui);
    d.name = "SDR++ server " + d.nativeArgs + (src.empty() ? "" : ": " + src) + " (network)";
    setCaps(d, st);
    return d;
}

void sdrppCaps(DeviceInfo& d) { setCaps(d, seen(d.nativeArgs)); }

std::unique_ptr<IqSource> makeSdrpp(const DeviceInfo& d) { return std::make_unique<SdrppSource>(d.nativeArgs); }

} // namespace native
} // namespace dect2
