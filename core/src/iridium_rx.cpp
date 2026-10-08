// Iridium receiver: the detector runs in feed(), the bursts are demodulated and decoded on a worker thread that reads them from a
// ring of input samples (no copy per burst on the feed side; a burst whose samples were overwritten before the worker got to it is
// dropped and counted).
#include "dect2/iridium_rx.h"
#include "dect2/iridium_frame.h"
#include "dect2/iridium_phy.h"
#include "dect2/iridium_sbd.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <exception>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace dect2 {

using namespace iridium;

namespace {
struct Job {
    DetectedBurst b;
    uint64_t gen = 0;
};
struct Dot { double t; float fk; uint8_t kind; };
constexpr size_t kQueueCap = 256;
} // namespace

struct IridiumReceiver::Impl {
    // configuration (mu)
    std::mutex mu;
    double rate = 0, offsetHz = 0, centerHz = 1622e6, thrDb = 13;
    std::function<void(const std::string&)> log;
    std::function<void(const IridiumBurstBits&, const IridiumFrame&)> burstCb;   // (wmu)

    // feed side (fmu)
    std::mutex fmu;
    BurstDetector det;
    // input samples as 16-bit pairs (scale 8192: +-4 full scale, finer than any radio's converter): 0.4 s or more at any rate,
    // so a worker held up for a while by a busy machine still finds its bursts
    std::vector<int16_t> ring;
    std::vector<cf32> tmp;
    size_t ringMask = 0, ringSize = 0;
    std::atomic<uint64_t> written{0};
    std::vector<DetectedBurst> found;
    std::deque<DetectedBurst> waiting;        // found, but the end margin is not in the ring yet
    std::complex<double> nco{1, 0}, ncoStep{1, 0};
    double sinceReport = 0;
    bool offline = false;

    // queue (qmu)
    std::mutex qmu;
    std::condition_variable qcv, idleCv;
    std::deque<Job> q;
    bool stop = false, busy = false;
    std::atomic<uint64_t> gen{0};
    std::thread worker;

    // worker (wmu while a burst is processed)
    std::mutex wmu;
    BurstDemod demod;
    std::vector<cf32> seg;
    IridiumMsgAssembler assembler;
    IridiumIdaAssembler idaAsm;
    struct Recent { double t, f; float level; };
    std::deque<Recent> recent;

    // results (mu)
    IridiumTelemetry tel;                     // persistent parts (counters, tables)
    std::map<int, IridiumSatInfo> sats;
    std::deque<Dot> dots;
    uint64_t lastBursts = 0, lastDemod = 0, lastUw = 0, lastFrames = 0, framesTotal = 0;
    double snrSum = 0, confSum = 0; int snrN = 0;
    double lastOkTime = -1e9, lastUwTime = -1e9;
    double ibcUnix = 0, ibcAt = 0;
    bool loggedTime = false;

    void startWorker() {
        worker = std::thread([this] { run(); });
    }
    void run() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(qmu);
                qcv.wait(lk, [&] { return stop || !q.empty(); });
                if (stop) return;
                j = q.front(); q.pop_front();
                busy = true;
            }
            {
                std::lock_guard<std::mutex> wl(wmu);
                if (j.gen == gen.load()) {
                    try { process(j); }
                    catch (const std::exception& e) { fprintf(stderr, "iridium worker: %s\n", e.what()); }
                }
            }
            {
                std::lock_guard<std::mutex> lk(qmu);
                busy = false;
            }
            idleCv.notify_all();
        }
    }
    void logLine(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void process(const Job& j) {
        const DetectedBurst& b = j.b;
        const uint64_t len = b.end - b.start;
        bool lost = written.load() - b.start > ringSize || len > ringSize / 2;
        if (!lost) {
            seg.resize(len);
            const size_t s0 = (size_t)(b.start & ringMask);
            const size_t first = std::min((size_t)len, ringSize - s0);
            constexpr float k = 1.f / 8192;
            for (size_t i = 0; i < first; i++) seg[i] = cf32(ring[2 * (s0 + i)] * k, ring[2 * (s0 + i) + 1] * k);
            for (size_t i = first; i < len; i++) seg[i] = cf32(ring[2 * (i - first)] * k, ring[2 * (i - first) + 1] * k);
            lost = written.load() - b.start > ringSize;   // overwritten while copying
        }
        double r, cHz;
        { std::lock_guard<std::mutex> lk(mu); r = rate; cHz = centerHz; }
        const double tBurst = b.start / r;
        if (lost) {
            std::lock_guard<std::mutex> lk(mu);
            if (j.gen == gen.load()) tel.dropped++;
            return;
        }
        const double absHz = cHz + b.freqHz;
        const int maxSym = absHz >= kSimplexMinHz - 40e3 ? kMaxSymbolsSimplex : kMaxSymbolsNormal;
        DemodResult d;
        bool dup = false;
        demod.demod(seg.data(), seg.size(), b.freqHz, maxSym, d);
        IridiumFrame f;
        bool haveFrame = false;
        IridiumBurstBits bb;
        if (d.uwOk) {
            // the same burst found twice (two detections of one strong burst, or its skirt): keep the first, or the stronger
            const double tUw = tBurst + d.uwTime;
            for (const auto& q : recent)
                if (std::fabs(q.t - tUw) < 2e-4 && (std::fabs(q.f - d.freqHz) < 3e3 || (std::fabs(q.f - d.freqHz) < 110e3 && q.level > d.levelDb + 15))) { d.uwOk = false; dup = true; break; }
            if (!dup) {
                recent.push_back(Recent{tUw, d.freqHz, d.levelDb});
                if (recent.size() > 64) recent.pop_front();
            }
        }
        if (d.uwOk) {
            bb.bits = d.bits;
            bb.downlink = d.downlink;
            bb.freqHz = cHz + d.freqHz;
            bb.timeSec = tBurst + d.uwTime;
            bb.levelDb = d.levelDb;
            bb.confidence = d.confidence;
            f = decodeIridiumBurst(bb);
            haveFrame = true;
        }
        if (haveFrame && burstCb) burstCb(bb, f);
        std::vector<IridiumPagerMessage> done;
        if (haveFrame && f.type == IridiumType::MSG) done = assembler.feed(f, bb.timeSec);
        std::vector<IridiumIdaPacket> packets;
        if (haveFrame && f.type == IridiumType::IDA) packets = idaAsm.feed(f, bb.downlink, bb.freqHz, bb.timeSec);
        std::vector<std::string> logs;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (j.gen != gen.load()) return;
            IridiumTelemetry& t = tel;
            if (dup) { t.duplicates++; return; }
            if (d.found) t.demodulated++;
            uint8_t kind = 0;
            if (d.uwOk) {
                kind = 1;
                t.uwOk++;
                (d.downlink ? t.downlink : t.uplink)++;
                snrSum += d.snrDb; confSum += d.confidence; snrN++;
                lastUwTime = bb.timeSec;
            }
            if (haveFrame) {
                const int ti = std::clamp((int)f.type, 0, kIridiumTypeSlots - 1);
                t.typeCount[ti]++;
                if (f.type == IridiumType::Voice) t.voiceFrames++;
                if (f.ok) {
                    kind = 2;
                    t.blocksOk++;
                    framesTotal++;
                    t.dataValid = true;
                    lastOkTime = bb.timeSec;
                    const int ch = nearestChannel(bb.freqHz);
                    t.cfoHz = bb.freqHz - channelHz(std::clamp(ch, 0, kDuplexChannels + kSimplexChannels - 1));
                } else if (f.type != IridiumType::Voice) {
                    t.blocksBad++;
                }
                if (f.ok && f.satId >= 0 && f.satId < 128) {
                    auto it = sats.find(f.satId);
                    if (it == sats.end()) {
                        IridiumSatInfo si; si.id = f.satId;
                        it = sats.emplace(f.satId, si).first;
                        char m[96];
                        snprintf(m, sizeof m, "Iridium: satellite %d heard (%s, beam %d)", f.satId, f.typeName.c_str(), f.beamId);
                        logs.push_back(m);
                    }
                    IridiumSatInfo& si = it->second;
                    si.frames++;
                    si.lastHeard = bb.timeSec;
                    si.freqOffsetHz = t.cfoHz;
                    if (f.beamId >= 0 && f.beamId < 64 && std::find(si.beams.begin(), si.beams.end(), f.beamId) == si.beams.end() && si.beams.size() < 48) {
                        si.beams.push_back(f.beamId);
                        std::sort(si.beams.begin(), si.beams.end());
                    }
                    if (f.type == IridiumType::IRA && f.hasPosition && f.altKm > 100) { si.hasPos = true; si.lat = f.lat; si.lon = f.lon; si.altKm = f.altKm; }
                }
                if (f.ok && f.type == IridiumType::IRA) {
                    IridiumRingAlert ra;
                    ra.time = bb.timeSec; ra.sat = f.satId; ra.beam = f.beamId; ra.hasPos = f.hasPosition;
                    ra.lat = f.lat; ra.lon = f.lon; ra.altKm = f.altKm; ra.paged = f.paged; ra.freqHz = bb.freqHz;
                    t.ringAlerts.push_back(ra);
                    if (t.ringAlerts.size() > 100) t.ringAlerts.erase(t.ringAlerts.begin());
                    t.pagedTotal += std::max(0, f.paged);
                    if (f.hasPosition) {
                        IridiumMapPoint mp;
                        mp.time = bb.timeSec; mp.sat = f.satId; mp.beam = f.beamId;
                        mp.lat = (float)f.lat; mp.lon = (float)f.lon; mp.altKm = (float)f.altKm; mp.satellite = f.altKm > 100;
                        t.positions.push_back(mp);
                        if (t.positions.size() > 500) t.positions.erase(t.positions.begin(), t.positions.begin() + 50);
                    }
                }
                if (f.ok && f.type == IridiumType::IBC) {
                    IridiumIbcInfo ib;
                    ib.time = bb.timeSec; ib.sat = f.satId; ib.beam = f.beamId; ib.hasTime = f.hasTime; ib.unixTime = f.unixTime;
                    t.ibc.push_back(ib);
                    if (t.ibc.size() > 32) t.ibc.erase(t.ibc.begin());
                    if (f.hasTime) {
                        ibcUnix = f.unixTime; ibcAt = bb.timeSec; t.hasTime = true;
                        if (!loggedTime) {
                            loggedTime = true;
                            const time_t tt = (time_t)f.unixTime;
                            struct tm g;
#ifdef _WIN32
                            gmtime_s(&g, &tt);
#else
                            gmtime_r(&tt, &g);
#endif
                            char m[96];
                            snprintf(m, sizeof m, "Iridium: time %04d-%02d-%02d %02d:%02d:%02d UTC from satellite %d", g.tm_year + 1900, g.tm_mon + 1, g.tm_mday, g.tm_hour, g.tm_min, g.tm_sec, f.satId);
                            logs.push_back(m);
                        }
                    }
                }
            }
            for (const auto& m : done) {
                IridiumPagerMsg pm;
                pm.time = m.timeSec; pm.ric = m.ric; pm.seq = m.seq; pm.text = m.text.substr(0, 400); pm.complete = m.complete;
                t.messages.push_back(pm);
                if (t.messages.size() > 100) t.messages.erase(t.messages.begin());
                char hdr[64];
                snprintf(hdr, sizeof hdr, "Iridium pager %d%s: ", m.ric, m.complete ? "" : " (incomplete)");
                logs.push_back(hdr + pm.text.substr(0, 120));
            }
            for (const auto& pk : packets) {
                if (!pk.complete) continue;
                t.sbdPackets++;
                const IridiumSbd sbd = iridiumParseSbd(pk.data, pk.downlink);
                if (!sbd.acars.valid) continue;
                IridiumAcarsMsg am;
                am.time = pk.timeSec; am.downlink = pk.downlink; am.mode = sbd.acars.mode; am.ack = sbd.acars.ack; am.blockId = sbd.acars.blockId;
                am.reg = sbd.acars.reg; am.label = sbd.acars.label; am.seq = sbd.acars.seq; am.flight = sbd.acars.flight;
                am.text = sbd.acars.text.substr(0, 240); am.more = sbd.acars.more;
                t.acars.push_back(am);
                if (t.acars.size() > 50) t.acars.erase(t.acars.begin());
                logs.push_back("Iridium ACARS " + am.reg + " " + am.label + ": " + am.text.substr(0, 100));
            }
            dots.push_back(Dot{tBurst, (float)(b.freqHz / 1e3), kind});
            if (dots.size() > 4000) dots.pop_front();
        }
        for (const auto& s : logs) logLine(s);
    }

    // feed side
    void pushReady(bool all) {
        const uint64_t w = written.load();
        while (!waiting.empty() && (all || waiting.front().end <= w)) {
            DetectedBurst b = waiting.front(); waiting.pop_front();
            if (b.end > w) b.end = w;
            if (b.end <= b.start) continue;
            bool full = false;
            {
                std::lock_guard<std::mutex> lk(qmu);
                if (q.size() >= kQueueCap) full = true;
                else q.push_back(Job{b, gen.load()});
            }
            if (full) { std::lock_guard<std::mutex> lk(mu); tel.dropped++; }
            else qcv.notify_one();
        }
    }
    void waitIdle() {
        std::unique_lock<std::mutex> lk(qmu);
        idleCv.wait(lk, [&] { return stop || (q.empty() && !busy); });
    }
    void report() {
        std::lock_guard<std::mutex> lk(mu);
        IridiumTelemetry& t = tel;
        const double now = written.load() / std::max(1.0, rate);
        const double win = now - lastReportAt;
        t.seq++;
        t.timeSec = now;
        t.inputRate = rate;
        t.centerMhz = centerHz / 1e6;
        t.bursts = burstsFound;
        if (win > 0.1) {   // a short last window (flush) keeps the rates of the one before
            t.burstsPerSec = (float)((t.bursts - lastBursts) / win);
            t.demodPerSec = (float)((t.demodulated - lastDemod) / win);
            t.uwPerSec = (float)((t.uwOk - lastUw) / win);
            t.framesPerSec = (float)((framesTotal - lastFrames) / win);
            lastBursts = t.bursts; lastDemod = t.demodulated; lastUw = t.uwOk; lastFrames = framesTotal;
            lastReportAt = now;
            if (snrN > 0) { t.snrDb = (float)(snrSum / snrN); t.confidence = (float)(confSum / snrN); }
            snrSum = confSum = 0; snrN = 0;
        }
        t.state = now - lastOkTime < 2.0 ? 2 : now - lastUwTime < 2.0 ? 1 : 0;
        if (t.hasTime) t.iridiumUtc = ibcUnix + (now - ibcAt);
        t.sats.clear();
        for (const auto& kv : sats) t.sats.push_back(kv.second);
        while (!dots.empty() && dots.front().t < now - 2.0) dots.pop_front();
        t.scatter.clear();
        const size_t from = dots.size() > 2000 ? dots.size() - 2000 : 0;
        for (size_t i = from; i < dots.size(); i++) t.scatter.push_back(IridiumBurstDot{(float)std::max(0.0, now - dots[i].t), dots[i].fk, dots[i].kind});
    }
    uint64_t burstsFound = 0;   // (mu)
    double lastReportAt = 0;    // (mu)

    void clearResults() {        // mu held
        const uint64_t s = tel.seq;
        tel = IridiumTelemetry();
        tel.seq = s;
        sats.clear(); dots.clear();
        lastBursts = lastDemod = lastUw = lastFrames = framesTotal = 0;
        snrSum = confSum = 0; snrN = 0;
        lastOkTime = lastUwTime = -1e9;
        ibcUnix = ibcAt = 0; loggedTime = false;
        burstsFound = 0;
        lastReportAt = 0;
    }
};

IridiumReceiver::IridiumReceiver() : p_(std::make_unique<Impl>()) { p_->startWorker(); }
IridiumReceiver::~IridiumReceiver() {
    {
        std::lock_guard<std::mutex> lk(p_->qmu);
        p_->stop = true;
    }
    p_->qcv.notify_all();
    p_->idleCv.notify_all();
    if (p_->worker.joinable()) p_->worker.join();
}

void IridiumReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> fl(p_->fmu);
    p_->gen++;
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->q.clear(); }
    std::lock_guard<std::mutex> wl(p_->wmu);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    p_->sinceReport = 0;
    p_->waiting.clear();
    p_->written = 0;
    if (inputRateHz <= 0) return;
    p_->det.configure(inputRateHz, p_->thrDb);
    p_->demod.configure(inputRateHz);
    size_t rs = 1;
    while (rs < 0.4 * inputRateHz || rs < 262144) rs <<= 1;
    p_->ring.assign(2 * rs, 0);
    p_->ringMask = rs - 1;
    p_->ringSize = rs;
    p_->tmp.resize(8192);
    p_->ncoStep = std::polar(1.0, -2 * M_PI * p_->offsetHz / inputRateHz);
    p_->nco = 1;
    p_->assembler.reset();
    p_->idaAsm.reset();
    p_->recent.clear();
    p_->clearResults();
}
void IridiumReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> fl(p_->fmu);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    if (p_->rate > 0) p_->ncoStep = std::polar(1.0, -2 * M_PI * hz / p_->rate);
}
bool IridiumReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= iridiumTuning().minSampleRate - 1;
}
void IridiumReceiver::reset() {
    std::lock_guard<std::mutex> fl(p_->fmu);
    p_->gen++;
    { std::lock_guard<std::mutex> lk(p_->qmu); p_->q.clear(); }
    std::lock_guard<std::mutex> wl(p_->wmu);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->sinceReport = 0;
    p_->waiting.clear();
    p_->written = 0;
    if (p_->rate > 0) p_->det.reset();
    p_->nco = 1;
    p_->assembler.reset();
    p_->idaAsm.reset();
    p_->recent.clear();
    p_->clearResults();
}
void IridiumReceiver::feed(const cf32* x, size_t n) {
    Impl& P = *p_;
    std::lock_guard<std::mutex> fl(P.fmu);
    double rate, off;
    { std::lock_guard<std::mutex> lk(P.mu); rate = P.rate; off = P.offsetHz; }
    if (rate <= 0 || P.ring.empty()) return;
    const size_t rs = P.ringSize;
    while (n > 0) {
        const size_t m = std::min(n, P.tmp.size());
        const cf32* src = x;
        if (off != 0) {
            for (size_t i = 0; i < m; i++) {
                P.tmp[i] = x[i] * cf32((float)P.nco.real(), (float)P.nco.imag());
                P.nco *= P.ncoStep;
            }
            P.nco /= std::abs(P.nco);
            src = P.tmp.data();
        }
        // into the ring first: a burst the detector ends here must find its samples there
        const uint64_t w = P.written.load();
        const size_t s0 = (size_t)(w & P.ringMask);
        const size_t first = std::min(m, rs - s0);
        auto q16 = [](float v) { return (int16_t)std::clamp(v * 8192.f, -32767.f, 32767.f); };   // truncation: 1/8192 at most
        for (size_t i = 0; i < first; i++) { P.ring[2 * (s0 + i)] = q16(src[i].real()); P.ring[2 * (s0 + i) + 1] = q16(src[i].imag()); }
        for (size_t i = first; i < m; i++) { P.ring[2 * (i - first)] = q16(src[i].real()); P.ring[2 * (i - first) + 1] = q16(src[i].imag()); }
        P.written = w + m;
        P.found.clear();
        P.det.feed(src, m, P.found);
        if (!P.found.empty()) {
            std::lock_guard<std::mutex> lk(P.mu);
            P.burstsFound += P.found.size();
        }
        for (const auto& b : P.found) P.waiting.push_back(b);
        std::sort(P.waiting.begin(), P.waiting.end(), [](const DetectedBurst& a, const DetectedBurst& b) { return a.end < b.end; });
        P.pushReady(false);
        if (P.offline) P.waitIdle();
        x += m; n -= m;
        P.sinceReport += (double)m;
        const double per = 0.25 * rate;
        while (P.sinceReport >= per) {
            P.sinceReport -= per;
            if (P.offline) P.waitIdle();
            P.report();
        }
    }
}
bool IridiumReceiver::telemetry(IridiumTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}
void IridiumReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void IridiumReceiver::setCenterMhz(double mhz) { std::lock_guard<std::mutex> lk(p_->mu); if (mhz > 0) p_->centerHz = mhz * 1e6; }
void IridiumReceiver::setThresholdDb(double db) {
    std::lock_guard<std::mutex> fl(p_->fmu);
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->thrDb = db;
    // not configure(): that restarts the detector's sample count while the ring's goes on, and every later burst would be read from
    // the wrong place in the ring
    if (p_->rate > 0) p_->det.setThreshold(db);
}
void IridiumReceiver::setBurstCallback(std::function<void(const IridiumBurstBits&, const IridiumFrame&)> cb) {
    std::lock_guard<std::mutex> wl(p_->wmu);
    p_->burstCb = std::move(cb);
}
void IridiumReceiver::setOffline(bool on) { std::lock_guard<std::mutex> fl(p_->fmu); p_->offline = on; }
void IridiumReceiver::flush() {
    {
        std::lock_guard<std::mutex> fl(p_->fmu);
        p_->pushReady(true);
    }
    p_->waitIdle();
    std::lock_guard<std::mutex> fl(p_->fmu);
    if (p_->rate > 0) p_->report();
}

ModeTuning iridiumTuning() {
    ModeTuning t;
    t.stdMode = 21; t.id = "iridium"; t.name = "Iridium";
    t.minMhz = 1610; t.maxMhz = 1630; t.defMhz = 1622;
    t.sampleRate = 10000000;      // 1617 .. 1627 MHz: every duplex channel above 1617 MHz and the simplex channels
    t.basebandHz = 9000000;       // keeps 1616..1617 MHz from folding onto the simplex channels at the band edge
    t.bandwidthMhz = 10.5;
    t.minSampleRate = 2000000;    // a part of the band (e.g. 2.4 Msps on 1626.25 MHz: the simplex channels)
    t.tuneOffsetHz = 0;           // bursts all over the band, the DC spike hits at most one channel
    return t;
}

} // namespace dect2
