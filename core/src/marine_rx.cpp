#include "dect2/marine_rx.h"
#include "dect2/marine_dsc.h"
#include "dect2/marine_dsp.h"
#include "dect2/marine_navtex.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>

namespace dect2 {

using namespace marine;

namespace {
bool near(double f, double target, double tol) { return std::fabs(f - target) <= tol; }

struct Enabled { bool navtex = false, dscHf = false, dscVhf = false, fax = false; };

Enabled chooseServices(int service, double f) {
    Enabled e;
    const bool vhf = f > 30e6;
    if (service == 1) { e.navtex = true; return e; }
    if (service == 2) { if (f <= 0) { e.dscHf = e.dscVhf = true; } else if (vhf) e.dscVhf = true; else e.dscHf = true; return e; }
    if (service == 3) { e.fax = true; return e; }
    if (f <= 0) { e.navtex = e.dscHf = e.dscVhf = e.fax = true; return e; }       // auto, frequency unknown: everything
    if (near(f, 490e3, 1500) || near(f, 518e3, 1500) || near(f, 4209.5e3, 1000)) { e.navtex = true; return e; }
    static const double dscHf[] = {2187.5e3, 4207.5e3, 6312e3, 8414.5e3, 12577e3, 16804.5e3};
    for (double d : dscHf) if (near(f, d, 1000)) { e.dscHf = true; return e; }
    if (near(f, 156.525e6, 10e3)) { e.dscVhf = true; return e; }
    e.fax = true;
    return e;
}

int64_t wallNow() { return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
} // namespace

struct MarineReceiver::Impl {
    std::mutex mu;                 // the processing chain (feed, configure, reset)
    mutable std::mutex telMu;      // the report
    double rate = 0, offsetHz = 0;
    std::atomic<int> service{0};
    std::atomic<double> freqHz{0};
    std::atomic<int> faxLpm{0}, faxIoc{0};     // 0 = detect from the start tone and the phasing
    std::atomic<double> faxSlant{0};
    std::atomic<bool> faxAuto{true};
    std::atomic<bool> faxDirty{true};
    std::atomic<bool> clearReq{false};
    std::function<void(const std::string&)> log;

    Front front;
    Spectrum spec;
    FskSlicer fsk;
    VhfSlicer vhf;
    FaxAudio faxAudio;
    FaxDecoder fax;
    NavtexDecoder nav[2] = {NavtexDecoder(false), NavtexDecoder(true)};
    DscDecoder dsc[2] = {DscDecoder(false, false), DscDecoder(true, false)};
    DscDecoder dscv[2] = {DscDecoder(false, true), DscDecoder(true, true)};
    std::vector<cf32> base;
    std::vector<float> audio;

    uint64_t inSamples = 0;
    double sinceReport = 0;
    Spectrum::Fsk fskInfo;
    double lastNavSec = -1e9, lastDscSec = -1e9, lastFaxSec = -1e9;
    uint64_t faxLinesSeen = 0, faxDone = 0;
    int prevFaxState = 0;
    bool dscVhfSeen = false;
    MarineTelemetry tel;           // the report in progress (guarded by telMu)
    double fskNoise = 0;

    Impl() {
        nav[0].setCallback([this](const NavtexMessage& m) { onNavtex(m); });
        nav[1].setCallback([this](const NavtexMessage& m) { onNavtex(m); });
        for (auto& d : dsc) d.setCallback([this](const DscCall& c) { onDsc(c, false); });
        for (auto& d : dscv) d.setCallback([this](const DscCall& c) { onDsc(c, true); });
    }

    double secs() const { return rate > 0 ? (double)inSamples / rate : 0.0; }
    void say(const std::string& s) { if (log) log(s); }

    void trimText() {
        // keep the full text of the newest messages; older ones shrink to a preview so that the report stays small
        size_t budget = 60000;
        for (size_t i = tel.navtex.size(); i-- > 0;) {
            auto& m = tel.navtex[i];
            if (m.text.size() <= budget) { budget -= m.text.size(); continue; }
            const size_t keep = std::min<size_t>(m.text.size(), 160);
            m.text.resize(keep); m.textCut = true;
            budget = budget > keep ? budget - keep : 0;
        }
    }

    void onNavtex(const NavtexMessage& m0) {
        NavtexMessage m = m0;
        m.rxSec = secs(); m.rxTime = wallNow();
        std::lock_guard<std::mutex> lk(telMu);
        for (auto& e : tel.navtex) {
            if (e.header == m.header && e.text == m.text && !e.textCut && e.complete == m.complete) { e.repeats++; e.rxSec = m.rxSec; e.rxTime = m.rxTime; return; }
        }
        tel.navtex.push_back(m);
        if (tel.navtex.size() > 100) tel.navtex.erase(tel.navtex.begin());
        trimText();
        tel.navtexCount++;
        if (m.complete && m.errors == 0) tel.blocksOk++; else tel.blocksBad++;
        tel.dataValid = true;
        lastNavSec = m.rxSec;
        char b[160];
        snprintf(b, sizeof b, "NAVTEX %s%s: %s%s, %u characters, %u unreadable", m.header.c_str(), m.complete ? "" : " (cut short)", m.subjectName.c_str(), "", m.chars, m.errors);
        say(b);
    }

    void onDsc(const DscCall& c0, bool vhfPath) {
        // a call that failed its check and names no sender says nothing: noise that happened to pass for a call (a fading fax signal
        // gave one on the VHF path, "Unknown format from ? to ?", and switched the active service away from the fax)
        if (!c0.eccOk && c0.fromMmsi.empty()) return;
        DscCall c = c0;
        c.vhf = vhfPath;
        c.rxSec = secs(); c.rxTime = wallNow();
        std::lock_guard<std::mutex> lk(telMu);
        tel.dsc.push_back(c);
        if (tel.dsc.size() > 100) tel.dsc.erase(tel.dsc.begin());
        tel.dscCount++;
        if (c.eccOk) tel.blocksOk++; else tel.blocksBad++;
        tel.dataValid = true;
        if (c.eccOk) { lastDscSec = c.rxSec; dscVhfSeen = vhfPath; }   // only a checked call says which service is on the air
        say("DSC " + c.text);
    }

    void resetAll() {
        front.reset(); spec.reset(); fsk.reset(); vhf.reset(); faxAudio.reset(); fax.reset();
        for (auto& n : nav) n.reset();
        for (auto& d : dsc) d.reset();
        for (auto& d : dscv) d.reset();
        base.clear(); audio.clear();
        inSamples = 0; sinceReport = 0; fskInfo = Spectrum::Fsk();
        lastNavSec = lastDscSec = lastFaxSec = -1e9;
        faxLinesSeen = 0; prevFaxState = 0; faxDirty = true;
    }

    void applyFaxSettings() {
        if (!faxDirty.exchange(false)) return;
        fax.setLpm(faxLpm.load());
        fax.setIoc(faxIoc.load());
        fax.setSlantPpm(faxSlant.load());
        fax.setAutoSlant(faxAuto.load());
    }

    void publish();
};

void MarineReceiver::Impl::publish() {
    const double now = secs();
    const FaxStatus fs = fax.status();
    const Enabled en = chooseServices(service.load(), freqHz.load());
    std::lock_guard<std::mutex> lk(telMu);
    if (clearReq.exchange(false)) { tel.navtex.clear(); tel.dsc.clear(); }
    tel.seq++;
    tel.serviceSetting = service.load();
    const bool navLock = nav[0].locked() || nav[1].locked();
    const bool dscLock = dsc[0].locked() || dsc[1].locked();
    const bool dscvLock = dscv[0].locked() || dscv[1].locked();
    tel.navtexLocked = navLock; tel.dscLocked = dscLock || dscvLock;
    if (fs.state == 2 && fs.lines > faxLinesSeen) { lastFaxSec = now; }
    if (fs.state >= 1 && fs.state <= 2) lastFaxSec = now;
    if (fs.state == 3 && prevFaxState != 3 && fs.lines > 0) { faxDone++; tel.blocksOk++; }
    prevFaxState = fs.state;
    faxLinesSeen = (uint64_t)std::max(0, fs.lines);
    if (fs.lines > 0) tel.dataValid = true;
    // which service is the one heard
    int active = 0;
    if (navLock) active = 1; else if (dscLock) active = 2; else if (dscvLock) active = 4;
    else if (fs.state == 1 || fs.state == 2) active = 3;
    else {
        double best = 120; // seconds
        if (now - lastNavSec < best) { best = now - lastNavSec; active = 1; }
        if (now - lastDscSec < best) { best = now - lastDscSec; active = dscVhfSeen ? 4 : 2; }
        if (now - lastFaxSec < best) { best = now - lastFaxSec; active = 3; }
        if (active == 0 && fskInfo.found && (en.navtex || en.dscHf)) active = en.navtex ? 1 : 2;
    }
    tel.serviceActive = active;
    tel.dscVhfActive = active == 4;
    const bool fskOk = fskInfo.found && (en.navtex || en.dscHf);
    tel.state = (navLock || dscLock || dscvLock || fs.state == 1 || fs.state == 2) ? 2 : (fskOk || fs.state == 3 || (active != 0)) ? 1 : 0;
    const bool faxOn = active == 3;
    tel.cfoHz = fskOk ? fskInfo.centreHz : faxOn && fs.state >= 1 ? fs.blackHz - 1500.0 : 0.0;     // fax: the black level against its nominal place
    tel.snrDb = (float)(active == 4 ? 0.0 : fskOk ? fskInfo.snrDb : fs.snrDb);                 // not measured on VHF
    tel.toneHighHz = fskOk ? fskInfo.highHz : 0; tel.toneLowHz = fskOk ? fskInfo.lowHz : 0;
    tel.shiftHz = fskOk ? fskInfo.highHz - fskInfo.lowHz : 0;
    tel.baudEst = fskOk ? fsk.baud() : 0;
    tel.fskLevelDb = fskInfo.levelDb;
    spec.audioDb(tel.spectrumDb, tel.specLoHz, tel.specHiHz, 342);
    tel.fax.state = fs.state; tel.fax.ioc = fs.ioc; tel.fax.lpm = fs.lpm; tel.fax.lines = fs.lines;
    tel.fax.slantPpm = fs.slantPpm; tel.fax.toneHz = fs.toneHz; tel.fax.snrDb = fs.snrDb;
    tel.fax.blackHz = fs.blackHz; tel.fax.phasingLines = fs.phasingLines;
    tel.fax.width = (int)std::floor(fs.ioc * 3.14159265358979 + 1e-9);
    tel.fax.thumbRows = (fs.lines * 256 + tel.fax.width / 2) / std::max(1, tel.fax.width);
    // a cheap stand-in for the picture's change counter; the viewer asks latestImage() with its own sequence number
    if (fs.state == 2 || fs.state == 3) tel.fax.imageSeq = (uint64_t)fs.lines + (uint64_t)faxDone * 100000ull;
}

MarineReceiver::MarineReceiver() : p_(std::make_unique<Impl>()) {}
MarineReceiver::~MarineReceiver() = default;

void MarineReceiver::configure(double inputRateHz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->rate = inputRateHz;
    if (inputRateHz >= marineTuning().minSampleRate - 1) {
        p_->front.configure(inputRateHz);
        p_->front.setOffsetHz(p_->offsetHz);
    }
    p_->fax.configure(12000.0);
    p_->resetAll();
}

void MarineReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    p_->front.setOffsetHz(hz);
}

bool MarineReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= marineTuning().minSampleRate - 1;
}

void MarineReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetAll();
    std::lock_guard<std::mutex> lt(p_->telMu);
    const uint64_t s = p_->tel.seq;      // the report number never restarts
    const int svc = p_->tel.serviceSetting;
    p_->tel = MarineTelemetry();
    p_->tel.seq = s; p_->tel.serviceSetting = svc;
}

void MarineReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.rate < marineTuning().minSampleRate - 1) return;
    s.applyFaxSettings();
    const Enabled en = chooseServices(s.service.load(), s.freqHz.load());
    s.base.clear();
    s.front.process(x, n, s.base);
    s.inSamples += n;
    const cf32* b = s.base.data();
    const size_t nb = s.base.size();
    if (nb) {
        if (s.spec.push(b, nb)) {
            s.fskInfo = s.spec.findFsk(170.0, 600.0);
            if (s.fskInfo.found) s.fsk.setCentreHz(s.fskInfo.centreHz);
        }
        if (en.navtex || en.dscHf) {
            s.fsk.process(b, nb, [&](float v) {
                if (en.navtex) { s.nav[0].pushBit(v); s.nav[1].pushBit(v); }          // B = 1 = the higher tone
                if (en.dscHf) { s.dsc[0].pushBit(-v); s.dsc[1].pushBit(-v); }         // M.493: the higher tone is B = 0
            });
        }
        if (en.dscVhf) {
            s.vhf.process(b, nb, [&](float v) { s.dscv[0].pushBit(v); s.dscv[1].pushBit(v); });   // 1300 Hz = Y = 1
        }
        if (en.fax) {
            s.audio.clear();
            s.faxAudio.process(b, nb, s.audio);
            if (!s.audio.empty()) s.fax.push(s.audio.data(), s.audio.size());
        }
    }
    s.sinceReport += (double)n;
    const double per = 0.25 * s.rate;
    while (s.sinceReport >= per) {
        s.sinceReport -= per;
        s.publish();
    }
}

bool MarineReceiver::telemetry(MarineTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->telMu);
    if (p_->tel.seq <= lastSeq) return false;
    out = p_->tel;
    return true;
}

void MarineReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->mu); p_->log = std::move(cb); }
void MarineReceiver::setService(int s) { p_->service = s < 0 || s > 3 ? 0 : s; }
void MarineReceiver::setFrequencyHz(double hz) { p_->freqHz = hz; }
void MarineReceiver::setFaxLpm(int v) { p_->faxLpm = v; p_->faxDirty = true; }
void MarineReceiver::setFaxIoc(int v) { p_->faxIoc = v; p_->faxDirty = true; }
void MarineReceiver::setFaxSlantPpm(double v) { p_->faxSlant = v; p_->faxDirty = true; }
void MarineReceiver::setFaxAutoSlant(bool on) { p_->faxAuto = on; p_->faxDirty = true; }
void MarineReceiver::clearMessages() { p_->clearReq = true; }
bool MarineReceiver::latestImage(FaxImage& out, uint64_t& seq) const { return p_->fax.latestImage(out, seq); }
FaxStatus MarineReceiver::faxStatus() const { return p_->fax.status(); }

ModeTuning marineTuning() {
    ModeTuning t;
    t.stdMode = 17; t.id = "marine"; t.name = "Marine";
    t.minMhz = 0.1; t.maxMhz = 174; t.defMhz = 0.518;
    t.sampleRate = 2000000;
    t.basebandHz = 1750000;
    t.bandwidthMhz = 0.01;
    t.minSampleRate = 250000;
    t.tuneOffsetHz = 20000;
    return t;
}

} // namespace dect2
