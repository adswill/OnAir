// The remote-control API inside the app (core/src/net/remote_api.cpp is the server): the host that carries the commands out on the main thread, the
// published telemetry, the preferences and the settings panel (in View > Data feeds). Off by default; RX 1 only.
#include "../app.h"
#include "../panels/tx_state.h"
#include "dect2/ham/ham.h"
#include "dect2/net/map_guard.h"
#include "../ui/typography.h"
#include "../state/listen_freq.h"
#include "dect2/automation/engine.h"
#include "dect2/general/general_rx.h"
#include "dect2/net/api_policy.h"
#include "dect2/net/api_servers.h"
#include "dect2/net/remote_api.h"
#include "../memory/memory_options.h"
#include <cmath>
#include <filesystem>
#include "dect2/results/rx_json.h"
#include "dect2/results/sessions_json.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>

using namespace dect2;

namespace {

double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

const char* kindName(DeviceInfo::Kind k) {
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

// The warnings that go with a transmit command through the API (the command is the operator's decision; nothing is refused)
std::string txApiWarnings(const DeviceInfo* d, double hz, double gainDb, bool amp, bool dry) {
    std::vector<std::string> w;
    if (!dry) {
        w.push_back("You are responsible for the law where you are and for your hardware; overdriving or a long duty cycle can damage the radio.");
        if (!dect2::ham::hamSignalInBand(hz, 0, 0)) w.push_back("The frequency is outside the amateur bands: you need a licence or permission for it.");
        if (d && d->txGainMaxDb > d->txGainMinDb && gainDb >= d->txGainMaxDb - 3) w.push_back("The TX gain is at the top of the range: the signal will splatter.");
        if (d && (gainDb < d->txGainMinDb - 1e-9 || (d->txGainMaxDb > d->txGainMinDb && gainDb > d->txGainMaxDb + 1e-9))) w.push_back("The TX gain is outside the radio's range.");
        if (amp) w.push_back("The amplifier is on: overdriving can damage the radio.");
        w.push_back("Connect an antenna, an attenuator or a dummy load, and a band filter for the band.");
        w.push_back("While you transmit, and for 10 minutes after, nothing is uploaded to the OnAir map server.");
    }
    std::string o = "[";
    for (size_t i = 0; i < w.size(); i++) o += (i ? "," : "") + std::string("\"") + jsonEscape(w[i]) + "\"";
    return o + "]";
}

struct Host : RemoteApiHost {
    // ---- main-thread hand-over: the server's threads queue a job, tick() (every frame) runs it
    struct Job {
        std::function<bool(App&, std::string&)> fn;
        std::mutex m;
        std::condition_variable cv;
        bool running = false, done = false, cancelled = false, ok = false;
        std::string err;
    };
    std::mutex qMu;
    std::deque<std::shared_ptr<Job>> q;
    std::atomic<double> lastTick{-1e9};
    std::atomic<bool> shutdown{false};
    // the published telemetry
    std::mutex snapMu;
    std::string telJson = "{\"running\":false,\"available\":false}";
    uint64_t telSeq = 0;

    bool run(const std::function<bool(App&, std::string&)>& fn, std::string& err) {
        if (shutdown) { err = "the application is closing"; return false; }
        if (steadyNow() - lastTick > 1.5) { err = "the application is not responding (is its window minimised or busy?)"; return false; }
        auto j = std::make_shared<Job>();
        j->fn = fn;
        { std::lock_guard<std::mutex> lk(qMu); if (q.size() > 64) { err = "too many requests waiting"; return false; } q.push_back(j); }
        std::unique_lock<std::mutex> lk(j->m);
        if (!j->cv.wait_for(lk, std::chrono::seconds(3), [&] { return j->done || shutdown.load(); })) {
            if (!j->running) { j->cancelled = true; err = "the application is busy, try again"; return false; }
            j->cv.wait(lk, [&] { return j->done; });   // it has started: wait for its result rather than report a failure for something that happens
        }
        if (!j->done) { err = "the application is closing"; return false; }
        err = j->err;
        return j->ok;
    }
    // for the *Json calls
    std::string query(const std::function<std::string(App&)>& f) {
        std::string out, err;
        const bool ok = run([&](App& a, std::string&) { out = f(a); return true; }, err);
        return ok ? out : "{\"error\":\"" + jsonEscape(err) + "\"}";
    }

    void tick(App& a, bool on) {
        lastTick = steadyNow();
        std::deque<std::shared_ptr<Job>> todo;
        { std::lock_guard<std::mutex> lk(qMu); todo.swap(q); }
        for (auto& j : todo) {
            std::unique_lock<std::mutex> lk(j->m);
            if (j->cancelled) continue;
            j->running = true;
            lk.unlock();
            std::string err;
            const bool ok = j->fn(a, err);
            lk.lock();
            j->ok = ok; j->err = err; j->done = true;
            j->cv.notify_all();
        }
        if (!on) return;
        const bool running = a.engine.running();
        const uint64_t pub = a.rx.seq * 2 + (running ? 1 : 0);
        std::lock_guard<std::mutex> lk(snapMu);
        if (pub == telSeq && telJson.size() > 40) return;
        telSeq = pub;
        std::string j = rxTelemetryJson(a.rx, &a.quality.report());
        telJson = std::string("{\"running\":") + (running ? "true" : "false") + ",\"available\":" + (a.rx.seq ? "true" : "false") + "," + j.substr(1);
    }
    void abort() {
        shutdown = true;
        std::lock_guard<std::mutex> lk(qMu);
        for (auto& j : q) { std::lock_guard<std::mutex> l2(j->m); j->cancelled = true; j->cv.notify_all(); }
        q.clear();
    }

    // ---- the rest of the control surface (docs/REMOTE-API.md). Nothing here transmits or touches keys, credentials or security settings.
    apipol::EventRing ring{512};
    std::string sessionsJson(const std::string& q) override { return dect2::sessionsJson(q); }
    std::string memoriesJson() override {
        return query([](App&) {
            std::string o = "{\"memories\":[";
            size_t n = 0;
            for (const dect2::Memory& m : memoryBank().all()) {
                if (n++ >= 2000) break;
                if (n > 1) o += ',';
                o += "{\"id\":" + std::to_string(m.id) + ",\"name\":\"" + jsonEscape(m.name) + "\",\"freqHz\":" + std::to_string(m.freqHz) + ",\"mode\":\"" + jsonEscape(m.mode) + "\",\"group\":\"" + jsonEscape(m.group) +
                     "\",\"tags\":\"" + jsonEscape(m.tags) + "\"}";
            }
            return o + "]}";
        });
    }
    std::string schedulerJson() override { return query([](App&) { return tvRecJobsJson(); }); }
    std::string audioJson() override {
        return query([](App& a) { char b[96]; snprintf(b, sizeof b, "{\"volume\":%.3f,\"muted\":%s}", (double)a.volume, a.muted ? "true" : "false"); return std::string(b); });
    }
    std::string recordingJson() override {
        return query([](App& a) {
            const dect2::RecordingStats st = a.engine.recordingStats();
            std::string name = st.path.substr(st.path.find_last_of("/\\") == std::string::npos ? 0 : st.path.find_last_of("/\\") + 1);   // the file name only
            char b[160];
            snprintf(b, sizeof b, "{\"iq\":{\"active\":%s,\"seconds\":%.1f,\"bytes\":%llu,\"file\":\"", st.active ? "true" : "false", st.seconds, (unsigned long long)st.bytes);
            return std::string(b) + jsonEscape(name) + "\"},\"audio\":{\"active\":" + (a.engine.general().recording() ? "true" : "false") + "}}";
        });
    }
    bool setVolume(double v, std::string& err) override { return run([v](App& a, std::string&) { a.volume = (float)std::max(0.0, std::min(1.0, v)); return true; }, err); }
    bool setMute(bool on, std::string& err) override { return run([on](App& a, std::string&) { a.muted = on; return true; }, err); }
    bool setSquelch(bool on, bool au, double db, std::string& err) override {
        return run([=](App& a, std::string& e) {
            if (a.family != 55) { e = "the squelch setting is for the General receiver; select that mode first"; return false; }
            generalApiSquelch(on, au, db);
            return true;
        }, err);
    }
    bool setBandwidthHz(double hz, std::string& err) override {
        return run([hz](App& a, std::string& e) {
            if (isGeneralFamily(a.family)) { e = "the General receiver has its own filter widths in the app"; return false; }
            a.engine.setBandwidthAuto(false);
            a.engine.setBandwidth(hz / 1e6);
            return true;
        }, err);
    }
    bool setAutoGain(bool on, std::string& err) override {
        return run([on](App& a, std::string& e) {
            if (!a.devices[(size_t)a.devIdx].isRadio()) { e = "this source has no gain control"; return false; }
            a.agcOn = on;
            savePrefs(a);
            return true;
        }, err);
    }
    bool recordStart(const std::string& kind, std::string& err) override {
        return run([kind](App& a, std::string& e) {
            if (!a.engine.running()) { e = "the receiver is not running"; return false; }
            if (kind == "audio") {
                if (a.family != 55) { e = "audio recording is for the General receiver"; return false; }
                return generalApiRecord(a, true, e);
            }
            if (a.engine.recordingStats().active) { e = "already recording"; return false; }
            const dect2::FileFormat f = a.recFormat == 1 ? dect2::FileFormat::CF32 : dect2::FileFormat::CS8;
            std::error_code ec;
            std::filesystem::create_directories(plat::dataDir() + "/recordings", ec);
            if (!a.engine.startRecording(plat::dataDir() + "/recordings/" + a.engine.recordingName(f), f, e)) return false;
            a.recDonePath.clear();
            return true;
        }, err);
    }
    bool recordStop(std::string& err) override {
        return run([](App& a, std::string&) {
            std::string ignored;
            if (a.engine.recordingStats().active) a.engine.stopRecording();
            if (a.engine.general().recording()) generalApiRecord(a, false, ignored);
            return true;
        }, err);
    }
    // ---- transmit: the Transmit window's state, started and stopped by command; the answer carries the warnings
    std::string txStatusJson() override {
        return query([](App& a) {
            txui::TxUi& s = txui::ui();
            const bool on = s.session.running();
            const double left = dect2::mapguard::holdLeftSec(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
            char b[120];
            snprintf(b, sizeof b, ",\"frequencyHz\":%.0f,\"gainDb\":%.1f,\"mapHoldSeconds\":%.0f", s.freqMhz * 1e6, (double)s.gainDb, left > 1e8 ? -1.0 : left);
            (void)a;
            return std::string("{\"transmitting\":") + (on ? "true" : "false") + ",\"mode\":\"" + jsonEscape(s.modeId) + "\",\"radio\":\"" + jsonEscape(s.radioName) + "\",\"error\":\"" +
                   jsonEscape(s.error) + "\"" + b + ",\"mapSharingPaused\":" + (left > 0 ? "true" : "false") + "}";
        });
    }
    bool txStart(const std::map<std::string, JsonVal>& body, std::string& json, std::string& err) override {
        std::string warnings = "[]";
        const bool ok = run([&](App& a, std::string& e) {
            txui::TxUi& s = txui::ui();
            auto str = [&](const char* k, std::string& out) { auto it = body.find(k); if (it == body.end() || it->second.kind != JsonVal::Str) return false; out = it->second.str; return true; };
            auto num = [&](const char* k, double& out) { auto it = body.find(k); if (it == body.end() || it->second.kind != JsonVal::Num) return false; out = it->second.num; return true; };
            auto flag = [&](const char* k, bool& out) { auto it = body.find(k); if (it == body.end() || it->second.kind != JsonVal::Bool) return false; out = it->second.b; return true; };
            if (s.session.running()) { e = "already transmitting: stop it first"; return false; }
            txui::ensureTxMode(a, s);
            std::string id;
            if (str("mode", id)) {
                const dect2::TxModeSpec* m = dect2::txModeById(id);
                if (!m) { e = "no such transmit mode: " + id; return false; }
                txui::chooseMode(a, s, *m);
            }
            const dect2::TxModeSpec* mode = dect2::txModeById(s.modeId);
            if (!mode) { e = "choose a mode: give \"mode\" (a transmit mode id)"; return false; }
            double v = 0; bool f = false; std::string t;
            if (num("frequency_hz", v) || num("hz", v)) s.freqMhz = v / 1e6; else if (num("mhz", v)) s.freqMhz = v;
            if (num("gain_db", v)) { s.gainDb = (float)v; s.gainSet = true; }
            if (flag("amp", f)) s.ampOn = f;
            if (flag("loop", f)) s.loop = f;
            if (num("seconds", v) && v >= 0.1) { s.onceSeconds = (int)v; s.loop = false; }
            if (flag("test", f)) s.dryRun = f;
            auto setc = [&](const char* k, char* dst, size_t n) { if (str(k, t)) snprintf(dst, n, "%s", t.c_str()); };
            setc("callsign", s.callsign, sizeof s.callsign); setc("grid", s.grid, sizeof s.grid); setc("dest", s.dest, sizeof s.dest);
            setc("name", s.name, sizeof s.name); setc("text", s.text, sizeof s.text);
            if (num("device", v) && v >= 0 && (size_t)v < a.devices.size()) s.radioKey = deviceClaimKey(a.devices[(size_t)v]);
            int conflict = 0;
            const DeviceInfo* radio = txui::bindRadio(a, s, conflict);
            if (!txui::noRadio(s)) {
                if (!radio) { e = "no radio that can transmit"; return false; }
                if (conflict) { e = "a receiver holds that radio: stop the receiver first, or use a radio that can do both"; return false; }
            }
            bool carrier = false; flag("carrier", carrier);
            if (!txui::startTx(a, s, radio, mode, carrier)) {
                e = s.askNotice ? "open the Transmit window and read the one-time notice first (it is shown once)" : s.error.empty() ? "the transmission did not start" : s.error;
                return false;
            }
            warnings = txApiWarnings(radio, s.freqMhz * 1e6, s.gainDb, s.ampOn, s.dryRun || s.destKind == 1);
            return true;
        }, err);
        if (ok) json = "{\"ok\":true,\"transmitting\":true,\"warnings\":" + warnings + "}";
        return ok;
    }
    bool txStop(std::string& json, std::string& err) override {
        const bool ok = run([](App& a, std::string&) { txui::stopTx(a, txui::ui(), "stopped through the remote API"); return true; }, err);
        if (ok) json = "{\"ok\":true,\"transmitting\":false}";
        return ok;
    }
    bool eventsSince(uint64_t& cursor, size_t maxN, std::vector<std::string>& out, bool& gap) override { ring.since(cursor, maxN, out, &gap); return true; }

    // ---- the API
    std::string statusJson() override {
        return query([](App& a) {
            const DeviceInfo& d = a.devices[(size_t)std::max(0, std::min(a.devIdx, (int)a.devices.size() - 1))];
            const bool run = a.engine.running();
            std::string mode = "";
            for (int i = 0; i < kNumModes; i++) if (kModes[i].family == a.family && (!kModes[i].subId || subIndexOf(kModes[i].subId) == a.sub)) { mode = kModes[i].name; break; }
            const double gain = d.isGeneric() ? a.tune.gainDb : (double)(a.tune.lnaDb + a.tune.vgaDb + (a.tune.ampOn ? 14 : 0));
            char b[320];
            std::string o = std::string("{\"running\":") + (run ? "true" : "false") + ",\"receiver\":" + std::to_string(a.rxId) + ",\"deviceIndex\":" + std::to_string(a.devIdx) +
                            ",\"device\":\"" + jsonEscape(d.name) + "\",\"deviceKind\":\"" + kindName(d.kind) + "\",\"family\":" + std::to_string(a.family) + ",\"mode\":\"" + jsonEscape(mode) + "\"";
            snprintf(b, sizeof b, ",\"frequencyHz\":%.0f,\"centreHz\":%.0f,\"sampleRateHz\":%.0f,\"gainDb\":%s,\"agc\":%s,\"gainMinDb\":%s,\"gainMaxDb\":%s", listenMhz(a) * 1e6, radioCenterMhz(a) * 1e6,
                     run ? a.engine.radioRate() : a.tune.sampleRate, jsonNum(gain).c_str(), a.agcOn ? "true" : "false",
                     jsonNum(d.isGeneric() ? d.gainMinDb : 0).c_str(), jsonNum(d.isGeneric() ? d.gainMaxDb : (d.kind == DeviceInfo::HackRF ? 116 : 0)).c_str());
            return o + b + ",\"autogain\":" + autogainStatusJson(a) + "}";   // read-only: the mode, meters, advice, recent changes (docs/AUTOGAIN.md)
        });
    }
    std::string devicesJson() override {
        return query([](App& a) {
            std::string o = "[";
            for (size_t i = 0; i < a.devices.size(); i++) {
                const DeviceInfo& d = a.devices[i];
                if (i) o += ',';
                o += "{\"index\":" + std::to_string(i) + ",\"name\":\"" + jsonEscape(d.name) + "\",\"kind\":\"" + kindName(d.kind) + "\",\"board\":\"" + jsonEscape(d.board) + "\"";
                if (d.kind != DeviceInfo::File) o += ",\"serial\":\"" + jsonEscape(d.serial) + "\"";   // a file's "serial" is its path: not given out
                o += ",\"minRateHz\":" + jsonNum(d.minRateHz) + ",\"maxRateHz\":" + jsonNum(d.maxRateHz) + ",\"gainMinDb\":" + jsonNum(d.gainMinDb) + ",\"gainMaxDb\":" + jsonNum(d.gainMaxDb) +
                     ",\"minFreqHz\":" + jsonNum(d.minFreqHz) + ",\"maxFreqHz\":" + jsonNum(d.maxFreqHz) + ",\"selected\":" + (a.devIdx == (int)i ? "true" : "false") + "}";
            }
            return o + "]";
        });
    }
    std::string modesJson() override {
        std::string o = "[";   // constant: no need to go through the main thread
        for (int i = 0; i < kNumModes; i++) {
            if (i) o += ',';
            o += "{\"family\":" + std::to_string(kModes[i].family) + ",\"name\":\"" + jsonEscape(kModes[i].name) + "\",\"sub\":" + (kModes[i].subId ? "\"" + jsonEscape(kModes[i].subId) + "\"" : "null") + "}";
        }
        return o + "]";
    }
    std::string telemetryJson(uint64_t& seq) override {
        std::lock_guard<std::mutex> lk(snapMu);
        seq = telSeq;
        return telJson;
    }
    std::string spectrumJson(int maxBins, uint64_t& seq) override {
        uint64_t sq = 0;
        std::string j = query([&](App& a) {
            if (!a.engine.running() || a.spec.dbfs.empty()) return std::string("{\"running\":false,\"dbfs\":[]}");
            sq = a.spec.seq;
            const std::vector<float>& s = a.spec.dbfs;
            const size_t n = s.size(), out = std::min<size_t>(n, (size_t)std::max(16, maxBins)), grp = (n + out - 1) / out;
            char b[200];
            snprintf(b, sizeof b, "{\"running\":true,\"seq\":%llu,\"centerHz\":%.0f,\"sampleRateHz\":%.0f,\"rmsDbfs\":%s,\"peak\":%s,\"clipFraction\":%s,\"bins\":%zu,\"dbfs\":[",
                     (unsigned long long)a.spec.seq, radioCenterMhz(a) * 1e6, a.engine.radioRate(), jsonNum(a.spec.stats.rmsDbfs).c_str(), jsonNum(a.spec.stats.peak).c_str(),
                     jsonNum(a.spec.stats.clipFraction).c_str(), (n + grp - 1) / grp);
            std::string o = b;
            for (size_t i = 0; i < n; i += grp) {   // the strongest bin of every group, so narrow carriers survive the decimation
                float m = s[i];
                for (size_t k = i + 1; k < std::min(n, i + grp); k++) m = std::max(m, s[k]);
                if (i) o += ',';
                snprintf(b, sizeof b, "%.1f", m);
                o += b;
            }
            return o + "]}";
        });
        seq = sq;
        return j;
    }
    bool start(int dev, std::string& err) override {
        return run([dev](App& a, std::string& e) {
            if (a.engine.running()) { e = "already running"; return false; }
            if (dev >= (int)a.devices.size()) { e = "no such device (see /api/v1/devices)"; return false; }
            if (dev >= 0) a.devIdx = dev;
            startReceiver(a);
            if (!a.engine.running()) { e = "could not start the receiver" + (a.startErr.empty() ? std::string(" (see the log)") : ": " + a.startErr); return false; }
            return true;
        }, err);
    }
    bool stop(std::string& err) override {
        return run([](App& a, std::string&) { if (a.engine.running()) a.engine.stop(); return true; }, err);
    }
    bool setMode(int family, const std::string& sub, std::string& err) override {
        return run([family, sub](App& a, std::string& e) {
            if (a.engine.running()) { e = "stop the receiver before switching mode"; return false; }
            bool known = false;
            for (int i = 0; i < kNumModes; i++) known = known || (kModes[i].family == family && (sub.empty() || (kModes[i].subId && sub == kModes[i].subId)));
            if (!known) { e = "no such mode (see /api/v1/modes)"; return false; }
            selectMode(a, family, sub.empty() ? nullptr : sub.c_str());
            return a.family == family;
        }, err);
    }
    bool setFrequencyHz(double hz, std::string& err) override {
        return run([hz](App& a, std::string& e) {
            if (!(hz >= 0)) { e = "the frequency must be zero or more"; return false; }   // (also NaN) the only refusal: no radio takes a negative frequency
            tuneFreq(a, hz / 1e6);
            return true;
        }, err);
    }
    bool setSampleRateHz(double hz, std::string& err) override {
        return run([hz](App& a, std::string& e) {
            const DeviceInfo& d = a.devices[(size_t)a.devIdx];
            if (d.kind == DeviceInfo::File) { e = "a recording plays at its own rate"; return false; }
            if (hz > 0 && !std::isfinite(hz)) { e = "not a sample rate"; return false; }
            // 3.4: a rate outside what the radio lists is passed on like any typed one (the driver decides); the app puts the old rate back if it refuses
            const double prev = savedSampleRate(a);
            saveSampleRate(a, hz);   // what the "Rate" control does
            const bool restart = a.engine.running() && d.isRadio();
            const double old = a.tune.sampleRate;
            a.tune.centerHz = a.freqMhz * 1e6;
            applyBandwidth(a);
            if (restart && a.tune.sampleRate != old) {
                startReceiver(a); a.mpd.reset(); a.quality.reset();
                if (!a.engine.running() && hz > 0) {   // the radio refused it: its own words, and the previous rate is back
                    e = "the radio refused this sample rate" + (a.engine.startError().empty() ? std::string() : ": " + a.engine.startError());
                    saveSampleRate(a, prev); applyBandwidth(a); startReceiver(a);
                    return false;
                }
            }
            return true;
        }, err);
    }
    bool setGainDb(double db, std::string& err) override {
        return run([db](App& a, std::string& e) {
            const DeviceInfo& d = a.devices[(size_t)a.devIdx];
            if (!d.isRadio()) { e = "this source has no gain control"; return false; }
            if (d.isGeneric()) {
                if (d.gainMaxDb > d.gainMinDb && (db < d.gainMinDb - 0.01 || db > d.gainMaxDb + 0.01)) { e = "outside the gain range of this radio"; return false; }
                a.tune.gainDb = std::round(db);
            } else {
                if (db < 0 || db > 116) { e = "the HackRF's total gain is 0 to 116 dB"; return false; }
                if (!a.tune.ampOn && db > 102) { e = "above 102 dB the amp is needed: switch it on yourself first (it can be damaged by strong signals)"; return false; }
                if (a.tune.ampOn && db < 14) { e = "the amp is on (+14 dB): the total is 14 dB or more"; return false; }
                const GainSetting g = agcModel(a).split((int)std::lround(db), a.tune.ampOn, 40);
                a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp;
            }
            a.agcOn = false;   // a gain that was set by hand is not the AGC's to change
            if (a.engine.running()) { a.tune.centerHz = a.freqMhz * 1e6; a.engine.retune(a.tune); }
            savePrefs(a);
            return true;
        }, err);
    }
};

// The rigctl commands, carried out on the main thread like every other command
struct RigAdapter : apipol::RigctlRadio {
    explicit RigAdapter(Host& h) : host(h) {}
    Host& host;
    static int demodOf(const std::string& m) { return m == "FM" || m == "FMN" || m == "NFM" || m == "PKTFM" ? dect2::GenNfm : m == "WFM" ? dect2::GenWfm : m == "AM" || m == "AMN" || m == "SAM" ? dect2::GenAm : m == "USB" || m == "PKTUSB" ? dect2::GenUsb : m == "LSB" || m == "PKTLSB" ? dect2::GenLsb : m == "CW" || m == "CWR" ? dect2::GenCw : -1; }
    bool getFreq(double& hz) override { std::string e; return host.run([&](App& a, std::string&) { hz = listenMhz(a) * 1e6; return true; }, e); }   // the listening frequency (the VFO), as the big readout
    bool setFreq(double hz) override { std::string e; return host.run([hz](App& a, std::string&) { tuneFreq(a, hz / 1e6); return true; }, e); }
    bool getMode(std::string& m, int& pb) override {
        std::string e;
        return host.run([&](App& a, std::string&) {
            if (a.family != 55) return false;   // only the General receiver has a demodulator that rigctl names
            const std::string n = dect2::generalDemodName(generalApiDemod());
            m = n == "NFM" ? "FM" : n;
            pb = 0;
            return true;
        }, e);
    }
    bool getPtt(bool& on) override { std::string e; return host.run([&](App&, std::string&) { on = txui::ui().session.running(); return true; }, e); }
    bool setPtt(bool on) override {
        std::string json, e;
        if (on) { const std::map<std::string, JsonVal> none; return host.txStart(none, json, e); }
        return host.txStop(json, e);
    }
    bool setMode(const std::string& m, int) override {
        const int d = demodOf(m);
        std::string e;
        return d >= 0 && host.run([d](App& a, std::string&) { return isGeneralFamily(a.family) && generalApiSetDemod(d); }, e);
    }
};

struct State {
    Host host;
    RemoteApiServer srv{host};   // after the host: stopped first
    RemoteApiConfig cfg;
    RigAdapter rig{host};
    RigctlServer rigSrv{rig};
    RigctlConfig rigCfg;
    UdpOut udp;
    UdpOutConfig udpCfg;
    ~State() {
        dect2::automation::global().setStreamer(nullptr, 0);
        host.abort();   // connection threads waiting for the (gone) main thread are released before the server joins them
    }
};
State& st() { static State s; return s; }

// The message stream (WebSocket / polling ring and UDP): one hook on the automation engine while either output is on. It runs on the decoder's thread and only
// formats a line, pushes it into the ring and sends one datagram.
void applyStreamer() {
    State& s = st();
    if (!s.cfg.on && !s.udpCfg.on) { dect2::automation::global().setStreamer(nullptr, 0); return; }
    dect2::automation::global().setStreamer([](const dect2::automation::Event& e) {
        State& t = st();
        const std::string line = apipol::eventLine(e, 0, apipol::kMaxUdpLine - 1);
        if (t.cfg.on) t.host.ring.push(line);
        if (t.udpCfg.on) t.udp.send(line);
    }, dect2::automation::kAllFamilies);
}
void applyAll() {
    State& s = st();
    s.srv.configure(s.cfg);
    s.rigCfg.token = s.cfg.token;   // rigctl uses the API token for "\auth"
    s.rigSrv.configure(s.rigCfg);
    s.udp.configure(s.udpCfg);
    applyStreamer();
}

} // namespace

void remoteApiLoadPrefs(App& a) {
    if (a.rxId != 1) return;
    plat::Prefs& d = plat::prefs();
    State& s = st();
    RemoteApiConfig& c = s.cfg;
    c.on = d.getB("apiOn", false); c.port = (int)d.getI("apiPort", c.port); c.lan = d.getI("apiLan", 0) != 0; c.token = d.getS("apiToken", "");
    c.requireTokenLocal = d.getB("apiLocalAuth", true);   // a program on this computer sends the token too
    c.ratePerSec = 20; c.rateBurst = 40;
    if (!apipol::tokenUsable(c.token)) { c.token = remoteApiNewToken(); d.setS("apiToken", c.token); }   // stored in the preferences only; never logged
    s.rigCfg.on = d.getB("rigctlOn", false); s.rigCfg.port = (int)d.getI("rigctlPort", 4532); s.rigCfg.allowLan = d.getI("rigctlLan", 0) != 0; s.rigCfg.token = c.token;
    s.udpCfg.on = d.getB("udpOn", false); s.udpCfg.host = d.getS("udpHost", "255.255.255.255"); s.udpCfg.port = (int)d.getI("udpPort", 0);
    if (c.on || s.rigCfg.on || s.udpCfg.on) applyAll();
}

void remoteApiSavePrefs(const App& a) {
    if (a.rxId != 1) return;
    plat::Prefs& d = plat::prefs();
    const State& s = st();
    const RemoteApiConfig& c = s.cfg;
    d.setB("apiOn", c.on); d.setI("apiPort", c.port); d.setI("apiLan", c.lan ? 1 : 0); d.setS("apiToken", c.token); d.setB("apiLocalAuth", c.requireTokenLocal);
    d.setB("rigctlOn", s.rigCfg.on); d.setI("rigctlPort", s.rigCfg.port); d.setI("rigctlLan", s.rigCfg.allowLan ? 1 : 0);
    d.setB("udpOn", s.udpCfg.on); d.setS("udpHost", s.udpCfg.host); d.setI("udpPort", s.udpCfg.port);
}

void remoteApiTick(App& a) {
    if (a.rxId != 1) return;
    State& s = st();
    s.host.tick(a, s.cfg.on);
}

void remoteApiSection(App& a) {
    State& s = st();
    RemoteApiConfig& c = s.cfg;
    typo::separatorText(tr("Remote control API (HTTP and WebSocket)"));
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Let scripts and other programs control this receiver"), &c.on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * gUi);
    int port = c.port;
    ImGui::InputInt(tr("Port"), &port, 0, 0);
    c.port = std::max(1, std::min(65535, port));
    ch |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::Indent();
    if (ImGui::Checkbox(tr("Let other computers connect (LAN, needs the token)"), &c.lan)) { ch = true; if (c.lan && c.token.size() < 16) c.token = remoteApiNewToken(); }
    if (c.lan) {
        char b[96];
        snprintf(b, sizeof b, "%s", c.token.c_str());
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputText(tr("Token"), b, sizeof b)) c.token = b;
        ch |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("New token"))) { c.token = remoteApiNewToken(); ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy token"))) ImGui::SetClipboardText(c.token.c_str());
    }
    const RemoteApiStatus ss = s.srv.status();
    if (!c.on) ImGui::TextDisabled("%s", tr("off"));
    else if (!ss.listening) ImGui::TextColored(pal::badRed(), "%s", ss.lastError.empty() ? tr("starting") : ss.lastError.c_str());
    else {
        ImGui::TextColored(pal::okGreen(), tr("listening on %s port %d, %d HTTP and %d WebSocket client(s), %d requests, %d refused"), c.lan ? "all addresses" : "127.0.0.1", s.srv.config().port,
                           ss.httpClients, ss.wsClients, (int)ss.requests, (int)ss.rejected);
        if (!ss.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", ss.lastError.c_str());
    }
    ch |= ImGui::Checkbox(tr("Programs on this computer need the token too"), &c.requireTokenLocal);
    if (c.requireTokenLocal && !c.lan) {   // (with LAN on, the token field above is shown)
        char tb[96];
        snprintf(tb, sizeof tb, "%s", c.token.c_str());
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputText(tr("Token##api"), tb, sizeof tb)) c.token = tb;
        ch |= ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("New token##api"))) { c.token = remoteApiNewToken(); ch = true; }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy token##api"))) ImGui::SetClipboardText(c.token.c_str());
    }
    ImGui::PushTextWrapPos(460 * gUi);
    ImGui::TextDisabled("%s", tr("Programs on this computer need no token; any other address must send it as 'Authorization: Bearer <token>'. The API lists the radios, starts and stops the receiver, "
                                 "sets mode, frequency, sample rate and gain, and gives the telemetry and the spectrum as JSON, also as a WebSocket stream. Endpoints: see README.md."));
    ImGui::PopTextWrapPos();
    ImGui::Unindent();
    // rigctl (Hamlib rigctld subset) and the UDP output of decoded messages
    bool ch2 = false;
    ch2 |= ImGui::Checkbox(tr("rigctl server (Hamlib, for gpredict and logging programs)"), &s.rigCfg.on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    int rp = s.rigCfg.port;
    ImGui::InputInt(tr("rigctl port"), &rp, 0, 0);
    s.rigCfg.port = std::max(1, std::min(65535, rp));
    ch2 |= ImGui::IsItemDeactivatedAfterEdit();
    ch2 |= ImGui::Checkbox(tr("rigctl: let other computers connect (changes need \\auth <token> first)"), &s.rigCfg.allowLan);
    if (s.rigCfg.on && !s.rigSrv.error().empty()) ImGui::TextColored(pal::badRed(), "%s", s.rigSrv.error().c_str());
    ch2 |= ImGui::Checkbox(tr("Send decoded messages as UDP JSON lines (network only, no file; use View > Integrations > Save messages for files)"), &s.udpCfg.on);
    char hb[32];
    snprintf(hb, sizeof hb, "%s", s.udpCfg.host.c_str());
    ImGui::SetNextItemWidth(130 * gUi);
    if (ImGui::InputText(tr("UDP address"), hb, sizeof hb)) s.udpCfg.host = hb;
    ch2 |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * gUi);
    int up = s.udpCfg.port;
    ImGui::InputInt(tr("UDP port"), &up, 0, 0);
    s.udpCfg.port = std::max(0, std::min(65535, up));
    ch2 |= ImGui::IsItemDeactivatedAfterEdit();
    if (s.udpCfg.on && !s.udp.error().empty()) ImGui::TextColored(pal::badRed(), "%s", s.udp.error().c_str());
    if (ch || ch2) { s.srv.configure(c); s.rigCfg.token = c.token; s.rigSrv.configure(s.rigCfg); s.udp.configure(s.udpCfg); applyStreamer(); remoteApiSavePrefs(a); savePrefs(a); }
}
