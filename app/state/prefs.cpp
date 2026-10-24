// Saved settings: loadPrefs() and savePrefs(). The plain keys (a flag, a number with limits, a text) are one table, kKeys,
// that both directions walk, so a key is spelled once; the keys that need real logic (the mode and its gains, the
// spectrum range, the FM station lists, the wide-capture mask) are written out below the table.
#include "../app.h"
#include "state_internal.h"
#include <climits>
#include <cstring>
#include <chrono>

namespace {

enum class Kind { Flag, FlagInt, Int, Scaled, Float, Double, Text, Chars, FlagFn };

// One saved setting. The accessor `at` is typed per kind, so a field of the wrong type does not compile.
struct PrefKey {
    const char* key;
    Kind kind;
    union {
        bool* (*b)(App&); int* (*i)(App&); float* (*f)(App&); double* (*d)(App&); std::string* (*s)(App&); char* (*c)(App&);
    } at;
    double def = 0;          // the value when the key is not stored (ignored when `keep`)
    double lo = 1, hi = 0;   // loaded value is clamped to [lo, hi] (no clamp when lo > hi)
    int scale = 1;           // Scaled: stored as round(value * scale)
    size_t chars = 0;        // Chars: size of the buffer
    bool guard = false;      // load only when the key is stored
    bool keep = false;       // a missing key keeps the current value
    bool early = false;      // loaded before the mode (the mode's start-up uses the frequency and gains)
    bool shared = false;     // saved by receiver 1 only: the other receivers share it
    bool rx1Load = false;    // loaded by receiver 1 only
    int (*fix)(int) = nullptr;   // Int: turns an odd stored number into a valid one
    bool (*getFn)(const App&) = nullptr;   // FlagFn: a setting kept behind a getter / setter (the engine)
    void (*setFn)(App&, bool) = nullptr;
};

#define AT(expr) [](App& a) { return &(expr); }
// builders: the arguments are key, accessor, then what the kind needs; the trailing flags say how it loads and saves
PrefKey flagB(const char* k, bool* (*at)(App&), bool def, bool guard = false, bool keep = false) {
    PrefKey p{k, Kind::Flag, {}}; p.at.b = at; p.def = def; p.guard = guard; p.keep = keep; return p;
}
PrefKey flagI(const char* k, bool* (*at)(App&), bool def, bool guard = true) {   // stored as the number 0 / 1
    PrefKey p{k, Kind::FlagInt, {}}; p.at.b = at; p.def = def; p.guard = guard; return p;
}
PrefKey flagFn(const char* k, bool (*get)(const App&), void (*set)(App&, bool)) {
    PrefKey p{k, Kind::FlagFn, {}}; p.getFn = get; p.setFn = set; return p;
}
PrefKey num(const char* k, int* (*at)(App&), int def, int lo, int hi, bool guard = false, bool keep = false) {
    PrefKey p{k, Kind::Int, {}}; p.at.i = at; p.def = def; p.lo = lo; p.hi = hi; p.guard = guard; p.keep = keep; return p;
}
PrefKey numFix(const char* k, int* (*at)(App&), int def, int (*fix)(int)) {
    PrefKey p{k, Kind::Int, {}}; p.at.i = at; p.def = def; p.guard = true; p.fix = fix; return p;
}
PrefKey scaled(const char* k, float* (*at)(App&), int scale, int def, double lo, double hi, bool guard) {
    PrefKey p{k, Kind::Scaled, {}}; p.at.f = at; p.scale = scale; p.def = def; p.lo = lo; p.hi = hi; p.guard = guard; return p;
}
PrefKey real(const char* k, float* (*at)(App&), double lo, double hi, bool guard = false) {   // a float kept as is, an existing value kept when missing
    PrefKey p{k, Kind::Float, {}}; p.at.f = at; p.lo = lo; p.hi = hi; p.guard = guard; p.keep = true; return p;
}
PrefKey dbl(const char* k, double* (*at)(App&), double def, bool guard = false, bool keep = false) {
    PrefKey p{k, Kind::Double, {}}; p.at.d = at; p.def = def; p.guard = guard; p.keep = keep; return p;
}
PrefKey text(const char* k, std::string* (*at)(App&), bool guard = false) {
    PrefKey p{k, Kind::Text, {}}; p.at.s = at; p.guard = guard; return p;
}
PrefKey chars(const char* k, char* (*at)(App&), size_t n, bool guard = false) {
    PrefKey p{k, Kind::Chars, {}}; p.at.c = at; p.chars = n; p.guard = guard; return p;
}
PrefKey early(PrefKey p) { p.early = true; return p; }
PrefKey shared(PrefKey p) { p.shared = true; return p; }
PrefKey rx1(PrefKey p) { p.rx1Load = true; return p; }

int fixDtmbBw(int v) { return v == 6 ? 6 : 8; }
int fixIsdbtBw(int v) { return v == 0 || v == 7 || v == 8 ? v : 6; }
int fixRegion(int v) { return v == 1 || v == 2 ? v : 0; }
int fixDeemph(int v) { return v == 75 ? 75 : v == 0 ? 0 : 50; }   // 0: off
int fixAudioBw(int v) { return v == 12000 || v == 8000 || v == 5000 ? v : 0; }
int fixChanKhz(int v) { return v > 0 ? std::min(300, std::max(100, v)) : 0; }
int fixTone(int v) { return std::max(-6, std::min(6, v)); }
int fixRecFormat(int v) { return v == 1 || v == 2 ? v : 0; }

// Every plain setting, in the order of the settings file's sections. Keys are the names stored on disk: never rename one.
const std::vector<PrefKey>& keyTable() {
    static const std::vector<PrefKey> t = {
        // ---- this receiver: radio gains and frequency (loaded first), the spectrum, FM, the DVB choices
        shared(early(num("hamlibAddModel", AT(a.hamlibAddModel), 0, 1, INT_MAX, false, true))),
        shared(early(num("hamlibAddBaud", AT(a.hamlibAddBaud), 0, 0, INT_MAX))),
        shared(early(chars("hamlibAddPath", AT(a.hamlibAddPath[0]), sizeof(App::hamlibAddPath)))),
        early(dbl("freqMhz", AT(a.freqMhz), 0, true, true)),
        early(num("lna", AT(a.tune.lnaDb), 0, 1, 0, true, true)),
        early(num("vga", AT(a.tune.vgaDb), 0, 1, 0, true, true)),
        early(dbl("gain", AT(a.tune.gainDb), 0, true, true)),
        early(text("gainDev", AT(a.gainDev))),
        early(flagB("amp", AT(a.tune.ampOn), false)),
        shared(scaled("uiTextScale", AT(a.uiTextScale), 100, 100, 0.8, 2, true)),
        shared(flagI("touchUi", AT(a.touchUi), false)), shared(scaled("touchScale", AT(a.touchScale), 100, 135, 1.0, 2, true)),
        num("tetraSlotMode", AT(a.tetraSlotMode), 0, 0, 3, true), num("tetraSlotNum", AT(a.tetraSlotNum), 1, 1, 4, true),
        num("tetraSlotId", AT(a.tetraSlotId), 0, 0, 16777215, true),
        num("tetraSingle", AT(a.tetraSingle), 0, 0, 1, true),
        num("wfMap3", AT(a.wf.map), cmap::NavyHeat, -1, (int)Waterfall::NumMaps - 1, true),   // 3.0: a new key, everyone starts on Navy heat (the old looks set their own maps)
        flagB("wfAutoFloor", AT(a.wf.autoTrack), true),   // new key with the floor-anchored range (3.0): everyone starts on auto
        flagB("specAutoDb", AT(a.specAutoDb), true), flagB("specPerHz", AT(a.specPerHz), true),
        flagB("wfAvg", AT(a.wf.rowAverage), false, false, true), flagB("wfClock", AT(a.wf.clockLabels), false, false, true),
        real("wfGamma", AT(a.wf.gamma), 0.4, 2.5),
        real("wfBright", AT(a.wf.brightness), -0.5, 0.5), real("wfContrast", AT(a.wf.contrast), 0.25, 4),
        num("wfDetailAuto", AT(a.wf.detail), -1, -1, Waterfall::kMaxCols, true), flagB("wfPerMode", AT(a.wfPerMode), false, false, true),
        flagB("wf3d", AT(a.wf.s3d.on), false), num("wf3dRows", AT(a.wf.s3d.rows), 256, 64, 1024), real("wf3dHeight", AT(a.wf.s3d.heightScale), 0.05, 1.5),
        flagB("wf3dSmooth", AT(a.wf.s3d.smooth), false), flagB("wf3dPeak", AT(a.wf.s3d.peak), true), flagB("wf3dMarker", AT(a.wf.s3d.marker), true),
        flagB("wf3dShade", AT(a.wf.s3d.shade), true), num("wf3dPreset", AT(a.wf.s3d.preset), 0, 0, w3d::NumPresets - 1),
        real("wf3dYaw", AT(a.wf.s3d.cam.yaw), -3.2, 3.2), real("wf3dPitch", AT(a.wf.s3d.cam.pitch), 0.03, 1.553), real("wf3dDist", AT(a.wf.s3d.cam.dist), 0.35, 8),
        text("wfCustom", AT(a.wfCustom)), text("specMarkers", AT(a.specMarkers)),
        num("traceColor", AT(a.traceColor), 0, 0, cmap::kNumTracePresets - 1, true), num("holdColor", AT(a.holdColor), -1, -1, cmap::kNumTracePresets - 1, true),
        flagB("minHold", AT(a.minHold), false, false, true), flagB("showLive", AT(a.showLive), false, false, true),
        flagB("nbOn", AT(a.nbOn), false, false, true), real("nbThr", AT(a.nbThr), 2, 40), real("nbWidthUs", AT(a.nbWidthUs), 2, 2000),
        real("specZoom", AT(a.specZoom), 0, 1, true),
        flagB("specHold", AT(a.peakHold), false, false, true),
        real("holdDecay", AT(a.holdDecay), 0, 60),
        real("specAvg", AT(a.specAvgSec), 0, 5), num("specFft", AT(a.specFft), 0, 0, 262144, true), real("specLine", AT(a.specLine), 0.6, 3),
        flagB("specFill", AT(a.specFill), true, false, true), flagB("specGrid", AT(a.specGrid), false, false, true), flagB("specPeaks", AT(a.specPeaks), false, false, true), flagB("specLabels", AT(a.specLabels), true, false, true), num("specLabelMask", AT(a.specLabelMask), 63, 0, 63, true),
        flagB("specReadout", AT(a.specReadout), false, false, true), flagB("specDbm", AT(a.specDbm), false, false, true),
        num("specLook", AT(a.specLook), 1, 0, slook::NumLooks - 1, true), num("specAxisLabels", AT(a.specAxisLabels), -1, -1, 1, true),
        real("specLabelSize", AT(a.specLabelSize), 0.7, 1.6), num("specSmoothTrace", AT(a.specSmoothTrace), -1, -1, 4, true),
        flagB("specNfMarker", AT(a.specNfMarker), false, false, true), flagB("specSmoothZoom", AT(a.specSmoothZoom), true, false, true),
        num("specSnapHz", AT(a.specSnapHz), 0, 0, 1000000, true), flagB("wfLowPass", AT(a.wfLowPass), false, false, true),
        real("wfFloorBelow", AT(a.wf.autoBelow), 0, 40), real("wfFloorSpan", AT(a.wf.autoSpan), 20, 120),
        num("tuningMode", AT(a.tuningMode), 0, 0, 1, true),
        flagB("centreTuning", AT(a.centreTuning), false, false, true), flagB("dragMovesTuned", AT(a.dragMovesTuned), false, false, true),
        flagI("fmMono", AT(a.fmMono), false),
        numFix("fmDeemph", AT(a.fmDeemph), 50, fixDeemph),
        numFix("fmBass", AT(a.fmBass), 0, fixTone), numFix("fmTreble", AT(a.fmTreble), 0, fixTone),
        flagI("fmWarm", AT(a.fmWarm), false),
        num("fmStereo", AT(a.fmStereo), 0, 0, 2, true), numFix("fmAudioBw", AT(a.fmAudioBw), 0, fixAudioBw), num("fmSquelch", AT(a.fmSquelch), 0, 0, 40, true),
        numFix("fmRegion", AT(a.fmRegion), 0, fixRegion),
        numFix("fmChanKhz", AT(a.fmChanKhz), 0, fixChanKhz),
        flagI("pagerNarrow", AT(a.pagerNarrow), false), flagI("packetNarrow", AT(a.packetNarrow), false),
        shared(num("compute", AT(a.computeMode), 0, 0, 2, true, true)),   // indexes the CPU / GPU / Auto names
        num("standard", AT(a.stdMode), 0, 0, 2, true, true),      // 0 auto, 1 DVB-T2, 2 DVB-T: engineStd() hands it on
        num("bw", AT(a.bwIdx), 0, 0, (int)(sizeof kBw / sizeof *kBw) - 1),
        shared(flagB("bwAuto", AT(a.bwAuto), false, true, true)),
        // ---- shared by all receivers (written by receiver 1)
        shared(flagB("newUi", AT(a.newUi), true, true)),
        shared(flagB("dockOn", AT(a.dockOn), false, true)),
        shared(scaled("splitSide", AT(a.splitSide), 1, 0, 1, 0, false)), shared(scaled("splitRight", AT(a.splitRight), 1, 0, 1, 0, false)),
        shared(scaled("splitBottom", AT(a.splitBottom), 1, 0, 1, 0, false)), shared(scaled("splitDock", AT(a.splitDock), 1, 0, 1, 0, false)),
        shared(scaled("splitBot2", AT(a.splitBot2), 1, 0, 1, 0, false)),
        shared(text("modeGroupsOpen", AT(a.modeGroupsOpen))),
        shared(text("modeHidden", AT(a.modeHidden))), shared(text("modeHiddenGroups", AT(a.modeHiddenGroups))), shared(text("modeFavs", AT(a.modeFavs))),
        shared(flagB("modeFavsOnly", AT(a.modeFavsOnly), false)),
        shared(num("uiVariant", AT(a.uiVariant), 0, 0, 7, true)),
        shared(numFix("dtmbBw", AT(a.dtmbBwMhz), 8, fixDtmbBw)),
        shared(numFix("isdbtBw", AT(a.isdbtBwMhz), 6, fixIsdbtBw)),
        shared(flagI("atvFm", AT(a.atvFm), false)),
        shared(num("atvNbtv", AT(a.atvNbtv), 0, 0, 2, true)),
        shared(num("atvNbtvLines", AT(a.atvNbtvLines), 32, 24, 240, true)),
        shared(scaled("atvNbtvFps", AT(a.atvNbtvFps), 10, 125, 1, 60, true)),
        shared(flagI("mtvOn", AT(a.mtvOn), false)),
        shared(num("mtvMax", AT(a.mtvMax), 0, 0, 8, true)),
        shared(num("mtvGrid", AT(a.mtvGrid), 0, 0, 2, true)),
        shared(flagI("reduceCpuHidden", AT(a.reduceCpuHidden), true)),
        shared(flagI("perfOverlay", AT(a.perfOverlay), false)),
        shared(num("powerLowMode", AT(a.powerLowMode), 1, 0, 2, true)), shared(num("powerHidden", AT(a.powerHidden), 1, 0, 2, true)),
        shared(flagI("discordOn", AT(a.discordOn), false)), shared(flagI("discordFreq", AT(a.discordFreq), true)),
        shared(rx1(flagI("mapSend", AT(a.mapSend), false, false))), shared(rx1(flagI("mapConsent", AT(a.mapConsent), false, false))), shared(rx1(flagI("mapMsgs", AT(a.mapMsgs), false, false))), shared(rx1(flagI("mapJam", AT(a.mapJam), false, false))), shared(rx1(flagI("mapHam", AT(a.mapHam), false, false))), shared(rx1(flagI("mapHamConsent", AT(a.mapHamConsent), false, false))), shared(rx1(flagI("mapHam6", AT(a.mapHam6), false, false))), shared(rx1(flagI("mapVoice", AT(a.mapVoice), false, false))),
        shared(rx1(flagI("mapDeclined", AT(a.mapDeclined), false, false))), shared(rx1(text("mapDeclinedVersion", AT(a.mapDeclinedVersion)))),
        shared(rx1(text("mapStationId", AT(a.mapStationId)))), shared(rx1(text("mapServer", AT(a.mapServer)))), shared(rx1(text("mapName", AT(a.mapName)))),
        shared(flagI("lightUi", AT(a.lightUi), false)), shared(flagI("highContrast", AT(a.highContrast), false)),
        shared(text("uiLang", AT(a.uiLang))),
        shared(text("bandPlan", AT(a.bandPlan))),
        shared(num("uiTheme", AT(a.uiTheme), 1, 0, 2, true)),
        shared(num("audioIq", AT(a.audioIqMode), 0, 0, 3)),
        shared(num("audioChan", AT(a.audioChan), 0, 0, 2)), shared(dbl("audioIfHz", AT(a.audioIfHz), 0)), shared(flagI("audioInvert", AT(a.audioInvert), false)),
        shared(flagI("audioDc", AT(a.audioDc), true)), shared(flagI("audioAfView", AT(a.audioAfView), true)), shared(dbl("audioGainDb", AT(a.audioGainDb), 0)), shared(num("audioRate", AT(a.audioRate), 48000, 8000, 192000)),
        shared(flagFn("smoothGaps", [](const App& a) { return a.engine.player().conceal(); }, [](App& a, bool v) { a.engine.player().setConceal(v); })),
        shared(flagFn("dcRemove", [](const App& a) { return (bool)a.engine.iqFix().dc.load(); }, [](App& a, bool v) { a.engine.iqFix().dc = v; })),
        shared(flagFn("offsetTune", [](const App& a) { return a.engine.autoOffset(); }, [](App& a, bool v) { a.engine.setAutoOffset(v); })),
        shared(flagFn("iqCorrect", [](const App& a) { return (bool)a.engine.iqFix().iq.load(); }, [](App& a, bool v) { a.engine.iqFix().iq = v; })),
        shared(text("filePath", AT(a.file.path), true)),
        shared(dbl("fileCentreHz", AT(a.file.centreHz), 0)),
        shared(flagFn("fileDcNotch", [](const App& a) { return a.engine.dcNotch(); }, [](App& a, bool v) { a.engine.setDcNotch(v); })),
        shared(chars("outPath", AT(a.filePath[0]), sizeof(App::filePath), true)),
        shared(chars("udpHost", AT(a.udpHost[0]), sizeof(App::udpHost), true)),
        shared(num("udpPort", AT(a.out.port), 0, 1, 0, true, true)),
        shared(flagB("udpRtp", AT(a.out.rtp), false)), shared(flagB("dropNull", AT(a.out.dropNull), false)),
        shared(flagB("popTop", AT(a.popTop), true)),
        shared(flagB("subsOn", AT(a.subsOn), true)), shared(text("subsPick", AT(a.subsPick))), shared(text("tvLang", AT(a.tvLang))), shared(num("ttxPage", AT(a.ttxPage), 100, 100, 899)),
        shared(flagB("updCheck", AT(a.updCheck), true)), shared(flagB("updAuto", AT(a.updAuto), true)), shared(flagB("updBeta", AT(a.updPre), false)),
        shared(dbl("updLast", AT(a.updLast), 0)), shared(text("updSkip", AT(a.updSkip))),
        shared(num("updSrc", AT(a.updSrc), 0, 0, 2)), shared(flagB("updMirrorOn", AT(a.updMirrorOn), false)), shared(text("updMirror", AT(a.updMirror))),
    };
    return t;
}

void loadKey(plat::Prefs& d, App& a, const PrefKey& k) {
    if (k.guard && !d.has(k.key)) return;
    switch (k.kind) {
    case Kind::Flag: { bool* p = k.at.b(a); *p = d.getB(k.key, k.keep ? *p : k.def != 0); break; }
    case Kind::FlagInt: *k.at.b(a) = d.getI(k.key, (long)k.def) != 0; break;
    case Kind::Int: {
        int* p = k.at.i(a);
        int v = (int)d.getI(k.key, k.keep ? *p : (int)k.def);
        if (k.lo <= k.hi) v = std::max((int)k.lo, std::min((int)k.hi, v));
        *p = k.fix ? k.fix(v) : v;
        break;
    }
    case Kind::Scaled: {
        float v = (float)d.getI(k.key, (long)k.def) / (float)k.scale;
        if (k.lo <= k.hi) v = std::max((float)k.lo, std::min((float)k.hi, v));
        *k.at.f(a) = v;
        break;
    }
    case Kind::Float: {
        float* p = k.at.f(a);
        float v = (float)d.getD(k.key, *p);
        if (k.lo <= k.hi) v = std::max((float)k.lo, std::min((float)k.hi, v));
        *p = v;
        break;
    }
    case Kind::Double: { double* p = k.at.d(a); *p = d.getD(k.key, k.keep ? *p : k.def); break; }
    case Kind::Text: *k.at.s(a) = d.getS(k.key, ""); break;
    case Kind::Chars: snprintf(k.at.c(a), k.chars, "%s", d.getS(k.key, "").c_str()); break;
    case Kind::FlagFn: k.setFn(a, d.getB(k.key, false)); break;
    }
}

void saveKey(plat::Prefs& d, const App& ca, const PrefKey& k) {
    App& a = const_cast<App&>(ca);   // the accessors are shared with loading; saving only reads through them
    switch (k.kind) {
    case Kind::Flag: d.setB(k.key, *k.at.b(a)); break;
    case Kind::FlagInt: d.setI(k.key, *k.at.b(a) ? 1 : 0); break;
    case Kind::Int: d.setI(k.key, *k.at.i(a)); break;
    case Kind::Scaled: d.setI(k.key, (int)std::lround(*k.at.f(a) * k.scale)); break;
    case Kind::Float: d.setD(k.key, *k.at.f(a)); break;
    case Kind::Double: d.setD(k.key, *k.at.d(a)); break;
    case Kind::Text: d.setS(k.key, *k.at.s(a)); break;
    case Kind::Chars: d.setS(k.key, k.at.c(a)); break;
    case Kind::FlagFn: d.setB(k.key, k.getFn(a)); break;
    }
}

// the table keys of one load phase (early: before the mode is restored; late: after it)
void loadKeys(plat::Prefs& d, App& a, bool earlyPhase) {
    for (const PrefKey& k : keyTable()) {
        if (k.early != earlyPhase) continue;
        if (k.rx1Load && a.rxId != 1) continue;
        loadKey(d, a, k);
    }
}

// the table keys of this receiver, or (shared) the ones receiver 1 owns
void saveKeys(plat::Prefs& d, const App& a, bool sharedKeys) {
    for (const PrefKey& k : keyTable()) if (k.shared == sharedKeys) saveKey(d, a, k);
}

// the stations the FM scan found: mhz|name|type|snr|stereo|rds|pi (hex)|tp|radio text|scrolling name, one per line
void loadFmStations(plat::Prefs& d, App& a) {
    if (!d.has("fmStations")) return;
    std::string st = d.getS("fmStations", ""), line;
    size_t pos = 0;
    while (pos < st.size()) {
        size_t e = st.find('\n', pos);
        if (e == std::string::npos) e = st.size();
        line = st.substr(pos, e - pos); pos = e + 1;
        std::vector<std::string> f; size_t q = 0;
        for (;;) { size_t bar = line.find('|', q); if (bar == std::string::npos) { f.push_back(line.substr(q)); break; } f.push_back(line.substr(q, bar - q)); q = bar + 1; }
        if (f.size() < 6) continue;
        App::FmScan::Res r;
        r.mhz = atof(f[0].c_str()); r.name = f[1]; r.pty = f[2]; r.snr = (float)atof(f[3].c_str()); r.stereo = f[4] == "1"; r.rds = f[5] == "1"; r.found = r.mhz >= 87.5 && r.mhz <= 108.01;
        if (f.size() >= 10) { r.pi = (int)strtol(f[6].c_str(), nullptr, 16); r.tp = f[7] == "1"; r.rt = f[8]; r.psText = f[9]; }   // since full RDS
        if (r.found) a.fmScan.results.push_back(r);
    }
}

void saveFmStations(plat::Prefs& d, const App& a) {
    std::string st;
    for (const auto& r : a.fmScan.results) {
        if (!r.found) continue;
        auto clean = [](std::string s) { for (auto& c : s) if (c == '|' || c == '\n') c = ' '; return s; };
        char b[64]; snprintf(b, sizeof b, "%.1f|", r.mhz);
        st += b + clean(r.name) + "|" + clean(r.pty) + "|" + std::to_string((int)r.snr) + "|" + (r.stereo ? "1" : "0") + "|" + (r.rds ? "1" : "0");
        char pi[8]; snprintf(pi, sizeof pi, "%04X", r.pi & 0xFFFF);
        st += std::string("|") + pi + "|" + (r.tp ? "1" : "0") + "|" + clean(r.rt) + "|" + clean(r.psText) + "\n";
    }
    if (!a.fmScan.running) d.setS("fmStations", st);   // not a half-finished scan
}

// the mode (family and protocol entry) and the gains and frequency each mode remembers
void loadModeAndGains(plat::Prefs& d, App& a) {
    if (d.has("family")) {
        const int f = std::max(0, std::min(kNumFamilies - 1, (int)d.getI("family", 0)));
        // the protocol entry; settings from before the split (HF digital, Mesh, Pagers, Marine as one entry) open the family's first
        a.sub = subIndexOf(d.getS("subMode", "").c_str());
        a.family = f;
        if (!curSub(a)) a.sub = firstSubOf(f);
        setFamilyFlags(a, f);
        if (a.fmMode && !(a.freqMhz > 0)) a.freqMhz = 100.0;   // a saved FM frequency outside the broadcast band is kept
    }
    for (int f = 0; f < kNumSlots; f++) {   // as many as savePrefs writes
        const std::string k = slotKey(f);
        if (!d.has(("gLna" + k).c_str())) continue;
        a.famGain[f] = {(int)d.getI(("gLna" + k).c_str(), 32), (int)d.getI(("gVga" + k).c_str(), 20), d.getB(("gAmp" + k).c_str(), false), true};
    }
    {   // a protocol entry never used yet starts with the gains its family had as one entry
        int n = 0;
        const SubMode* t = subModes(n);
        for (int i = 0; i < n && i < kNumSubModes; i++) if (!a.famGain[kNumFamilies + i].known && t[i].stdMode >= 8) a.famGain[kNumFamilies + i] = a.famGain[t[i].stdMode - 2];
    }
    if (const int sl = modeSlot(a); sl >= 0 && sl < kNumSlots && a.famGain[sl].known) { a.tune.lnaDb = a.famGain[sl].lna; a.tune.vgaDb = a.famGain[sl].vga; a.tune.ampOn = a.famGain[sl].amp; }
    for (int f = 6; f < kNumSlots; f++) a.famFreq[f] = d.getD(("fFreq" + slotKey(f)).c_str(), 0.0);
    if (const SubMode* sm = curSub(a)) {   // the protocol entry's own frequency
        const ModeTuning* mt = modeTuning(sm->stdMode);
        const double fr = a.famFreq[modeSlot(a)];
        if (mt && !(a.freqMhz >= mt->minMhz && a.freqMhz <= mt->maxMhz && fr > 0)) a.freqMhz = fr >= mt->minMhz && fr <= mt->maxMhz ? fr : sm->defMhz;
    }
}

void saveModeAndGains(plat::Prefs& d, const App& a) {
    d.setI("family", a.family);
    { const SubMode* sm = curSub(a); d.setS("subMode", sm ? sm->id : ""); }
    const int curSlot = modeSlot(a);
    for (int f = 0; f < kNumSlots; f++) {   // gains per mode (the current mode from the live settings)
        const bool cur = f == curSlot;
        if (!cur && !a.famGain[f].known) continue;
        const std::string k = slotKey(f);
        d.setI(("gLna" + k).c_str(), cur ? a.tune.lnaDb : a.famGain[f].lna);
        d.setI(("gVga" + k).c_str(), cur ? a.tune.vgaDb : a.famGain[f].vga);
        d.setB(("gAmp" + k).c_str(), cur ? a.tune.ampOn : a.famGain[f].amp);
    }
    for (int f = 6; f < kNumSlots; f++) if (a.famFreq[f] > 0 || (f == curSlot && a.family >= 6)) d.setD(("fFreq" + slotKey(f)).c_str(), f == curSlot ? a.freqMhz : a.famFreq[f]);
}

// the waterfall and spectrum ranges and the waterfall speed (the paired min / max keys are kept only together)
void loadSpectrumRanges(plat::Prefs& d, App& a) {
    if (d.has("wfMin") && d.has("wfMax")) { a.wf.minDb = (float)d.getD("wfMin", a.wf.minDb); a.wf.maxDb = (float)d.getD("wfMax", a.wf.maxDb); }
    a.wf.setRate((float)d.getD("wfRate", 0.0));
    if (d.has("specMin") && d.has("specMax")) { a.yMin = (float)d.getD("specMin", a.yMin); a.yMax = (float)d.getD("specMax", a.yMax); }
    if (!(a.yMax - a.yMin >= 10)) { a.yMin = -110; a.yMax = -30; }
    if (!(a.wf.maxDb - a.wf.minDb >= 3)) { a.wf.minDb = -100; a.wf.maxDb = -32; }
}

void saveSpectrumRanges(plat::Prefs& d, const App& a) {
    d.setD("wfMin", a.wf.minDb); d.setD("wfMax", a.wf.maxDb); d.setD("wfRate", a.wf.rowsPerSec);
    d.setD("specMin", a.yMin); d.setD("specMax", a.yMax);
}

}  // namespace

void loadPrefs(App& a) {
    plat::Prefs& d = plat::prefs();
    a.netRadios = netRadiosFromString(d.getS("netRadios", ""));
    a.synthGroups = synthGroupsFromString(d.getS("synthGroups", ""));
    a.hamlibRigs = hamlibRigsFromString(d.getS("hamlibRigs", ""));
    {   // radios hidden from the list, one name per line
        a.hiddenRadios.clear();
        std::string cur;
        for (char c : d.getS("hiddenRadios", "") + "\n") { if (c == '\n') { if (!cur.empty()) a.hiddenRadios.push_back(cur); cur.clear(); } else cur += c; }
        setHiddenRadios(a.hiddenRadios);
    }
    setNetworkDiscovery(d.getB("netDiscovery", false));   // listing searches the LAN by itself only when the user turned it on (#215)
    loadKeys(d, a, true);   // the Hamlib picker, the frequency and the gains
    a.recFormat = fixRecFormat((int)d.getI("recFormat", 0));
    a.recSigmf = d.getB("recSigmf", true);
    a.audioRecFmt = std::max(0, std::min(2, (int)d.getI("audioRecFmt", 0)));
    a.audioRecGate = d.getB("audioRecGate", false); a.audioRecSplit = d.getB("audioRecSplit", false); a.audioRecKeep = d.getB("audioRecKeep", false);
    a.monOn = d.getB("monOn", false); a.monDemod = std::max(0, std::min(4, (int)d.getI("monDemod", 4)));
    a.monVol = (float)d.getD("monVol", 1.0); a.monVol2 = (float)d.getD("monVol2", 1.0);
    a.monLatency = std::max(20, std::min(1000, (int)d.getI("monLatency", 100))); a.monYield = d.getB("monYield", true);
    a.monDev = d.getS("monDev", ""); a.monDev2 = d.getS("monDev2", ""); a.monOut2 = d.getB("monOut2", false);
    a.monNet = d.getB("monNet", false); a.monPort = std::max(1024, std::min(65535, (int)d.getI("monPort", 7355)));
    a.monBindAll = d.getB("monBindAll", false); a.monWav = d.getB("monWav", true);
    a.tvBufMb = (int)d.getI("tvBufMb", 256); a.engine.player().setBufferMB(a.tvBufMb); a.tvBufMb = a.engine.player().bufferMB();
    a.tvRecMin = std::max(0, (int)d.getI("tvRecMin", 0));
    a.tvRecAtOn = d.getB("tvRecAtOn", false);
    a.tvRecAtH = std::max(0, std::min(23, (int)d.getI("tvRecAtH", 0)));
    a.tvRecAtM = std::max(0, std::min(59, (int)d.getI("tvRecAtM", 0)));
    a.engine.setRecordSigmf(a.recSigmf);
    timeMachineLoad(a);
    loadModeAndGains(d, a);
    loadKeys(d, a, false);
    loadSpectrumRanges(d, a);
    if (d.has("wideMaskLo")) a.wideFamMask = (uint64_t)(uint32_t)d.getI("wideMaskLo", 0) | ((uint64_t)(uint32_t)d.getI("wideMaskHi", 0) << 32);
    for (int k = 0; k < 2; k++) {   // families 64-191
        const std::string lo = "wideMask" + std::to_string(k + 2) + "Lo", hi = "wideMask" + std::to_string(k + 2) + "Hi";
        if (d.has(lo.c_str())) a.wideFamMaskHi[k] = (uint64_t)(uint32_t)d.getI(lo.c_str(), 0) | ((uint64_t)(uint32_t)d.getI(hi.c_str(), 0) << 32);
    }
    {   // the whole-span detector's settings
        dect2::WideTuning& w = a.wideTuning;
        w.onDb = d.getD("wideOnDb", w.onDb); w.offDb = d.getD("wideOffDb", w.offDb); w.minSnrDb = d.getD("wideMinSnr", w.minSnrDb);
        w.dwellSec = d.getD("wideDwell", w.dwellSec); w.holdSec = d.getD("wideHold", w.holdSec);
        w.maxCarriers = (int)d.getI("wideMax", w.maxCarriers); w.cpuBudget = d.getD("wideCpu", w.cpuBudget);
        w.usePriors = d.getI("widePriors", w.usePriors ? 1 : 0) != 0; w.priorsOnly = d.getI("widePriorsOnly", w.priorsOnly ? 1 : 0) != 0;
        a.wideTuningDirty = true;
    }
    if (a.rxId == 1 && a.mapSend && !a.mapConsent) a.mapSend = false;   // never on without consent
    if (a.rxId == 1 && a.mapHam && !a.mapHamConsent) a.mapHam = false;   // ham contacts: never on without their own consent
    i18n::set(a.uiLang.empty() ? plat::uiLanguage() : a.uiLang);
    loadFmStations(d, a);
    if (d.has("fmSaved")) {   // the saved-stations library, separate from the band-scan list in fmStations
        for (const auto& r : fmSavedDecode(d.getS("fmSaved", ""))) a.fmSaved.push_back(r);
    }
    if (a.rxId == 1) loadFeedPrefs(a);   // the feeds are shared by all receivers: RX 1 owns their settings
    a.channels = d.getChannels();
    tvHdLoad(a);
}

// Tuning events (a click, a drag, the wheel, a step button) used to rewrite the whole settings file at once, on the UI thread, every time:
// a held drag meant dozens of file writes a second between the mouse and the retune. savePrefsSoon() only notes that a save is due; the
// frame loop (savePrefsTick) writes it after the tuning has been still for kSoonDelay, and at exit (savePrefsFlush) at the latest.
static const App* gSoonApp = nullptr;
static double gSoonDue = 0;
static constexpr double kSoonDelay = 0.6;
static double nowSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static tunestat::Stats gTuneStats;
tunestat::Stats& tuneStats() { return gTuneStats; }
void savePrefsSoon(const App& a) {
    if (gSoonApp && gSoonApp != &a) savePrefs(*gSoonApp);
    gSoonApp = &a; gSoonDue = nowSec() + kSoonDelay;
    gTuneStats.savesDeferred++;
}
void savePrefsTick() { if (gSoonApp && nowSec() >= gSoonDue) { const App* p = gSoonApp; gSoonApp = nullptr; savePrefs(*p); } }
void savePrefsFlush() { if (gSoonApp) { const App* p = gSoonApp; gSoonApp = nullptr; savePrefs(*p); } }

void savePrefs(const App& a) {
    if (&a == gSoonApp) gSoonApp = nullptr;   // this write covers it
    const double t0 = nowSec();
    struct Timer { double t0; ~Timer() { const double ms = (nowSec() - t0) * 1000; gTuneStats.saves++; gTuneStats.saveMsSum += ms; if (ms > gTuneStats.saveMsMax) gTuneStats.saveMsMax = ms; } } timer{t0};
    plat::Prefs& d = plat::prefs();
    if (a.rxId == 1) {
        d.setS("netRadios", netRadiosToString(a.netRadios));
        d.setS("synthGroups", synthGroupsToString(a.synthGroups));
        d.setS("hamlibRigs", hamlibRigsToString(a.hamlibRigs));
        { std::string j; for (const auto& n : a.hiddenRadios) j += n + "\n"; d.setS("hiddenRadios", j); }
        d.setB("netDiscovery", networkDiscovery());
    }
    d.setS("devName", a.devIdx >= 2 && a.devIdx < (int)a.devices.size() ? a.devices[a.devIdx].name : std::string());   // the radio of this receiver, found again at the next start
    saveModeAndGains(d, a);
    saveKeys(d, a, false);
    saveSpectrumRanges(d, a);
    if (a.rxId > 1) { d.flush(); return; }   // a further receiver keeps only its own radio, mode, frequency and gains: everything else is shared and written by RX 1
    saveKeys(d, a, true);
    d.setI("wideMaskLo", (int)(uint32_t)(a.wideFamMask & 0xffffffffu)); d.setI("wideMaskHi", (int)(uint32_t)(a.wideFamMask >> 32));
    for (int k = 0; k < 2; k++) {
        const std::string lo = "wideMask" + std::to_string(k + 2) + "Lo", hi = "wideMask" + std::to_string(k + 2) + "Hi";
        d.setI(lo.c_str(), (int)(uint32_t)(a.wideFamMaskHi[k] & 0xffffffffu)); d.setI(hi.c_str(), (int)(uint32_t)(a.wideFamMaskHi[k] >> 32));
    }
    {
        const dect2::WideTuning& w = a.wideTuning;
        d.setD("wideOnDb", w.onDb); d.setD("wideOffDb", w.offDb); d.setD("wideMinSnr", w.minSnrDb); d.setD("wideDwell", w.dwellSec);
        d.setD("wideHold", w.holdSec); d.setI("wideMax", w.maxCarriers); d.setD("wideCpu", w.cpuBudget);
        d.setI("widePriors", w.usePriors ? 1 : 0); d.setI("widePriorsOnly", w.priorsOnly ? 1 : 0);
    }
    saveFmStations(d, a);
    d.setS("fmSaved", fmSavedEncode(a.fmSaved));   // the saved-stations library, not the scan results
    d.setChannels(a.channels);
    tvHdSave(a);
    saveFeedPrefs(a);
    d.flush();
}
