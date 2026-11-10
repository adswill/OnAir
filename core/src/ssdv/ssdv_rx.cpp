// SSDV receiver, see ssdv_rx.h
#include "dect2/ssdv/ssdv_rx.h"
#include "dect2/ssdv/wenet_tel.h"
#include "dect2/ssdv/ssdv_proto.h"
#include "dect2/ssdv/ssdv_tel.h"
#include "dect2/ssdv/wenet.h"
#include "dect2/packet/packet_demod.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

using pktdemod::kMidRate;

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

int pop16(unsigned v) { int n = 0; for (v &= 0xFFFF; v; v &= v - 1) n++; return n; }

constexpr int kSyncBauds[] = {1200, 2400, 4800, 9600, 19200};
constexpr int kAsyncBauds[] = {300, 600, 1200};

// A packet being collected after its sync word
struct Cap {
    std::array<uint8_t, ssdv::kPktSize> pkt{};
    int nbytes = 2, nbits = 0;
    uint8_t cur = 0;
    bool exact = false;
};

struct Chain {
    int baud = 1200;
    bool async = false, inv = false, narrow = true;
    double sps = 80;
    // sampler
    std::vector<float> box;
    size_t bpos = 0;
    double bsum = 0;
    float hi = 0, lo = 0;
    bool init = false;
    float ph = -0.5f;
    bool lev = false, prevLev = false;
    // sync mode
    uint32_t sr = 0;
    // async mode
    int astate = 0, abit = 0;      // 0 idle, 1 data, 2 stop 1, 3 second stop or next start
    uint8_t acur = 0;
    uint8_t b1 = 0, b0 = 0;        // the last two bytes
    std::vector<Cap> caps;

    void setup(int bd, bool as, bool iv) {
        baud = bd; async = as; inv = iv; narrow = bd <= 2400;
        sps = kMidRate / bd;
        box.assign((size_t)std::max(1.0, std::round(sps * 0.8)), 0.f);   // integrate over most of a bit: the best noise figure with the clock loop below
        reset();
    }
    void reset() {
        std::fill(box.begin(), box.end(), 0.f);
        bpos = 0; bsum = 0; hi = lo = 0; init = false; ph = -0.5f; lev = prevLev = false;
        sr = 0; astate = 0; abit = 0; acur = 0; b1 = b0 = 0; caps.clear();
    }
};

struct ImageState {
    ssdv::Assembler as;
    std::string call;
    int imageId = 0;
    int64_t firstUtc = 0, lastUtc = 0;
    double lastSec = 0;
    uint64_t rev = 0, builtRev = 0, savedRev = 0;
    std::shared_ptr<const std::vector<uint8_t>> jpeg;
    std::string savedPath;
    bool saved = false;
};

} // namespace

struct SsdvReceiver::Impl {
    mutable std::mutex procMu, telMu;
    pktdemod::FmFront fe;
    double fs = 0;
    bool isReady = false;
    std::function<void(const std::string&)> log;
    std::vector<Chain> chains;
    int fixedBaud = 0;
    wenet::Demod wn;
    uint64_t wenetOther = 0, wenetIdle = 0;
    bool lastWenet = false;
    WenetGps lastGps; WenetOrient lastOrient; std::vector<WenetText> wenetTexts;
    std::string saveDir;
    // results
    std::deque<SsdvPacketLog> packets;
    std::deque<ImageState> images;      // newest first
    uint64_t ok = 0, bad = 0, corrected = 0, done = 0;
    double lastGoodSec = -1e9;
    int lastBaud = 0;
    bool lastAsync = false;
    uint64_t seq = 0;
    int64_t nextReport = (int64_t)(0.25 * kMidRate);
    int lastState = 0;
    SsdvTelemetry pub;

    static bool isWenetBaud(int b) { return b == wenet::kUartBaud || b == wenet::kRawBaud; }
    void say(const std::string& s) { if (log) log(s); }
    double now() const { return (double)fe.n96 / kMidRate; }

    void configure(double rate) {
        fs = rate;
        isReady = false;
        fe.chanPass = 12000.0; fe.chanStop = 20000.0; fe.chanPass2 = 3500.0; fe.chanStop2 = 7500.0;
        fe.clipHz = 12000.f; fe.afcMaxHz = 12000.0;
        if (!fe.configure(fs, 190000)) return;   // the tuning asks 1 Msps of a radio; a recording of audio (ssdv_audio.h) comes at 192 kHz
        wn.configure(fs);
        wn.setCallback([this](const wenet::Hit& h) { onWenet(h); });
        applyMask();
        chains.clear();
        for (int inv = 0; inv < 2; inv++) {
            for (int b : kSyncBauds) { Chain c; c.setup(b, false, inv != 0); chains.push_back(std::move(c)); }
            for (int b : kAsyncBauds) { Chain c; c.setup(b, true, inv != 0); chains.push_back(std::move(c)); }
        }
        isReady = true;
        resetState();
        say(wn.ready() ? "SSDV: listening for 256-byte SSDV packets (plain FSK 1200 - 19200 baud, RTTY-framed 300 - 1200 baud, Wenet 115 / 96 kbaud)"
                       : "SSDV: listening for 256-byte SSDV packets (plain FSK 1200 - 19200 baud, RTTY-framed 300 - 1200 baud; Wenet needs a sample rate of 700 kHz or more)");
    }

    void resetState() {
        const double off = fe.offsetHz;
        fe.reset(); fe.offsetHz = off;
        for (auto& c : chains) c.reset();
        wn.reset();
        nextReport = fe.n96 + (int64_t)(0.25 * kMidRate);
    }

    void accept(int baud, bool async, bool inverted, bool isWenet, float snr, std::array<uint8_t, ssdv::kPktSize>& pkt, int rsErr, const ssdv::Info& in) {
        const int64_t utc = wallNow();
        ok++;
        if (rsErr > 0) corrected++;
        lastGoodSec = now(); lastBaud = baud; lastAsync = async; lastWenet = isWenet;
        SsdvPacketLog l;
        l.utc = utc; l.timeSec = now(); l.call = in.call; l.imageId = in.imageId; l.packetId = in.packetId; l.rsErrors = rsErr;
        l.baud = baud; l.async = async; l.inverted = inverted; l.wenet = isWenet; l.noFec = in.type == 1; l.eoi = in.eoi; l.snrDb = isWenet ? snr : (float)fe.snrEma;
        packets.push_front(l);
        if (packets.size() > kSsdvPacketsMax) packets.pop_back();
        // the picture
        ImageState* im = nullptr;
        for (auto& i : images) if (i.call == in.call && i.imageId == in.imageId) { im = &i; break; }
        if (!im) {
            images.emplace_front();
            im = &images.front();
            im->call = in.call; im->imageId = in.imageId; im->firstUtc = utc;
            if (images.size() > kSsdvImagesMax) images.pop_back();
        }
        if (im->as.add(pkt.data())) { im->rev++; im->saved = false; }
        im->lastUtc = utc; im->lastSec = now();
        say("SSDV: " + in.call + " picture " + std::to_string(in.imageId) + " packet " + std::to_string(in.packetId));
    }

    // a Wenet frame passed the LDPC code and its CRC: the first byte is the packet type
    void onWenet(const wenet::Hit& h) {
        const uint8_t type = h.pkt[0];
        if (type == 0x55) {
            std::array<uint8_t, ssdv::kPktSize> pk;
            std::copy(h.pkt.begin(), h.pkt.end(), pk.begin());
            int e = 0;
            ssdv::Info info;
            if (ssdv::check(pk.data(), &e, &info)) { accept(h.baud, false, h.inverted, true, h.snrDb, pk, e, info); return; }
            bad++;
            return;
        }
        lastGoodSec = now(); lastBaud = h.baud; lastAsync = false; lastWenet = true;
        if (type == 0x56) wenetIdle++; else wenetOther++;
        if (type == 0x01) { WenetGps g; if (wenetParseGps(h.pkt.data(), wenet::kPayloadBytes, g)) lastGps = g; }
        else if (type == 0x02) { WenetOrient o; if (wenetParseOrient(h.pkt.data(), wenet::kPayloadBytes, o)) lastOrient = o; }
        else if (type == 0x00) { WenetText m; if (wenetParseText(h.pkt.data(), wenet::kPayloadBytes, m)) { wenetTexts.push_back(m); if (wenetTexts.size() > 50) wenetTexts.erase(wenetTexts.begin()); } }
    }

    void applyMask() {
        wn.setMask(fixedBaud == 0 ? (wenet::Demod::kMaskUart | wenet::Demod::kMaskRaw) : fixedBaud == wenet::kUartBaud ? wenet::Demod::kMaskUart : fixedBaud == wenet::kRawBaud ? wenet::Demod::kMaskRaw : 0);
    }

    void tryPacket(const Chain& c, Cap& cp) {
        int e = 0;
        ssdv::Info info;
        if (ssdv::check(cp.pkt.data(), &e, &info)) accept(c.baud, c.async, c.inv, false, (float)fe.snrEma, cp.pkt, e, info);
        else if (cp.exact) bad++;
    }

    void startCap(Chain& c, uint8_t typeByte, bool exact) {
        if (c.caps.size() >= 6) c.caps.erase(c.caps.begin());
        Cap cp;
        cp.pkt[0] = 0x55; cp.pkt[1] = typeByte; cp.exact = exact;
        c.caps.push_back(cp);
    }

    // a bit (already corrected for polarity) arrives
    void pushCaps(Chain& c, int b) {
        for (size_t i = 0; i < c.caps.size();) {
            Cap& cp = c.caps[i];
            cp.cur = (uint8_t)(cp.cur << 1 | b);
            if (++cp.nbits == 8) { cp.pkt[(size_t)cp.nbytes++] = cp.cur; cp.nbits = 0; }
            if (cp.nbytes >= ssdv::kPktSize) { Cap done = cp; c.caps.erase(c.caps.begin() + (long)i); tryPacket(c, done); continue; }
            i++;
        }
    }

    void onBit(Chain& c, int b) {
        b ^= c.inv ? 1 : 0;
        if (!c.async) {
            pushCaps(c, b);
            c.sr = (c.sr << 1) | (uint32_t)b;
            const unsigned w = c.sr & 0xFFFF;
            const int d6 = pop16(w ^ 0x5566), d7 = pop16(w ^ 0x5567);
            const int d = std::min(d6, d7);
            // counted as a failed packet only when it looks like a real start: the byte before is a preamble byte (0x55, one bit wrong at most)
            const bool lead = pop16(((c.sr >> 16) & 0xFF) ^ 0x55) <= 1;
            if (d <= 1) startCap(c, d7 < d6 ? 0x67 : 0x66, d == 0 && lead);
            return;
        }
        // UART: start bit 0, eight data bits LSB first, one or two stop bits 1
        switch (c.astate) {
        case 0: if (b == 0) { c.astate = 1; c.abit = 0; c.acur = 0; } break;
        case 1:
            c.acur = (uint8_t)(c.acur | (b << c.abit));
            if (++c.abit == 8) c.astate = 2;
            break;
        case 2:
            if (b != 1) { c.astate = 0; break; }   // framing error
            onByte(c, c.acur);
            c.astate = 3;
            break;
        default:   // second stop bit, or the start bit of the next byte (one stop bit only)
            if (b == 1) c.astate = 0;
            else { c.astate = 1; c.abit = 0; c.acur = 0; }
            break;
        }
    }

    void onByte(Chain& c, uint8_t v) {
        for (size_t i = 0; i < c.caps.size();) {
            Cap& cp = c.caps[i];
            cp.pkt[(size_t)cp.nbytes++] = v;
            if (cp.nbytes >= ssdv::kPktSize) { Cap done = cp; c.caps.erase(c.caps.begin() + (long)i); tryPacket(c, done); continue; }
            i++;
        }
        c.b1 = c.b0; c.b0 = v;
        if (c.b1 == 0x55 && (c.b0 == 0x66 || c.b0 == 0x67)) startCap(c, c.b0, true);
    }

    void runChain(Chain& c, const float* d, size_t n) {
        const float step = (float)(c.baud / kMidRate);
        const float dec = (float)(1.0 / (c.sps * 40.0));
        for (size_t i = 0; i < n; i++) {
            const float x = d[i];
            c.bsum += x - c.box[c.bpos];
            c.box[c.bpos] = x;
            if (++c.bpos == c.box.size()) c.bpos = 0;
            const float v = (float)(c.bsum / (double)c.box.size());
            if (!c.init) { c.hi = c.lo = v; c.init = true; }
            if (v > c.hi) c.hi = v; else c.hi += (v - c.hi) * dec;
            if (v < c.lo) c.lo = v; else c.lo += (v - c.lo) * dec;
            const float mid = 0.5f * (c.hi + c.lo);
            c.lev = v > mid;
            if (c.lev != c.prevLev) c.ph *= 0.8f;
            c.prevLev = c.lev;
            c.ph += step;
            if (c.ph >= 0.5f) { c.ph -= 1.f; onBit(c, c.lev ? 1 : 0); }
        }
    }

    void saveImage(ImageState& im) {
        if (saveDir.empty() || !im.jpeg || im.jpeg->empty()) return;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(saveDir.c_str())), ec);
        char ts[32];
        const time_t tt = (time_t)im.firstUtc;
        struct tm lt;
#ifdef _WIN32
        gmtime_s(&lt, &tt);
#else
        gmtime_r(&tt, &lt);
#endif
        std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &lt);
        char name[96];
        std::snprintf(name, sizeof name, "/%s-%d-%s%s.jpg", im.call.c_str(), im.imageId, ts, im.as.complete() ? "" : "-partial");
        const std::string path = saveDir + name;
        std::ofstream f(std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(im.jpeg->data()), (std::streamsize)im.jpeg->size());
        if (!im.savedPath.empty() && im.savedPath != path) { std::error_code e2; std::filesystem::remove(std::filesystem::path(reinterpret_cast<const char8_t*>(im.savedPath.c_str())), e2); }
        im.savedPath = path; im.savedRev = im.rev; im.saved = true;
    }

    void housekeeping(bool force) {
        const double t = now();
        for (auto& im : images) {
            if (im.rev != im.builtRev) {
                im.jpeg = std::make_shared<const std::vector<uint8_t>>(im.as.jpeg());
                im.builtRev = im.rev;
                if (im.as.complete() && !im.saved) { done++; saveImage(im); say("SSDV: picture " + std::to_string(im.imageId) + " from " + im.call + " is complete"); }
            }
            if (!im.saved && im.savedRev != im.rev && (force || t - im.lastSec > 20.0)) saveImage(im);
        }
    }

    void publish() {
        housekeeping(false);
        const double t = now();
        SsdvTelemetry o;
        o.timeSec = t; o.inputRate = fs;
        o.dataValid = t - lastGoodSec < 30.0;
        o.state = o.dataValid ? 2 : (fe.carrierRecent(5) ? 1 : 0);
        o.cfoHz = fe.cfoHz(); o.snrDb = (float)fe.snrEma; o.levelDb = fe.takeLevelDb();
        o.blocksOk = ok + wenetOther + wenetIdle; o.blocksBad = bad; o.baud = lastBaud; o.async = lastAsync; o.wenetGps = lastGps; o.wenetOrient = lastOrient; o.wenetTexts = wenetTexts; o.wenet = lastWenet; o.wenetOther = wenetOther; o.wenetIdle = wenetIdle; o.fixedBaud = fixedBaud; o.corrected = corrected; o.imagesDone = done;
        o.packets.assign(packets.begin(), packets.end());
        for (auto& im : images) {
            SsdvImage s;
            s.call = im.call; s.imageId = im.imageId; s.width = im.as.width(); s.height = im.as.height(); s.quality = im.as.quality();
            s.packets = (int)im.as.packets(); s.lastPacket = im.as.lastPacketId(); s.missing = im.as.missing(); s.complete = im.as.complete();
            s.firstUtc = im.firstUtc; s.lastUtc = im.lastUtc; s.rev = im.rev; s.jpeg = im.jpeg; s.savedPath = im.savedPath; s.gaps = im.as.gapMcus();
            o.images.push_back(std::move(s));
        }
        if (o.state != lastState) {
            say(o.state == 2 ? "SSDV: decoding packets" : o.state == 1 ? "SSDV: a carrier, no good packet lately" : "SSDV: no signal");
            lastState = o.state;
        }
        o.seq = ++seq;
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }

    void feed(const cf32* x, size_t n) {
        if (!isReady || n == 0) return;
        const std::vector<float>& wide = fe.process(x, n);
        const std::vector<float>& narrow = fe.narrowDisc();
        if (wn.ready()) wn.feed(x, n);
        if (!wide.empty() && !isWenetBaud(fixedBaud))
            for (auto& c : chains) {
                if (fixedBaud && c.baud != fixedBaud) continue;
                const std::vector<float>& d = c.narrow ? narrow : wide;
                runChain(c, d.data(), d.size());
            }
        fe.commit();
        while (fe.n96 >= nextReport) { nextReport += (int64_t)(0.25 * kMidRate); publish(); }
    }
};

SsdvReceiver::SsdvReceiver() : p_(std::make_unique<Impl>()) {}
SsdvReceiver::~SsdvReceiver() = default;

void SsdvReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->procMu); p_->configure(r); }
void SsdvReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->procMu); p_->fe.setOffset(hz); p_->wn.setOffset(hz); }
bool SsdvReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->procMu); return p_->isReady; }
void SsdvReceiver::reset() { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->isReady) p_->resetState(); }
void SsdvReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->procMu); p_->log = std::move(cb); }
void SsdvReceiver::setBaud(int b) { std::lock_guard<std::mutex> lk(p_->procMu); if (p_->fixedBaud != b) { p_->fixedBaud = b; for (auto& c : p_->chains) c.reset(); p_->applyMask(); } }
void SsdvReceiver::setSaveDir(const std::string& d) { std::lock_guard<std::mutex> lk(p_->procMu); p_->saveDir = d; }
void SsdvReceiver::feed(const cf32* x, size_t n) { std::lock_guard<std::mutex> lk(p_->procMu); p_->feed(x, n); }
void SsdvReceiver::flush() { std::lock_guard<std::mutex> lk(p_->procMu); p_->housekeeping(true); p_->publish(); }

void SsdvReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->procMu);
    Impl& m = *p_;
    m.packets.clear(); m.images.clear(); m.ok = m.bad = m.corrected = m.done = 0; m.wenetOther = m.wenetIdle = 0; m.lastGoodSec = -1e9; m.lastBaud = 0;
    m.publish();
}

bool SsdvReceiver::telemetry(SsdvTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

const SsdvPreset* ssdvPresets(int& count) {
    static const SsdvPreset p[] = {{"434.650 MHz (balloons)", 434.650}, {"144.800 MHz (APRS)", 144.800}, {"437.200 MHz", 437.200}};
    count = (int)(sizeof p / sizeof p[0]);
    return p;
}

ModeTuning ssdvTuning() {
    ModeTuning t;
    t.stdMode = 208; t.id = "ssdv"; t.name = "SSDV pictures";
    t.minMhz = 30; t.maxMhz = 1000; t.defMhz = 434.650;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.05;
    t.minSampleRate = 1000000;
    t.tuneOffsetHz = 50000;
    return t;
}

} // namespace dect2
