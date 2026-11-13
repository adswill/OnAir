// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAVB for the engine: see kavb_live.h.
#include "dect2/kavb/kavb_live.h"
#include "dect2/kavb/kavb_av.h"
#include "dect2/kavb/kavb_rx.h"
#include "dect2/kavb/kavb_tel.h"
#include "dect2/dsp/exact_resampler.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <deque>
#include <fstream>
#include <mutex>

namespace dect2 {

namespace {

constexpr size_t kMaxRecent = 32;
constexpr double kFrameTimeoutSec = 2.0;        // no frame header for this long: back to "searching"
constexpr double kFlushIdleSec = 1.5;           // the open FEC groups are closed after this long without a frame
constexpr double kDwellSec = 0.1;               // how long each carrier hypothesis is tried while no frame header is found
constexpr double kLostSec = 0.5;                // no frame header for this long: the carrier search starts again around the last locked offset
constexpr double kStepFrac = 0.04;              // spacing of the hypotheses, fraction of the symbol rate (the receiver takes +-0.025 by itself; this leaves a margin)
constexpr int kMaxStep = 3;                     // hypotheses 0, +1, -1, ... +-3 steps: +-12% of the symbol rate
constexpr double kFoldFrac = 0.022;             // a locked frame whose own offset estimate is above this fraction moves the mixer to it (a jump costs the frames in flight, so only near the edge of the capture range)
constexpr double kTwoPi = 6.283185307179586;

double symRateOfListen(int p) {
    switch (p) {
    case 0: return 700e3;
    case 1: return 1050e3;
    default: return 1400e3;
    }
}

} // namespace

struct KavbLiveReceiver::Impl {
    std::mutex feedMu;                       // the processing state below
    std::mutex pubMu;                        // the published telemetry and the callback
    double rate = 0;
    bool ready = false;
    std::atomic<int> listen{0};              // wanted
    int curListen = -1;                      // built
    uint64_t samples = 0, nextReport = 0, seq = 0;

    ExactResampler rs;
    std::unique_ptr<kavb::Receiver> rx;
    std::unique_ptr<kavb::Depacketiser> dep;
    int depKey = -1;
    kavb::Depacketiser::Stats depBase{};     // stats of the depacketisers that were replaced
    std::vector<cf32> mixed, resampled;

    // coarse carrier search: the mixer steps through offset hypotheses while no frame is found, and stays where one is
    double shiftHz = 0;                      // removed from the input before the resampler
    double baseHz = 0;                       // the centre of the hypotheses: the last offset at which a frame was found
    int searchIdx = 0;                       // 0 .. 2 * kMaxStep: the hypothesis in use
    uint64_t dwellEnd = 0;                   // input sample count at which to try the next one
    std::complex<double> phasor{1.0, 0.0}, stepW{1.0, 0.0};

    KavbTelemetry cur;
    double lastFrameSec = -1e9;
    bool flushed = true, wasLocked = false;
    std::deque<std::pair<double, uint32_t>> goodBytes;
    KavbTelemetry pub;
    std::function<void(const std::string&)> log;
    std::string outPath;
    std::ofstream out;

    double now() const { return rate > 0 ? (double)samples / rate : 0.0; }

    void setStep() { stepW = std::polar(1.0, -kTwoPi * shiftHz / rate); }

    void addDepStats(const kavb::Depacketiser::Stats& s) {
        depBase.groupsOk += s.groupsOk; depBase.groupsRecovered += s.groupsRecovered; depBase.groupsLost += s.groupsLost;
        depBase.cellsMissing += s.cellsMissing; depBase.packets += s.packets; depBase.packetsDropped += s.packetsDropped;
    }

    void startStream() {
        curListen = listen.load();
        const double sym = symRateOfListen(curListen);
        ready = rate >= kavbTuning().minSampleRate - 1 && rs.configure(rate, sym * kavb::kSps);
        rs.reset();
        kavb::Receiver::Options o;
        o.expectedSymRate = sym;
        rx = std::make_unique<kavb::Receiver>(o);
        dep.reset();
        depKey = -1;
        depBase = kavb::Depacketiser::Stats();
        shiftHz = baseHz = 0; searchIdx = 0; phasor = {1.0, 0.0}; setStep();
        samples = 0; dwellEnd = (uint64_t)(kDwellSec * rate); nextReport = (uint64_t)(0.25 * rate);
        goodBytes.clear();
        const std::string keepPath = cur.savePath;
        cur = KavbTelemetry();
        cur.listenProfile = curListen;
        cur.symRate = sym;
        cur.savePath = keepPath;
        lastFrameSec = -1e9;
        flushed = true;
        wasLocked = false;
    }

    // The hypotheses in the order tried: 0, +1, -1, +2, -2, +3, -3 steps of kStepFrac symbol rates around baseHz.
    void searchStep() {
        const double sym = symRateOfListen(curListen);
        searchIdx = (searchIdx + 1) % (2 * kMaxStep + 1);
        const int k = (searchIdx + 1) / 2 * ((searchIdx & 1) ? 1 : -1);
        shiftHz = baseHz + k * kStepFrac * sym;
        setStep();
        dwellEnd = samples + (uint64_t)(kDwellSec * rate);
    }

    void onFrame(const kavb::RxFrame& f) {
        const double t = now();
        lastFrameSec = t;
        flushed = false;
        cur.profile = f.hdr.profile;
        cur.robust = f.hdr.robust;
        cur.fecProfile = f.hdr.fec;
        cur.esn0Db = cur.snrDb = (float)f.esn0Db;
        {   // a frame header passed its CRC: the search stops here; a frame far from the centre of the capture range moves the mixer to its offset
            const double sym = symRateOfListen(curListen);
            if (std::fabs(f.cfoHz) > kFoldFrac * sym) { shiftHz += f.cfoHz; setStep(); }
            baseHz = shiftHz;
            searchIdx = 0;
        }
        cur.cfoHz = (float)(shiftHz + (std::fabs(f.cfoHz) > kFoldFrac * symRateOfListen(curListen) ? 0.0 : f.cfoHz));
        cur.coarseHz = (float)shiftHz;
        if (f.allOk) cur.framesGood++; else cur.framesBad++;
        int ok = 0;
        for (const kavb::RxBlock& b : f.blocks) ok += b.ok ? 1 : 0;
        cur.blocksOk += (uint64_t)ok;
        cur.blocksBad += (uint64_t)(kavb::kBlocksPerFrame - ok);
        cur.constellationBits = f.prof.robust ? 2 : f.prof.bitsPerSym;
        cur.constellation.clear();
        for (const kavb::cf& s : f.dataSyms) cur.constellation.emplace_back(s.real(), s.imag());
        const int key = f.hdr.profile * 2 + (f.hdr.robust ? 1 : 0);
        if (!dep || key != depKey) {
            if (dep) { dep->flush(); handlePackets(dep->takePackets()); addDepStats(dep->stats()); }
            dep = std::make_unique<kavb::Depacketiser>(f.prof);
            depKey = key;
        }
        dep->pushFrame(f);
        handlePackets(dep->takePackets());
    }

    void handlePackets(const std::vector<kavb::AvPacket>& pk) {
        const double t = now();
        for (const kavb::AvPacket& p : pk) {
            if (p.type == kavb::PktType::Fill) continue;
            const int ty = (int)p.type;
            if (ty >= 0 && ty < 5) cur.packetsByType[ty]++;
            KavbPacketInfo pi;
            pi.type = ty; pi.stream = p.stream; pi.seq = p.seq; pi.bytes = (uint32_t)p.payload.size(); pi.atSec = t;
            cur.recent.insert(cur.recent.begin(), pi);
            if (cur.recent.size() > kMaxRecent) cur.recent.resize(kMaxRecent);
            static const char kId[] = "KAVB de ";
            if (p.type == kavb::PktType::Text && p.payload.size() > 8 && std::equal(kId, kId + 8, p.payload.begin())) {
                std::string c;
                for (size_t i = 8; i < p.payload.size() && c.size() < 8; i++) {
                    const char ch = (char)p.payload[i];
                    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || ch == '/') c += ch; else break;
                }
                if (!c.empty()) cur.callsign = c;
                continue;
            }
            cur.payloadBytes += p.payload.size();
            goodBytes.emplace_back(t, (uint32_t)p.payload.size());
            if (out.is_open()) {
                out.write((const char*)p.payload.data(), (std::streamsize)p.payload.size());
                if (out.good()) cur.savedBytes += p.payload.size();
                else { cur.saveError = "writing the output file failed"; out.close(); }
            }
        }
    }

    void report() {
        const double t = now();
        while (!goodBytes.empty() && t - goodBytes.front().first > 10.0) goodBytes.pop_front();
        double bytes = 0;
        for (const auto& g : goodBytes) bytes += g.second;
        if (dep && !flushed && t - lastFrameSec > kFlushIdleSec) {
            dep->flush();
            handlePackets(dep->takePackets());
            flushed = true;
        }
        const bool locked = t - lastFrameSec <= kFrameTimeoutSec;
        KavbTelemetry o = cur;
        o.seq = ++seq;
        o.state = locked ? 2 : samples ? 1 : 0;
        o.inputRate = rate;
        o.timeSec = t;
        o.dataValid = locked && o.blocksOk > 0;
        o.lastFrameAgeSec = lastFrameSec > -1e8 ? std::max(0.0, t - lastFrameSec) : 0.0;
        o.headerRejects = (uint64_t)rx->headerRejects();
        kavb::Depacketiser::Stats s = depBase;
        if (dep) {
            const kavb::Depacketiser::Stats& d = dep->stats();
            s.groupsOk += d.groupsOk; s.groupsRecovered += d.groupsRecovered; s.groupsLost += d.groupsLost;
            s.cellsMissing += d.cellsMissing; s.packets += d.packets; s.packetsDropped += d.packetsDropped;
        }
        o.groupsOk = (uint64_t)s.groupsOk; o.groupsRecovered = (uint64_t)s.groupsRecovered; o.groupsLost = (uint64_t)s.groupsLost;
        o.cellsMissing = (uint64_t)s.cellsMissing; o.packets = (uint64_t)s.packets; o.packetsDropped = (uint64_t)s.packetsDropped;
        o.throughputBps = bytes * 8.0 / std::max(1.0, std::min(10.0, t));
        std::function<void(const std::string&)> lg;
        {
            std::lock_guard<std::mutex> lk(pubMu);
            pub = std::move(o);
            lg = log;
        }
        if (lg && locked != wasLocked) lg(locked ? "KAVB frames found" : "KAVB frames stopped");
        wasLocked = locked;
    }
};

KavbLiveReceiver::KavbLiveReceiver() : p_(std::make_unique<Impl>()) {}
KavbLiveReceiver::~KavbLiveReceiver() = default;

void KavbLiveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.rate = r;
    m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KavbTelemetry();
    m.pub.listenProfile = m.curListen;
    m.pub.symRate = symRateOfListen(m.curListen);
    m.pub.savePath = m.outPath;
    m.pub.seq = ++m.seq;
    m.pub.inputRate = r;
}

bool KavbLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->feedMu);
    return p_->ready;
}

void KavbLiveReceiver::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.rate > 0) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KavbTelemetry();
    m.pub.listenProfile = m.curListen;
    m.pub.symRate = symRateOfListen(m.curListen);
    m.pub.savePath = m.outPath;
    m.pub.seq = ++m.seq;
    m.pub.inputRate = m.rate;
}

void KavbLiveReceiver::setListenProfile(int profile) { p_->listen.store(std::max(0, std::min(3, profile))); }
int KavbLiveReceiver::listenProfile() const { return p_->listen.load(); }

void KavbLiveReceiver::setOutputFile(const std::string& path) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.out.is_open()) m.out.close();
    m.out.clear();
    m.outPath = path;
    m.cur.savePath = path;
    m.cur.saveError.clear();
    m.cur.savedBytes = 0;
    if (!path.empty()) {
        m.out.open(path, std::ios::binary | std::ios::trunc);
        if (!m.out) m.cur.saveError = "the output file cannot be opened";
    }
}

void KavbLiveReceiver::clearPackets() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.cur.recent.clear();
    m.cur.framesGood = m.cur.framesBad = 0;
    m.cur.blocksOk = m.cur.blocksBad = 0;
    m.cur.payloadBytes = 0;
    for (auto& v : m.cur.packetsByType) v = 0;
    m.cur.callsign.clear();
    m.dep.reset();
    m.depKey = -1;
    m.depBase = kavb::Depacketiser::Stats();
    m.goodBytes.clear();
}

void KavbLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.rate <= 0) return;
    if (m.listen.load() != m.curListen) {
        const std::string keep = m.cur.callsign;
        m.startStream();
        m.cur.callsign = keep;
    }
    if (!m.ready) return;
    const size_t kBlock = 65536;
    for (size_t pos = 0; pos < n; pos += kBlock) {
        const size_t k = std::min(kBlock, n - pos);
        const cf32* src = x + pos;
        if (m.shiftHz != 0.0 || m.phasor != std::complex<double>(1.0, 0.0)) {
            m.mixed.resize(k);
            for (size_t i = 0; i < k; i++) {
                m.mixed[i] = src[i] * cf32((float)m.phasor.real(), (float)m.phasor.imag());
                m.phasor *= m.stepW;
                if ((i & 1023) == 1023) m.phasor /= std::abs(m.phasor);
            }
            m.phasor /= std::abs(m.phasor);
            src = m.mixed.data();
        }
        m.resampled.clear();
        m.rs.process(src, k, m.resampled);
        m.samples += k;
        if (m.samples >= m.dwellEnd && m.now() - m.lastFrameSec > kLostSec) m.searchStep();
        else if (m.samples >= m.dwellEnd) m.dwellEnd = m.samples + (uint64_t)(kDwellSec * m.rate);
        if (m.resampled.empty()) continue;
        for (const kavb::RxFrame& f : m.rx->push(m.resampled.data(), m.resampled.size())) m.onFrame(f);
        if (m.samples >= m.nextReport) {
            m.nextReport = m.samples + (uint64_t)(0.25 * m.rate);
            m.report();
        }
    }
}

bool KavbLiveReceiver::telemetry(KavbTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->pubMu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void KavbLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->pubMu); p_->log = std::move(cb); }

ModeTuning kavbTuning() {
    ModeTuning t;
    t.stdMode = 125; t.id = "kavb"; t.name = "KAVB";
    t.minMhz = 100; t.maxMhz = 6000; t.defMhz = 1290;   // amateur UHF / SHF video; modeTuning() opens the range
    t.sampleRate = 2.4e6; t.bandwidthMhz = 2.0; t.minSampleRate = 2e6;
    return t;
}

} // namespace dect2
