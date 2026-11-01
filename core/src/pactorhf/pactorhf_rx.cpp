// PACTOR-II / -III / -IV receiver (see pactorhf_rx.h).
#include "dect2/pactorhf/pactorhf_rx.h"
#include "dect2/pactorhf/pactorhf_dsp.h"
#include "dect2/pactorhf/pactorhf_tel.h"
#include "dect2/marine/marine_dsp.h"
#include "../audio/ham_listen.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

namespace dect2 {

namespace {
constexpr double kFs = 8000;
constexpr size_t kBufSec = 40;
double nowUtc() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
}

struct PactorHfReceiver::Impl {
    const int wanted;
    std::mutex mu;
    std::function<void(const std::string&)> log;
    double rate = 0, offsetHz = 0;
    PactorHfTelemetry pub;
    std::atomic<bool> resetReq{true}, offsetReq{false}, clearReq{false};
    // feed thread
    double curRate = 0;
    bool running = false;
    marine::Front front;
    hfdig::UsbAudio usb;
    std::vector<cf32> base;
    std::vector<float> aud;
    std::vector<float> buf;                 // last kBufSec seconds of audio (ring)
    size_t wr = 0; int64_t na = 0;
    double secPow = 0; int secN = 0;
    std::vector<float> levelHist;
    int64_t nIn = 0;
    double nextReport = 0, nextAnalysis = 0, analysisEvery = 4.0;
    double t0 = 0;
    // worker
    std::thread worker;
    std::mutex wm;
    std::condition_variable wcv;
    bool job = false, stop = false, busy = false;
    std::vector<float> jobAudio;
    double jobTime = 0;
    // results (under mu)
    pactorhf::Analysis last;
    bool haveLast = false;
    std::vector<PactorHfEvent> events;
    uint64_t analyses = 0, detections = 0, copiesTotal = 0;
    int lastVariant = -1, lastCarriers = -1;
    bool changed = true;
    double lastSigTime = -1e9;

    explicit Impl(int v) : wanted(v) {
        buf.assign((size_t)(kBufSec * kFs), 0.f);
        worker = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(wm); stop = true; }
        wcv.notify_all();
        if (worker.joinable()) worker.join();
    }

    void say(const std::string& s) {
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); cb = log; }
        if (cb) cb(s);
    }

    void addEvent(const std::string& s, double sigTime) {
        PactorHfEvent e;
        e.utc = (int64_t)(t0 + sigTime); e.timeSec = sigTime; e.text = s;
        events.insert(events.begin(), e);
        if (events.size() > kPactorHfEventsMax) events.pop_back();
        changed = true;
    }

    void resetState() {
        double off;
        std::function<void(const std::string&)> cb;
        { std::lock_guard<std::mutex> lk(mu); curRate = rate; off = offsetHz; cb = log; }
        running = curRate >= pactorHfTuning(wanted).minSampleRate - 1;
        if (running) { front.configure(curRate); front.setOffsetHz(off); }
        usb.reset();
        std::fill(buf.begin(), buf.end(), 0.f);
        wr = 0; na = 0; secPow = 0; secN = 0;
        nIn = 0; nextReport = 0; nextAnalysis = analysisEvery * kFs;
        t0 = nowUtc();
        {
            std::lock_guard<std::mutex> lk(mu);
            pub.inputRate = curRate; haveLast = false; lastVariant = -1; lastCarriers = -1;
        }
        changed = true;
        if (cb && curRate > 0) {
            static const char* kMsg[] = {"", "", "PACTOR-II: two D-PSK carriers 200 Hz apart, 100 baud, upper sideband (physical layer only: the channel coding is proprietary)",
                                         "PACTOR-III: 2 .. 18 D-PSK carriers on a 120 Hz grid, 100 baud, upper sideband (physical layer only: the channel coding is proprietary)",
                                         "PACTOR-IV: one PSK carrier at 1500 Hz, 1800 baud, upper sideband (physical layer only: the channel coding is proprietary)"};
            cb(running ? kMsg[wanted] : "PACTOR: the sample rate is too low (250 kHz at least)");
        }
    }

    // ---------------------------------------------------------------- worker
    void run() {
        for (;;) {
            std::vector<float> a;
            double tt = 0;
            {
                std::unique_lock<std::mutex> lk(wm);
                wcv.wait(lk, [&] { return job || stop; });
                if (stop) return;
                a.swap(jobAudio); tt = jobTime; job = false; busy = true;
            }
            pactorhf::Analysis r = pactorhf::analyse(a.data(), a.size(), wanted);
            {
                std::lock_guard<std::mutex> lk(mu);
                last = std::move(r); haveLast = true; analyses++;
                const auto& L = last;
                if (L.found) {
                    detections++;
                    if (L.variant != lastVariant || L.nCarriers != lastCarriers) {
                        char b[200];
                        if (L.variant == 4) snprintf(b, sizeof b, "PACTOR-IV signal: 1800 baud, centre %.0f Hz, clock line %.0f dB, constellation %s", L.centreHz, L.lineDb, L.constellation ? (L.constellation == 16 ? "QAM (estimate)" : std::to_string(L.constellation).append("-PSK (estimate)").c_str()) : "unknown");
                        else snprintf(b, sizeof b, "%s signal: %d carriers%s%s, centre %.0f Hz, spacing %.0f Hz, %.2f baud", pactorHfName(L.variant), L.nCarriers, L.level ? ", speed level " : "", L.level ? std::to_string(L.level).c_str() : "", L.centreHz, L.spacingHz, L.baud);
                        addEvent(b, tt);
                        lastVariant = L.variant; lastCarriers = L.nCarriers;
                    }
                    lastSigTime = tt;
                } else if (lastVariant != 0 && tt - lastSigTime > 12) {
                    addEvent("signal gone", tt);
                    lastVariant = 0; lastCarriers = 0;
                }
                changed = true;
            }
            { std::lock_guard<std::mutex> lk(wm); busy = false; }
            wcv.notify_all();
        }
    }

    void submit() {
        std::lock_guard<std::mutex> lk(wm);
        if (job || busy) return;
        const size_t have = (size_t)std::min<int64_t>(na, (int64_t)buf.size());
        if (have < (size_t)(3 * kFs)) return;
        jobAudio.resize(have);
        for (size_t i = 0; i < have; i++) jobAudio[i] = buf[(wr + buf.size() - have + i) % buf.size()];
        jobTime = (double)na / kFs;
        job = true;
        wcv.notify_all();
    }

    // ---------------------------------------------------------------- audio
    void process(const float* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            buf[wr] = x[i];
            wr = (wr + 1) % buf.size();
            na++;
            secPow += (double)x[i] * x[i]; secN++;
            if (secN >= 8000) {
                levelHist.push_back((float)(10 * std::log10(secPow / secN + 1e-12)));
                if (levelHist.size() > kPactorHfHist) levelHist.erase(levelHist.begin());
                secPow = 0; secN = 0; changed = true;
            }
        }
        if ((double)na >= nextAnalysis) {
            nextAnalysis = (double)na + analysisEvery * kFs;
            submit();
        }
    }

    void report() {
        std::lock_guard<std::mutex> lk(mu);
        if (!changed && pub.seq > 0) return;
        PactorHfTelemetry& p = pub;
        p.seq++;
        p.wanted = wanted;
        p.timeSec = (double)na / kFs;
        p.inputRate = curRate;
        p.levelDb = levelHist.empty() ? -200.f : levelHist.back();
        p.levelHist = levelHist;
        p.analyses = analyses; p.detections = detections;
        p.events = events;
        if (haveLast && last.found) {
            const auto& L = last;
            p.variant = L.variant; p.baud = L.baud; p.centreHz = L.centreHz; p.spacingHz = L.spacingHz; p.bwHz = L.bwHz;
            p.clockPpm = L.clockPpm; p.lineDb = L.lineDb ? L.lineDb : L.snrDb; p.snrDb = (float)L.snrDb;
            p.nCarriers = L.nCarriers; p.level = L.level; p.constellation = L.constellation; p.note = L.note;
            p.carrierHz.clear(); p.carrierOrder.clear();
            for (const auto& c : L.car) { p.carrierHz.push_back(c.hz); p.carrierOrder.push_back(c.order); }
            p.framePeriod = L.frame.period; p.framePeriodSec = L.frame.periodSec; p.syncPos = L.frame.syncPos; p.syncLen = L.frame.syncLen;
            p.syncHex = L.frame.syncHex; p.frameCopies = L.frame.copies; p.frameMatch = L.frame.match;
            p.dataValid = L.frame.found;
            p.blocksOk = (uint64_t)L.frame.copies;
            p.state = L.frame.found ? 2 : 1;
            p.cfoHz = L.variant == 2 ? L.centreHz - 885.0 : L.variant == 3 ? L.centreHz - 1500.0 : L.variant == 4 ? L.centreHz - 1500.0 : 0;
            p.rows.clear();
            static const char* kDig = "0123456789abcdef";
            for (size_t c = 0; c < L.car.size() && c < 3; c++) {
                std::string row;
                const auto& s = L.car[c].sym;
                const size_t from = s.size() > kPactorHfRowSyms ? s.size() - kPactorHfRowSyms : 0;
                for (size_t i = from; i < s.size(); i++) row.push_back(kDig[s[i] & 15]);
                p.rows.push_back(row);
            }
        } else {
            p.variant = 0; p.state = 0; p.dataValid = false; p.nCarriers = 0; p.framePeriod = 0; p.carrierHz.clear(); p.carrierOrder.clear(); p.rows.clear();
            p.syncHex.clear(); p.note.clear(); p.blocksOk = 0; p.snrDb = 0; p.cfoHz = 0;
        }
        changed = false;
    }
};

PactorHfReceiver::PactorHfReceiver(int variant) : p_(std::make_unique<Impl>(variant == 3 ? 3 : variant == 4 ? 4 : 2)) {}
PactorHfReceiver::~PactorHfReceiver() = default;

void PactorHfReceiver::configure(double r) { std::lock_guard<std::mutex> lk(p_->mu); p_->rate = r; p_->resetReq = true; }
void PactorHfReceiver::setSignalOffset(double hz) { std::lock_guard<std::mutex> lk(p_->mu); p_->offsetHz = hz; p_->offsetReq = true; }
bool PactorHfReceiver::ready() const { std::lock_guard<std::mutex> lk(p_->mu); return p_->rate >= pactorHfTuning(p_->wanted).minSampleRate - 1; }
void PactorHfReceiver::reset() { p_->resetReq = true; }
void PactorHfReceiver::setAnalysisInterval(double sec) { p_->analysisEvery = std::max(0.5, sec); }

void PactorHfReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (m.resetReq.exchange(false)) { m.offsetReq = false; m.resetState(); }
    if (m.offsetReq.exchange(false) && m.running) { std::lock_guard<std::mutex> lk(m.mu); m.front.setOffsetHz(m.offsetHz); }
    if (m.clearReq.exchange(false)) { m.events.clear(); m.haveLast = false; m.lastVariant = -1; m.lastCarriers = -1; m.changed = true; }
    if (m.curRate <= 0) return;
    m.nIn += (int64_t)n;
    if (m.running) {
        m.base.clear();
        m.front.process(x, n, m.base);
        for (cf32& v : m.base) if (!std::isfinite(v.real()) || !std::isfinite(v.imag())) v = cf32(0, 0);
        m.aud.clear();
        double pw = 0;
        m.usb.process(m.base.data(), m.base.size(), m.aud, pw);
        m.process(m.aud.data(), m.aud.size());
    }
    while ((double)m.nIn >= m.nextReport) {
        m.nextReport += 0.25 * m.curRate;
        m.report();
    }
}

bool PactorHfReceiver::telemetry(PactorHfTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->pub.seq <= lastSeq) return false;
    out = p_->pub;
    return true;
}
void PactorHfReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void PactorHfReceiver::clearResults() {
    p_->clearReq = true;
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->pub.events.clear(); p_->pub.seq++;
}
void PactorHfReceiver::flush() {
    Impl& m = *p_;
    if (!m.running) return;
    // wait for a running analysis, then analyse what is buffered now
    for (int i = 0; i < 2; i++) {
        {
            std::unique_lock<std::mutex> lk(m.wm);
            m.wcv.wait(lk, [&] { return !m.job && !m.busy; });
        }
        m.submit();
        std::unique_lock<std::mutex> lk(m.wm);
        m.wcv.wait(lk, [&] { return !m.job && !m.busy; });
    }
    m.changed = true;
    m.report();
}

ModeTuning pactorHfTuning(int variant) {
    ModeTuning t;
    t.stdMode = variant == 3 ? 240 : variant == 4 ? 241 : 239;
    t.id = variant == 3 ? "pactor3" : variant == 4 ? "pactor4" : "pactor2";
    t.name = variant == 3 ? "PACTOR-III" : variant == 4 ? "PACTOR-IV" : "PACTOR-II";
    t.minMhz = 1.8; t.maxMhz = 30; t.defMhz = variant == 2 ? 14.0 + 0.0720 : 14.0 + 0.0720;   // a starting point on 20 m (upper sideband dial)
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = variant == 2 ? 0.004 : 0.004;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

} // namespace dect2
