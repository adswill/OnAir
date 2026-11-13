// Hamlib CAT control for classic transceivers and receivers (an IC-R8600 and the like): frequency and mode follow in both directions
// while the receiver runs; IQ or speaker audio comes through a sound card when configured.
// Receiving never keys the transmitter: only HamlibTx (the transmit subsystem, an operator action) calls PTT.
#ifdef DECT2_HAVE_HAMLIB

#include "dect2/audio/audioin.h"
#include "dect2/common/thread_name.h"
#include "dect2/ham/ham.h"
#include "dect2/source/hamlib_link.h"
#include "dect2/source/hamlib_rig.h"
#include "dect2/source/rig_setup.h"
#include "dect2/source/source.h"
#ifdef DECT2_HAMLIB_DYNAMIC
#include "native_common.h"   // DynLib, libNames: found like the radio drivers' libraries
#endif
#include "hamlib_api.h"   // <hamlib/rig.h>, or the same calls loaded at run time (DECT2_HAMLIB_DYNAMIC, the Windows packages)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include "dect2/common/str.h"

namespace dect2 {

namespace {

using Clock = std::chrono::steady_clock;

std::once_flag gHamInit;
std::mutex gHamInitMu;   // rig_init / rig_open of different rigs one at a time (the backends keep global tables)
bool gHamLoaded = false;

bool hamLoad(std::string& err) {
    std::string le;
    if (!hamlibLibLoad(le)) { err = le; return false; }   // the run-time loaded library (Windows): not found, or too old
    std::call_once(gHamInit, [] {
        rig_set_debug(RIG_DEBUG_ERR);   // the default prints a trace line per call
        const int r = rig_load_all_backends();
        gHamLoaded = (r == RIG_OK || r == -RIG_ENIMPL);
    });
    if (!gHamLoaded) err = "Hamlib: could not load the rig backends";
    return gHamLoaded;
}

std::string upper(std::string s) { return dect2::str::upper(std::move(s)); }

rmode_t modeFromName(const std::string& name) {
    std::string le;
    if (name.empty() || !hamLoad(le)) return RIG_MODE_NONE;
    const rmode_t m = rig_parse_mode(upper(name).c_str());
    return m;
}

std::string modeName(rmode_t m) {
    const char* s = rig_strrmode(m);
    return s ? s : "";
}

} // namespace

#ifdef DECT2_HAMLIB_DYNAMIC
namespace hamdyn {
Api& api() { static Api a; return a; }
}
namespace {
std::mutex gLibMu;
native::DynLib gLib;
bool gLibOk = false;
double gLibTried = -1e9;
std::string gLibErr;
}
bool hamlibLibLoad(std::string& err) {
    std::lock_guard<std::mutex> lk(gLibMu);
    if (gLibOk) return true;
    const double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
    if (now - gLibTried < 5) { err = gLibErr; return false; }
    gLibTried = now;
    // Hamlib's own Windows build and MSYS2 call it libhamlib-4.dll; next to OnAir.exe, in the OnAir codecs folder or on the PATH (#495)
    if (!gLib.h && !gLib.open(native::libNames("hamlib", {".4"}, {"libhamlib-4.dll", "hamlib-4.dll", "libhamlib.dll", "hamlib.dll"}))) {
#if defined(_WIN32)
        gLibErr = "Hamlib: libhamlib-4.dll not found. Put it (with the DLLs from the bin folder of Hamlib's Windows zip) next to OnAir.exe or in the OnAir codecs folder; OnAir looks again every few seconds (docs/OPTIONAL-LIBRARIES.md)";
#elif defined(__APPLE__)
        gLibErr = "Hamlib: libhamlib.4.dylib not found (brew install hamlib, or put it in the OnAir codecs folder; docs/OPTIONAL-LIBRARIES.md)";
#else
        gLibErr = "Hamlib: libhamlib.so.4 not found (install libhamlib4 or put it in the OnAir codecs folder; docs/OPTIONAL-LIBRARIES.md)";
#endif
        err = gLibErr;
        return false;
    }
    Api& a = hamdyn::api();
    bool ok = gLib.get(a.init, "rig_init") && gLib.get(a.open, "rig_open") && gLib.get(a.close, "rig_close") && gLib.get(a.cleanup, "rig_cleanup") &&
              gLib.get(a.setFreq, "rig_set_freq") && gLib.get(a.getFreq, "rig_get_freq") && gLib.get(a.setMode, "rig_set_mode") && gLib.get(a.getMode, "rig_get_mode") &&
              gLib.get(a.setPtt, "rig_set_ptt") && gLib.get(a.setSplitVfo, "rig_set_split_vfo") && gLib.get(a.setSplitFreq, "rig_set_split_freq") &&
              gLib.get(a.getSplitFreq, "rig_get_split_freq") && gLib.get(a.getInfo, "rig_get_info") && gLib.get(a.parseMode, "rig_parse_mode") &&
              gLib.get(a.strrmode, "rig_strrmode") && gLib.get(a.loadAllBackends, "rig_load_all_backends") && gLib.get(a.setDebug, "rig_set_debug") &&
              gLib.get(a.error, "rigerror") && gLib.get(a.getCaps, "rig_get_caps") && gLib.get(a.tokenLookup, "rig_token_lookup") && gLib.get(a.setConf, "rig_set_conf") &&
              gLib.get(a.listForeach, "rig_list_foreach");
    if (!ok) {
        gLibErr = "Hamlib: " + gLib.path + " is not a Hamlib 4 library (a function is missing): use Hamlib 4.0 or later";
        err = gLibErr;
        return false;
    }
    fprintf(stderr, "Hamlib: %s loaded at run time\n", gLib.path.c_str());
    fflush(stderr);
    gLibOk = true;
    gLibErr.clear();
    return true;
}
std::string hamlibLibPath() { std::lock_guard<std::mutex> lk(gLibMu); return gLibOk ? gLib.path : std::string(); }
#endif

bool hamlibLibraryReady(std::string* why) {
    std::string e;
    const bool ok = hamlibLibLoad(e);
    if (why) *why = ok ? std::string() : e;
    return ok;
}

bool hamlibModeKnown(const std::string& name) { return modeFromName(name) != RIG_MODE_NONE; }

bool hamlibSameMode(const std::string& a, const std::string& b) {
    const rmode_t ma = modeFromName(a);
    return ma != RIG_MODE_NONE && ma == modeFromName(b);
}

std::string hamlibCanonicalMode(const std::string& name) {
    const rmode_t m = modeFromName(name);
    return m == RIG_MODE_NONE ? std::string() : modeName(m);
}

std::vector<RigModelInfo> hamlibModelList() {
    std::string err;
    std::vector<RigModelInfo> out;
    if (!hamLoad(err)) return out;
    std::lock_guard<std::mutex> lk(gHamInitMu);
    rig_list_foreach(
        [](const rig_caps* c, rig_ptr_t data) -> int {
            auto* v = static_cast<std::vector<RigModelInfo>*>(data);
            RigModelInfo m;
            m.id = (int)c->rig_model;
            m.mfg = c->mfg_name ? c->mfg_name : "";
            m.model = c->model_name ? c->model_name : "";
            m.status = (int)c->status;
            m.network = c->port_type == RIG_PORT_NETWORK || c->port_type == RIG_PORT_UDP_NETWORK;
            if (c->port_type == RIG_PORT_SERIAL) {
                m.baudMin = c->serial_rate_min;
                m.baudMax = c->serial_rate_max;
                m.dataBits = c->serial_data_bits > 0 ? c->serial_data_bits : 8;
                m.stopBits = c->serial_stop_bits > 0 ? c->serial_stop_bits : 1;
            }
            v->push_back(std::move(m));
            return 1;   // keep going
        },
        &out);
    std::sort(out.begin(), out.end(), [](const RigModelInfo& a, const RigModelInfo& b) {
        if (a.mfg != b.mfg) return a.mfg < b.mfg;
        if (a.model != b.model) return a.model < b.model;
        return a.id < b.id;
    });
    return out;
}

// ---- HamlibLink -------------------------------------------------------------------------------------------------------------------
struct HamlibLink::Impl {
    std::mutex mu;                 // one Hamlib call at a time on this rig (a RIG is not thread safe)
    RIG* rig = nullptr;
    std::atomic<bool> open{false};

    void closeLocked() {
        open = false;
        if (rig) {
            rig_close(rig);
            rig_cleanup(rig);
            rig = nullptr;
        }
    }
    bool setConf(const char* name, const std::string& val) {
        const token_t t = rig_token_lookup(rig, name);
        return t != 0 && rig_set_conf(rig, t, val.c_str()) == RIG_OK;
    }
};

HamlibLink::HamlibLink() : p_(new Impl) {}
HamlibLink::~HamlibLink() { close(); }

bool HamlibLink::open(const HamlibConn& c, std::string& err) {
    close();
    if (!hamLoad(err)) return false;
    std::lock_guard<std::mutex> lk(p_->mu);
    std::lock_guard<std::mutex> gl(gHamInitMu);
    p_->rig = rig_init((rig_model_t)c.model);
    if (!p_->rig) {
        err = "Hamlib: no rig with model number " + std::to_string(c.model);
        return false;
    }
    const rig_caps* caps = rig_get_caps((rig_model_t)c.model);
    const bool portless = caps && caps->port_type == RIG_PORT_NONE;   // the dummy rig has no port: a path would be taken as a network address
    if (!portless && !c.path.empty() && !p_->setConf("rig_pathname", c.path)) {
        err = "Hamlib: this rig model takes no port setting";
        p_->closeLocked();
        return false;
    }
    if (c.baud > 0) p_->setConf("serial_speed", std::to_string(c.baud));   // not for a network rig: it has no such setting
    if (c.timeoutMs > 0) p_->setConf("timeout", std::to_string(c.timeoutMs));
    if (c.retries >= 0) p_->setConf("retry", std::to_string(c.retries));
    if (c.pttType >= 1 && c.pttType <= 3) {   // RTS / DTR keying (on the CAT port or a port of its own), or none for VOX
        static const char* const kPtt[] = {"RIG", "RTS", "DTR", "None"};
        if (!p_->setConf("ptt_type", kPtt[c.pttType])) {
            err = std::string("Hamlib: this rig model cannot key PTT by ") + kPtt[c.pttType];
            p_->closeLocked();
            return false;
        }
        if (c.pttType != 3 && !c.pttPath.empty() && !p_->setConf("ptt_pathname", c.pttPath)) {
            err = "Hamlib: the PTT port " + c.pttPath + " cannot be set for this rig model";
            p_->closeLocked();
            return false;
        }
    }
    const int r = rig_open(p_->rig);
    if (r != RIG_OK) {
        err = std::string("Hamlib open ") + (c.path.empty() ? "rig" : c.path) + ": " + rigerror(r);
        p_->closeLocked();
        return false;
    }
    p_->open = true;
    return true;
}

void HamlibLink::close() {
    std::lock_guard<std::mutex> lk(p_->mu);
    p_->closeLocked();
}

bool HamlibLink::isOpen() const { return p_->open.load(); }

bool HamlibLink::setFreq(double hz, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    const int r = rig_set_freq(p_->rig, RIG_VFO_CURR, (freq_t)hz);
    if (r != RIG_OK) { err = std::string("Hamlib set frequency: ") + rigerror(r); return false; }
    return true;
}

bool HamlibLink::getFreq(double& hz, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    freq_t f = 0;
    const int r = rig_get_freq(p_->rig, RIG_VFO_CURR, &f);
    if (r != RIG_OK) { err = std::string("Hamlib read frequency: ") + rigerror(r); return false; }
    hz = (double)f;
    return true;
}

bool HamlibLink::setMode(const std::string& mode, int passbandHz, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    const rmode_t m = modeFromName(mode);
    if (m == RIG_MODE_NONE) { err = "Hamlib: unknown mode " + mode; return false; }
    const int r = rig_set_mode(p_->rig, RIG_VFO_CURR, m, passbandHz > 0 ? (pbwidth_t)passbandHz : RIG_PASSBAND_NORMAL);
    if (r != RIG_OK) { err = std::string("Hamlib set mode: ") + rigerror(r); return false; }
    return true;
}

bool HamlibLink::getMode(std::string& mode, int& passbandHz, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    rmode_t m = RIG_MODE_NONE;
    pbwidth_t pb = 0;
    const int r = rig_get_mode(p_->rig, RIG_VFO_CURR, &m, &pb);
    if (r != RIG_OK) { err = std::string("Hamlib read mode: ") + rigerror(r); return false; }
    mode = modeName(m);
    passbandHz = (int)pb;
    return true;
}

bool HamlibLink::setPtt(bool on, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    const int r = rig_set_ptt(p_->rig, RIG_VFO_CURR, on ? RIG_PTT_ON : RIG_PTT_OFF);
    if (r != RIG_OK) { err = std::string("Hamlib PTT: ") + rigerror(r); return false; }
    return true;
}

bool HamlibLink::setSplit(bool on, double txHz, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    int r = rig_set_split_vfo(p_->rig, RIG_VFO_CURR, on ? RIG_SPLIT_ON : RIG_SPLIT_OFF, RIG_VFO_TX);
    if (r != RIG_OK) { err = std::string("Hamlib split: ") + rigerror(r); return false; }
    if (on) {
        r = rig_set_split_freq(p_->rig, RIG_VFO_CURR, (freq_t)txHz);
        if (r != RIG_OK) { err = std::string("Hamlib split frequency: ") + rigerror(r); return false; }
    }
    return true;
}

bool HamlibLink::getSplitFreq(double& hz, std::string& err) {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) { err = "Hamlib: rig not open"; return false; }
    freq_t f = 0;
    const int r = rig_get_split_freq(p_->rig, RIG_VFO_CURR, &f);
    if (r != RIG_OK) { err = std::string("Hamlib read split frequency: ") + rigerror(r); return false; }
    hz = (double)f;
    return true;
}

std::string HamlibLink::info() const {
    std::lock_guard<std::mutex> lk(p_->mu);
    if (!p_->rig) return {};
    const char* s = rig_get_info(p_->rig);
    return s ? s : "";
}

// ---- the source -------------------------------------------------------------------------------------------------------------------
namespace {

DeviceInfo audioDevFor(const DeviceInfo& ham) {
    DeviceInfo a;
    a.kind = DeviceInfo::Audio;
    a.board = "audio";
    if (ham.nativeArgs.empty()) {
        a.serial = "silence";
        a.name = "silence";
    } else {
        // the sound card as it was typed in: an id, "Default input" or a device name (#358); a name that matches no card stays as it is, so
        // the audio source reports the card as missing
        std::string id = ham.nativeArgs;
        resolveAudioInputId(ham.nativeArgs, listAudioInputs(), id);
        a.serial = id;
        a.name = ham.nativeArgs;
    }
    return a;
}

std::string hamModeOf(const TuneSettings& s) {
    const std::string m = hamlibCanonicalMode(radioOption(s, "hamlib_mode", "USB"));
    return m.empty() ? "USB" : m;
}

class HamlibSource : public IqSource {
public:
    explicit HamlibSource(DeviceInfo d) : dev_(std::move(d)) {}
    ~HamlibSource() override { stop(); }

    bool start(const TuneSettings& s, IqRing& ring, std::string& err) override {
        stop();
        if (!parseHamlibSerial(dev_.serial, conn_.model, conn_.path, conn_.baud)) { err = "bad Hamlib rig id"; return false; }
        if (!link_.open(conn_, err)) return false;
        mode_ = appMode_ = hamModeOf(s);
        std::string e2;
        if (!link_.setFreq(s.centerHz, err)) { link_.close(); return false; }
        if (!link_.setMode(mode_, 0, e2)) err.clear(), noteMode(e2);   // a receiver may refuse a mode it lacks: tuning still works
        hz_ = s.centerHz;
        readBack();
        iq_ = makeAudioSource(audioDevFor(dev_));
        if (!iq_ || !iq_->start(s, ring, err)) {
            link_.close();
            iq_.reset();
            publish(false, "");
            return false;
        }
        rate_ = iq_->sampleRate();
        stopFlag_ = false;
        publishLocked(true, "", false);
        poll_ = dect2::namedThread("onair-hamlib", [this] { pollLoop(); });
        return true;
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stopFlag_ = true;
        }
        cv_.notify_all();
        if (poll_.joinable()) poll_.join();
        if (iq_) { iq_->stop(); iq_.reset(); }
        link_.close();
        HamlibRigState st;   // inactive
        setHamlibRigState(st);
    }

    bool retune(const TuneSettings& s, std::string& err) override {
        const std::string mode = hamModeOf(s);
        {
            std::lock_guard<std::mutex> lk(mu_);
            wantHz_ = s.centerHz;
            wantMode_ = mode;
            if (!link_.isOpen()) {   // the link is down: the poll thread applies this when the rig is back
                pending_ = true;
                err = "Hamlib rig not connected: " + lastError_ + " (reconnecting)";
            }
        }
        if (!link_.isOpen()) return iq_ ? iq_->retune(s, err) || true : true;
        std::string e;
        bool changed = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            changed = std::fabs(s.centerHz - hz_) > 0.5;
        }
        if (changed) {
            if (!link_.setFreq(s.centerHz, e)) { noteLost(e); err = e; return false; }
        }
        bool modeChanged;
        { std::lock_guard<std::mutex> lk(mu_); modeChanged = mode != appMode_; appMode_ = mode; }   // only when the operator changed it in OnAir
        if (modeChanged) {
            if (!link_.setMode(mode, 0, e)) err = e;   // refused mode: keep the frequency change
            else { std::lock_guard<std::mutex> lk(mu_); mode_ = mode; }
        }
        if (changed) { std::lock_guard<std::mutex> lk(mu_); hz_ = s.centerHz; }
        if (changed) readBack();
        { std::lock_guard<std::mutex> lk(mu_); publishLocked(true, "", false); }
        std::string e3;
        if (iq_ && !iq_->retune(s, e3)) { err = e3; return false; }
        return true;
    }

    double sampleRate() const override { return rate_; }
    bool realtimeHardware() const override { return true; }

private:
    void noteMode(const std::string& e) { std::lock_guard<std::mutex> lk(mu_); lastError_ = e; }

    // after setting: the rig may round the dial (10 Hz steps); take what it says so the poll does not mistake it for a knob turn
    void readBack() {
        double hz = 0;
        std::string e, m;
        int pb = 0;
        if (link_.getFreq(hz, e) && hz > 0) { std::lock_guard<std::mutex> lk(mu_); hz_ = hz; }
        if (link_.getMode(m, pb, e) && !m.empty()) { std::lock_guard<std::mutex> lk(mu_); mode_ = m; }
    }

    void publish(bool connected, const std::string& status) {
        std::lock_guard<std::mutex> lk(mu_);
        publishLocked(connected, status, false);
    }
    // mu_ held
    void publishLocked(bool connected, const std::string& status, bool fromRig) {
        HamlibRigState st;
        st.active = true;
        st.connected = connected;
        st.hz = hz_;
        st.mode = mode_;
        if (fromRig) seq_++;
        st.seq = seq_;
        st.status = status;
        setHamlibRigState(st);
    }

    void noteLost(const std::string& e) {
        std::lock_guard<std::mutex> lk(mu_);
        lastError_ = e;
        publishLocked(false, e, false);
    }

    void pollLoop() {
        int fails = 0;
        auto nextReconnect = Clock::now();
        int backoffMs = 1000;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait_for(lk, std::chrono::milliseconds(link_.isOpen() ? 400 : 250), [this] { return stopFlag_; });
                if (stopFlag_) return;
            }
            std::string e;
            if (link_.isOpen()) {
                double hz = 0;
                std::string m;
                int pb = 0;
                const bool okF = link_.getFreq(hz, e);
                const bool okM = okF && link_.getMode(m, pb, e);
                if (!okF || !okM) {
                    if (++fails >= 2) {   // two failures in a row: the rig is gone (unplugged, rigctld killed): drop the link and retry
                        link_.close();
                        backoffMs = 1000;
                        nextReconnect = Clock::now() + std::chrono::milliseconds(backoffMs);
                        noteLost(e);
                    }
                    continue;
                }
                fails = 0;
                std::lock_guard<std::mutex> lk(mu_);
                bool changed = false;
                if (hz > 0 && std::fabs(hz - hz_) > 1.0) { hz_ = hz; changed = true; }
                if (!m.empty() && m != mode_) { mode_ = m; changed = true; }
                wantHz_ = hz_; wantMode_ = mode_;
                if (changed) publishLocked(true, "", true);
                else if (!lastPublishedConnected_) publishLocked(true, "", false);
                lastPublishedConnected_ = true;
            } else if (Clock::now() >= nextReconnect) {
                lastPublishedConnected_ = false;
                if (link_.open(conn_, e)) {
                    fails = 0;
                    bool p;
                    double wh;
                    std::string wm;
                    { std::lock_guard<std::mutex> lk(mu_); p = pending_; wh = wantHz_; wm = wantMode_; pending_ = false; }
                    std::string e2;
                    if (p && wh > 0) { link_.setFreq(wh, e2); if (!wm.empty()) link_.setMode(wm, 0, e2); }
                    readBack();
                    std::lock_guard<std::mutex> lk(mu_);
                    lastError_.clear();
                    publishLocked(true, "", false);
                    lastPublishedConnected_ = true;
                } else {
                    backoffMs = std::min(backoffMs * 2, 5000);
                    nextReconnect = Clock::now() + std::chrono::milliseconds(backoffMs);
                    noteLost(e);
                }
            }
        }
    }

    DeviceInfo dev_;
    HamlibConn conn_;
    HamlibLink link_;
    std::unique_ptr<IqSource> iq_;
    double rate_ = 48000;

    std::thread poll_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopFlag_ = false;
    bool pending_ = false;
    bool lastPublishedConnected_ = true;
    double hz_ = 0, wantHz_ = 0;
    std::string mode_, appMode_, wantMode_, lastError_;
    uint64_t seq_ = 0;
};

class HamlibTx : public TxSink {
public:
    explicit HamlibTx(DeviceInfo d) : dev_(std::move(d)) {}
    ~HamlibTx() override { close(); }

    bool open(const TxTune& t, std::string& err) override {
        close();
        HamlibConn c;
        if (!parseHamlibSerial(dev_.serial, c.model, c.path, c.baud)) { err = "bad Hamlib rig id"; return false; }
        // outside the amateur bands: a warning, not a refusal (the operator decides; the rig itself refuses what it cannot transmit)
        if (!ham::hamBandAt(txRadioHz(t))) fprintf(stderr, "warning: %.3f MHz is not an amateur band. You are responsible for being allowed to transmit here.\n", txRadioHz(t) / 1e6);
        if (!rig_.open(c, err)) return false;
        TuneSettings ts;
        ts.centerHz = t.centerHz;
        ts.radio = t.radio;
        if (!rig_.setFreq(txRadioHz(t), err)) { close(); return false; }
        std::string e2;
        rig_.setMode(hamModeOf(ts), 0, e2);
        if (!rig_.setPtt(true, err)) { close(); return false; }
        keyed_ = true;
        rate_ = t.sampleRate > 0 ? t.sampleRate : 48000;
        t0_ = Clock::now();
        sent_ = 0;
        return true;
    }

    bool write(const cf32*, size_t n, std::string& err) override {
        if (n == 0) return true;
        sent_ += (uint64_t)n;
        const auto due = t0_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(sent_ / rate_));
        std::this_thread::sleep_until(due);
        (void)err;
        return true;
    }

    bool retune(const TxTune& t, std::string& err) override {
        TuneSettings ts;
        ts.centerHz = t.centerHz;
        ts.radio = t.radio;
        if (!ham::hamBandAt(txRadioHz(t))) fprintf(stderr, "warning: %.3f MHz is not an amateur band. You are responsible for being allowed to transmit here.\n", txRadioHz(t) / 1e6);
        if (!rig_.setFreq(txRadioHz(t), err)) return false;
        std::string e2;
        rig_.setMode(hamModeOf(ts), 0, e2);
        return true;
    }

    void close() override {
        if (keyed_) {   // only a sink that keyed the rig unkeys it
            std::string e;
            rig_.setPtt(false, e);
            keyed_ = false;
        }
        rig_.close();
    }

    double sampleRate() const override { return rate_; }
    bool realtime() const override { return true; }
    std::string name() const override { return dev_.name; }
    bool takesIq() const override { return false; }

private:
    DeviceInfo dev_;
    HamlibLink rig_;
    bool keyed_ = false;
    double rate_ = 48000;
    Clock::time_point t0_;
    uint64_t sent_ = 0;
};

} // namespace

bool hamlibSupported() { return true; }
std::unique_ptr<IqSource> makeHamlibSource(const DeviceInfo& d) { return std::make_unique<HamlibSource>(d); }
std::unique_ptr<TxSink> makeHamlibTxSink(const DeviceInfo& d) { return std::make_unique<HamlibTx>(d); }

} // namespace dect2

#endif // DECT2_HAVE_HAMLIB
