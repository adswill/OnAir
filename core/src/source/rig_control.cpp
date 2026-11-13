// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Rig control for transmitting (see rig_control.h): error hints, finding the rig, audio pairing, the watched CAT link and the CAT HamRigIo.
#include "dect2/source/rig_control.h"
#include "dect2/audio/audioout.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

namespace dect2 {

namespace {
std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
// lower case without spaces, dashes, underscores and dots: "IC-7300" "ic_7300" "IC 7300" are all "ic7300"
std::string squash(const std::string& s) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c)) o += (char)tolower(c);
    return o;
}
bool has(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

int modelIdFor(const std::vector<RigModelInfo>& all, const std::string& mfg, const std::string& model) {
    const std::string m = squash(model), f = squash(mfg);
    for (const auto& r : all) if (squash(r.model) == m && (f.empty() || squash(r.mfg).find(f) != std::string::npos || f.find(squash(r.mfg)) != std::string::npos)) return r.id;
    return 0;
}
}  // namespace

const char* rigPttName(int t) {
    switch (t) { case 1: return "RTS"; case 2: return "DTR"; case 3: return "VOX"; default: return "CAT"; }
}

std::string hamlibErrorHint(const std::string& err) {
    if (err.empty()) return err;
    const std::string e = lower(err);
    const char* hint = nullptr;
    if (has(e, "timed out") || has(e, "timeout"))
        hint = "The rig does not answer: check the USB cable, that the CAT baud rate matches the rig's menu (CI-V address too on an Icom), and that no "
               "other program (WSJT-X, flrig, a logger) has the port open.";
    else if (has(e, "connection refused") || has(e, "network"))
        hint = "Nothing answers at that address: start rigctld (rigctld -m <model> -r <port>) or FlRig (its XML-RPC server, port 12345) first.";
    else if (has(e, "permission") || has(e, "access") || has(e, "busy"))
        hint = "The port cannot be opened: on Linux add yourself to the dialout group (then log in again); close any other program that uses it.";
    else if (has(e, "io error") || has(e, "no such") || has(e, "not found") || has(e, "i/o"))
        hint = "The port is not there: is the rig switched on and the USB cable in? On Windows check the COM number in Device Manager.";
    else if (has(e, "protocol"))
        hint = "The rig answers in another protocol: pick the exact model (QRP Labs QDX / QMX use Kenwood TS-480) and check the rig's CAT settings.";
    else if (has(e, "rejected"))
        hint = "The rig refused the command: it may be in a menu or locked, or the mode / frequency is not valid on this rig.";
    else if (has(e, "not available") || has(e, "not implemented") || has(e, "feature"))
        hint = "This rig or its Hamlib driver cannot do that.";
    else if (has(e, "no hamlib"))
        hint = "This build has no Hamlib: install the package built with Hamlib (or brew install hamlib / libhamlib-dev and build again).";
    return hint ? err + ". " + hint : err;
}

std::vector<NetworkRigPreset> networkRigPresets(const std::vector<RigModelInfo>& all) {
    int rigctl = modelIdFor(all, "Hamlib", "NET rigctl"), flrig = modelIdFor(all, "FLRig", "FLRig");
    if (!rigctl) rigctl = 2;
    if (!flrig) flrig = 4;
    return {{"rigctld (Hamlib network)", rigctl, "localhost:4532"}, {"FlRig", flrig, "localhost:12345"}};
}

// ---- finding the rig ----
const std::vector<RigHint>& commonRigs() {
    static const std::vector<RigHint> v = {
        {"Icom", "IC-7300", {115200, 19200}},  {"Icom", "IC-705", {115200, 19200}},   {"Icom", "IC-9700", {115200, 19200}},
        {"Icom", "IC-7610", {115200, 19200}},  {"Icom", "IC-7100", {19200}},          {"Yaesu", "FT-991", {38400, 4800}},
        {"Yaesu", "FTDX-10", {38400}},         {"Yaesu", "FT-710", {38400}},          {"Yaesu", "FT-891", {38400, 4800}},
        {"Yaesu", "FT-817", {38400, 9600}},    {"Kenwood", "TS-590SG", {115200, 9600}}, {"Kenwood", "TS-890S", {115200}},
        {"Kenwood", "TS-2000", {57600, 9600}}, {"Elecraft", "K3", {38400}},           {"Elecraft", "KX3", {38400}},
        {"Elecraft", "KX2", {38400}},          {"Elecraft", "K4", {38400}},           {"Xiegu", "G90", {19200}},
        {"Xiegu", "X6100", {19200, 115200}},   {"QRP Labs", "QDX", {115200, 9600}},   {"Kenwood", "TS-480", {115200, 9600}},
    };
    return v;
}

std::vector<RigHint> rigHintsForPort(const SerialPortInfo& p) {
    const std::string s = squash(p.path + " " + p.display);
    std::vector<RigHint> out;
    auto add = [&](const char* mfg, const char* model) {
        for (const auto& c : commonRigs())
            if (c.mfg == mfg && c.model == model) { for (const auto& o : out) if (o.model == c.model) return; out.push_back(c); return; }
        out.push_back({mfg, model, {}});
    };
    static const struct { const char* key; const char* mfg; const char* model; } kNamed[] = {
        {"ic7300", "Icom", "IC-7300"},   {"ic705", "Icom", "IC-705"},       {"ic9700", "Icom", "IC-9700"},  {"ic7610", "Icom", "IC-7610"},
        {"ic7100", "Icom", "IC-7100"},   {"ft991", "Yaesu", "FT-991"},      {"ftdx10", "Yaesu", "FTDX-10"}, {"ft710", "Yaesu", "FT-710"},
        {"ft891", "Yaesu", "FT-891"},    {"ts590", "Kenwood", "TS-590SG"},  {"ts890", "Kenwood", "TS-890S"}, {"ts2000", "Kenwood", "TS-2000"},
        {"kx3", "Elecraft", "KX3"},      {"kx2", "Elecraft", "KX2"},        {"elecraftk4", "Elecraft", "K4"}, {"x6100", "Xiegu", "X6100"},
        {"xiegu", "Xiegu", "G90"},       {"qdx", "QRP Labs", "QDX"},        {"qmx", "QRP Labs", "QDX"},
    };
    for (const auto& n : kNamed) if (has(s, n.key)) add(n.mfg, n.model);
    if (has(s, "qdx") || has(s, "qmx") || has(s, "qrplabs")) add("Kenwood", "TS-480");   // the QRP Labs rigs speak TS-480 CAT
    if (has(s, "elecraft")) { add("Elecraft", "K3"); add("Elecraft", "KX3"); add("Elecraft", "KX2"); add("Elecraft", "K4"); }
    if (has(s, "cp2105")) { add("Yaesu", "FT-991"); add("Yaesu", "FTDX-10"); add("Yaesu", "FT-710"); add("Yaesu", "FT-891"); }   // Yaesu's dual UART
    if (has(s, "cp210") && !has(s, "cp2105")) { add("Icom", "IC-7300"); add("Icom", "IC-9700"); add("Icom", "IC-7610"); add("Kenwood", "TS-590SG"); }
    if (has(s, "ch340") || has(s, "ch341")) { add("Xiegu", "G90"); add("Xiegu", "X6100"); }
    return out;
}

std::vector<std::pair<int, int>> rigDetectPlan(const std::vector<RigModelInfo>& all, const std::vector<RigHint>& hints, int maxTries) {
    std::vector<std::pair<int, int>> plan;
    auto addHint = [&](const RigHint& h) {
        const int id = modelIdFor(all, h.mfg, h.model);
        if (!id) return;
        std::vector<int> bauds = h.bauds;
        if (bauds.empty()) { const int i = findRigModel(all, id); const int b = i >= 0 ? rigDefaultBaud(all[i]) : 0; bauds.push_back(b); }
        for (int b : bauds) {
            if ((int)plan.size() >= maxTries) return;
            if (std::find(plan.begin(), plan.end(), std::make_pair(id, b)) == plan.end()) plan.push_back({id, b});
        }
    };
    for (const auto& h : hints) addHint(h);
    for (const auto& h : commonRigs()) addHint(h);
    return plan;
}

RigDetectResult detectRig(const std::vector<std::pair<int, int>>& plan, const std::function<bool(int, int)>& probe, const std::vector<RigModelInfo>& all) {
    RigDetectResult r;
    for (const auto& [model, baud] : plan) {
        r.tries++;
        if (!probe(model, baud)) continue;
        r.found = true; r.model = model; r.baud = baud;
        const int i = findRigModel(all, model);
        const std::string label = i >= 0 ? rigModelLabel(all[i]) : "model " + std::to_string(model);
        const std::string mfg = i >= 0 ? squash(all[i].mfg) : std::string();
        r.note = "answered as " + label + (baud > 0 ? " at " + std::to_string(baud) + " baud" : std::string());
        if (mfg == "kenwood" || mfg == "yaesu" || mfg == "elecraft" || mfg == "qrplabs")
            r.note += "; rigs of this make share CAT commands, so check that the model is yours";
        return r;
    }
    r.note = "no rig answered: check the cable and that the rig is on, then choose the model by hand";
    return r;
}

bool hamlibProbe(int model, const std::string& path, int baud, std::string& err) {
    HamlibLink l;
    HamlibConn c;
    c.model = model; c.path = path; c.baud = baud; c.timeoutMs = 400; c.retries = 0;
    if (!l.open(c, err)) return false;
    double hz = 0;
    const bool ok = l.getFreq(hz, err) && hz > 0;
    l.close();
    return ok;
}

int pairRigAudio(const HamlibRig& r, const std::vector<RigModelInfo>& all, const std::vector<std::string>& outputs) {
    const int i = findRigModel(all, r.model);
    const std::string model = i >= 0 ? squash(all[i].model) : std::string();
    auto find = [&](auto pred) { for (size_t k = 0; k < outputs.size(); k++) if (pred(squash(outputs[k]))) return (int)k; return -1; };
    int k = -1;
    if (model.size() >= 3) k = find([&](const std::string& o) { return o.find(model) != std::string::npos; });
    if (k < 0) k = find([](const std::string& o) { return has(o, "qdx") || has(o, "qmx") || has(o, "truusb") || has(o, "digirig"); });
    if (k < 0) k = find([](const std::string& o) { return has(o, "usbaudiocodec"); });   // the TI PCM2903 in Icom / Yaesu / Kenwood rigs
    if (k < 0) k = find([](const std::string& o) { return has(o, "usbpnpsound") || has(o, "usbaudiodevice") || has(o, "cmedia"); });   // interface cables
    return k;
}

// ---- the real CAT ----
namespace {
class HamlibCat : public RigCat {
public:
    bool open(const HamlibConn& c, std::string& err) override { return l_.open(c, err); }
    void close() override { l_.close(); }
    bool setFreq(double hz, std::string& err) override { return l_.setFreq(hz, err); }
    bool getFreq(double& hz, std::string& err) override { return l_.getFreq(hz, err); }
    bool setMode(const std::string& m, std::string& err) override { return l_.setMode(m, 0, err); }
    bool setPtt(bool on, std::string& err) override { return l_.setPtt(on, err); }
    bool setSplit(bool on, double hz, std::string& err) override { return l_.setSplit(on, hz, err); }
    bool getSplitFreq(double& hz, std::string& err) override { return l_.getSplitFreq(hz, err); }
private:
    HamlibLink l_;
};
}  // namespace
std::unique_ptr<RigCat> makeHamlibCat() { return std::make_unique<HamlibCat>(); }

// ---- RigKeeper ----
void RigKeeper::setProfile(const HamlibRig& r) { std::lock_guard<std::mutex> l(mu_); prof_ = r; }
HamlibRig RigKeeper::profile() const { std::lock_guard<std::mutex> l(mu_); return prof_; }
RigKeeper::State RigKeeper::state() const { std::lock_guard<std::mutex> l(mu_); return st_; }
std::string RigKeeper::status() const { std::lock_guard<std::mutex> l(mu_); return status_; }
double RigKeeper::dialHz() const { std::lock_guard<std::mutex> l(mu_); return dial_; }
bool RigKeeper::keyed() const { std::lock_guard<std::mutex> l(mu_); return keyed_; }

HamlibConn RigKeeper::conn() const {
    HamlibConn c;
    c.model = prof_.model; c.path = prof_.path; c.baud = prof_.baud;
    c.pttType = prof_.pttType; c.pttPath = prof_.pttPath;
    return c;
}

void RigKeeper::fail(const std::string& e, double now) {
    cat_->close();
    st_ = Down;
    status_ = hamlibErrorHint(e.empty() ? std::string("the rig stopped answering") : e);
    retryAt_ = now + backoff_;
}

bool RigKeeper::connect(std::string& err) {
    std::lock_guard<std::mutex> l(mu_);
    wanted_ = true;
    cat_->close();
    std::string e;
    if (prof_.model <= 0 || prof_.path.empty()) e = "choose the rig model and its port first";
    else if (cat_->open(conn(), e)) {   // a port that opens is not yet a rig: it must answer a read
        double hz = 0;
        if (cat_->getFreq(hz, e)) {
            dial_ = hz;
            st_ = Up; status_.clear(); backoff_ = 1; lastPing_ = now_;
            return true;
        }
    }
    fail(e, now_);
    err = status_;
    return false;
}

void RigKeeper::disconnect() {
    std::lock_guard<std::mutex> l(mu_);
    wanted_ = false;
    if (keyed_ && prof_.pttType != 3) { std::string e; cat_->setPtt(false, e); }
    keyed_ = false;
    cat_->close();
    st_ = Off;
    status_.clear();
}

void RigKeeper::tick(double nowSec, double pingSec) {
    std::lock_guard<std::mutex> l(mu_);
    now_ = nowSec;
    if (!wanted_ || keyed_) return;
    if (st_ == Up && nowSec - lastPing_ >= pingSec) {
        lastPing_ = nowSec;
        double hz = 0;
        std::string e;
        if (cat_->getFreq(hz, e)) dial_ = hz;
        else fail(e, nowSec);
    } else if (st_ == Down && nowSec >= retryAt_) {
        std::string e;
        if (cat_->open(conn(), e)) { st_ = Up; status_.clear(); backoff_ = 1; lastPing_ = -1e9; }
        else { backoff_ = std::min(30.0, backoff_ * 2); fail(e, nowSec); }
    }
}

bool RigKeeper::tuneForTx(double hz, const std::string& mode, std::string& err, double tolHz) {
    std::lock_guard<std::mutex> l(mu_);
    if (st_ != Up) { err = "the rig is not connected" + (status_.empty() ? std::string() : ": " + status_); return false; }
    std::string e;
    double back = 0;
    if (prof_.split) {
        if (!cat_->setSplit(true, hz, e) || !cat_->getSplitFreq(back, e)) { fail(e, now_); err = status_; return false; }
    } else {
        if (!cat_->setFreq(hz, e) || !cat_->getFreq(back, e)) { fail(e, now_); err = status_; return false; }
        dial_ = back;
    }
    if (std::fabs(back - hz) > tolHz) {
        char b[160];
        snprintf(b, sizeof b, "the rig is at %.0f Hz, not at %.0f Hz: not sent (check the rig's VFO and band)", back, hz);
        err = b;
        return false;
    }
    if (!mode.empty() && !cat_->setMode(mode, e)) {   // a rig without a data mode: plain USB carries the same audio
        std::string e2;
        if (mode == "USB" || !cat_->setMode("USB", e2)) { err = hamlibErrorHint(e); return false; }
    }
    return true;
}

bool RigKeeper::checkWhileKeyed(double nowSec, double intervalSec, std::string& err) {
    std::lock_guard<std::mutex> l(mu_);
    if (!keyed_ || prof_.pttType == 3 || st_ != Up) return st_ != Down;   // VOX: the CAT link may not even be in use
    if (nowSec - keyedPing_ < intervalSec) return true;
    keyedPing_ = nowSec;
    double hz = 0;
    std::string e;
    if (cat_->getFreq(hz, e)) return true;
    st_ = Down;   // the port stays open: PTT off is still tried on it
    status_ = hamlibErrorHint(e);
    retryAt_ = now_ + backoff_;
    err = status_;
    return false;
}

bool RigKeeper::ptt(bool on, std::string& err) {
    std::lock_guard<std::mutex> l(mu_);
    if (prof_.pttType == 3) { keyed_ = on; return true; }   // VOX: the rig keys itself from the audio
    if (on) {
        if (st_ != Up) { err = "the rig is not connected" + (status_.empty() ? std::string() : ": " + status_); return false; }
        std::string e;
        if (!cat_->setPtt(true, e)) { std::string e2; cat_->setPtt(false, e2); err = hamlibErrorHint(e); return false; }
        keyed_ = true;
        keyedPing_ = -1e9;
        return true;
    }
    keyed_ = false;
    std::string e;
    if (cat_->setPtt(false, e)) return true;
    std::string e2;
    if (cat_->setPtt(false, e2)) return true;                // once more
    cat_->close();                                             // closing the port drops RTS / DTR
    st_ = Down;
    status_ = "PTT release failed (" + e + "): switch the rig to receive by hand";
    retryAt_ = now_ + backoff_;
    err = status_;
    return false;
}

// ---- the sound card ----
namespace {
class SoundCardAudio : public RigAudio {
public:
    bool start(const std::string& device, int rate, std::string& err) override {
        out_.setDevice(device);
        if (!out_.start(rate)) { err = "cannot open the sound card output" + (device.empty() ? std::string() : " " + device); return false; }
        out_.setStartThreshold(rate / 20);
        return true;
    }
    int write(const float* m, int n) override {
        st_.resize((size_t)n * 2);
        for (int i = 0; i < n; i++) st_[2 * i] = st_[2 * i + 1] = m[i];
        return out_.write(st_.data(), n);
    }
    int buffered() override { return out_.bufferedFrames(); }
    void flush() override { out_.flush(); }
    void stop() override { out_.stop(); }
private:
    AudioOut out_;
    std::vector<float> st_;
};
}  // namespace
std::unique_ptr<RigAudio> makeSoundCardAudio() { return std::make_unique<SoundCardAudio>(); }

// ---- CatRigIo ----
bool CatRigIo::prepare(const ham::HamTxCheck& gate, std::string& err) {
    {
        std::lock_guard<std::mutex> l(mu_);
        prepared_ = false;
        err_.clear();
    }
    std::string gw;
    if (!ham::hamTxAllowed(gate, gw)) { err = gw; return false; }
    err.clear();
    if (rig_.state() != RigKeeper::Up && !rig_.connect(err)) return false;
    const HamlibRig p = rig_.profile();
    if (!rig_.tuneForTx(gate.dialHz, p.dataMode.empty() ? "PKTUSB" : p.dataMode, err)) return false;
    std::lock_guard<std::mutex> l(mu_);
    prepared_ = true;
    return true;
}

bool CatRigIo::ptt(bool on, std::string& err) {
    if (on) {
        std::lock_guard<std::mutex> l(mu_);
        if (!prepared_) { err = "the rig is keyed only through the ham transmitter (Arm), not directly"; return false; }
    } else {
        std::lock_guard<std::mutex> l(mu_);
        prepared_ = false;
    }
    return rig_.ptt(on, err);
}

bool CatRigIo::beginAudio(int rate, std::string& err) {
    drained_ = false;
    return audio_->start(rig_.profile().audioOutId, rate, err);
}

int CatRigIo::write(const float* mono, int frames) {
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    std::string e;
    if (rig_.state() == RigKeeper::Down || !rig_.checkWhileKeyed(now, checkSec_, e)) {
        std::lock_guard<std::mutex> l(mu_);
        err_ = "the rig stopped answering: " + (e.empty() ? rig_.status() : e);
        return -1;
    }
    return audio_->write(mono, frames);
}

int CatRigIo::pending() {
    const int b = audio_->buffered();
    if (b == 0 && !drained_) { drained_ = true; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }   // the device's own buffer
    return b;
}

void CatRigIo::flushAudio() { audio_->flush(); }
void CatRigIo::endAudio() {
    audio_->stop();
    std::lock_guard<std::mutex> l(mu_);
    prepared_ = false;
}

} // namespace dect2
