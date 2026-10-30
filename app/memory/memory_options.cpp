// What a memory captures from a receiver and how it is applied again (see memory_options.h). The option table lists the App fields of the
// modes that keep their settings there; modes with state of their own register theirs (the general receiver: general_ui.cpp).
#include "memory_options.h"
#include "../ui/toolbar_internal.h"
#include <algorithm>
#include <cmath>
#include <ctime>

namespace {

std::vector<MemoryOption>& table() { static std::vector<MemoryOption> t; return t; }
std::string b2s(bool v) { return v ? "1" : "0"; }
int s2i(const std::string& v, int def = 0) { char* e = nullptr; const long x = strtol(v.c_str(), &e, 10); return e == v.c_str() ? def : (int)x; }

struct Builtin {
    Builtin() {
        auto add = [](MemoryOption o) { table().push_back(o); };
        add({"fmDeemph", [](const App& a) { return a.fmMode; }, [](const App& a) { return std::to_string(a.fmDeemph); }, [](App& a, const std::string& v) { a.fmDeemph = s2i(v) == 75 ? 75 : 50; }});
        add({"fmMono", [](const App& a) { return a.fmMode; }, [](const App& a) { return b2s(a.fmMono); }, [](App& a, const std::string& v) { a.fmMono = v == "1"; a.fmStereo = a.fmMono ? 1 : 0; }});
        add({"fmChanKhz", [](const App& a) { return a.fmMode; }, [](const App& a) { return std::to_string(a.fmChanKhz); },
             [](App& a, const std::string& v) { const int k = s2i(v); a.fmChanKhz = k <= 0 ? 0 : std::max(10, k); }});
        add({"fmRegion", [](const App& a) { return a.fmMode; }, [](const App& a) { return std::to_string(a.fmRegion); }, [](App& a, const std::string& v) { a.fmRegion = std::max(0, std::min(2, s2i(v))); }});
        add({"pagerNarrow", [](const App& a) { return a.family == 23; }, [](const App& a) { return b2s(a.pagerNarrow); }, [](App& a, const std::string& v) { a.pagerNarrow = v == "1"; }});
        add({"packetNarrow", [](const App& a) { return a.family == 24; }, [](const App& a) { return b2s(a.packetNarrow); }, [](App& a, const std::string& v) { a.packetNarrow = v == "1"; }});
        add({"dtmbBwMhz", [](const App& a) { return a.family == 7; }, [](const App& a) { return std::to_string(a.dtmbBwMhz); }, [](App& a, const std::string& v) { setDtmbBandwidth(a, s2i(v) == 6 ? 6 : 8); }});
        add({"isdbtBwMhz", [](const App& a) { return a.family == 4; }, [](const App& a) { return std::to_string(a.isdbtBwMhz); },
             [](App& a, const std::string& v) { const int m = s2i(v); setIsdbtBandwidth(a, m == 7 || m == 8 || m == 0 ? m : 6); }});
        // the DVB-T2 / DVB-T channel width: "auto" or the index into kBw
        add({"dvbBw", [](const App& a) { return a.family == 0; }, [](const App& a) { return a.bwAuto ? std::string("auto") : std::to_string(a.bwIdx); },
             [](App& a, const std::string& v) { const int n = (int)(sizeof kBw / sizeof *kBw); const int i = s2i(v, -1); setDvbBandwidth(a, v == "auto" || i < 0 || i >= n ? -1 : i); }});
        add({"atvFm", [](const App& a) { return a.family == 8; }, [](const App& a) { return b2s(a.atvFm); }, [](App& a, const std::string& v) { a.atvFm = v == "1"; }});
        add({"atvNbtv", [](const App& a) { return a.family == 8; }, [](const App& a) { return std::to_string(a.atvNbtv); }, [](App& a, const std::string& v) { a.atvNbtv = std::max(0, std::min(2, s2i(v))); }});
    }
};
const Builtin gBuiltin;

dect2::MemoryBank gBank;
bool gLoaded = false;
std::string gPath;

}  // namespace

const std::vector<MemoryOption>& memoryOptions() { return table(); }
MemoryOptionReg::MemoryOptionReg(MemoryOption o) { table().push_back(o); }

std::string memoryModeKey(const ModeDef& m) { return scanModeKey(m); }
int memoryModeIndex(const std::string& key) { return scanModeIndex(key); }

dect2::Memory memoryCapture(const App& a) {
    dect2::Memory m;
    m.freqHz = (uint64_t)std::llround(a.freqMhz * 1e6);
    if (a.family == 55) { const double hz = generalListenHz(a); if (hz > 0) m.freqHz = (uint64_t)std::llround(hz); }   // the general receiver listens off its dial
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) { m.mode = memoryModeKey(kModes[i]); break; }
    m.family = a.family;
    if (const dect2::SubMode* s = curSub(a)) m.subId = s->id;
    m.stdMode = engineStd(a);
    m.sampleRateHz = savedSampleRate(a);
    m.bandwidthHz = a.tune.bandwidthMhz * 1e6;
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size()) m.radio = a.devices[a.devIdx].name;
    m.hasGain = true; m.lna = a.tune.lnaDb; m.vga = a.tune.vgaDb; m.amp = a.tune.ampOn; m.gainDb = a.tune.gainDb;
    for (const MemoryOption& o : memoryOptions()) if (o.applies(a)) m.options[o.key] = o.get(a);
    return m;
}

std::string memorySuggestName(const App& a) {
    if (a.fmMode && !a.rx.fm.psName.empty()) {   // the station name the RDS sends
        std::string n = a.rx.fm.psName;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (!n.empty()) return n;
    }
    if (a.fmMode && !a.rx.fm.callSign.empty()) return a.rx.fm.callSign;
    std::string mode;
    for (int i = 0; i < kNumModes; i++) if (modeSelected(a, kModes[i])) { mode = kModes[i].name; break; }
    char b[96];
    snprintf(b, sizeof b, "%s %s MHz", mode.c_str(), dect2::memoryFreqMhzText((uint64_t)std::llround(a.freqMhz * 1e6)).c_str());
    return b;
}

namespace {
// The App as the recall's host. The mode is set the way the Scanner does it (setMode, then startReceiver); a running receiver is stopped for that
// and started again at the end, so the frequency, options and gains are all in place when it opens.
struct AppHost : dect2::MemoryHost {
    App& a;
    bool restart = false, wasRunning = false;
    explicit AppHost(App& app) : a(app) { wasRunning = a.engine.running(); }
    bool hasMode(const std::string& id) override { return memoryModeIndex(id) >= 0; }
    bool setMode(const dect2::Memory& m, std::string& note) override {
        const int mi = memoryModeIndex(m.mode);
        if (mi < 0) { note = "unknown mode"; return false; }
        const ModeDef& md = kModes[mi];
        if (txViewFlag()) { note = "leave the transmit view first"; return false; }
        if (modeSelected(a, md)) return true;
        if (a.scanner.progress().running && wasRunning) { note = "a channel scan is using the radio: stop it first"; return false; }
        if (wasRunning) { a.engine.stop(); restart = true; note = std::string("restarted in ") + md.name; }
        ::setMode(a, md.family, subIndexOf(md.subId));
        return true;
    }
    void setOption(const std::string& k, const std::string& v) override {
        for (const MemoryOption& o : memoryOptions()) if (k == o.key && o.applies(a)) { o.set(a, v); return; }   // an option of another mode, or one this build lacks, is ignored
    }
    void setGains(const dect2::Memory& m) override {
        a.tune.lnaDb = m.lna; a.tune.vgaDb = m.vga; a.tune.ampOn = m.amp; a.tune.gainDb = m.gainDb;
        const int slot = modeSlot(a);
        if (slot >= 0 && slot < kNumSlots) a.famGain[slot] = {m.lna, m.vga, m.amp, true};   // what the mode's own gain memory would hold
        if (a.engine.running() && !restart) a.engine.retune(a.tune);
    }
    void setSampleRate(double hz) override {
        if (std::fabs(hz - savedSampleRate(a)) < 1) return;
        saveSampleRate(a, hz);
        if (a.engine.running() && !restart) { a.engine.stop(); restart = true; }   // the rate is read when the radio opens
    }
    void setFrequencyHz(uint64_t hz) override {
        const double mhz = (double)hz / 1e6;
        if (a.family == 55) generalRecallHz(a, (double)hz);
        if (a.engine.running() && !restart) { tuneFreq(a, mhz); return; }
        a.freqMhz = std::min(dect2::kMaxTuneMhz, std::max(0.001, mhz));
        a.tune.centerHz = a.freqMhz * 1e6;
        savePrefs(a);
    }
};
}  // namespace

bool memoryRecallApp(App& a, const dect2::Memory& m, bool withMode, std::string& note) {
    AppHost h(a);
    const dect2::RecallResult r = dect2::memoryRecall(h, m, withMode);
    note = r.note;
    if (!r.ok) return false;
    if (h.restart) startReceiver(a);
    else savePrefs(a);
    if (note.empty() && !h.wasRunning) note = r.modeChanged ? "set up for the next start" : "";
    return true;
}

// ---- the shared bank
dect2::MemoryBank& memoryBank() {
    if (!gLoaded) {
        gLoaded = true;
        const dect2::MemoryLoadReport r = gBank.load(memoryFilePath());
        for (const std::string& l : r.log) fprintf(stderr, "memories: %s\n", l.c_str());
    }
    return gBank;
}
void memoryChanged() {
    std::string err;
    if (!memoryBank().save(memoryFilePath(), &err)) fprintf(stderr, "memories: %s\n", err.c_str());
}
const std::string& memoryFilePath() {
    if (gPath.empty()) gPath = plat::dataDir() + "/memories.json";
    return gPath;
}
void memoryTestSetPath(const std::string& path) { gPath = path; gLoaded = false; gBank.clear(); }
