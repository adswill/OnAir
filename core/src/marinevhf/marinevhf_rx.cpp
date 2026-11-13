// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Marine VHF receiver (see marinevhf_rx.h): the carriers of the span, one narrow-FM channel receiver each.
#include "dect2/marinevhf/marinevhf_rx.h"
#include "dect2/marinevhf/marinevhf_tel.h"
#include "dect2/audio/audioout.h"
#include "dect2/common/timecompat.h"
#include "dect2/engine/wide_bank.h"
#include "dect2/wb/wb_export.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>

namespace dect2 {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = kMarineVhfAudioRate;
constexpr int kBlock = 480;                       // 10 ms: the squelch is decided per block
constexpr int kLookBlocks = 3;                    // the audio runs 30 ms late, so the first syllable is not cut
constexpr int kHangBlocks = 15;                   // the squelch stays open 150 ms after the signal went
constexpr double kDevHz = 5000.0;                 // full scale
constexpr double kDeemphTau = 750e-6;
constexpr double kDeemphGain = 4.8;               // unity at about 1 kHz
constexpr int kMaxKeys = 64;
constexpr double kMaxRecSec = 300.0;

// Second order filter (RBJ cookbook), double precision, real signal
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    void design(bool high, double fc, double fs, double q) {
        const double w = 2 * kPi * fc / fs, al = std::sin(w) / (2 * q), c = std::cos(w), a0 = 1 + al;
        if (high) { b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = b0; }
        else { b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0; }
        a1 = -2 * c / a0; a2 = (1 - al) / a0;
        z1 = z2 = 0;
    }
    double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
};

struct Act {
    std::atomic<int> open{0};
    std::atomic<float> level{-200.f};
    std::atomic<double> mhz{0};
};

// What the channel receivers share with the receiver that owns them. Outlives the bank.
struct Ctx {
    std::atomic<int> squelch{50};
    std::atomic<bool> deemph{true};
    std::atomic<bool> recOn{false};
    std::atomic<double> recMinSec{0.5};
    std::mutex dirMu;
    std::string recDir;
    std::atomic<int> playKey{-1};
    std::atomic<double> spanSec{0};
    Act act[kMaxKeys];
    std::function<void(int key, const float* x, size_t n)> audio;       // the sound card (called from a pool thread, only for the playing channel)
    std::function<void(const MarineVhfCall&)> callSink;                 // a finished transmission (pool thread)
    std::function<void(int number, char suffix, const float* x, size_t n)> tap;   // tests; set before the first feed, must be thread safe
    std::string dir() { std::lock_guard<std::mutex> lk(dirMu); return recDir; }
};

std::string callFileName(const std::string& dir, int64_t wall, int number, char suffix, double mhz) {
    struct tm tmv {};
    localTime((time_t)wall, &tmv);
    char b[160];
    snprintf(b, sizeof b, "marinevhf_%04d%02d%02d-%02d%02d%02d_ch%d%s_%.3fMHz.wav", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             number, suffix ? std::string(1, suffix).c_str() : "", mhz);
    return (std::filesystem::path(dir) / b).string();
}

// One carrier: narrow FM voice on one channel
class ChannelRx {
public:
    ChannelRx() : log_(1.0) {
        log_.setClock([] { return unixNow(); });
        log_.setOnClose([this](const marinevhf::CallEntry& e) { finishCall(e); });
        hp300_.design(true, 300, kRate, 0.7071);
        lp3k_.design(false, 3000, kRate, 0.7071);
        hp55_.design(true, 5500, kRate, 0.7071);
        aDe_ = 1.0 - std::exp(-1.0 / (kRate * kDeemphTau));
    }
    ~ChannelRx() {
        log_.closeAll(tSec_);
        if (ctx_ && key_ >= 0 && key_ < kMaxKeys) { ctx_->act[key_].open = 0; ctx_->act[key_].level = -200.f; }
    }
    void configure(double) {}
    void bind(Ctx* c, int key, int number, char suffix, double mhz) {
        ctx_ = c; key_ = key; number_ = number; suffix_ = suffix; mhz_ = mhz;
        if (key_ >= 0 && key_ < kMaxKeys) ctx_->act[key_].mhz = mhz;
    }
    bool telemetry(MarineVhfCarrierTel& out, uint64_t lastSeq) {
        std::lock_guard<std::mutex> lk(mu_);
        if (tel_.seq <= lastSeq) return false;
        out = tel_;
        return true;
    }

    void feed(const cf32* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const cf32 s = x[i];
            pw_ += (double)std::norm(s);
            const cf32 d = s * std::conj(prev_);
            prev_ = s;
            const double f = std::atan2((double)d.imag(), (double)d.real()) * (kRate / (2 * kPi));
            const double v = f / kDevHz;
            const double lo = lp3k_.run(hp300_.run(v));
            const double hi = hp55_.run(v);
            plo_ += lo * lo; phi_ += hi * hi;
            de_ += aDe_ * (lo - de_);
            const double a = (deemph_ ? de_ * kDeemphGain : lo) * 0.5;
            blk_[(size_t)nb_++] = (float)std::max(-1.0, std::min(1.0, a));   // a click of the discriminator (no signal) may not overload the sound card
            if (nb_ == kBlock) { block(); nb_ = 0; pw_ = plo_ = phi_ = 0; }
        }
    }

private:
    struct Blk { std::array<float, kBlock> a; bool gate; };

    void block() {
        deemph_ = ctx_ ? ctx_->deemph.load(std::memory_order_relaxed) : true;
        const double lvl = 10 * std::log10(pw_ / kBlock + 1e-20);
        const double lo = plo_ / kBlock, hi = phi_ / kBlock;
        if (!init_) { qlo_ = lo; qhi_ = hi; init_ = true; }
        else { qlo_ += 0.4 * (lo - qlo_); qhi_ += 0.4 * (hi - qhi_); }
        const double quiet = 10 * std::log10((qlo_ + 1e-7) / (qhi_ + 1e-7));
        const int sq = ctx_ ? ctx_->squelch.load(std::memory_order_relaxed) : 50;
        const double thr = -8.0 + 0.1 * std::max(0, std::min(100, sq));   // noise alone measures about -8 dB, a clean signal +5 dB
        bool rose = false;
        if (!open_) {
            if (quiet > thr) { if (++above_ >= 2) { open_ = true; rose = true; hang_ = kHangBlocks; } }
            else above_ = 0;
        } else {
            if (quiet > thr - 2) hang_ = kHangBlocks;
            else if (--hang_ <= 0) { open_ = false; above_ = 0; }
        }
        blocks_++;
        tSec_ = (double)blocks_ * kBlock / kRate;
        if (rose && !callActive_) { callActive_ = true; callSpan_ = ctx_ ? ctx_->spanSec.load() : 0; }
        log_.update(tSec_, 0, open_, lvl);
        if (key_ >= 0 && key_ < kMaxKeys && ctx_) {
            ctx_->act[key_].open = open_ ? 1 : 0;
            ctx_->act[key_].level = (float)lvl;
        }
        // the delayed, gated audio
        Blk b; b.a = blk_; b.gate = open_;
        if (rose) for (auto& q : q_) q.gate = true;   // the blocks before the squelch opened are let through
        q_.push_back(b);
        if (q_.size() > (size_t)kLookBlocks) { emit(q_.front()); q_.pop_front(); }
        // telemetry: on a change of the squelch, and about 4 times a second
        if (open_ != telOpen_ || (blocks_ % 25) == 0) {
            std::lock_guard<std::mutex> lk(mu_);
            telOpen_ = open_;
            tel_.seq++;
            tel_.open = open_;
            tel_.state = open_ ? 2 : 0;
            tel_.dataValid = true;
            tel_.levelDb = lvl;
            tel_.quietDb = quiet;
            tel_.snrDb = (float)std::max(0.0, quiet - (-11.0));
        }
    }

    void emit(const Blk& b) {
        static const std::array<float, kBlock> zeros{};
        const float* p = b.gate ? b.a.data() : zeros.data();
        if (ctx_) {
            if (ctx_->tap && number_) ctx_->tap(number_, suffix_, p, kBlock);
            if (key_ >= 0 && ctx_->audio && ctx_->playKey.load(std::memory_order_relaxed) == key_) ctx_->audio(key_, p, kBlock);
        }
        if (callActive_) {
            if (b.gate && !recStarted_) recStarted_ = true;
            if (recStarted_ && rec_.size() < (size_t)(kMaxRecSec * kRate)) {
                rec_.insert(rec_.end(), p, p + kBlock);
                if (b.gate) recGateEnd_ = rec_.size();
            }
        }
    }

    void finishCall(const marinevhf::CallEntry& e) {
        MarineVhfCall c;
        c.key = key_; c.number = number_; c.suffix = suffix_; c.mhz = mhz_;
        c.wallTime = e.wallTime; c.startSec = callSpan_; c.durationSec = e.durationSec();
        c.peakDb = e.peakDb; c.meanDb = e.meanDb;
        // the delayed blocks still in the queue belong to this call
        while (!q_.empty()) { emit(q_.front()); q_.pop_front(); }
        if (ctx_ && ctx_->recOn.load() && recGateEnd_ > 0 && (double)recGateEnd_ / kRate >= ctx_->recMinSec.load()) {
            const std::string dir = ctx_->dir();
            if (!dir.empty()) {
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                std::string path = callFileName(dir, e.wallTime, number_, suffix_, mhz_);
                for (int k = 2; std::filesystem::exists(path, ec) && k < 100; k++) {
                    path = callFileName(dir, e.wallTime, number_, suffix_, mhz_);
                    path.insert(path.size() - 4, "_" + std::to_string(k));
                }
                rec_.resize(recGateEnd_);
                std::string err;
                if (wb::writeWavMono16(path, rec_, (int)kRate, err)) c.file = path;
            }
        }
        rec_.clear(); recGateEnd_ = 0; recStarted_ = false; callActive_ = false;
        if (ctx_ && ctx_->callSink) ctx_->callSink(c);
    }

    Ctx* ctx_ = nullptr;
    int key_ = -1, number_ = 0;
    char suffix_ = 0;
    double mhz_ = 0;
    marinevhf::CallLog log_;
    Biquad hp300_, lp3k_, hp55_;
    double aDe_ = 0.1, de_ = 0;
    bool deemph_ = true;
    cf32 prev_{1.f, 0.f};
    std::array<float, kBlock> blk_{};
    int nb_ = 0;
    double pw_ = 0, plo_ = 0, phi_ = 0, qlo_ = 0, qhi_ = 0;
    bool init_ = false, open_ = false;
    int above_ = 0, hang_ = 0;
    uint64_t blocks_ = 0;
    double tSec_ = 0, callSpan_ = 0;
    bool callActive_ = false, recStarted_ = false;
    std::deque<Blk> q_;
    std::vector<float> rec_;
    size_t recGateEnd_ = 0;
    std::mutex mu_;
    MarineVhfCarrierTel tel_;
    bool telOpen_ = false;
};

using Bank = WideBank<ChannelRx, MarineVhfCarrierTel>;

} // namespace

struct MarineVhfReceiver::Impl {
    struct Stat { int calls = 0; double totalSec = 0; double lastHeardSpan = -1; int64_t lastHeardWall = 0; double peakDb = -200; };

    Ctx ctx;
    std::function<void(const std::string&)> log;
    std::mutex mu;                       // the settings and the scan (never held while the bank runs)
    std::mutex callMu;                   // the call list and the counts
    double rate = 0, offHz = 0, centerHz = 0, dialHz = 0;
    double gridOrigin = -1e18;
    bool cfgDone = false;
    int carrierLimit = 10;
    std::atomic<int> regionIdx{0};
    std::atomic<bool> absKnown{false};
    std::atomic<bool> silent{false};
    std::atomic<float> volume{1.f};
    std::atomic<bool> muted{false};
    uint64_t samples = 0;
    double lastDecide = -1;
    // what plays
    int listenKey = -1;
    bool scanOn = false, prio16 = true;
    std::vector<std::pair<int, char>> scanList;
    double dwellSec = 0.4, hangSec = 2.0;
    marinevhf::ChannelScan scan;
    int autoKey = -1;
    double lastAutoOpen = 0;
    // results
    std::deque<MarineVhfCall> calls;
    std::map<int, Stat> stats;
    uint64_t callSerial = 0;
    int recFiles = 0;
    // sound
    std::mutex audioMu;
    std::unique_ptr<AudioOut> audioOut;
    int lastAudioKey = -2;
    // telemetry
    MarineVhfTelemetry tel;
    double lastBuild = -1e9;
    uint64_t bankSeq = 0;
    std::vector<WideCarrier> carrierRows;
    bool carrierRowsValid = false;
    Bank bank;   // last: the channel receivers (destroyed first) still call into the members above

    Impl() : bank(makeOps()) {
        ctx.audio = [this](int key, const float* x, size_t n) { audioWrite(key, x, n); };
        ctx.callSink = [this](const MarineVhfCall& c) { callDone(c); };
        scan.setConfig(scanCfg());
    }

    marinevhf::Region region() const { return (marinevhf::Region)regionIdx.load(); }

    WideOps<ChannelRx, MarineVhfCarrierTel> makeOps() {
        WideOps<ChannelRx, MarineVhfCarrierTel> o;
        o.outRate = kRate;
        o.occupiedHz = 8000;
        o.preRollSec = 0.25;
        o.setup = [](ChannelRx&) {};
        o.bind = [this](ChannelRx& rx, double absHz) {
            int key = -1, number = 0; char suffix = 0; double mhz = 0;
            if (absKnown.load()) {
                const marinevhf::Region r = region();
                bool coast = false;
                const marinevhf::Channel* ch = marinevhf::findFrequency(r, absHz / 1e6, 0.0075, &coast);
                if (ch) {
                    key = (int)(ch - &marinevhf::channels(r)[0]);
                    number = ch->number; suffix = ch->suffix; mhz = coast ? ch->coastMhz : ch->shipMhz;
                    if (key >= kMaxKeys) key = -1;
                }
            }
            rx.bind(&ctx, key, number, suffix, mhz);
        };
        o.clear = [](ChannelRx&) {};
        o.rowState = [](const MarineVhfCarrierTel& t) { return t.open ? 2 : 0; };
        o.info = [](const MarineVhfCarrierTel& t) { return t.open ? std::string("voice") : std::string(); };
        return o;
    }

    marinevhf::ScanConfig scanCfg() const {
        marinevhf::ScanConfig c;
        c.dwellSec = dwellSec; c.hangSec = hangSec; c.singleTuner = false;
        c.priorityKey = prio16 ? keyOf(16, 0) : -1;
        return c;
    }
    int keyOf(int number, char suffix) const {
        const auto& v = marinevhf::channels(region());
        for (size_t i = 0; i < v.size(); i++) if (v[i].number == number && v[i].suffix == suffix) return (int)i;
        if (!suffix) for (size_t i = 0; i < v.size(); i++) if (v[i].number == number) return (int)i;
        return -1;
    }
    double centerAbs() const { return centerHz > 0 ? centerHz : dialHz > 0 ? dialHz - offHz : 0.0; }

    void regrid() {   // under mu: the raster of the finder follows the radio's centre
        if (rate <= 0) return;
        const double c = centerAbs();
        absKnown = c > 0;
        double origin;
        if (c > 0) origin = std::fmod(-c, kMarineVhfStepHz); else origin = std::fmod(offHz, kMarineVhfStepHz);
        if (origin < 0) origin += kMarineVhfStepHz;
        bank.setCenterHz(c > 0 ? c : 0.0);
        if (cfgDone && std::fabs(origin - gridOrigin) < 1.0) return;
        CarrierCfg cfg;
        cfg.stepHz = kMarineVhfStepHz;
        cfg.gridOriginHz = origin;
        cfg.maxCarriers = carrierLimit;
        cfg.onDb = 9; cfg.offDb = 5;
        cfg.holdSec = 5; cfg.debounceSec = 0.12; cfg.avgSec = 0.05;
        bank.configure(rate, cfg);
        gridOrigin = origin; cfgDone = true;
    }

    void rebuildScan(double now) {   // under mu
        scan.stop();
        std::vector<int> keys;
        if (scanOn) {
            if (scanList.empty()) {
                const auto& v = marinevhf::channels(region());
                for (size_t i = 0; i < v.size(); i++) if (v[i].voice()) keys.push_back((int)i);
            } else {
                for (auto& p : scanList) { const int k = keyOf(p.first, p.second); if (k >= 0) keys.push_back(k); }
            }
        }
        scan.setChannels(keys);
        scan.setConfig(scanCfg());
        scan.start(now);
    }

    void decide(double now) {   // under mu
        const int n = (int)std::min<size_t>(marinevhf::channels(region()).size(), kMaxKeys);
        auto open = [&](int k) { return k >= 0 && k < n && ctx.act[k].open.load(std::memory_order_relaxed) != 0; };
        const int r = scan.update(now, open);
        int play = -1;
        if (scanOn) play = r;
        else if (listenKey >= 0) play = r >= 0 ? r : listenKey;
        else if (r >= 0) play = r;
        else {   // automatic: the strongest open voice channel, sticky
            const auto& v = marinevhf::channels(region());
            int best = -1; float bl = -1e9f;
            for (int k = 0; k < n; k++) if (open(k) && v[(size_t)k].voice() && ctx.act[k].level.load() > bl) { best = k; bl = ctx.act[k].level.load(); }
            if (autoKey >= 0 && open(autoKey)) {
                lastAutoOpen = now;
                if (best >= 0 && bl > ctx.act[autoKey].level.load() + 6.f) autoKey = best;
            } else if (autoKey < 0 || now - lastAutoOpen >= 1.0) autoKey = best;
            if (autoKey >= 0 && open(autoKey)) lastAutoOpen = now;
            play = autoKey;
        }
        ctx.playKey = play;
    }

    void audioWrite(int key, const float* x, size_t n) {
        if (silent.load()) return;
        std::lock_guard<std::mutex> lk(audioMu);
        if (!audioOut) { audioOut = std::make_unique<AudioOut>(); audioOut->start(48000); audioOut->setStartThreshold(48000 / 4); }
        audioOut->setVolume(volume.load()); audioOut->setMuted(muted.load());
        if (key != lastAudioKey) { audioOut->flush(); lastAudioKey = key; }
        if (audioOut->bufferedFrames() > 48000 * 3 / 2) audioOut->flush();
        std::vector<float> st(n * 2);
        for (size_t i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = x[i];
        audioOut->write(st.data(), (int)n);
    }

    void callDone(const MarineVhfCall& c0) {
        MarineVhfCall c = c0;
        std::string line;
        {
            std::lock_guard<std::mutex> lk(callMu);
            c.serial = ++callSerial;
            calls.push_front(c);
            if (calls.size() > kMarineVhfCallMax) calls.pop_back();
            Stat& s = stats[c.key];
            s.calls++; s.totalSec += c.durationSec; s.lastHeardSpan = c.startSec + c.durationSec; s.lastHeardWall = c.wallTime; s.peakDb = std::max(s.peakDb, c.peakDb);
            if (!c.file.empty()) recFiles++;
            char b[200];
            if (c.number) snprintf(b, sizeof b, "Marine VHF ch %d%c (%.3f MHz): %.1f s, peak %.0f dB%s", c.number, c.suffix ? c.suffix : ' ', c.mhz, c.durationSec, c.peakDb, c.file.empty() ? "" : ", recorded");
            else snprintf(b, sizeof b, "Marine VHF off-channel carrier: %.1f s", c.durationSec);
            line = b;
        }
        if (log) log(line);
    }
};

MarineVhfReceiver::MarineVhfReceiver() : p_(new Impl()) {}
MarineVhfReceiver::~MarineVhfReceiver() = default;

void MarineVhfReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->cfgDone = false;
    p_->samples = 0; p_->lastDecide = -1;
    p_->ctx.spanSec = 0;
    if (inputRateHz >= marineVhfTuning().minSampleRate - 1) p_->regrid();
    p_->rebuildScan(0);
}
void MarineVhfReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offHz = hz; p_->regrid(); }
void MarineVhfReceiver::setCenterHz(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->centerHz = hz; p_->regrid(); }
void MarineVhfReceiver::setDialHz(double hz) { if (hz <= 0) return; std::lock_guard<std::mutex> lk(p_->mu); p_->dialHz = hz; p_->regrid(); }   // 0 (a recording: not known on a retune) keeps the last
bool MarineVhfReceiver::ready() const { return p_->rate >= marineVhfTuning().minSampleRate - 1 && p_->bank.ready(); }
void MarineVhfReceiver::reset() { p_->bank.reset(); std::lock_guard<std::mutex> lk(p_->mu); p_->autoKey = -1; p_->ctx.playKey = -1; }
void MarineVhfReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void MarineVhfReceiver::feed(const cf32* x, size_t n) {
    if (!ready() || !n) return;
    p_->ctx.spanSec = (double)p_->samples / p_->rate;
    p_->bank.feed(x, n);
    p_->samples += n;
    const double now = (double)p_->samples / p_->rate;
    if (now - p_->lastDecide >= 0.05) {
        p_->lastDecide = now;
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->decide(now);
    }
}

void MarineVhfReceiver::clearResults() {
    p_->bank.clearResults();
    std::lock_guard<std::mutex> lk(p_->callMu);
    p_->calls.clear(); p_->stats.clear(); p_->recFiles = 0; p_->callSerial = 0;
}

void MarineVhfReceiver::setRegion(marinevhf::Region r) {
    if ((int)r == p_->regionIdx.load() || (unsigned)r >= marinevhf::kRegionCount) return;
    {
        std::lock_guard<std::mutex> lk(p_->mu);
        p_->regionIdx = (int)r;
        p_->listenKey = -1;
        p_->autoKey = -1;
        p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate));
    }
    p_->bank.reset();   // the carriers are bound to channels of the old plan
    std::lock_guard<std::mutex> lk(p_->callMu);
    p_->stats.clear();
}
marinevhf::Region MarineVhfReceiver::region() const { return p_->region(); }

void MarineVhfReceiver::listen(int number, char suffix) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->listenKey = number > 0 ? p_->keyOf(number, suffix) : -1;
    p_->autoKey = -1;
    p_->lastDecide = -1;
}
void MarineVhfReceiver::setScan(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->scanOn = on;
    p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate));
}
void MarineVhfReceiver::setScanChannels(const std::vector<std::pair<int, char>>& ch) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->scanList = ch;
    p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate));
}
void MarineVhfReceiver::setScanTiming(double dwellSec, double hangSec) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->dwellSec = std::max(0.05, dwellSec); p_->hangSec = std::max(0.0, hangSec);
    p_->scan.setConfig(p_->scanCfg());
}
void MarineVhfReceiver::setPriority16(bool on) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->prio16 = on;
    p_->rebuildScan((double)p_->samples / std::max(1.0, p_->rate));
}
void MarineVhfReceiver::setRecording(bool on, const std::string& dir, double minSec) {
    { std::lock_guard<std::mutex> lk(p_->ctx.dirMu); p_->ctx.recDir = dir; }
    p_->ctx.recMinSec = minSec;
    p_->ctx.recOn = on && !dir.empty();
}
void MarineVhfReceiver::setSquelch(int percent) { p_->ctx.squelch = std::max(0, std::min(100, percent)); }
void MarineVhfReceiver::setDeemphasis(bool on) { p_->ctx.deemph = on; }
void MarineVhfReceiver::setCarrierLimit(int n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    n = std::max(1, std::min(24, n));
    if (n == p_->carrierLimit) return;
    p_->carrierLimit = n;
    p_->cfgDone = false;
    p_->regrid();
}
void MarineVhfReceiver::setVolume(float v) { p_->volume = std::max(0.f, std::min(1.f, v)); }
void MarineVhfReceiver::setMuted(bool m) { p_->muted = m; }
void MarineVhfReceiver::setSilent(bool s) { p_->silent = s; }
void MarineVhfReceiver::setChannelTap(std::function<void(int, char, const float*, size_t)> cb) { p_->ctx.tap = std::move(cb); }
int MarineVhfReceiver::carrierCount() const { return (int)p_->bank.liveCount(); }
std::pair<int, char> MarineVhfReceiver::playingChannel() const {
    const int k = p_->ctx.playKey.load();
    const auto& v = marinevhf::channels(p_->region());
    if (k < 0 || (size_t)k >= v.size()) return {0, 0};
    return {v[(size_t)k].number, v[(size_t)k].suffix};
}

bool MarineVhfReceiver::telemetry(MarineVhfTelemetry& out, uint64_t lastSeq) {
    Impl& I = *p_;
    const double wall = systemNowSec();
    if (wall - I.lastBuild >= 0.1) {
        I.lastBuild = wall;
        MarineVhfCarrierTel prim;
        std::vector<WideCarrier> rows;
        uint64_t bs = 0;
        if (I.bank.snapshot(prim, rows, I.bankSeq, &bs)) { I.bankSeq = bs; I.carrierRows = rows; I.carrierRowsValid = true; }
        MarineVhfTelemetry t;
        marinevhf::Region reg;
        double span;
        int playKey, listenKey;
        bool scanOn;
        int scanState;
        {
            std::lock_guard<std::mutex> lk(I.mu);
            reg = I.region();
            span = I.ctx.spanSec.load();
            playKey = I.ctx.playKey.load();
            listenKey = I.listenKey; (void)listenKey;
            scanOn = I.scanOn;
            scanState = (int)I.scan.state();
            t.inputRate = I.rate;
            t.centerHz = I.centerAbs();
        }
        const auto& chs = marinevhf::channels(reg);
        t.region = (int)reg;
        t.timeSec = span;
        t.carriers = (int)I.bank.liveCount();
        t.scanOn = scanOn; t.scanState = scanState;
        t.spanCovers = I.rate >= 1.0e6;
        t.playingKey = playKey;
        if (playKey >= 0 && (size_t)playKey < chs.size()) {
            t.playingNumber = chs[(size_t)playKey].number; t.playingSuffix = chs[(size_t)playKey].suffix;
            t.playingMhz = I.ctx.act[playKey].mhz.load();
        }
        std::vector<bool> inScan(chs.size(), false);
        {
            std::lock_guard<std::mutex> lk(I.mu);
            if (I.scanOn) {
                if (I.scanList.empty()) { for (size_t i = 0; i < chs.size(); i++) inScan[i] = chs[i].voice(); }
                else for (auto& p : I.scanList) { const int k = I.keyOf(p.first, p.second); if (k >= 0) inScan[(size_t)k] = true; }
            }
        }
        // rows: the finder's view of each channel, the squelch state of the receivers, the counts
        std::vector<const WideCarrier*> byKey(chs.size(), nullptr);
        if (I.carrierRowsValid && I.absKnown.load()) {
            for (const WideCarrier& w : I.carrierRows) {
                const double abs = t.centerHz + w.hz;
                bool coast = false;
                const marinevhf::Channel* c = marinevhf::findFrequency(reg, abs / 1e6, 0.0075, &coast);
                if (!c) continue;
                const size_t k = (size_t)(c - &chs[0]);
                if (!byKey[k] || w.levelDb > byKey[k]->levelDb) byKey[k] = &w;
            }
        }
        {
            std::lock_guard<std::mutex> lk(I.callMu);
            t.callsTotal = I.callSerial;
            t.recordedFiles = I.recFiles;
            for (size_t k = 0; k < chs.size(); k++) {
                MarineVhfChannelRow r;
                r.key = (int)k; r.number = chs[k].number; r.suffix = chs[k].suffix; r.mhz = chs[k].shipMhz;
                r.inScan = inScan[k];
                r.playing = (int)k == playKey;
                if (k < (size_t)kMaxKeys) {
                    r.open = I.ctx.act[k].open.load() != 0;
                    const double mhz = I.ctx.act[k].mhz.load();
                    if (r.open && mhz > 0) r.mhz = mhz;
                }
                if (byKey[k]) { r.active = byKey[k]->active || r.open; r.levelDb = byKey[k]->levelDb; }
                auto it = I.stats.find((int)k);
                if (it != I.stats.end()) {
                    r.calls = it->second.calls; r.totalSec = it->second.totalSec; r.lastHeardWall = it->second.lastHeardWall; r.peakDb = it->second.peakDb;
                    r.lastHeardAgoSec = std::max(0.0, span - it->second.lastHeardSpan);
                }
                if (r.open) r.lastHeardAgoSec = 0;
                t.rows.push_back(r);
            }
            for (const MarineVhfCall& c : I.calls) t.calls.push_back(c);
        }
        for (const auto& r : t.rows) if (r.open) { t.state = 2; break; }
        if (t.state == 0) t.state = (I.rate >= marineVhfTuning().minSampleRate - 1 && I.bank.ready()) ? 1 : 0;
        t.dataValid = t.callsTotal > 0;
        t.blocksOk = t.callsTotal;
        t.seq = I.tel.seq + 1;
        I.tel = std::move(t);
    }
    if (I.tel.seq <= lastSeq) return false;
    out = I.tel;
    return true;
}

ModeTuning marineVhfTuning() {
    ModeTuning t;
    t.stdMode = 157;
    t.id = "marinevhf";
    t.name = "Marine VHF";
    t.minMhz = 156.0;
    t.maxMhz = 162.1;
    t.defMhz = 159.0;               // the middle of the marine band: a wide capture holds every channel
    t.sampleRate = 8000000;
    t.basebandHz = 0;
    t.bandwidthMhz = 6.0;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 12500;         // the dial is on a channel, the DC spike falls half way between two
    return t;
}

} // namespace dect2
