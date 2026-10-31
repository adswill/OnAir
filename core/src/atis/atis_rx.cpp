// ATIS receiver, see atis_rx.h
#include "dect2/atis/atis_rx.h"
#include "dect2/atis/atis_proto.h"
#include "dect2/atis/atis_tel.h"
#include "dect2/common/mathconst.h"
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/marine/marine_mmsi.h"
#include "dect2/audio/hf_demod.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

namespace dect2 {

namespace {

// The carrier-offset branches: the tone slicer correlates at exactly 1300 and 2100 Hz, so a radio that is 700 Hz off puts both tones 700 Hz away.
// Each branch shifts the baseband by this many hertz before the discriminator; the one whose shift cancels the carrier error decodes.
constexpr double kBranchHz[] = {0.0, -500.0, 500.0, -1000.0, 1000.0, -1600.0, 1600.0};
constexpr int kBranches = (int)(sizeof kBranchHz / sizeof kBranchHz[0]);
constexpr double kDupSec = 0.25;   // the same burst found by more than one branch / polarity (they finish within one input block; two bursts are never this close)

int64_t wallNow() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Branch {
    double hz = 0;
    double phase = 0;
    marine::VhfSlicer slicer;
    marine::DscDecoder dec[2];
    std::vector<cf32> mixed;
    Branch() : dec{marine::DscDecoder(false, true), marine::DscDecoder(false, true)} {}
};

} // namespace

struct AtisReceiver::Impl {
    std::mutex mu, telMu;
    double fs = 0, offsetHz = 0;
    bool running = false;
    marine::Front front;
    std::vector<cf32> base;
    std::vector<std::unique_ptr<Branch>> br;
    hfdig::UsbAudio nfm;
    hfdig::Listen listen;
    std::vector<float> aud;
    std::function<void(const std::string&)> log;

    uint64_t nIn = 0;          // input samples
    uint64_t nBase = 0;        // 24 kHz samples
    double nextReport = 0;
    double powEma = 0, floorEma = 1e-12;
    double lastToneSec = -1e9, lastMsgSec = -1e9;
    uint64_t ok = 0, bad = 0, other = 0, serial = 0, seq = 0;
    double lastCfo = 0;
    std::deque<AtisMessage> msgs;
    std::vector<AtisStation> stations;   // newest first
    AtisTelemetry pub;

    Impl() {
        nfm.setDemod(HfDemodNfm);
        listen.on = true;
    }

    double nowSec() const { return fs > 0 ? (double)nIn / fs : 0.0; }
    double baseSec() const { return (double)nBase / marine::kBaseRate; }

    void rebuild() {
        running = false;
        if (fs < 250e3) return;
        front.configure(fs);
        front.setOffsetHz(offsetHz);
        front.reset();
        base.clear();
        br.clear();
        for (int i = 0; i < kBranches; i++) {
            auto b = std::make_unique<Branch>();
            b->hz = kBranchHz[i];
            const int bi = i;
            for (int p = 0; p < 2; p++) b->dec[p].setCallback([this, bi, p](const DscCall& c) { onCall(c, bi, p == 1); });
            br.push_back(std::move(b));
        }
        nfm.reset(); listen.reset();
        nIn = nBase = 0; nextReport = 0.25 * fs;
        powEma = 0; floorEma = 1e-12;
        lastToneSec = lastMsgSec = -1e9;
        running = true;
        publish();
    }

    void onCall(const DscCall& c, int branch, bool inverted) {
        if (c.format != atis::kFormatSpecifier) {
            if (c.format >= 0 && c.eccOk) other++;
            return;
        }
        const double t = baseSec();
        atis::Id id = atis::idFromSymbols(c.symbols);
        if (!id.valid) return;
        if (c.eccOk && !id.partial) {} else if (id.digits.find_first_not_of('~') == std::string::npos) return;   // nothing readable at all
        AtisMessage m;
        m.utc = wallNow(); m.timeSec = t;
        m.digits = id.digits; m.mmsi = id.mmsi; m.country = id.country; m.callsign = id.callsign;
        m.standardZ = id.standardZ;
        m.checkOk = c.eccOk; m.repaired = c.repaired; m.erasures = c.erasures; m.inverted = inverted;
        m.levelDb = (float)(10 * std::log10(powEma + 1e-20));
        m.cfoHz = -br[(size_t)branch]->hz;
        if (!id.partial) {
            std::string nm = marine::ShipDb::global().name(id.mmsi);
            if (nm.empty()) nm = marine::ShipDb::global().name(id.digits.substr(0, 9));
            m.name = nm;
        }
        // the same burst heard by several branches: a good copy beats a failed one, one message per burst
        for (auto it = msgs.begin(); it != msgs.end(); ++it) {
            if (t - it->timeSec > kDupSec) break;
            if (!(it->digits == m.digits || !it->checkOk || !m.checkOk)) continue;   // two checked, different numbers: two transmitters
            if (it->checkOk || !m.checkOk) return;   // a checked copy of this burst is already there, or two failed ones: keep the first
            if (bad > 0) bad--;
            msgs.erase(it);                           // a failed copy gives way to the checked one
            break;
        }
        m.serial = ++serial;
        lastCfo = m.cfoHz;
        if (m.checkOk) { ok++; lastMsgSec = t; } else bad++;
        msgs.push_front(m);
        if (msgs.size() > kAtisMessagesMax) msgs.pop_back();
        if (m.checkOk || !m.standardZ) updateStation(m);
        if (log) {
            char b[200];
            snprintf(b, sizeof b, "ATIS %s%s %s %s", m.digits.c_str(), m.checkOk ? "" : " (check failed)", m.country.c_str(), m.callsign.c_str());
            log(b);
        }
    }

    void updateStation(const AtisMessage& m) {
        if (!m.checkOk) return;
        for (size_t i = 0; i < stations.size(); i++) if (stations[i].digits == m.digits) {
            AtisStation s = stations[i];
            s.count++; s.lastSeen = m.utc; s.bestLevelDb = std::max(s.bestLevelDb, m.levelDb);
            if (s.name.empty()) s.name = m.name;
            stations.erase(stations.begin() + (long)i);
            stations.insert(stations.begin(), s);
            return;
        }
        AtisStation s;
        s.digits = m.digits; s.mmsi = m.mmsi; s.country = m.country; s.callsign = m.callsign; s.name = m.name;
        s.count = 1; s.firstSeen = s.lastSeen = m.utc; s.bestLevelDb = m.levelDb;
        stations.insert(stations.begin(), s);
        if (stations.size() > kAtisStationsMax) stations.pop_back();
    }

    void process(const cf32* x, size_t n) {
        base.clear();
        front.process(x, n, base);
        nIn += n;
        if (base.empty()) return;
        // channel power and its floor
        double p = 0;
        for (const cf32& v : base) p += (double)std::norm(v);
        p /= (double)base.size();
        powEma += 0.1 * (p - powEma);
        floorEma = std::min(floorEma * 1.0005 + 1e-14, powEma > 0 ? powEma : floorEma);
        if (floorEma < 1e-14) floorEma = 1e-14;
        // the voice
        if (listen.on.load()) {
            aud.clear(); double pw = 0;
            nfm.process(base.data(), base.size(), aud, pw);
            if (!aud.empty()) listen.play(aud.data(), aud.size());
        }
        for (auto& bp : br) {
            Branch& b = *bp;
            b.mixed.resize(base.size());
            const double w = -2 * math::kPi * b.hz / marine::kBaseRate;
            for (size_t i = 0; i < base.size(); i++) {
                b.mixed[i] = base[i] * cf32((float)std::cos(b.phase), (float)std::sin(b.phase));
                b.phase += w;
                if (b.phase > math::kPi) b.phase -= 2 * math::kPi; else if (b.phase < -math::kPi) b.phase += 2 * math::kPi;
            }
            b.slicer.process(b.mixed.data(), b.mixed.size(), [&](float v) { b.dec[0].pushBit(v); b.dec[1].pushBit(v); });
            if (b.dec[0].locked() || b.dec[1].locked()) lastToneSec = baseSec() + (double)0;
        }
        nBase += base.size();
    }

    void publish() {
        const double t = baseSec();
        AtisTelemetry o;
        o.timeSec = t; o.inputRate = fs;
        o.dataValid = t - lastMsgSec < 60.0;
        o.state = o.dataValid ? 2 : (t - lastToneSec < 30.0 ? 1 : 0);
        o.cfoHz = lastCfo;
        o.levelDb = (float)(10 * std::log10(powEma + 1e-20));
        o.snrDb = (float)(10 * std::log10((powEma + 1e-20) / (floorEma + 1e-20)));
        o.blocksOk = ok; o.blocksBad = bad; o.otherCalls = other;
        o.listening = listen.on.load();
        o.messages.assign(msgs.begin(), msgs.end());
        o.stations = stations;
        o.seq = ++seq;
        std::lock_guard<std::mutex> lk(telMu);
        pub = std::move(o);
    }
};

AtisReceiver::AtisReceiver() : p_(std::make_unique<Impl>()) {}
AtisReceiver::~AtisReceiver() = default;

void AtisReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->fs = r; p_->rebuild(); }
void AtisReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); if (hz != p_->offsetHz) { p_->offsetHz = hz; if (p_->fs > 0) p_->rebuild(); } }
bool AtisReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->running; }
void AtisReceiver::reset() { std::lock_guard<std::mutex> lk(p_->mu); if (p_->fs > 0) p_->rebuild(); }
void AtisReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }

void AtisReceiver::clearResults() {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    m.msgs.clear(); m.stations.clear();
    m.ok = m.bad = m.other = 0; m.lastMsgSec = -1e9;
    m.publish();
}

void AtisReceiver::feed(const cf32* x, size_t n) {
    std::lock_guard<std::mutex> lk(p_->mu);
    Impl& m = *p_;
    if (!m.running || n == 0) return;
    m.process(x, n);
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.fs;
        m.publish();
    }
}

void AtisReceiver::flush() { std::lock_guard<std::mutex> lk(p_->mu); p_->publish(); }

bool AtisReceiver::telemetry(AtisTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}

void AtisReceiver::setListen(bool on) { p_->listen.on = on; }
bool AtisReceiver::listening() const { return p_->listen.on.load(); }
void AtisReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void AtisReceiver::setMuted(bool m) { p_->listen.muted = m; }
void AtisReceiver::setSilent(bool s) { p_->listen.silent = s; }
void AtisReceiver::setListenTap(std::function<void(const float* x, size_t n)> cb) { p_->listen.setTap(std::move(cb)); }

ModeTuning atisTuning() {
    ModeTuning t;
    t.stdMode = 213; t.id = "atis"; t.name = "ATIS (inland waterways)";
    t.minMhz = 156.0; t.maxMhz = 162.1; t.defMhz = 156.5;   // inland channel 10 (intership / Rhine); the inland channels are 156.3 - 162.0
    t.sampleRate = 1e6;
    t.basebandHz = 0;
    t.bandwidthMhz = 0.5;
    t.minSampleRate = 250e3;
    t.tuneOffsetHz = 150e3;   // the radio sits 150 kHz above the channel, clear of the DC spike
    return t;
}

} // namespace dect2
