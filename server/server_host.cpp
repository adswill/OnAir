#include "server_host.h"
#include "web_ui.h"
#include "dect2/audio/audio_monitor.h"
#include "dect2/audio/hf_demod.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/net/netradio.h"
#include "dect2/results/rx_json.h"
#include "dect2/source/agc.h"
#include "dect2/source/gain.h"
#include "dect2/source/iq_file.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    e_.p25().setSilent(true); e_.mac().setSilent(true);
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
    for (int s = 8; s < 200; s++) {
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
    gainDb_ = cfg_.num("gain_db", 30);
    const int dm = demodFromName(cfg_.get("audio_demod", "auto"));
    demod_ = dm < 0 ? 4 : dm;
    wantRun_ = cfg_.flag("autostart", false);
    snapTh_ = std::thread([this] { snapLoop(); });
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
    if (d.isRadio() && rateHz_ > 0) t.sampleRate = rateHz_;
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
    quality_.reset();
    if (!e_.start(d, tune_, file)) {
        size_t n = 0;
        const auto log = e_.logSnapshot(n);
        err = "could not start the receiver" + (log.empty() ? std::string() : ": " + log.back());
        return false;
    }
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
    c.on = any && modes_[(size_t)modeIdx_].family != 7;   // FM broadcast has its own stereo audio (the tap); every other mode is heard through the monitor
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
    cfg_.setNum("gain_db", gainDb_);
    cfg_.set("audio_demod", demodName(demod_));
    if (devIdx_ >= 0 && devIdx_ < (int)devices_.size() && devices_[(size_t)devIdx_].kind != DeviceInfo::Synthetic) cfg_.set("device", devices_[(size_t)devIdx_].name);
    else if (devIdx_ >= 0 && devIdx_ < (int)devices_.size()) cfg_.set("device", "test");
    cfg_.set("autostart", wantRun_ ? "1" : "0");   // what was running stays running after a restart of the service
    dirty_ = true;
}

void ServerHost::shutdown() { std::lock_guard<std::mutex> lk(mu_); stopLocked(); }

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
    return std::string("{\"running\":") + (run ? "true" : "false") + ",\"wanted\":" + (wantRun_ ? "true" : "false") + ",\"deviceIndex\":" + std::to_string(devIdx_) + ",\"device\":\"" +
           jsonEscape(d ? d->name : "") + "\",\"deviceKind\":\"" + (d ? kindName(d->kind) : "unknown") + "\",\"family\":" + std::to_string(m.family) + ",\"mode\":\"" + jsonEscape(m.name) +
           "\",\"modeId\":\"" + jsonEscape(m.id) + "\",\"sub\":" + (m.sub.empty() ? "null" : "\"" + jsonEscape(m.sub) + "\"") + ",\"demod\":\"" + demodName(demod_) + "\",\"error\":\"" + jsonEscape(err_) + "\"" + b;
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

void ServerHost::snapLoop() {
    uint64_t rxSeq = 0, spSeq = 0;
    while (!quit_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        if (!e_.running()) {
            std::lock_guard<std::mutex> lk(snapMu_);
            if (telJson_.find("\"running\":true") != std::string::npos) { telJson_ = "{\"running\":false,\"available\":false}"; telSeq_++; }
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

bool ServerHost::spectrumBinary(int bins, uint64_t& seq, std::string& frame) {
    std::lock_guard<std::mutex> lk(snapMu_);
    if (spec_.dbfs.empty()) return false;
    seq = spec_.seq;
    const std::vector<float> v = decimateMax(spec_.dbfs, bins);
    const float floorDb = -120.f, topDb = 0.f;
    frame.clear();
    frame.push_back('\x01');
    auto put32 = [&](uint32_t x) { for (int i = 0; i < 4; i++) frame.push_back((char)((x >> (8 * i)) & 0xFF)); };
    auto putF = [&](float f) { uint32_t x; memcpy(&x, &f, 4); put32(x); };
    put32((uint32_t)spec_.seq);
    putF((float)specCentreHz_); putF((float)specRateHz_); putF(floorDb); putF(topDb);
    for (float d : v) frame.push_back((char)(unsigned char)std::lround(std::max(0.f, std::min(1.f, (d - floorDb) / (topDb - floorDb))) * 255.f));
    return true;
}

// ---------------------------------------------------------------- audio for the browser
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
        if (listeners_.size() >= 4) return -1;
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

} // namespace onair_server
