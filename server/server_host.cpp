#include "dect2/common/thread_name.h"
#include "server_host.h"
#include "web_ui.h"
#include "web/web_hub.h"
#include "../app/state/rx_width.h"
#include "dect2/engine/widespan.h"
#include "dect2/audio/audio_monitor.h"
#include "dect2/audio/hf_demod.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/net/netradio.h"
#include "dect2/results/rx_json.h"
#include "dect2/results/web_schema.h"
#include "dect2/results/web_options.h"
#include "dect2/source/agc.h"
#include "dect2/source/gain.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <cstring>

using namespace dect2;

namespace onair_server {

static double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static const char* kindName(DeviceInfo::Kind k) {
    switch (k) {
    case DeviceInfo::Synthetic: return "synthetic";
    case DeviceInfo::File: return "file";
    case DeviceInfo::HackRF: return "hackrf";
    case DeviceInfo::Soapy: return "soapy";
    case DeviceInfo::Native: return "native";
    case DeviceInfo::Audio: return "audio";
    case DeviceInfo::Hamlib: return "hamlib";
    }
    return "unknown";
}
static const char* demodName(int d) {
    switch (d) { case 0: return "usb"; case 1: return "lsb"; case 2: return "nfm"; case 3: return "am"; default: return "auto"; }
}
static int demodFromName(const std::string& s) {
    return s == "usb" ? 0 : s == "lsb" ? 1 : s == "nfm" ? 2 : s == "am" ? 3 : s == "auto" ? 4 : -1;
}

ServerHost::ServerHost(ConfigFile& cfg) : cfg_(cfg) {
    // No sound from the machine itself: the sound goes to the browser. Everything the command line (dect2cli) silences is silenced here too.
    e_.setComputeMode(2);
    e_.fm().setSilent(true); e_.vor().setSilent(true); e_.m17().setSilent(true); e_.cw().setSilent(true);
    e_.psk().setListen(false); e_.js8().setListen(false); e_.ale().setListen(false); e_.hfdig().setListen(false);
    e_.specx().setSilent(true); e_.uplink().setSilent(true); e_.specan().setSilent(true); e_.astro().setSilent(true);
    e_.iono().setSilent(true); e_.qo100().setSilent(true); e_.general().setSilent(true); e_.hfbeacon().setSilent(true);
    e_.same().setSilent(true); e_.dstar().setSilent(true); e_.nxdn().setSilent(true); e_.dpmr().setSilent(true);
    e_.p25().setSilent(true); e_.mac().setSilent(true); e_.dmr().setSilent(true);
    // Digital voice: the receivers' decoded speech (the same sound the desktop plays) goes to the browser while such a mode is chosen.
    // Only clear calls reach these taps: encrypted voice is decoded only with keys the user imported, as in the desktop app.
    auto v8 = [this](const float* x, size_t n) { if (voiceTap_.load(std::memory_order_relaxed)) voiceIn(x, n, 8000); };
    e_.dmr().setAudioTap([this](const float* l, const float* r, size_t n) { if (voiceTap_.load(std::memory_order_relaxed)) audioIn(l, r, n, false); });
    e_.p25().setAudioTap(v8); e_.p25p2().setAudioTap(v8); e_.nxdn().setAudioTap(v8); e_.dpmr().setAudioTap(v8);
    e_.dstar().setAudioTap(v8); e_.m17().setAudioTap(v8);
    e_.fusion().setAudioTap([this](const float* x, size_t n, int rate) { if (voiceTap_.load(std::memory_order_relaxed)) voiceIn(x, n, rate); });
    e_.fm().setAudioTap([this](const float* l, const float* r, size_t n) { if (fmTap_.load(std::memory_order_relaxed)) audioIn(l, r, n, false); });
    e_.monitor().setSink([this](const float* s, int frames) { audioIn(s, nullptr, (size_t)frames, true); });
    buildModes();
}

ServerHost::~ServerHost() {
    quit_ = true;
    if (snapTh_.joinable()) snapTh_.join();
    e_.stop();
}

void ServerHost::buildModes() {
    modes_.clear();
    struct B { int f; const char* id; const char* name; double mhz; };
    static const B b[] = {{0, "dvb", "DVB-T / T2 (automatic)", 522}, {1, "t2", "DVB-T2", 522}, {2, "t", "DVB-T", 522}, {3, "atsc", "ATSC", 575}, {4, "dab", "DAB / DAB+", 225.648},
                          {5, "atsc3", "ATSC 3.0", 575}, {6, "isdbt", "ISDB-T", 527}, {7, "fm", "FM broadcast", 100}};
    for (const B& x : b) modes_.push_back({x.f, x.id, x.name, "", x.mhz});
    for (int s = 8; s < 1000; s++) {   // every ModeTuning row (the families of the mode table + 2; 3.9 blocks reach 899)
        const ModeTuning* mt = modeTuning(s);
        if (mt && *mt->id) modes_.push_back({s, mt->id, mt->name, "", mt->defMhz});
    }
    int n = 0;
    const SubMode* sm = subModes(n);
    for (int i = 0; i < n; i++) modes_.push_back({sm[i].stdMode, sm[i].id, sm[i].name, sm[i].id, sm[i].defMhz});
}

const ModeEntry* ServerHost::findMode(const std::string& idOrNumber) const {
    for (const ModeEntry& m : modes_) if (m.id == idOrNumber) return &m;
    char* e = nullptr;
    const long v = strtol(idOrNumber.c_str(), &e, 10);
    if (e && *e == 0 && e != idOrNumber.c_str()) for (const ModeEntry& m : modes_) if (m.family == v && m.sub.empty()) return &m;
    return nullptr;
}

// ---------------------------------------------------------------- devices
void ServerHost::refreshDevices(bool force) {
    if (!force && nowSec() - devicesAt_ < 5) return;
    if (e_.running()) return;   // a radio in use cannot be listed again
    std::vector<NetRadio> nr;
    for (const std::string& r : cfg_.getAll("net_radio")) for (const NetRadio& x : netRadiosFromString(r)) nr.push_back(x);
    setNetworkRadios(nr);
    const std::string keep = devIdx_ >= 0 && devIdx_ < (int)devices_.size() ? devices_[(size_t)devIdx_].name : std::string();
    std::string err;
    devices_ = listRadios(err);
    DeviceInfo syn;
    syn.kind = DeviceInfo::Synthetic; syn.name = "Test signal (no radio)";
    devices_.push_back(syn);
    const std::string f = cfg_.get("file");
    if (!f.empty()) { DeviceInfo d; d.kind = DeviceInfo::File; d.name = "File: " + f; d.serial = f; devices_.push_back(d); }
    devicesAt_ = nowSec();
    if (!keep.empty()) for (size_t i = 0; i < devices_.size(); i++) if (devices_[i].name == keep) { devIdx_ = (int)i; return; }
    devIdx_ = std::min(devIdx_, (int)devices_.size() - 1);
}

void ServerHost::pickDevice(const std::string& want) {
    if (want.empty()) {   // the first real radio, else the test signal
        devIdx_ = 0;
        for (size_t i = 0; i < devices_.size(); i++) if (devices_[i].isRadio()) { devIdx_ = (int)i; return; }
        for (size_t i = 0; i < devices_.size(); i++) if (devices_[i].kind == DeviceInfo::Synthetic) devIdx_ = (int)i;
        return;
    }
    char* e = nullptr;
    const long v = strtol(want.c_str(), &e, 10);
    if (e && *e == 0 && e != want.c_str() && v >= 0 && v < (long)devices_.size()) { devIdx_ = (int)v; return; }
    for (size_t i = 0; i < devices_.size(); i++) if (devices_[i].name == want) { devIdx_ = (int)i; return; }
    std::string lw = want;
    for (char& c : lw) c = (char)tolower((unsigned char)c);
    for (size_t i = 0; i < devices_.size(); i++) {
        std::string n = devices_[i].name;
        for (char& c : n) c = (char)tolower((unsigned char)c);
        if (n.find(lw) != std::string::npos) { devIdx_ = (int)i; return; }
    }
    pickDevice("");
}

void ServerHost::init() {
    std::lock_guard<std::mutex> lk(mu_);
    refreshDevices(true);
    pickDevice(cfg_.get("device"));
    const ModeEntry* m = findMode(cfg_.get("mode", "fm"));
    if (!m) m = findMode("fm");
    modeIdx_ = (int)(m - modes_.data());
    freqHz_ = cfg_.num("freq_mhz", m->defMhz) * 1e6;
    rateHz_ = cfg_.num("rate_msps", 0) * 1e6;
    captureBwMhz_ = cfg_.num("capture_bw_mhz", 0);
    gainDb_ = cfg_.num("gain_db", 30);
    const int dm = demodFromName(cfg_.get("audio_demod", "auto"));
    demod_ = dm < 0 ? 4 : dm;
    wantRun_ = cfg_.flag("autostart", false);
    snapTh_ = dect2::namedThread("onair-snap", [this] { snapLoop(); });
}

// ---------------------------------------------------------------- tuning and start
GainSetting hackrfGain(const DeviceInfo& d, double db, bool amp) {
    const GainModel m = gainModelFor(d);
    const int tot = (int)std::lround(std::max((double)m.minTotal(amp), std::min((double)m.maxTotal(amp, amp, 40), db)));
    return m.split(tot, amp, 40);
}

TuneSettings ServerHost::buildTune(const DeviceInfo& d) const {
    const ModeEntry& m = modes_[(size_t)modeIdx_];
    TuneSettings t;
    const ModeTuning* mt = m.family >= 8 ? modeTuning(m.family) : nullptr;
    if (mt) { t.bandwidthMhz = mt->bandwidthMhz; t.sampleRate = mt->sampleRate; t.basebandFilterHz = mt->basebandHz; }
    else if (m.family == 7) { t.bandwidthMhz = 0.25; t.sampleRate = 4e6; t.basebandFilterHz = 2.5e6; }
    else {
        t.bandwidthMhz = (m.family == 3 || m.family == 5 || m.family == 6) ? 6 : 8;   // the channel width of the mode; DVB-T2 and DVB-T are 8 MHz unless the user's settings say otherwise
        t.sampleRate = t.bandwidthMhz >= 7 ? 10e6 : 8e6;   // HackRF Pro: exact tuning only at <= 10 Msps (dect2cli does the same)
    }
    t.centerHz = freqHz_;
    t.synth.mode = m.family >= 4 ? m.family : 0;
    if (!m.sub.empty()) if (const SubMode* sm = subModeById(m.sub)) applySubModeSynth(t.synth, *sm);
    dect2::setWideSpanCapHz(captureBwMhz_ > 0 ? 1e12 : 0);   // a custom bandwidth lifts the default cap on whole-span decoding
    if (d.isRadio() || d.kind == DeviceInfo::Synthetic) rxApplyWidth(t, captureBwMhz_, mt ? mt->minSampleRate : 0);   // the custom capture bandwidth (same call as the app)
    if (d.isRadio() && rateHz_ > 0) t.sampleRate = rateHz_;
    else if (d.isRadio() && mt && mt->stdMode == 22 && captureBwMhz_ <= 0) {   // Mesh on an RTL-SDR: 1.024 Msps for the 250 kHz channel, as in the window (#386)
        if (const double r = meshRtlRateHz(d.board, 250e3); r > 0) { t.sampleRate = r; t.basebandFilterHz = 0.45 * r; }
    }
    if (d.isGeneric()) {
        t.sampleRate = linkRateFor(d, t, t.sampleRate, minSampleRateFor(m.family, t.bandwidthMhz));
        t.gainDb = std::max(d.gainMinDb, std::min(d.gainMaxDb > d.gainMinDb ? d.gainMaxDb : 200.0, gainDb_));
    } else if (d.kind == DeviceInfo::HackRF) {
        const GainSetting g = hackrfGain(d, gainDb_, cfg_.flag("amp", false));
        t.lnaDb = g.lna; t.vgaDb = g.vga; t.ampOn = g.amp;
    }
    return t;
}

bool ServerHost::startLocked(std::string& err) {
    if (e_.running()) { err = "already running"; return false; }
    refreshDevices(false);
    if (devIdx_ < 0 || devIdx_ >= (int)devices_.size()) { err = "no radio"; return false; }
    const DeviceInfo& d = devices_[(size_t)devIdx_];
    const ModeEntry& m = modes_[(size_t)modeIdx_];
    FileOptions file;
    if (d.kind == DeviceInfo::File) {
        file.path = d.serial; file.format = guessFormat(file.path);
        IqFileInfo info; std::string perr;
        if (probeIqFile(file.path, info, perr)) {
            file.path = info.dataPath; file.offsetBytes = info.offsetBytes; file.endBytes = info.endBytes; file.bigEndian = info.bigEndian;
            file.format = info.format;
            if (info.sampleRate > 0) file.sampleRate = info.sampleRate;
        }
        if (cfg_.has("file_rate")) file.sampleRate = cfg_.num("file_rate", 10) * 1e6;
        file.loop = true;
    }
    tune_ = buildTune(d);
    e_.setStandard(m.family);
    applySubMode(e_, m.sub.empty() ? nullptr : subModeById(m.sub));
    e_.setBandwidthAuto(false);
    e_.setSpectrumEnabled(true);
    fmTap_ = m.family == 7;
    {
        const ModeTuning* mt = modeTuning(m.family);
        const std::string id = mt && mt->id ? mt->id : "";
        voiceTap_ = id == "dmr" || id == "p25" || id == "p25p2" || id == "nxdn" || id == "dpmr" || id == "dstar" || id == "m17" || id == "fusion";
    }
    applyOptionsLocked();
    quality_.reset();
    if (!e_.start(d, tune_, file)) {
        size_t n = 0;
        const auto log = e_.logSnapshot(n);
        err = "could not start the receiver" + (log.empty() ? std::string() : ": " + log.back());
        return false;
    }
    applyOptionsLocked();   // again: start() may have set the receiver up afresh
    applyMonitor();
    return true;
}

void ServerHost::stopLocked() {
    if (e_.running()) e_.stop();
}

bool ServerHost::retuneLocked(std::string& err) {
    if (!e_.running()) return true;
    tune_.centerHz = freqHz_;
    if (!e_.retune(tune_)) { err = "the radio did not accept that"; return false; }
    return true;
}

int ServerHost::effectiveDemod() const {
    if (demod_ != 4) return demod_;
    const double f = freqHz_;
    return f < 1.8e6 ? HfDemodAm : f < 10e6 ? HfDemodLsb : f < 30e6 ? HfDemodUsb : HfDemodNfm;
}

void ServerHost::applyMonitor() {
    bool any;
    { std::lock_guard<std::mutex> lk(aMu_); any = !listeners_.empty(); }
    MonitorConfig c;
    c.on = any && modes_[(size_t)modeIdx_].family != 7 && !voiceTap_.load();   // FM broadcast and the digital voice modes have their own audio (the taps); every other mode is heard through the monitor
    c.demod = effectiveDemod();
    c.yield = false;
    c.net = false;
    e_.monitor().configure(c);
}

void ServerHost::saveState() {
    const ModeEntry& m = modes_[(size_t)modeIdx_];
    cfg_.set("mode", m.sub.empty() ? m.id : m.sub);
    cfg_.setNum("freq_mhz", freqHz_ / 1e6);
    cfg_.setNum("rate_msps", rateHz_ / 1e6);
    cfg_.setNum("capture_bw_mhz", captureBwMhz_);
    cfg_.setNum("gain_db", gainDb_);
    cfg_.set("audio_demod", demodName(demod_));
    if (devIdx_ >= 0 && devIdx_ < (int)devices_.size() && devices_[(size_t)devIdx_].kind != DeviceInfo::Synthetic) cfg_.set("device", devices_[(size_t)devIdx_].name);
    else if (devIdx_ >= 0 && devIdx_ < (int)devices_.size()) cfg_.set("device", "test");
    cfg_.set("autostart", wantRun_ ? "1" : "0");   // what was running stays running after a restart of the service
    dirty_ = true;
}

void ServerHost::shutdown() { std::lock_guard<std::mutex> lk(mu_); stopLocked(); }

void ServerHost::setWebHub(onair_web::WebHub* hub) {
    if (!hub) { e_.setIqTap(nullptr); return; }
    // the raw samples at the radio's rate, centred on the radio's own frequency (the receivers' offset tuning is not applied to them)
    e_.setIqTap([this, hub](const std::complex<float>* x, size_t n, double rate) { hub->pushIq(x, n, e_.captureCentreHz(), rate); });
}

std::string ServerHost::modeId() const {
    std::lock_guard<std::mutex> lk(mu_);
    return modeIdx_ >= 0 && (size_t)modeIdx_ < modes_.size() ? modes_[(size_t)modeIdx_].id : std::string();
}

void ServerHost::editConfig(const std::function<void(ConfigFile&)>& f) {
    std::lock_guard<std::mutex> lk(mu_);
    f(cfg_);
    dirty_ = true;
}

std::string ServerHost::lastError() const { std::lock_guard<std::mutex> lk(mu_); return err_; }

void ServerHost::supervise(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (wantRun_ && !e_.running() && nowSec() - lastTry_ > 10) {   // a radio that was unplugged, or one that is not up yet at boot
            lastTry_ = nowSec();
            devicesAt_ = -1e9;
            refreshDevices(false);
            std::string err;
            if (!startLocked(err)) { if (err != err_) fprintf(stderr, "onair-server: %s (trying again every 10 s)\n", err.c_str()); err_ = err; }
            else err_.clear();
        }
    }
    if (dirty_.exchange(false)) {
        std::string err;
        std::lock_guard<std::mutex> lk(mu_);
        if (!cfg_.save(path, err)) fprintf(stderr, "onair-server: %s\n", err.c_str());
    }
}

// ---------------------------------------------------------------- the API
bool ServerHost::start(int dev, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    refreshDevices(false);
    if (dev >= (int)devices_.size()) { err = "no such device (see /api/v1/devices)"; return false; }
    if (dev >= 0) devIdx_ = dev;
    wantRun_ = true;
    const bool ok = startLocked(err);
    if (ok) err_.clear(); else err_ = err;
    saveState();
    return ok;
}

bool ServerHost::stop(std::string&) {
    std::lock_guard<std::mutex> lk(mu_);
    wantRun_ = false;
    stopLocked();
    saveState();
    return true;
}

bool ServerHost::setMode(int family, const std::string& sub, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    int found = -1;
    for (size_t i = 0; i < modes_.size() && found < 0; i++) if (modes_[i].family == family && (sub.empty() ? modes_[i].sub.empty() : modes_[i].sub == sub)) found = (int)i;
    if (found < 0 && sub.empty()) for (size_t i = 0; i < modes_.size() && found < 0; i++) if (modes_[i].family == family) found = (int)i;
    if (found < 0) { err = "no such mode (see /api/v1/modes)"; return false; }
    const bool was = e_.running();
    stopLocked();
    modeIdx_ = found;
    freqHz_ = modes_[(size_t)found].defMhz * 1e6;
    saveState();
    if (was && wantRun_) { const bool ok = startLocked(err); err_ = ok ? std::string() : err; return ok; }   // the browser does not make the user stop first
    return true;
}

bool ServerHost::setFrequencyHz(double hz, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!(hz > 0)) { err = "the frequency must be above zero"; return false; }
    freqHz_ = hz;
    applyMonitor();
    saveState();
    return retuneLocked(err);
}

bool ServerHost::setSampleRateHz(double hz, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (devIdx_ >= 0 && devIdx_ < (int)devices_.size() && !devices_[(size_t)devIdx_].isRadio()) { err = "this source runs at its own rate"; return false; }
    rateHz_ = hz;
    const bool was = e_.running();
    stopLocked();
    saveState();
    if (was && wantRun_) { const bool ok = startLocked(err); err_ = ok ? std::string() : err; return ok; }
    return true;
}

bool ServerHost::setCaptureBandwidthHz(double hz, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (devIdx_ >= 0 && devIdx_ < (int)devices_.size() && !(devices_[(size_t)devIdx_].isRadio() || devices_[(size_t)devIdx_].kind == DeviceInfo::Synthetic)) { err = "this source runs at its own rate"; return false; }
    captureBwMhz_ = hz > 0 ? hz / 1e6 : 0;
    const bool was = e_.running();
    stopLocked();
    saveState();
    if (was && wantRun_) { const bool ok = startLocked(err); err_ = ok ? std::string() : err; return ok; }
    return true;
}

bool ServerHost::setGainDb(double db, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (devIdx_ >= 0 && devIdx_ < (int)devices_.size() && !devices_[(size_t)devIdx_].isRadio()) { err = "this source has no gain control"; return false; }
    gainDb_ = db;
    if (e_.running()) {
        const DeviceInfo& d = devices_[(size_t)devIdx_];
        if (d.isGeneric()) tune_.gainDb = std::max(d.gainMinDb, std::min(d.gainMaxDb > d.gainMinDb ? d.gainMaxDb : 200.0, db));
        else { const GainSetting g = hackrfGain(d, db, cfg_.flag("amp", false)); tune_.lnaDb = g.lna; tune_.vgaDb = g.vga; tune_.ampOn = g.amp; }
    }
    saveState();
    return retuneLocked(err);
}

std::string ServerHost::statusJson() {
    std::lock_guard<std::mutex> lk(mu_);
    const bool run = e_.running();
    const ModeEntry& m = modes_[(size_t)modeIdx_];
    const DeviceInfo* d = devIdx_ >= 0 && devIdx_ < (int)devices_.size() ? &devices_[(size_t)devIdx_] : nullptr;
    char b[512];
    snprintf(b, sizeof b, ",\"frequencyHz\":%.0f,\"sampleRateHz\":%.0f,\"gainDb\":%s,\"gainMinDb\":%s,\"gainMaxDb\":%s,\"droppedSamples\":%llu,\"loadPct\":%.0f}",
             freqHz_, run ? e_.radioRate() : (rateHz_ > 0 ? rateHz_ : 0.0), jsonNum(gainDb_).c_str(), jsonNum(d && d->isGeneric() ? d->gainMinDb : 0).c_str(),
             jsonNum(d ? (d->isGeneric() ? d->gainMaxDb : d->kind == DeviceInfo::HackRF ? (cfg_.flag("amp", false) ? 116 : 102) : 0) : 0).c_str(), (unsigned long long)e_.droppedSamples(), (double)e_.sampleLoss().loadPct);
    std::string meshJ;   // Mesh (LoRa): frames saved by undoing sample jumps since the program started (RxFrame::jumpsFixed)
    if (m.family == 22) meshJ = ",\"meshJumpFrames\":" + std::to_string((unsigned long long)e_.mesh().framesWithJumpsFixed());
    return std::string("{\"running\":") + (run ? "true" : "false") + ",\"wanted\":" + (wantRun_ ? "true" : "false") + ",\"deviceIndex\":" + std::to_string(devIdx_) + ",\"device\":\"" +
           jsonEscape(d ? d->name : "") + "\",\"deviceKind\":\"" + (d ? kindName(d->kind) : "unknown") + "\",\"family\":" + std::to_string(m.family) + ",\"mode\":\"" + jsonEscape(m.name) +
           "\",\"modeId\":\"" + jsonEscape(m.id) + "\",\"sub\":" + (m.sub.empty() ? "null" : "\"" + jsonEscape(m.sub) + "\"") + ",\"demod\":\"" + demodName(demod_) + "\",\"error\":\"" + jsonEscape(err_) + "\"" + meshJ + b;
}

std::string ServerHost::devicesJson() {
    std::lock_guard<std::mutex> lk(mu_);
    refreshDevices(false);
    std::string o = "[";
    for (size_t i = 0; i < devices_.size(); i++) {
        const DeviceInfo& d = devices_[i];
        if (i) o += ',';
        o += "{\"index\":" + std::to_string(i) + ",\"name\":\"" + jsonEscape(d.name) + "\",\"kind\":\"" + kindName(d.kind) + "\",\"board\":\"" + jsonEscape(d.board) + "\"";
        if (d.kind != DeviceInfo::File) o += ",\"serial\":\"" + jsonEscape(d.serial) + "\"";
        o += ",\"minRateHz\":" + jsonNum(d.minRateHz) + ",\"maxRateHz\":" + jsonNum(d.maxRateHz) + ",\"gainMinDb\":" + jsonNum(d.gainMinDb) + ",\"gainMaxDb\":" + jsonNum(d.gainMaxDb) +
             ",\"minFreqHz\":" + jsonNum(d.minFreqHz) + ",\"maxFreqHz\":" + jsonNum(d.maxFreqHz) + ",\"selected\":" + (devIdx_ == (int)i ? "true" : "false") + "}";
    }
    return o + "]";
}

std::string ServerHost::modesJson() {
    std::string o = "[";
    for (size_t i = 0; i < modes_.size(); i++) {
        const ModeEntry& m = modes_[i];
        if (i) o += ',';
        o += "{\"family\":" + std::to_string(m.family) + ",\"id\":\"" + jsonEscape(m.id) + "\",\"name\":\"" + jsonEscape(m.name) + "\",\"sub\":" + (m.sub.empty() ? "null" : "\"" + jsonEscape(m.sub) + "\"") +
             ",\"defaultMhz\":" + jsonNum(m.defMhz) + "}";
    }
    return o + "]";
}

std::string ServerHost::telemetryJson(uint64_t& seq) {
    std::lock_guard<std::mutex> lk(snapMu_);
    seq = telSeq_;
    return telJson_;
}

std::string ServerHost::panelJson(uint64_t& seq) {
    std::lock_guard<std::mutex> lk(snapMu_);
    if (rxSnapSeq_ == 0) { seq = 0; return "null"; }
    if (panelSeq_ != rxSnapSeq_ || panelJson_.empty()) {
        dect2::webschema::panelJson(rxSnap_, panelJson_);
        panelSeq_ = rxSnapSeq_;
    }
    seq = panelSeq_;
    return panelJson_;
}

void ServerHost::captureInfo(double& centreHz, double& rateHz) {
    std::lock_guard<std::mutex> lk(snapMu_);
    centreHz = specCentreHz_;
    rateHz = specRateHz_;
}

void ServerHost::snapLoop() {
    uint64_t rxSeq = 0, spSeq = 0;
    while (!quit_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        if (!e_.running()) {
            std::lock_guard<std::mutex> lk(snapMu_);
            if (telJson_.find("\"running\":true") != std::string::npos) { telJson_ = "{\"running\":false,\"available\":false}"; telSeq_++; rxSnapSeq_ = 0; panelJson_.clear(); }
            spec_.dbfs.clear();
            continue;
        }
        SpectrumFrame sf;
        const bool gotSpec = e_.latestSpectrum(sf, spSeq);
        RxTelemetry t;
        const bool gotRx = e_.latestRx(t, rxSeq);
        std::lock_guard<std::mutex> lk(snapMu_);
        if (gotSpec) { spSeq = sf.seq; spec_ = std::move(sf); specCentreHz_ = e_.captureCentreHz(); specRateHz_ = e_.radioRate(); }
        if (gotRx) {
            rxSeq = t.seq;
            quality_.update(t);
            telJson_ = std::string("{\"running\":true,\"available\":true,") + rxTelemetryJson(t, &quality_.report()).substr(1);
            telSeq_++;
            rxSnap_ = std::move(t);
            rxSnapSeq_ = rxSeq;
        }
    }
}

// the strongest bin of every group, so narrow carriers survive the decimation
static std::vector<float> decimateMax(const std::vector<float>& s, int maxBins) {
    const size_t n = s.size(), out = std::min<size_t>(n, (size_t)std::max(16, maxBins)), grp = (n + out - 1) / out;
    std::vector<float> r;
    for (size_t i = 0; i < n; i += grp) {
        float m = s[i];
        for (size_t k = i + 1; k < std::min(n, i + grp); k++) m = std::max(m, s[k]);
        r.push_back(m);
    }
    return r;
}

std::string ServerHost::spectrumJson(int maxBins, uint64_t& seq) {
    std::lock_guard<std::mutex> lk(snapMu_);
    if (spec_.dbfs.empty()) { seq = 0; return "{\"running\":false,\"dbfs\":[]}"; }
    seq = spec_.seq;
    const std::vector<float> v = decimateMax(spec_.dbfs, maxBins);
    char b[256];
    snprintf(b, sizeof b, "{\"running\":true,\"seq\":%llu,\"centerHz\":%.0f,\"sampleRateHz\":%.0f,\"rmsDbfs\":%s,\"peak\":%s,\"clipFraction\":%s,\"bins\":%zu,\"dbfs\":[",
             (unsigned long long)spec_.seq, specCentreHz_, specRateHz_, jsonNum(spec_.stats.rmsDbfs).c_str(), jsonNum(spec_.stats.peak).c_str(), jsonNum(spec_.stats.clipFraction).c_str(), v.size());
    std::string o = b;
    for (size_t i = 0; i < v.size(); i++) { if (i) o += ','; snprintf(b, sizeof b, "%.1f", v[i]); o += b; }
    return o + "]}";
}

bool ServerHost::spectrumBinary(int bins, uint64_t& seq, std::string& frame) { return spectrumBinaryZoom(bins, 1, 0, seq, frame); }

bool ServerHost::spectrumBinaryZoom(int bins, double zoom, double centreHz, uint64_t& seq, std::string& frame) {
    std::lock_guard<std::mutex> lk(snapMu_);
    if (spec_.dbfs.empty()) return false;
    seq = spec_.seq;
    const size_t total = spec_.dbfs.size();
    double rate = specRateHz_, centre = specCentreHz_;
    std::vector<float> part;
    const std::vector<float>* src = &spec_.dbfs;
    if (std::isfinite(zoom) && zoom > 1.0001 && rate > 0 && total >= 128) {
        const double z = std::min(zoom, (double)total / 64.0);   // at least 64 bins stay
        const size_t n = std::max<size_t>(64, (size_t)std::lround((double)total / z));
        const double binHz = rate / (double)total;
        // the first bin of the part: centred on centreHz, moved inside the capture
        const double c = std::isfinite(centreHz) && centreHz > 0 ? centreHz : centre;
        long long first = (long long)std::llround((c - (centre - rate / 2)) / binHz - (double)n / 2);
        first = std::max(0LL, std::min((long long)(total - n), first));
        part.assign(spec_.dbfs.begin() + first, spec_.dbfs.begin() + first + (long long)n);
        rate = binHz * (double)n;
        centre = (specCentreHz_ - specRateHz_ / 2) + binHz * ((double)first + (double)n / 2);
        src = &part;
    }
    const std::vector<float> v = decimateMax(*src, std::min<int>(bins, (int)src->size()));
    const float floorDb = -120.f, topDb = 0.f;
    frame.clear();
    frame.push_back('\x01');
    auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; i++) frame.push_back((char)((x >> (8 * i)) & 0xFF)); };
    auto putF = [&](float f) { uint32_t x; memcpy(&x, &f, 4); put32(x); };
    put32((uint32_t)spec_.seq);
    putF((float)centre); putF((float)rate); putF(floorDb); putF(topDb);
    for (float d : v) frame.push_back((char)(unsigned char)std::lround(std::max(0.f, std::min(1.f, (d - floorDb) / (topDb - floorDb))) * 255.f));
    return true;
}

// ---------------------------------------------------------------- mode options
int ServerHost::telemetryStandard() const {
    const int f = modes_[(size_t)modeIdx_].family;
    return f >= 8 ? f - 1 : -1;   // engine standard = family + 2, telemetry standard = family + 1 (docs/WEB-SCHEMA-3.9.md)
}

void ServerHost::applyOptionsLocked() {
    const int sd = telemetryStandard();
    const char* schema = sd >= 0 ? webschema::optionsJson(sd) : nullptr;
    if (!schema) return;
    const std::string pre = "opt." + std::to_string(sd) + ".";
    // every option the schema names that the settings file holds a value for
    for (const char* p = std::strstr(schema, "{\"n\":\""); p; p = std::strstr(p + 6, "{\"n\":\"")) {
        const char* q = std::strchr(p + 6, '"');
        if (!q) break;
        const std::string name(p + 6, (size_t)(q - p - 6));
        if (!cfg_.has(pre + name)) continue;
        std::string why;
        webschema::applyOption(e_, sd, name, cfg_.num(pre + name, 0), why);
    }
}

bool ServerHost::setOption(const std::string& name, double value, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    const int sd = telemetryStandard();
    if (sd < 0 || !webschema::optionsJson(sd)) { err = "this mode has no options of its own"; return false; }
    if (!webschema::applyOption(e_, sd, name, value, err)) return false;
    cfg_.setNum("opt." + std::to_string(sd) + "." + name, value);
    dirty_ = true;
    return true;
}

std::string ServerHost::optionValuesJson() {
    std::lock_guard<std::mutex> lk(mu_);
    const int sd = telemetryStandard();
    std::string o = "{\"standard\":" + std::to_string(sd) + ",\"values\":{";
    const char* schema = sd >= 0 ? webschema::optionsJson(sd) : nullptr;
    bool first = true;
    const std::string pre = "opt." + std::to_string(sd) + ".";
    for (const char* p = schema ? std::strstr(schema, "{\"n\":\"") : nullptr; p; p = std::strstr(p + 6, "{\"n\":\"")) {
        const char* q = std::strchr(p + 6, '"');
        if (!q) break;
        const std::string name(p + 6, (size_t)(q - p - 6));
        if (!cfg_.has(pre + name)) continue;
        if (!first) o += ',';
        first = false;
        o += "\"" + name + "\":" + jsonNum(cfg_.num(pre + name, 0));
    }
    return o + "}}";
}

std::string ServerHost::audioSource() const {
    return fmTap_.load() ? "fm" : voiceTap_.load() ? "voice" : "monitor";
}

// ---------------------------------------------------------------- audio for the browser
// Speech at its codec's rate (8 kHz for AMBE, IMBE, Codec2) up to the 48 kHz audioIn takes, by linear interpolation
void ServerHost::voiceIn(const float* x, size_t n, int rate) {
    if (!x || !n || rate <= 0 || rate > 48000) return;
    if (rate != voiceRate_) { voiceRate_ = rate; voicePhase_ = 0; voiceLast_ = 0; }
    const double step = (double)rate / 48000.0;
    std::vector<float> out;
    out.reserve((size_t)((double)n / step) + 2);
    for (size_t i = 0; i < n; i++) {
        const float a = voiceLast_, b = x[i];
        while (voicePhase_ < 1.0) { out.push_back(a + (b - a) * (float)voicePhase_); voicePhase_ += step; }
        voicePhase_ -= 1.0;
        voiceLast_ = b;
    }
    audioIn(out.data(), out.data(), out.size(), false);
}

void ServerHost::audioIn(const float* a, const float* b, size_t n, bool interleaved) {
    std::lock_guard<std::mutex> lk(aMu_);
    if (listeners_.empty()) return;
    for (auto& kv : listeners_) {
        Listener& l = kv.second;
        for (size_t i = 0; i < n; i++) {
            const float v = interleaved ? 0.5f * (a[2 * i] + a[2 * i + 1]) : 0.5f * (a[i] + b[i]);
            l.acc += v;
            if (++l.cnt >= l.step) {
                const int s = (int)std::lround(std::max(-1.0, std::min(1.0, l.acc / l.step)) * 32767.0);
                l.pcm.push_back((char)(s & 0xFF)); l.pcm.push_back((char)((s >> 8) & 0xFF));
                l.acc = 0; l.cnt = 0;
            }
        }
        const size_t cap = (size_t)l.rate * 2;   // one second: a listener that does not fetch loses the oldest audio, not memory
        if (l.pcm.size() > cap) l.pcm.erase(0, l.pcm.size() - cap);
    }
}

int ServerHost::audioOpen(int rateHz) {
    static const int ok[] = {8000, 12000, 16000, 24000, 48000};
    int r = 8000;
    for (int x : ok) if (x <= rateHz) r = x;
    int id;
    {
        std::lock_guard<std::mutex> lk(aMu_);
        if (listeners_.size() >= 32) return -1;   // the web app shares one monitor; a listener is only a copy of its audio
        id = nextListener_++;
        Listener l;
        l.rate = r; l.step = 48000 / r;
        listeners_[id] = l;
    }
    std::lock_guard<std::mutex> lk(mu_);   // the monitor is switched on by the first listener
    applyMonitor();
    return id;
}

bool ServerHost::audioRead(int id, std::string& pcm) {
    std::lock_guard<std::mutex> lk(aMu_);
    auto it = listeners_.find(id);
    if (it == listeners_.end()) return false;
    pcm.swap(it->second.pcm);
    it->second.pcm.clear();
    if (pcm.size() > 8192) pcm.erase(0, pcm.size() - 8192);   // one message at a time stays small
    return true;
}

void ServerHost::audioClose(int id) {
    { std::lock_guard<std::mutex> lk(aMu_); listeners_.erase(id); }
    std::lock_guard<std::mutex> lk(mu_);
    applyMonitor();
}

std::string ServerHost::indexHtml() { return kIndexHtml; }

int ServerHost::handleExtra(const std::string& method, const std::string& path, const std::map<std::string, JsonVal>& body, std::string& json) {
    if (path == "/api/v1/x/server" && method == "GET") {
        std::lock_guard<std::mutex> lk(mu_);
        json = std::string("{\"name\":\"OnAir headless server\",\"version\":\"") + ONAIR_SERVER_VERSION + "\",\"demod\":\"" + demodName(demod_) + "\",\"autostart\":" + (cfg_.flag("autostart", false) ? "true" : "false") + "}";
        return 200;
    }
    if (path == "/api/v1/x/demod" && method == "POST") {
        auto it = body.find("demod");
        const int d = it != body.end() && it->second.kind == JsonVal::Str ? demodFromName(it->second.str) : -1;
        if (d < 0) { json = "demod must be auto, usb, lsb, nfm or am"; return 400; }
        std::lock_guard<std::mutex> lk(mu_);
        demod_ = d;
        applyMonitor();
        saveState();
        json = "{\"ok\":true}";
        return 200;
    }
    if (path == "/api/v1/x/autostart" && method == "POST") {
        auto it = body.find("on");
        if (it == body.end() || it->second.kind != JsonVal::Bool) { json = "give on: true or false"; return 400; }
        std::lock_guard<std::mutex> lk(mu_);
        wantRun_ = it->second.b;
        saveState();
        json = "{\"ok\":true}";
        return 200;
    }
    return 0;
}

static std::string defaultRecDir() {
#ifdef _WIN32
    const char* h = getenv("USERPROFILE");
#else
    const char* h = getenv("HOME");
#endif
    return (std::filesystem::path(h ? h : ".") / "OnAir-recordings").string();
}

bool ServerHost::recordStart(const std::string& kind, std::string& err) {
    if (kind != "iq") { err = "the server records IQ only (kind iq); audio is recorded by the browser"; return false; }
    std::lock_guard<std::mutex> lk(mu_);
    if (!e_.running()) { err = "the receiver is not running"; return false; }
    if (!recPath_.empty()) { err = "a recording is running: " + recPath_; return false; }
    const std::string dir = cfg_.get("rec_dir", defaultRecDir());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    char name[256];
    snprintf(name, sizeof name, "onair-%s-%.4fMHz-%s_%.3fmsps.cf32", modes_[(size_t)modeIdx_].id.c_str(), freqHz_ / 1e6, stamp, e_.radioRate() / 1e6);
    const std::string path = (std::filesystem::path(dir) / name).string();
    if (!e_.startRecording(path, FileFormat::CF32, err)) return false;
    recPath_ = path;
    recStart_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return true;
}

bool ServerHost::recordStop(std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (recPath_.empty()) { err = "no recording is running"; return false; }
    e_.stopRecording();
    recPath_.clear();
    return true;
}

std::string ServerHost::recordingJson() {
    std::lock_guard<std::mutex> lk(mu_);
    if (recPath_.empty()) return "{\"recording\":false}";
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() - recStart_;
    return "{\"recording\":true,\"path\":\"" + jsonEscape(recPath_) + "\",\"seconds\":" + jsonNum(secs) + "}";
}

} // namespace onair_server
