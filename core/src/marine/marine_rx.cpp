#include "../audio/ham_listen.h"
#include "dect2/automation/engine.h"
#include "dect2/marine/marine_rx.h"
#include "dect2/marine/marine_dsc.h"
#include "dect2/marine/marine_dsp.h"
#include "dect2/marine/marine_bands.h"
#include "dect2/marine/marine_navtex.h"
#include "dect2/results/results_io.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include "dect2/common/timecompat.h"

namespace dect2 {

using namespace marine;

namespace {
bool nearBy(double f, double target, double tol) { return std::fabs(f - target) <= tol; }

struct Enabled { bool navtex = false, dscHf = false, dscVhf = false, fax = false; };

Enabled chooseServices(int service, double f) {
    Enabled e;
    const bool vhf = f > 30e6;
    if (service == 1) { e.navtex = true; return e; }
    if (service == 2) { if (f <= 0) { e.dscHf = e.dscVhf = true; } else if (vhf) e.dscVhf = true; else e.dscHf = true; return e; }
    if (service == 3) { e.fax = true; return e; }
    if (f <= 0) { e.navtex = e.dscHf = e.dscVhf = e.fax = true; return e; }       // auto, frequency unknown: everything
    if (nearBy(f, 490e3, 1500) || nearBy(f, 518e3, 1500) || nearBy(f, 4209.5e3, 1000)) { e.navtex = true; return e; }
    static const double dscHf[] = {2187.5e3, 4207.5e3, 6312e3, 8414.5e3, 12577e3, 16804.5e3};
    for (double d : dscHf) if (nearBy(f, d, 1000)) { e.dscHf = true; return e; }
    if (nearBy(f, 156.525e6, 10e3)) { e.dscVhf = true; return e; }
    e.fax = true;
    return e;
}

int64_t wallNow() { return dect2::unixNow(); }
} // namespace

namespace {
// One more MF/HF DSC channel decoded from the same capture: its own mixer, FSK finder, slicer and decoders
struct ParChannel {
    double hz = 0;
    Front front;
    Spectrum spec;
    FskSlicer fsk;
    DscDecoder dec[2] = {DscDecoder(false, false), DscDecoder(true, false)};
    std::vector<cf32> base;
    Spectrum::Fsk info;
    uint32_t calls = 0;
};
}

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
    std::atomic<bool> faxClearReq{false};     // clearFax(): the receiver thread zeroes its fax bookkeeping
    std::function<void(const std::string&)> log;

    Front front;
    Spectrum spec;
    FskSlicer fsk;
    VhfSlicer vhf;
    FaxAudio faxAudio;
    hfdig::Listen listen;          // "Listen": the fax / NAVTEX / DSC tones on the sound card (off by default)
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
    std::vector<std::unique_ptr<ParChannel>> par;
    std::vector<double> parWant;
    bool parDirty = false;
    MarineTelemetry tel;           // the report in progress (guarded by telMu)
    double lastNow = 0;            // signal time of the last report (telMu): the kept messages move back by it when the clock starts again
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
        if (automation::wants(automation::FMsg)) {   // "Save decoded messages", Integrations, automations
            automation::Event ex;
            ex.str("header", m.header).str("subject", m.subjectName).num("number", m.number).flag("complete", m.complete).num("errors", m.errors).num("chars", m.chars);
            std::string st(1, m.station);
            automation::emitMessage("navtex", "NAVTEX", freqHz.load(), st, m.text, std::nan(""), std::move(ex));
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

    void onDsc(const DscCall& c0, bool vhfPath, double carrierHz = -1) {
        // a call that failed its check and names no sender says nothing: noise that happened to pass for a call (a fading fax signal
        // gave one on the VHF path, "Unknown format from ? to ?", and switched the active service away from the fax)
        if (!c0.eccOk && c0.fromMmsi.empty()) return;
        DscCall c = c0;
        c.vhf = vhfPath;
        c.freqHz = carrierHz >= 0 ? carrierHz : (vhfPath ? 156.525e6 : freqHz.load());
        c.rxSec = secs(); c.rxTime = wallNow();
        if (automation::wants(automation::FDsc)) {   // the automations (automation/engine.h)
            automation::Event ev("dsc.call");
            ev.at(automation::wallNow(), "Marine", c.freqHz);
            std::string sum = c.formatName + (c.categoryName.empty() ? "" : ", " + c.categoryName) + (c.distress ? ", " + c.natureName : "");
            ev.str("category", c.distress ? "distress" : c.categoryName).str("from_mmsi", c.fromMmsi).str("to_mmsi", c.to).str("summary", sum);
            ev.str("kind", c.kind).str("nature", c.nature >= 0 ? c.natureName : "").str("distress_mmsi", c.distressMmsi).str("from_country", c.fromCountry);
            ev.str("text", c.text);
            if (c.hasPos) ev.num("lat", c.lat).num("lon", c.lon);
            automation::emit(std::move(ev));
        }
        std::lock_guard<std::mutex> lk(telMu);
        tel.dsc.push_back(c);
        if (tel.dsc.size() > 100) tel.dsc.erase(tel.dsc.begin());
        tel.dscCount++;
        if (c.eccOk) tel.blocksOk++; else tel.blocksBad++;
        tel.dataValid = true;
        if (c.eccOk) { lastDscSec = c.rxSec; dscVhfSeen = vhfPath; }   // only a checked call says which service is on the air
        say("DSC " + c.text);
    }

    // telMu held: the sample count starts again, the kept messages stay in the past of the new clock
    void shiftTimes(double dt) {
        for (auto& m : tel.navtex) m.rxSec += dt;
        for (auto& c : tel.dsc) c.rxSec += dt;
    }

    void resetAll() {
        { std::lock_guard<std::mutex> lk(telMu); shiftTimes(-lastNow); lastNow = 0; }
        front.reset(); spec.reset(); fsk.reset(); vhf.reset(); faxAudio.reset(); fax.reset(); listen.reset();
        for (auto& n : nav) n.reset();
        for (auto& d : dsc) d.reset();
        for (auto& d : dscv) d.reset();
        base.clear(); audio.clear(); parDirty = true;
        inSamples = 0; sinceReport = 0; fskInfo = Spectrum::Fsk();
        lastNavSec = lastDscSec = lastFaxSec = -1e9;
        faxLinesSeen = 0; prevFaxState = 0; faxDirty = true;
    }

    // the channels of parWant that lie inside the capture, built again after a change of the list, the rate or the dial frequency
    void rebuildPar() {
        par.clear();
        const double dial = freqHz.load();
        if (rate < marineTuning().minSampleRate - 1 || dial <= 0 || dial > 30e6) return;
        // the capture is centred where the dial would sit without the offset the radio uses: offsetHz is the dial's place in the input
        const std::vector<double> in = dscInCapture(parWant, dial, 2.0 * (rate * 0.5 - std::fabs(offsetHz)));
        for (double f : in) {
            if (std::fabs(f - dial) < 500.0) continue;       // the main channel
            auto c = std::make_unique<ParChannel>();
            c->hz = f;
            c->front.configure(rate);
            c->front.setOffsetHz(offsetHz + (f - dial));
            ParChannel* raw = c.get();
            for (auto& d : c->dec) d.setCallback([this, raw](const DscCall& call) { raw->calls++; onDsc(call, false, raw->hz); });
            par.push_back(std::move(c));
        }
        parDirty = false;
    }

    void applyFaxSettings() {
        if (faxClearReq.exchange(false)) { faxLinesSeen = 0; prevFaxState = 0; }
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
    lastNow = now;
    tel.seq++;
    tel.serviceSetting = service.load();
    const bool navLock = nav[0].locked() || nav[1].locked();
    const bool dscLock = dsc[0].locked() || dsc[1].locked();
    const bool dscvLock = dscv[0].locked() || dscv[1].locked();
    tel.navtexLocked = navLock; tel.dscLocked = dscLock || dscvLock;
    tel.parallel.clear();
    for (auto& pc : par) {
        MarineTelemetry::ParInfo pi; pi.hz = pc->hz; pi.fsk = pc->info.found; pi.locked = pc->dec[0].locked() || pc->dec[1].locked(); pi.calls = pc->calls;
        tel.parallel.push_back(pi);
        if (pi.locked) tel.dscLocked = true;
    }
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
    // the decoder's change counter: new rows, and also a slant change or a roll of the rows already received (the line count was
    // used before, so a picture straightened by the slider, or after it had stopped, never reached the viewer)
    tel.fax.imageSeq = fs.imageSeq;
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
    p_->parDirty = true;
    p_->resetAll();
}

void MarineReceiver::setSignalOffset(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->offsetHz = hz;
    p_->front.setOffsetHz(hz);
    p_->parDirty = true;
}

bool MarineReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    return p_->rate >= marineTuning().minSampleRate - 1;
}

void MarineReceiver::reset() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->resetAll();
    std::lock_guard<std::mutex> lt(p_->telMu);
    MarineTelemetry t;                   // the report number never restarts; the messages stay (clearResults)
    t.seq = p_->tel.seq + 1; t.serviceSetting = p_->tel.serviceSetting;
    t.navtex = std::move(p_->tel.navtex); t.dsc = std::move(p_->tel.dsc);
    t.navtexCount = p_->tel.navtexCount; t.dscCount = p_->tel.dscCount;
    p_->tel = std::move(t);
}

void MarineReceiver::feed(const cf32* x, size_t n) {
    Impl& s = *p_;
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.rate < marineTuning().minSampleRate - 1) return;
    s.applyFaxSettings();
    if (s.parDirty) s.rebuildPar();
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
        for (auto& pc : s.par) {
            pc->base.clear();
            pc->front.process(x, n, pc->base);
            if (pc->base.empty()) continue;
            if (pc->spec.push(pc->base.data(), pc->base.size())) {
                pc->info = pc->spec.findFsk(170.0, 600.0);
                if (pc->info.found) pc->fsk.setCentreHz(pc->info.centreHz);
            }
            pc->fsk.process(pc->base.data(), pc->base.size(), [&](float v) { pc->dec[0].pushBit(-v); pc->dec[1].pushBit(-v); });
        }
        if (en.dscVhf) {
            s.vhf.process(b, nb, [&](float v) { s.dscv[0].pushBit(v); s.dscv[1].pushBit(v); });   // 1300 Hz = Y = 1
        }
        if (en.fax) {
            s.audio.clear();
            s.faxAudio.process(b, nb, s.audio);
            if (!s.audio.empty()) s.fax.push(s.audio.data(), s.audio.size());
        }
        if (s.listen.on.load()) s.listen.playBase(b, nb);   // fax tones, or the NAVTEX / DSC FSK tones
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
void MarineReceiver::setFrequencyHz(double hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (p_->freqHz.load() != hz) p_->parDirty = true;
    p_->freqHz = hz;
}
void MarineReceiver::setParallelDsc(const std::vector<double>& hz) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (hz == p_->parWant) return;
    p_->parWant = hz;
    p_->parDirty = true;
}
void MarineReceiver::setFaxLpm(int v) { p_->faxLpm = v; p_->faxDirty = true; }
void MarineReceiver::setFaxIoc(int v) { p_->faxIoc = v; p_->faxDirty = true; }
void MarineReceiver::setFaxSlantPpm(double v) { p_->faxSlant = v; p_->faxDirty = true; }
void MarineReceiver::setFaxAutoSlant(bool on) { p_->faxAuto = on; p_->faxDirty = true; }
void MarineReceiver::clearMessages() {
    std::lock_guard<std::mutex> lk(p_->telMu);
    p_->tel.navtex.clear(); p_->tel.dsc.clear();
    p_->tel.seq++;
}
void MarineReceiver::clearFax() {
    p_->fax.reset();                 // takes the decoder's own lock, so it is safe while the receiver thread pushes audio
    p_->faxClearReq = true;
}
void MarineReceiver::clearResults() { clearMessages(); clearFax(); }
std::string MarineReceiver::saveResults() const {
    std::lock_guard<std::mutex> lk(p_->telMu);
    return results::pack(p_->lastNow, p_->tel.navtex, p_->tel.dsc, p_->tel.navtexCount, p_->tel.dscCount);
}
bool MarineReceiver::loadResults(const std::string& blob) {
    double t0 = 0;
    std::vector<NavtexMessage> nav;
    std::vector<DscCall> dsc;
    uint64_t nn = 0, nd = 0;
    if (!results::unpack(blob, t0, nav, dsc, nn, nd)) return false;
    std::lock_guard<std::mutex> lk(p_->telMu);
    p_->tel.navtex = std::move(nav); p_->tel.dsc = std::move(dsc);
    p_->tel.navtexCount = nn; p_->tel.dscCount = nd;
    p_->shiftTimes(p_->lastNow - t0);
    p_->tel.seq++;
    return true;
}
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

void MarineReceiver::setListen(bool on) { p_->listen.on = on; }
void MarineReceiver::setVolume(float v) { p_->listen.volume = std::max(0.f, std::min(1.f, v)); }
void MarineReceiver::setMuted(bool m) { p_->listen.muted = m; }
void MarineReceiver::setSilent(bool s) { p_->listen.silent = s; }

} // namespace dect2
