// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Live transcripts in the app. See transcribe_app.h and docs/TRANSCRIBE.md.
#include "dect2/common/thread_name.h"
#include "dect2/common/diag.h"
#include "transcribe_app.h"
#include "app.h"
#include "adsb/adsb_acars.h"
#include "adsb/adsb_internal.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/updater.h"
#include "dect2/transcribe/audio_tap.h"
#include "dect2/transcribe/captions.h"
#include "dect2/transcribe/models.h"
#include "dect2/transcribe/moonshine_models.h"
#include "dect2/transcribe/transcriber.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "state/receivers.h"
#include "ui/theme.h"
#include "ui/ui_common.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <map>
#include <mutex>
#include <thread>

namespace tx {

namespace {

struct Rx1 {   // the settings are the program's, not one receiver's
    int old;
    Rx1() : old(plat::gPrefRx) { plat::gPrefRx = 1; }
    ~Rx1() { plat::gPrefRx = old; }
};

struct Cfg {
    bool enabled = true;        // the master switch: on, it only does anything once a speech model file is in the models folder
    bool airband = true;        // with the master switch on: the airband channels
    bool marine = true;         // marine VHF voice
    bool other = true;          // "Transcribe what I am listening to": the sound of any other mode (FM, AM, amateur, CB, digital voice ...)
    std::string model = "tiny.en";   // a catalogue id, or "file:<name>" for a file the user put in the models folder
    std::string lang = "en";    // "auto" needs a multilingual model
    bool translate = false;     // also keep an English translation of foreign speech (multilingual model)
    int threads = 2;            // CPU budget: threads of the speech engine
    int days = 7;               // retention
    int columns = 0x75;        // UTC, channel, text, aircraft, confidence; local time/frequency optional
    // auto subtitles
    bool cap = true;            // master switch for captions (separate from the transcripts: nothing is kept in a file)
    std::set<int> capModes;     // the standards it runs for (all that can be captioned until the user changes it)
    int capSource = 3;          // dect2::CaptionSource: TV subtitles from the station / generated / both / generated only where the station sends none
    int capSize = 100, capBg = 75, capLines = 2;
    bool capTop = false;
    int capShow = 0;            // 0 as spoken, 1 English translation when there is one, 2 both
    bool capSave = true;        // save .srt and .vtt next to an audio recording
    bool loaded = false;
};
Cfg C;

dect2::TranscriptStore gStore;
dect2::Transcriber gTr(gStore);
dect2::TranscriptStore gCapStore;             // captions: in memory only, never written to a file, never shared
dect2::Transcriber gCap(gCapStore);
std::map<int, std::unique_ptr<dect2::CaptionTrack>> gTracks;   // per receiver (guarded by gMu)
struct CapCtx { bool valid = false; int rx = 0, std = -1; double freqHz = 0; } gCapCtx;
dect2::ModelDownloader gDl;
dect2::MoonshineModelDownloader gMoonDl;
bool gWindowOpen = false;
bool gStarted = false;

std::mutex gMu;                               // the maps below, read by the tap threads
std::map<int, std::vector<dect2::SourceInfo>> gChanInfo;   // per receiver: the airband channels by index
std::map<std::string, std::string> gAircraft;              // normalised callsign or registration > ICAO address (hex), for the resolver
struct Ctx { bool valid = false; std::string mode, domain, channel; double freqHz = 0; } gCtx;   // what the sound of the other modes is
std::map<int, bool> gTapOn;                   // airband tap installed, per receiver
bool gSinkOn = false;

// speech engine loading, in the background
std::thread gLoadTh;
std::atomic<int> gLoadState{0};               // 0 none, 1 loading, 2 ready, 3 failed
std::string gLoadErr, gLoadedFile, gLoadingFile;
std::string gLoadedMoonLib, gLoadedOnnxLib, gLoadingMoonLib, gLoadingOnnxLib;
dect2::MoonshineDiscoveredLibraries gMoonLibraries;
bool gMoonLibrariesScanned = false;
void scanMoonLibraries(const std::string& root) {
    gMoonLibraries = dect2::discoverMoonshineLibraries(root);
    gMoonLibrariesScanned = true;
}
std::shared_ptr<dect2::SpeechBackend> gPending;
std::shared_ptr<dect2::SpeechBackend> gBackend;
bool gBackendEnglishOnly = true;

std::string gModelsDirCached;
std::vector<std::string> gUserFiles;
std::vector<std::string> gMoonshineDirs;
double gUserFilesAt = -100;

void save();
void load() {
    if (C.loaded) return;
    C.loaded = true;
    Rx1 r;
    plat::Prefs& d = plat::prefs();
    C.enabled = d.getB("txEnabled", true);
    C.airband = d.getB("txAirband", true);
    C.other = d.getB("txOther", true);
    C.marine = d.getB("txMarine", true);
    C.model = d.getS("txModel", "tiny.en");
    C.lang = d.getS("txLang", "en");
    C.translate = d.getB("txTranslate", false);
    C.threads = (int)std::max(1L, std::min(8L, d.getI("txThreads", 2)));
    C.days = (int)std::max(1L, std::min(365L, d.getI("txDays", 7)));
    C.columns = (int)d.getI("txColumns", 0x75) & 0x7f;
    C.cap = d.getB("capOn", true);
    {
        std::stringstream ss(d.getS("capModes", "*"));   // "*": every mode that can be captioned
        if (d.getS("capModes", "*") == "*") for (int k : dect2::captionStandards()) C.capModes.insert(k);
        std::string t;
        while (std::getline(ss, t, ',')) if (!t.empty() && dect2::captionStandardEligible(atoi(t.c_str()))) C.capModes.insert(atoi(t.c_str()));
    }
    C.capSource = (int)std::max(0L, std::min(3L, d.getI("capSource", 3)));
    C.capSize = (int)d.getI("capSize", 100); C.capBg = (int)d.getI("capBg", 75); C.capLines = (int)d.getI("capLines", 2);
    C.capTop = d.getB("capTop", false);
    C.capShow = (int)std::max(0L, std::min(2L, d.getI("capShow", 0)));
    C.capSave = d.getB("capSave", true);
    if (d.getI("txVer", 0) < 31) {
        // Settings saved by 3.0 have "other modes" and the captions off (the old defaults): FM and every other voice mode produced nothing. Once, turn
        // on what the one switch "Transcribe what I am listening to" means.
        C.other = C.marine = C.cap = true;
        for (int k : dect2::captionStandards()) C.capModes.insert(k);
        save();
    }
}
void save() {
    Rx1 r;
    plat::Prefs& d = plat::prefs();
    d.setB("txEnabled", C.enabled); d.setB("txAirband", C.airband); d.setB("txMarine", C.marine); d.setB("txOther", C.other);
    d.setS("txModel", C.model); d.setS("txLang", C.lang); d.setB("txTranslate", C.translate); d.setI("txThreads", C.threads); d.setI("txDays", C.days); d.setI("txColumns", C.columns);
    d.setB("capOn", C.cap); d.setI("capSource", C.capSource); d.setI("capSize", C.capSize); d.setI("capBg", C.capBg); d.setI("capLines", C.capLines);
    d.setI("txVer", 31);
    d.setB("capTop", C.capTop); d.setI("capShow", C.capShow); d.setB("capSave", C.capSave);
    { std::string m; for (int k : C.capModes) m += (m.empty() ? "" : ",") + std::to_string(k); d.setS("capModes", m); }
    d.flush();
}

std::string modelsDir() {
    if (gModelsDirCached.empty()) gModelsDirCached = plat::dataDir() + "/models";
    return gModelsDirCached;
}

int64_t wallMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

// the model file name for the setting, "" when there is none
std::string modelFile(const std::string& setting) {
    if (setting.compare(0, 10, "moonshine:") == 0) return setting.substr(10);
    if (setting.compare(0, 5, "file:") == 0) return setting.substr(5);
    if (const dect2::SpeechModel* m = dect2::findSpeechModel(setting)) return m->file;
    return "";
}
bool modelPresent(const std::string& setting) {
    if (setting.compare(0, 10, "moonshine:") == 0) {
        const std::string name = setting.substr(10);
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos || name.find('\\') != std::string::npos) return false;
        std::error_code ec;
        return std::filesystem::is_directory(dect2::u8file(modelsDir() + "/" + name), ec);
    }
    if (setting.compare(0, 5, "file:") == 0) {
        std::error_code ec;
        return std::filesystem::is_regular_file(dect2::u8file(modelsDir() + "/" + setting.substr(5)), ec);
    }
    const dect2::SpeechModel* m = dect2::findSpeechModel(setting);
    return m && dect2::speechModelInstalled(modelsDir(), *m);
}

// The model to use when the chosen one is not in the folder: the best one that is (English first: small.en hears the most words), so a
// downloaded or copied model works without visiting the settings. Nothing is downloaded here.
bool autoPickModel() {
    if (C.model == "whisper:none" || C.model == "moonshine:none") return false;
    if (modelPresent(C.model) || C.model.rfind("moonshine:", 0) == 0) return false;
    for (const char* id : {"small.en", "base.en", "tiny.en", "small", "base", "tiny"})
        if (modelPresent(id)) { C.model = id; return true; }
    for (const auto& f : dect2::userSpeechModels(modelsDir())) { C.model = "file:" + f; return true; }
    return false;
}

std::atomic<int64_t> gSoundMs{0};             // the last time the tap delivered sound
std::atomic<int64_t> gSegMs{0};               // the last time a speech piece was cut or a transcript line appeared
uint64_t gSegSeen = 0;
std::atomic<int64_t> gFirstSoundMs{0};        // when the sound began to arrive (0: none now)

// Keep the user's translation preference when switching temporarily to English or
// an English-only model, but never request translation in those configurations.
bool translationSupported() {
    if (C.lang == "en" || C.model.rfind("moonshine:", 0) == 0) return false;
    if (const dect2::SpeechModel* m = dect2::findSpeechModel(C.model)) return m->multilingual;
    // User-supplied models: use the filename until a backend is loaded.
    return !dect2::speechModelEnglishOnly(modelFile(C.model));
}

void applyConfig() {
    dect2::TranscribeConfig c = gTr.config();
    c.enabled = C.enabled;
    c.workers = 1;
    c.threads = C.threads;
    c.language = C.lang;
    c.translate = C.translate && translationSupported() && C.model.rfind("moonshine:", 0) != 0;
    c.queueMax = 6;
    gTr.configure(c);
    gStore.setLimits(20000, C.days);
    dect2::TranscribeConfig k = gCap.config();   // captions: the same language and CPU budget, one worker, no music, a short queue (late captions are useless)
    k.enabled = C.cap;
    k.workers = 1;
    k.threads = C.threads;
    k.language = C.lang;
    k.translate = C.translate && translationSupported() && C.model.rfind("moonshine:", 0) != 0;
    k.queueMax = 3;
    k.segmenter.rejectMusic = true;
    k.segmenter.maxSec = 12;     // shorter pieces: the first words show up sooner
    k.segmenter.hangSec = 0.5;
    gCap.configure(k);
    gCapStore.setLimits(2000, 1);
}

// ---------------------------------------------------------------- the taps

void onAirband(int rxId, int chan, const float* x, size_t n) {
    dect2::SourceInfo si;
    {
        std::lock_guard<std::mutex> lk(gMu);
        const auto it = gChanInfo.find(rxId);
        if (it == gChanInfo.end() || chan < 0 || (size_t)chan >= it->second.size()) return;
        si = it->second[(size_t)chan];
    }
    gTr.feed(si, x, n, 8000, wallMs());
}

void onCaptionSound(const float* mono, size_t n, int rate);

void onSound(const float* mono, size_t n, int rate) {
    { const int64_t t = wallMs(); gSoundMs.store(t, std::memory_order_relaxed); int64_t z = 0; gFirstSoundMs.compare_exchange_strong(z, t); }
    onCaptionSound(mono, n, rate);
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gCtx.valid) return;
        if (gCapCtx.valid && gCtx.mode == "audio") return;   // the caption engine already reads this sound; its lines also go to the transcripts (below)
    }
    dect2::SourceInfo si;
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gCtx.valid) return;
        si.key = 0x7000000000000LL; si.freqHz = gCtx.freqHz; si.mode = gCtx.mode; si.channel = gCtx.channel; si.domain = gCtx.domain;
    }
    gTr.feed(si, mono, n, rate, wallMs());
}

// the sound of the receiver being heard, for the captions: the audio clock first, then the speech
void onCaptionSound(const float* mono, size_t n, int rate) {
    dect2::SourceInfo si;
    dect2::CaptionTrack* tk = nullptr;
    {
        std::lock_guard<std::mutex> lk(gMu);
        if (!gCapCtx.valid) return;
        auto& t = gTracks[gCapCtx.rx];
        if (!t) t = std::make_unique<dect2::CaptionTrack>();
        tk = t.get();
        si.key = 0x7100000000000LL + gCapCtx.rx; si.freqHz = gCapCtx.freqHz; si.mode = "captions"; si.channel = "cap:" + std::to_string(gCapCtx.rx);
        si.domain = "general"; si.rejectMusic = true;
    }
    const int64_t now = wallMs();
    tk->onAudio(n, rate, now);
    gCap.feed(si, mono, n, rate, now);
}

// the aircraft on the screens, by the keys a spoken callsign or registration gives
void refreshAircraft() {
    std::map<std::string, std::string> m;
    for (App* r : receiversAll()) {
        if (!r) continue;
        for (const auto& ac : r->rx.adsb.aircraft) {
            if (ac.icao >> 24) continue;
            char hex[8];
            snprintf(hex, sizeof hex, "%06X", ac.icao & 0xFFFFFFu);
            if (!ac.callsign.empty()) m[dect2::acarsNormFlight(ac.callsign)] = hex;
            const std::string reg = adsbui::registrationOf(ac.icao);
            if (!reg.empty()) m[dect2::acarsNormReg(reg)] = hex;
        }
    }
    std::lock_guard<std::mutex> lk(gMu);
    gAircraft.swap(m);
}

void startLoad(const std::string& file) {
    if (gLoadTh.joinable()) gLoadTh.join();
    gLoadState = 1;
    gLoadingFile = file;
    const bool isMoon = C.model.rfind("moonshine:", 0) == 0;
    if (isMoon && !gMoonLibrariesScanned) scanMoonLibraries(modelsDir());
    gLoadingMoonLib = isMoon ? gMoonLibraries.selectedMoonshine : "";
    gLoadingOnnxLib = isMoon ? gMoonLibraries.selectedOnnx : "";
    const std::string path = modelsDir() + "/" + file;
    const int workers = 1;
    gLoadTh = dect2::namedThread("onair-asrload", [path, workers] {
        std::string err;
        const bool moonshine = std::filesystem::is_directory(dect2::u8file(path));
        auto b = moonshine ? dect2::makeMoonshineBackend(path, workers, &err)
                           : dect2::makeWhisperBackend(path, workers, &err);
        std::lock_guard<std::mutex> lk(gMu);
        if (b) { gPending = b; gLoadErr.clear(); gLoadState = 2; }
        else { DIAG(174763, "The speech model %s could not be loaded (%s), so transcripts and captions stay off.", path.c_str(), err.c_str()); gLoadErr = err; gLoadState = 3; }
    });
}

void ensureBackend() {
    const std::string want = ((C.enabled || C.cap) && modelPresent(C.model) && (C.model.rfind("moonshine:", 0) != 0 || dect2::moonshineAvailable())) ? modelFile(C.model) : "";
    if (gLoadState.load() == 2) {
        std::lock_guard<std::mutex> lk(gMu);
        if (gPending) {
            gBackend = gPending; gPending.reset();
            gBackendEnglishOnly = gBackend->englishOnly();
            gLoadedFile = gLoadingFile;
            gLoadedMoonLib = gLoadingMoonLib;
            gLoadedOnnxLib = gLoadingOnnxLib;
            gTr.setBackend(gBackend);
            gCap.setBackend(gBackend);
        }
    }
    if (want.empty()) {
        if (gBackend) { gTr.setBackend(nullptr); gCap.setBackend(nullptr); gBackend.reset(); gLoadedFile.clear(); }
        if (!C.enabled && !C.cap && gLoadState.load() == 3) gLoadState = 0;
        return;
    }
    if (gLoadState.load() == 1) return;
    if (C.model.rfind("moonshine:", 0) == 0 && !gMoonLibrariesScanned) scanMoonLibraries(modelsDir());
    const bool isMoon = C.model.rfind("moonshine:", 0) == 0;
    const bool matchesLoaded = want == gLoadedFile &&
        (!isMoon || (gLoadedMoonLib == gMoonLibraries.selectedMoonshine && gLoadedOnnxLib == gMoonLibraries.selectedOnnx));
    const bool failedSameRequest = gLoadState.load() == 3 && want == gLoadingFile &&
        (!isMoon || (gLoadingMoonLib == gMoonLibraries.selectedMoonshine && gLoadingOnnxLib == gMoonLibraries.selectedOnnx));
    if (!matchesLoaded && !failedSameRequest) startLoad(want);
}

bool backendReady() { return gBackend != nullptr; }

}  // namespace

bool& windowOpen() { return gWindowOpen; }

// What the transcription is doing, in plain words. level: 0 information, 1 working, 2 a problem the user can fix.
std::string statusLine(int* level) {
    int lv = 0;
    if (!level) level = &lv;
    *level = 0;
    if (!C.enabled && !C.cap) return tr("Transcription is off.");
    if (!dect2::whisperAvailable()) { *level = 2; return tr("This build has no speech engine."); }
    if (!modelPresent(C.model) && !backendReady()) { *level = 2; return tr("Model missing: download a speech model (Transcripts > Show settings > Speech model)."); }
    if (!backendReady()) {
        if (gLoadState.load() == 3) { *level = 2; std::lock_guard<std::mutex> lk(gMu); return gLoadErr.empty() ? std::string(tr("The speech model could not be loaded.")) : gLoadErr; }
        return tr("Model loading ...");
    }
    App* h = receiverHeard();
    const bool heard = h && h->engine.running();
    bool air = false;
    for (App* r : receiversAll()) if (r && r->engine.running() && r->rx.standard == 27 && C.enabled && C.airband) air = true;
    if (!heard && !air) return tr("Waiting for a receiver that plays sound.");
    if (heard && !air) {
        const int s = h->rx.standard;
        const bool covered = (C.cap && C.capModes.count(s) && dect2::captionStandardEligible(s)) || (C.enabled && (s == 16 ? C.marine : s == 27 ? C.airband : C.other));
        if (!covered) { *level = 2; return tr("This mode is not transcribed: tick 'Transcribe what I am listening to' in the settings."); }
    }
    const dect2::TranscriberStats a = gTr.stats(), b = gCap.stats();
    if (a.errors + b.errors && a.kept + b.kept == 0) { *level = 2; return std::string(tr("The speech engine failed: ")) + (a.error.empty() ? b.error : a.error); }
    if (a.busy || a.queued || b.busy || b.queued) { *level = 1; return tr("Transcribing speech ..."); }
    if (gTr.active() || gCap.active()) { *level = 1; return tr("Hearing speech ..."); }
    const int64_t now = wallMs(), snd = gSoundMs.load(), seg = gSegMs.load();
    if (!air && (snd == 0 || now - snd > 4000)) return tr("Waiting for sound from the receiver (is it muted or silent?).");
    if (a.segments + b.segments && a.kept + b.kept == 0 && now - seg < 10000) { return std::string(tr("Speech heard, nothing readable in it yet (")) + (a.lastFilter.empty() ? "noise" : a.lastFilter) + ")"; }
    if (now - std::max(seg, gFirstSoundMs.load()) > 10000) return tr("No speech heard in the last 10 s.");
    return tr("Listening for speech.");
}

bool indicator(std::string& text) {
    if (!C.enabled && !C.cap) return false;
    // Transcription is on by default but does nothing until a speech model is installed: that is not news worth a line in every
    // user's status bar. The installer stays in View > Transcripts and in the airband panel; the line shows once a model is there.
    if (!(dect2::whisperAvailable() || dect2::moonshineAvailable()) || (!modelPresent(C.model) && !backendReady())) return false;
    int lv = 0;
    text = statusLine(&lv);
    (void)lv;
    return true;
}

void shotReport() {
    load();
    int lv = 0;
    const std::string sl = statusLine(&lv);
    const dect2::TranscriberStats a = gTr.stats(), b = gCap.stats();
    fprintf(stderr, "tx: status [%d] %s | model %s ready %d | transcripts: segments %llu kept %llu filtered %llu (%s) errors %llu | captions: segments %llu kept %llu filtered %llu (%s) music %llu\n", lv, sl.c_str(),
            C.model.c_str(), backendReady() ? 1 : 0, (unsigned long long)a.segments, (unsigned long long)a.kept, (unsigned long long)a.filtered, a.lastFilter.c_str(), (unsigned long long)a.errors,
            (unsigned long long)b.segments, (unsigned long long)b.kept, (unsigned long long)b.filtered, b.lastFilter.c_str(), (unsigned long long)b.musicSkipped);
    for (const auto& e : gStore.find({}, 100)) fprintf(stderr, "tx: line [%s] %s\n", e.mode.c_str(), e.text.c_str());
}

void tick(App& first) {
    (void)first;
    load();
    if (!gStarted) {
        gStarted = true;
        gStore.setLimits(20000, C.days);
        std::string err;
        gStore.attachFile(plat::dataDir() + "/transcripts/transcripts.jsonl", &err);
        gTr.setResolver([](const dect2::CallsignHit& h) {
            std::lock_guard<std::mutex> lk(gMu);
            const auto it = gAircraft.find(h.key);
            return it == gAircraft.end() ? std::string() : it->second;
        });
        gCap.setOnEntry([](const dect2::TranscriptEntry& e) {
            int rx = 0;
            if (e.channel.compare(0, 4, "cap:") != 0) return;
            rx = atoi(e.channel.c_str() + 4);
            std::lock_guard<std::mutex> lk(gMu);
            const auto it = gTracks.find(rx);
            if (it != gTracks.end() && it->second) it->second->add(e);
            if (C.enabled && C.other && gCtx.valid && gCtx.mode == "audio") {   // one speech engine for both: the line is a transcript too
                dect2::TranscriptEntry t = e;
                t.mode = "audio"; t.channel = gCtx.channel; t.freqHz = gCapCtx.freqHz; t.id = 0;
                gStore.add(t);
            }
            gSegMs.store(wallMs(), std::memory_order_relaxed);
        });
        applyConfig();
    }
    static double last = 0;
    const double now = ImGui::GetTime();
    if (now - last < 1.0) return;
    last = now;
    static int slow = 0;
    if (++slow % 600 == 0) gStore.prune((int64_t)time(nullptr));
    static bool lastEnabled = false;
    static int lastThreads = 0;
    static std::string lastLang;
    static bool lastTr = false, lastCap = false;
    if (lastEnabled != C.enabled || lastThreads != C.threads || lastLang != C.lang || lastTr != C.translate || lastCap != C.cap) { lastCap = C.cap; lastTr = C.translate; lastEnabled = C.enabled; lastThreads = C.threads; lastLang = C.lang; applyConfig(); }
    if ((C.enabled || C.cap) && autoPickModel()) save();
    ensureBackend();
    { const int64_t nw = wallMs(); if (gSoundMs.load() && nw - gSoundMs.load() > 4000) gFirstSoundMs.store(0);
      const uint64_t sg = gTr.stats().segments + gCap.stats().segments; if (sg != gSegSeen) { gSegSeen = sg; gSegMs.store(nw); } }
    if (gDl.state() == dect2::ModelDownloader::State::Done) {
        const std::string id = gDl.modelId();
        if (!modelPresent(C.model) && C.model != "whisper:none" && C.model != "moonshine:none") { C.model = id; save(); }
    }

    const bool ready = backendReady() && C.enabled;
    if (ready) refreshAircraft();
    // airband: a tap on each channel of each running airband receiver
    for (App* r : receiversAll()) {
        if (!r) continue;
        const bool air = r->engine.running() && r->rx.standard == 27;
        const bool want = ready && C.airband && air;
        bool& on = gTapOn[r->rxId];
        if (air) {
            std::vector<dect2::SourceInfo> v;
            for (const auto& c : r->rx.airband.channels) {
                dect2::SourceInfo si;
                si.key = ((int64_t)r->rxId << 40) | (int64_t)std::llround(c.freqHz / 100.0);
                si.freqHz = c.freqHz; si.mode = "airband"; si.domain = "aviation";
                si.channel = c.name + (c.label.empty() ? std::string() : " " + c.label);
                v.push_back(si);
            }
            std::lock_guard<std::mutex> lk(gMu);
            gChanInfo[r->rxId] = std::move(v);
        }
        static std::map<int, double> tapAt;   // installed again now and then: a receiver closed and opened again under the same number has a new engine
        if (want && on && ImGui::GetTime() - tapAt[r->rxId] > 15) on = false;
        if (want && !on) {
            const int id = r->rxId;
            tapAt[id] = ImGui::GetTime();
            r->engine.airband().setChannelTap([id](int chan, const float* x, size_t n) { onAirband(id, chan, x, n); });
            on = true;
        } else if (!want && on) {
            r->engine.airband().setChannelTap(nullptr);
            on = false;
            std::lock_guard<std::mutex> lk(gMu);
            gChanInfo.erase(r->rxId);
        }
    }
    // the sound of the other modes
    bool sink = false;
    {
        std::lock_guard<std::mutex> lk(gMu);
        // captions: the receiver being heard, when its mode is ticked
        const CapCtx old = gCapCtx;
        gCapCtx.valid = false;
        if (C.cap && backendReady()) if (App* h = receiverHeard()) if (h->engine.running() && C.capModes.count(h->rx.standard) && dect2::captionStandardEligible(h->rx.standard)) {
            gCapCtx.valid = true; gCapCtx.rx = h->rxId; gCapCtx.std = h->rx.standard; gCapCtx.freqHz = h->freqMhz * 1e6;
        }
        if (old.valid && (!gCapCtx.valid || old.rx != gCapCtx.rx || old.std != gCapCtx.std || std::fabs(old.freqHz - gCapCtx.freqHz) > 1.0)) {
            // another station: the pending speech goes, the cues start again
            gCap.endSource(0x7100000000000LL + old.rx, wallMs());
            const auto it = gTracks.find(old.rx);
            if (it != gTracks.end() && it->second) it->second->reset();
        }
        gCtx.valid = false;
        if (ready && (C.marine || C.other)) {
            if (App* h = receiverHeard()) if (h->engine.running()) {
                const int std_ = h->rx.standard;
                if (std_ == 16 && C.marine) { gCtx.valid = true; gCtx.mode = "marine"; gCtx.domain = "marine"; gCtx.channel = "Marine VHF"; }
                else if (std_ != 16 && std_ != 27 && C.other) { gCtx.valid = true; gCtx.mode = "audio"; gCtx.domain = "general"; gCtx.channel = "Audio"; }
                gCtx.freqHz = h->freqMhz * 1e6;
            }
        }
        sink = gCtx.valid || gCapCtx.valid;
    }
    if (sink && !gSinkOn) { dect2::AudioTap::instance().setSink(onSound); gSinkOn = true; }
    else if (!sink && gSinkOn) { dect2::AudioTap::instance().setSink(nullptr); gSinkOn = false; gTr.endSource(0x7000000000000LL, wallMs()); }
}

void shutdown() {
    if (gSinkOn) { dect2::AudioTap::instance().setSink(nullptr); gSinkOn = false; }
    for (App* r : receiversAll()) if (r && gTapOn[r->rxId]) { r->engine.airband().setChannelTap(nullptr); gTapOn[r->rxId] = false; }
    gTr.shutdown();     // a transcription in progress is aborted and joined: it must not run while the program's statics are destroyed
    gCap.shutdown();
    gTr.setBackend(nullptr);
    gCap.setBackend(nullptr);
    gBackend.reset();
    if (gLoadTh.joinable()) gLoadTh.join();
    gDl.cancel();
}

// ---------------------------------------------------------------- the lists

namespace {

std::string tstr(int64_t wall) { return acarslog::timeText(wall); }

std::string entryLine(const dect2::TranscriptEntry& e) {
    std::string s = e.channel.empty() ? std::string() : e.channel + ": ";
    s += e.text;
    if (!e.translation.empty()) s += "\n[EN] " + e.translation;
    return s;
}

std::vector<dect2::TranscriptEntry> voiceFor(uint32_t icao, const std::string& callsign, const std::string& reg, size_t limit) {
    std::vector<dect2::TranscriptEntry> v = gStore.forAircraft(callsign, reg, limit);
    if (!(icao >> 24) && icao) {
        char hex[8];
        snprintf(hex, sizeof hex, "%06X", icao & 0xFFFFFFu);
        dect2::TranscriptFilter f;
        f.aircraft = hex;
        for (auto& e : gStore.find(f, limit)) {
            if (e.aircraft != hex) continue;
            bool dup = false;
            for (const auto& x : v) dup |= x.id == e.id;
            if (!dup) v.push_back(e);
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.wall != b.wall ? a.wall > b.wall : a.id > b.id; });
        if (limit && v.size() > limit) v.resize(limit);
    }
    return v;
}

}  // namespace

size_t voiceCount(uint32_t icao, const std::string& callsign, const std::string& reg) { return voiceFor(icao, callsign, reg, 50).size(); }

std::vector<dect2::AcarsLogEntry> voiceForTrack(const std::string& id, const std::string& label, int holders) {
    uint32_t icao = 0;
    bool hex = id.size() == 6;
    for (unsigned char c : id) if (!isxdigit(c)) hex = false;
    std::string reg;
    if (hex) { icao = (uint32_t)strtoul(id.c_str(), nullptr, 16); reg = adsbui::registrationOf(icao); }
    else reg = id;
    std::vector<dect2::AcarsLogEntry> out;
    for (const auto& e : voiceFor(icao, holders == 1 ? label : std::string(), reg, 20)) {
        if (e.mode != "airband" || (e.confidenceAvailable && e.confidence < 0.5f) || e.text.size() < 8) continue;
        dect2::AcarsLogEntry m;
        m.wall = e.wall; m.mode = "VOICE"; m.freqMHz = e.freqHz / 1e6; m.label = ""; m.cat = dect2::AcarsCat::Free;
        m.text = e.text; m.flight = e.callsign; m.downlink = true;
        out.push_back(std::move(m));
    }
    return out;
}

void voiceSection(const App& a, uint32_t icao, const std::string& callsign, const std::string& reg) {
    const std::vector<dect2::TranscriptEntry> v = voiceFor(icao, callsign, reg, 30);
    ImGui::Dummy(ImVec2(0, 4 * gUi));
    ImGui::Separator();
    ImGui::Text("%s%s", tr("Voice (radio calls)"), v.empty() ? "" : (" (" + std::to_string(v.size()) + ")").c_str());
    if (!v.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copy##voice"))) {
            std::string t;
            for (const auto& e : v) t += tstr(e.wall) + "  " + entryLine(e) + "\n";
            ImGui::SetClipboardText(t.c_str());
        }
    }
    if (v.empty()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", C.enabled ? tr("No radio call with this aircraft's callsign heard yet") : tr("Live transcription is off (View > Transcripts). Airband calls would be listed here by callsign."));
        ImGui::PopTextWrapPos();
        return;
    }
    ImGui::BeginChild("##voicelog", ImVec2(0, 180 * gUi), true);
    ImGui::PushFont(a.mono, 0);
    for (const auto& e : v) {
        ImGui::TextDisabled("%s  %s%s%s", tstr(e.wall).c_str(), e.channel.c_str(), e.speaker.empty() ? "" : "  ", e.speaker.c_str());
        ImGui::PushTextWrapPos(0); ImGui::TextUnformatted(e.text.c_str());
        if (!e.translation.empty()) { ImGui::TextDisabled("EN"); ImGui::SameLine(); ImGui::TextUnformatted(e.translation.c_str()); }
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 2 * gUi));
    }
    ImGui::PopFont();
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the live captions of the airband screen

void captions(App& a) {
    load();
    ImGui::PushID("##captions");
    std::string ind;
    if (!C.enabled || !backendReady()) {
        ImGui::AlignTextToFramePadding();
        {   // no model installed is the normal state: say "off" quietly, never a warning (the button below opens the installer)
            int lv = 0;
            const bool noModel = !(dect2::whisperAvailable() || dect2::moonshineAvailable()) || (!modelPresent(C.model) && !backendReady());
            const std::string sl = C.enabled && !noModel ? statusLine(&lv) : std::string(tr("Live captions: off."));
            ImGui::TextColored(lv == 2 ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", sl.c_str());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Set up transcripts"))) gWindowOpen = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turns what is said on the airband channels into text as it is heard. It needs a speech model that you download or put in the models folder."));
        ImGui::Spacing();
        ImGui::PopID();
        return;
    }
    const dect2::TranscriberStats st = gTr.stats();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal::warnAmber(), "%s", tr("Live captions"));
    ImGui::SameLine();
    if (st.busy || st.queued) ImGui::TextDisabled("%s", tr("working ..."));
    else if (gTr.active()) ImGui::TextDisabled("%s", tr("listening ..."));
    else ImGui::TextDisabled("%s", " ");
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Transcripts"))) gWindowOpen = true;
    if (!C.airband) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", tr("Airband is off in the transcript settings")); }
    dect2::TranscriptFilter f;
    f.mode = "airband";
    const auto v = gStore.find(f, 3);
    ImGui::BeginChild("##capbox", ImVec2(0, 78 * gUi), true);
    if (v.empty()) { int lv = 0; const std::string sl = statusLine(&lv); ImGui::TextDisabled("%s", sl.c_str()); }
    ImGui::PushTextWrapPos(0);
    for (auto it = v.rbegin(); it != v.rend(); ++it) {
        ImGui::TextDisabled("%s  %s", tstr(it->wall).c_str(), it->channel.c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(it->text.c_str());
    }
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    (void)a;
    ImGui::PopID();
}

// ---------------------------------------------------------------- the Transcripts window

namespace {

struct Ui {
    char query[96] = "";
    int searchIn = 0;                         // all, channel, text, aircraft
    int freqSel = 0;                           // 0 all, else index into freqs
    std::vector<double> freqs;
    std::vector<dect2::TranscriptEntry> rows;
    uint64_t ver = ~0ull;
    std::string key;
    double at = -10;
    size_t total = 0;
    int selected = -1;
    bool askDelete = false;
    const dect2::SpeechModel* askModel = nullptr;
    std::string note;
    bool settings = false;
    float splitRatio = 0.5f;
    bool splitLoaded = false;
    bool wasOpen = false;
    bool showNotice = false;
};
Ui U;

// Search the selected field, case-insensitively, keeping the existing all-words behaviour.
bool searchMatch(const dect2::TranscriptEntry& e, const char* query, int field) {
    std::string hay;
    if (field == 1) hay = e.channel;
    else if (field == 2) hay = e.text + " " + e.translation;
    else if (field == 3) hay = e.callsign + " " + e.reg + " " + e.aircraft;
    else hay = e.text + " " + e.translation + " " + e.channel + " " + e.callsign + " " + e.reg + " " + e.aircraft;
    std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    std::istringstream words(query);
    std::string word;
    while (words >> word) {
        std::transform(word.begin(), word.end(), word.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (hay.find(word) == std::string::npos) return false;
    }
    return true;
}

void refreshRows() {
    dect2::TranscriptFilter f;
    if (U.freqSel > 0 && (size_t)U.freqSel <= U.freqs.size()) f.freqHz = U.freqs[(size_t)U.freqSel - 1];
    char k[256];
    snprintf(k, sizeof k, "%s|%d|%d", U.query, U.searchIn, U.freqSel);
    const double now = ImGui::GetTime();
    if (gStore.version() == U.ver && U.key == k && now - U.at < 5) return;
    U.ver = gStore.version(); U.key = k; U.at = now;
    U.rows = gStore.find(f);
    U.rows.erase(std::remove_if(U.rows.begin(), U.rows.end(), [](const auto& e) {
        return !searchMatch(e, U.query, U.searchIn);
    }), U.rows.end());
    U.total = gStore.size();
    std::map<long long, int> fr;
    for (const auto& e : gStore.find({})) if (e.freqHz > 0) fr[(long long)std::llround(e.freqHz / 1000.0)]++;
    const double cur = (U.freqSel > 0 && (size_t)U.freqSel <= U.freqs.size()) ? U.freqs[(size_t)U.freqSel - 1] : 0;
    U.freqs.clear();
    for (auto& kv : fr) U.freqs.push_back((double)kv.first * 1000.0);
    U.freqSel = 0;
    if (cur > 0) for (size_t i = 0; i < U.freqs.size(); i++) if (std::fabs(U.freqs[i] - cur) < 1500) U.freqSel = (int)i + 1;
}

void exportRows(const char* ext) {
    std::string def = std::string("onair_transcripts.") + ext;
    const std::string path = plat::saveFileDialog(def.c_str());
    if (path.empty()) return;
    std::string data;
    if (!strcmp(ext, "txt")) data = dect2::transcriptsTxt(U.rows);
    else if (!strcmp(ext, "srt")) data = dect2::transcriptsSrt(U.rows);
    else if (!strcmp(ext, "json")) data = dect2::transcriptsJson(U.rows);
    else data = dect2::transcriptsCsv(U.rows);
    if (!strcmp(ext, "csv")) data = "\xEF\xBB\xBF" + data;   // Excel reads it as UTF-8 then
    FILE* f = dect2::u8fopen(path, "wb");
    char nb[1024];
    if (!f) { DIAG(357122, "Writing the transcript file %s failed (the folder may be read-only or missing), so nothing was saved.", path.c_str()); snprintf(nb, sizeof nb, tr("Cannot write %s"), path.c_str()); U.note = nb; return; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    snprintf(nb, sizeof nb, tr("Saved %s"), path.c_str());
    U.note = nb;
}

const char* kLangs[][2] = {{"en", "English"}, {"auto", "Auto-detect (needs a multilingual model)"}, {"de", "Deutsch"}, {"fr", "Français"}, {"es", "Español"},
                           {"it", "Italiano"}, {"nl", "Nederlands"}, {"pl", "Polski"}, {"pt", "Português"}, {"lt", "Lietuvių"}, {"ru", "Русский"}, {"hu", "Magyar"},
                           {"tr", "Türkçe"}, {"ar", "العربية"}, {"zh", "中文"}, {"ja", "日本語"}};

// A lightweight heading and rule keep the half-width settings pane scannable.
void settingsSection(const char* heading) {
    ImGui::Spacing();
    ImGui::TextUnformatted(heading);
    ImGui::Separator();
    ImGui::Spacing();
}

// Both speech engines use the same Models heading, folder action and spacing.
void settingsModelsHeading(const std::string& dir, const char* buttonId, const char* tooltip = nullptr) {
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Models"));
    const char* label = tr("Open folder");
    const float width = ImGui::CalcTextSize(label).x + 2 * ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + ImGui::GetStyle().ItemSpacing.x,
                             ImGui::GetContentRegionMax().x - width));
    if (ImGui::SmallButton(buttonId)) {
        std::error_code ec;
        std::filesystem::create_directories(dect2::u8file(dir), ec);
        dect2::openUrl(dir);
    }
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s\n%s", dir.c_str(), tooltip);
    ImGui::Separator();
    ImGui::Spacing();
}

// Position the Installed label or Download button like Whisper's model rows.
void settingsModelActionAlignment(bool installed) {
    const float labelEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    const char* action = installed ? tr("Installed") : tr("Download");
    const float actionWidth = ImGui::CalcTextSize(action).x +
        (installed ? 0.0f : 2 * ImGui::GetStyle().FramePadding.x);
    const float right = ImGui::GetContentRegionMax().x - actionWidth;
    // When the pane is too narrow the action follows on its own line.
    if (right > labelEnd + ImGui::GetStyle().ItemSpacing.x) ImGui::SameLine(right);
}

void refreshModelLists() {
    const double now = ImGui::GetTime();
    if (now - gUserFilesAt <= 3.0) return;
    gUserFiles = dect2::userSpeechModels(modelsDir());
    gMoonshineDirs.clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dect2::u8file(modelsDir()), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const auto name = it->path().filename().string();
        if (name.rfind("moonshine-", 0) == 0) gMoonshineDirs.push_back(name);
    }
    std::sort(gMoonshineDirs.begin(), gMoonshineDirs.end());
    gUserFilesAt = now;
}
void selectModel(const std::string& id) {
    C.model = id;
    if (id.rfind("moonshine:", 0) == 0) {
        C.translate = false;
        const auto end = id.find_last_of('-');
        if (end != std::string::npos) C.lang = id.substr(end + 1);
    }
    save(); applyConfig();
}
void modelSelector() {
    const std::string dir = modelsDir();
    refreshModelLists();
    bool moon = C.model.rfind("moonshine:", 0) == 0;
    const float labelWidth = 70 * gUi;
    ImGui::TextUnformatted(tr("Engine")); ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##txengine", moon ? "Moonshine" : "Whisper")) {
        if (ImGui::Selectable("Whisper", !moon)) {
            std::string chosen = "whisper:none";
            for (const auto& m : dect2::speechModels()) if (dect2::speechModelInstalled(dir, m)) { chosen = m.id; break; }
            if (chosen == "whisper:none" && !gUserFiles.empty()) chosen = "file:" + gUserFiles.front();
            selectModel(chosen);
        }
        ImGui::BeginDisabled(!dect2::moonshineAvailable());
        if (ImGui::Selectable("Moonshine", moon))
            selectModel(gMoonshineDirs.empty() ? "moonshine:none" : "moonshine:" + gMoonshineDirs.front());
        ImGui::EndDisabled();
        ImGui::EndCombo();
    }
    // The engine can change in the combo above; do not reuse the stale value.
    moon = C.model.rfind("moonshine:", 0) == 0;
    std::string cur = tr("none");
    if (modelPresent(C.model)) {
        if (moon) cur = C.model.substr(std::string("moonshine:").size());
        else if (C.model.rfind("file:", 0) == 0) cur = C.model.substr(std::string("file:").size());
        else cur = C.model;
    }
    ImGui::TextUnformatted(tr("Model")); ImGui::SameLine(labelWidth);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##txmodel", cur.c_str())) {
        if (moon) {
            for (const auto& name : gMoonshineDirs)
                if (ImGui::Selectable(name.c_str(), C.model == "moonshine:" + name)) selectModel("moonshine:" + name);
        } else {
            for (const auto& m : dect2::speechModels())
                if (dect2::speechModelInstalled(dir, m) && ImGui::Selectable(m.id, C.model == m.id)) selectModel(m.id);
            for (const auto& f : gUserFiles)
                if (ImGui::Selectable(f.c_str(), C.model == "file:" + f)) selectModel("file:" + f);
        }
        ImGui::EndCombo();
    }
    if (!modelPresent(C.model)) ImGui::TextDisabled("No installed model selected. See Speech model settings.");
}

// Wrap long runtime messages at the current settings-pane boundary.
void processingMessage(const char* message, const ImVec4* color = nullptr) {
    ImGui::PushTextWrapPos(0);
    if (color) ImGui::TextColored(*color, "%s", message);
    else ImGui::TextDisabled("%s", message);
    ImGui::PopTextWrapPos();
}

void whisperModelEntry(const std::string& dir, const char* id) {
    const dect2::SpeechModel* m = dect2::findSpeechModel(id);
    if (!m) return;
    const bool installed = dect2::speechModelInstalled(dir, *m);
    ImGui::PushID(id);
    ImGui::Text("%s (%d MB)", m->id, m->mib);
    settingsModelActionAlignment(installed);
    if (installed) ImGui::TextDisabled("%s", tr("Installed"));
    else if (ImGui::SmallButton(tr("Download"))) U.askModel = m;
    ImGui::PopID();
    ImGui::Spacing();
}

void settingsPanel(App& a) {
    if (!ImGui::BeginTabBar("##txsettingtabs")) return;
    if (ImGui::BeginTabItem(trw("Transcription"))) {
        settingsSection(tr("Speech engine and model"));
        modelSelector();
        bool changed = ImGui::Checkbox(tr("Transcribe speech"), &C.enabled);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Enable speech recognition for the selected sources."));
        if (changed) { save(); applyConfig(); }

        settingsSection(tr("Source"));
        bool sourcesChanged = false;
        ImGui::BeginDisabled(!C.enabled);
        sourcesChanged |= ImGui::Checkbox(tr("Airband channels"), &C.airband);
        sourcesChanged |= ImGui::Checkbox(tr("Marine VHF"), &C.marine);
        sourcesChanged |= ImGui::Checkbox(tr("Audio (transcribe what I am listening to)"), &C.other);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Current receiver audio in other supported modes (FM, AM, amateur, CB, digital voice ...). Airband and Marine VHF use their own source switches."));
        ImGui::EndDisabled();
        if (sourcesChanged) { save(); applyConfig(); }

        settingsSection(tr("Table headings"));
        const char* names[] = {tr("Time (UTC)"), tr("Time (local)"), tr("Channel"), tr("Frequency"), tr("Text"), tr("Aircraft"), tr("Confidence")};
        for (int i = 0; i < 7; ++i) {
            bool shown = (C.columns & (1 << i)) != 0;
            if (ImGui::Checkbox((std::string(names[i]) + "##txcol" + std::to_string(i)).c_str(), &shown)) {
                if (shown) C.columns |= (1 << i); else C.columns &= ~(1 << i);
                save();
            }
            if (i % 2 == 0 && i != 6) ImGui::SameLine();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Average word confidence reported by the speech recognizer; not a guarantee of accuracy."));

        settingsSection(tr("Translation"));
        std::string langName = C.lang;
        for (const auto& l : kLangs) if (C.lang == l[0]) langName = l[1];
        ImGui::TextUnformatted(tr("Language"));
        ImGui::SameLine(85 * gUi);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##txlang", langName.c_str())) {
            for (const auto& l : kLangs) {
                if (ImGui::Selectable(l[1], C.lang == l[0])) { C.lang = l[0]; save(); applyConfig(); }
            }
            ImGui::EndCombo();
        }
        const bool canTranslate = translationSupported();
        bool checked = canTranslate && C.translate;
        ImGui::BeginDisabled(!canTranslate);
        if (ImGui::Checkbox(tr("Translate to English"), &checked)) { C.translate = checked; save(); applyConfig(); }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", canTranslate
                ? tr("Translate foreign-language speech into English offline. Auto-detect is supported with a multilingual model.")
                : tr("Translation requires a multilingual model and a language other than English. The preference is remembered."));
        }
        if (!canTranslate && C.lang != "en") ImGui::TextDisabled("%s", tr("Select a multilingual model to enable translation."));

        settingsSection(tr("Processing and storage"));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderInt(tr("CPU threads"), &C.threads, 1, 8)) { save(); applyConfig(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Processor threads used by the speech recognizer for a transmission."));
        ImGui::SetNextItemWidth(160 * gUi);
        if (ImGui::SliderInt(tr("Keep transcripts (days)"), &C.days, 1, 90)) { save(); gStore.setLimits(20000, C.days); }
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(trw("Subtitles and captions"))) {
        captionsSettings(a);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem(trw("Speech model"))) {
        if (ImGui::BeginTabBar("##txenginetabs")) {
            if (ImGui::BeginTabItem(trw("Whisper"))) {
                const std::string dir = modelsDir();
                const double now = ImGui::GetTime();
                if (now - gUserFilesAt > 3) { gUserFiles = dect2::userSpeechModels(dir); gUserFilesAt = now; }

                settingsModelsHeading(dir, tr("Open folder"),
                    tr("Put a whisper.cpp model file (ggml-*.bin) here to use it."));

                const int ls = gLoadState.load();
                if (C.enabled && !modelPresent(C.model)) ImGui::TextColored(pal::warnAmber(), "%s", tr("No speech model installed or selected."));
                else if (C.enabled && ls == 1) ImGui::TextDisabled("%s", tr("Loading the speech model ..."));
                else if (C.enabled && ls == 3) { std::lock_guard<std::mutex> lk(gMu); ImGui::TextColored(pal::badRed(), "%s", gLoadErr.c_str()); }
                else if (C.enabled && backendReady()) ImGui::TextColored(pal::okGreen(), "%s", tr("Speech model loaded."));
                if (!dect2::whisperAvailable()) ImGui::TextColored(pal::badRed(), "%s", tr("This build has no speech engine."));

                const auto st = gDl.state();
                if (st == dect2::ModelDownloader::State::Downloading || st == dect2::ModelDownloader::State::Verifying) {
                    const double tot = (double)std::max<uint64_t>(1, gDl.totalBytes());
                    char ov[96];
                    snprintf(ov, sizeof ov, "%s %.0f / %.0f MB", st == dect2::ModelDownloader::State::Verifying ? tr("checking") : gDl.modelId().c_str(), (double)gDl.doneBytes() / 1048576.0, tot / 1048576.0);
                    ImGui::ProgressBar((float)(gDl.doneBytes() / tot), ImVec2(-1, 0), ov);
                    if (ImGui::SmallButton(tr("Cancel##txdl"))) gDl.cancel();
                } else if (st == dect2::ModelDownloader::State::Failed) {
                    ImGui::TextColored(pal::badRed(), "%s", gDl.error().c_str());
                }
                ImGui::TextDisabled("%s", tr("English-only models"));
                for (const char* id : {"tiny.en", "base.en", "small.en"}) whisperModelEntry(dir, id);
                ImGui::Spacing();
                ImGui::TextDisabled("%s", tr("Multilingual models"));
                for (const char* id : {"tiny", "base", "small"}) whisperModelEntry(dir, id);
                if (!gUserFiles.empty()) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("%s", tr("Other installed models"));
                    for (const auto& f : gUserFiles) ImGui::Text("%s (%s)", f.c_str(), tr("Installed"));
                }

                settingsSection(tr("Engine information"));
                ImGui::Text("%s: %s", tr("Library version"), dect2::whisperLibraryVersion());
                ImGui::Text("%s: %s", tr("Backend"), dect2::whisperAvailable() ? tr("Whisper (CPU)") : tr("Unavailable"));
                ImGui::Text("%s: %s", tr("Model status"), backendReady() ? tr("Ready") : ls == 1 ? tr("Loading") : ls == 3 ? tr("Failed") : tr("Not loaded"));
                settingsSection(tr("Processing"));
                const dect2::TranscriberStats ts = gTr.stats();
                std::ostringstream processing;
                processing << tr("Heard") << ' ' << ts.segments
                           << ", " << tr("kept") << ' ' << ts.kept
                           << ", " << tr("left out") << ' ' << ts.filtered
                           << ' ' << tr("(silence, noise, repeats)")
                           << ", " << tr("skipped") << ' ' << ts.dropped
                           << ' ' << tr("(too slow)")
                           << ", " << tr("speed") << ' ' << std::fixed << std::setprecision(2)
                           << ts.avgRtf << ' ' << tr("x real time");
                const std::string processingText = processing.str();
                processingMessage(processingText.c_str());
                ImGui::EndTabItem();
            }
            // Linux desktop only. Preserve a visible, disabled tab elsewhere.
            ImGui::BeginDisabled(!dect2::moonshineAvailable());
            if (ImGui::BeginTabItem("Moonshine")) {
                const std::string dir = modelsDir();
                refreshModelLists();
                if (!gMoonLibrariesScanned) scanMoonLibraries(dir);
                settingsSection("Runtime libraries");
                const bool librariesSelected = !gMoonLibraries.selectedMoonshine.empty() && !gMoonLibraries.selectedOnnx.empty();
                ImGui::TextDisabled("Using first colocated libraries for Moonshine and ONNX");
                // Show paths relative to the searched models root, never absolute system paths.
                const auto libraryLabel = [&](const std::string& path) {
                    if (path.empty()) return std::string("not found");
                    std::error_code ec;
                    const auto rel = std::filesystem::relative(dect2::u8file(path), dect2::u8file(dir), ec);
                    if (!ec && !rel.empty() && *rel.begin() != "..") return rel.generic_string();
                    return std::filesystem::path(dect2::u8file(path)).filename().string();
                };
                ImGui::TextUnformatted("Moonshine:");
                ImGui::TextDisabled("%s", libraryLabel(gMoonLibraries.selectedMoonshine).c_str());
                ImGui::TextUnformatted("ONNX Runtime:");
                ImGui::TextDisabled("%s", libraryLabel(gMoonLibraries.selectedOnnx).c_str());

                settingsModelsHeading(dir, "Open folder##moon");
                const auto downloadState = gMoonDl.state();
                const bool downloading = downloadState == dect2::MoonshineModelDownloader::State::Downloading ||
                                         downloadState == dect2::MoonshineModelDownloader::State::Verifying;
                if (downloading) {
                    const double total = (double)std::max<uint64_t>(1, gMoonDl.totalBytes());
                    ImGui::ProgressBar((float)(gMoonDl.doneBytes() / total), ImVec2(-1, 0));
                    if (ImGui::SmallButton("Cancel Moonshine download")) gMoonDl.cancel();
                } else if (downloadState == dect2::MoonshineModelDownloader::State::Failed)
                    ImGui::TextColored(pal::badRed(), "%s", gMoonDl.error().c_str());
                struct M { const char* name; const char* lang; int arch; };
                const M presets[] = {{"moonshine-tiny-streaming-en", "en", 2}, {"moonshine-small-streaming-en", "en", 4},
                                     {"moonshine-tiny-streaming-de", "de", 2}, {"moonshine-small-streaming-de", "de", 4}};
                static std::map<std::string, uint64_t> cachedModelSizes;
                static double sizesUpdatedAt = -100;
                const bool refreshSizes = ImGui::GetTime() - sizesUpdatedAt > 5;
                const auto modelLine = [&](const std::string& name, const char* lang, int arch, bool canDownload) {
                    const auto path = dect2::u8file(dir + "/" + name);
                    std::error_code ec;
                    const bool installed = std::filesystem::is_directory(path, ec);
                    uint64_t size = cachedModelSizes[name];
                    if (installed && (refreshSizes || !size)) {
                        size = 0;
                        std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
                        for (; !ec && it != end; it.increment(ec)) {
                            if (it->is_regular_file(ec)) size += it->file_size(ec);
                        }
                        cachedModelSizes[name] = size;
                    }
                    ImGui::PushID(name.c_str());
                    if (installed) ImGui::Text("%s (%.1f MB)", name.c_str(), size / 1048576.0);
                    else ImGui::Text("%s (size unknown)", name.c_str());
                    settingsModelActionAlignment(installed);
                    if (installed) ImGui::TextDisabled("%s", tr("Installed"));
                    else if (canDownload) {
                        ImGui::BeginDisabled(downloading || !librariesSelected);
                        if (ImGui::SmallButton(tr("Download"))) gMoonDl.start(dir, lang, arch, name);
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                    ImGui::Spacing();
                };
                if (refreshSizes) sizesUpdatedAt = ImGui::GetTime();
                for (const auto& m : presets) modelLine(m.name, m.lang, m.arch, true);
                for (const auto& name : gMoonshineDirs) {
                    bool catalog = false;
                    for (const auto& m : presets) if (name == m.name) catalog = true;
                    if (!catalog) modelLine(name, "", 0, false);
                }

                settingsSection("Engine information");
                const auto info = dect2::moonshineEngineInfo();
                const bool selectedInfo = info.moonshinePath == gMoonLibraries.selectedMoonshine && info.onnxPath == gMoonLibraries.selectedOnnx;
                ImGui::Text("Moonshine library version: %s", selectedInfo && !info.moonshineVersion.empty() ? info.moonshineVersion.c_str() : "Unknown (library unavailable)");
                ImGui::Text("ONNX Runtime version: %s", selectedInfo && !info.onnxVersion.empty() ? info.onnxVersion.c_str() : "Unknown (library unavailable)");
                const bool moonLoaded = gBackend && std::strcmp(gBackend->name(), "moonshine-voice") == 0 && gLoadedFile == modelFile(C.model);
                ImGui::Text("Model status: %s", moonLoaded ? "Loaded" : "Not loaded");

                settingsSection("Processing");
                const auto stats = gTr.stats();
                std::ostringstream moonProcessing;
                moonProcessing << "Heard " << stats.segments << ", kept " << stats.kept
                               << ", filtered " << stats.filtered << ", dropped " << stats.dropped
                               << ", errors " << stats.errors << ", speed " << std::fixed
                               << std::setprecision(2) << stats.avgRtf << " x real time";
                processingMessage(moonProcessing.str().c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndDisabled();
            if (!dect2::moonshineAvailable() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Moonshine support is currently available on Linux desktop only.");
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void modelPopups() {
    if (U.askModel) { ImGui::OpenPopup("##txdlask"); }
    if (ImGui::BeginPopupModal("##txdlask", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (U.askModel) {
            ImGui::PushTextWrapPos(480 * gUi);
            ImGui::Text(tr("Download the speech model %s (%d MB)?"), U.askModel->id, U.askModel->mib);
            ImGui::TextDisabled("%s", dect2::speechModelUrl(*U.askModel).c_str());
            ImGui::TextUnformatted(tr("OnAir asks the model repository (huggingface.co) for this one file and sends nothing else. The file is checked against a fixed SHA-256 checksum and thrown away if it does not match. The model is MIT licensed."));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button(tr("Download"), fitButton(tr("Download"), 140 * gUi))) {
                gDl.start(*U.askModel, modelsDir());
                U.askModel = nullptr;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
        }
        if (ImGui::Button(tr("Cancel##txask"), fitButton(tr("Cancel##txask"), 100 * gUi)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) { U.askModel = nullptr; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if (U.askDelete) { ImGui::OpenPopup("##txdel"); U.askDelete = false; }
    if (ImGui::BeginPopupModal("##txdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("Delete all saved transcripts? This cannot be undone."));
        if (ImGui::Button(tr("Delete all"), fitButton(tr("Delete all"), 140 * gUi))) { gStore.clear(); U.ver = ~0ull; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##txdelc"), fitButton(tr("Cancel##txdelc"), 100 * gUi)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

}  // namespace

void windowPanel(App& a) {
    load();
    if (!gWindowOpen) { U.wasOpen = false; return; }
    if (!U.splitLoaded) {
        U.splitLoaded = true;
        U.splitRatio = std::clamp((float)plat::basePrefs().getI("transcriptsSplitPermille", 500) / 1000.f, 0.15f, 0.85f);
    }
    if (!U.wasOpen) {
        U.wasOpen = true;
        U.showNotice = !plat::basePrefs().getB("transcriptsPrivacyAccepted", false);
        if (dect2::moonshineAvailable()) {
            scanMoonLibraries(modelsDir());
            if (!gMoonLibraries.selectedMoonshine.empty() && !gMoonLibraries.selectedOnnx.empty())
                dect2::initializeMoonshineEngineInfo(modelsDir());
        }
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1100 * gUi, vp->WorkSize.x * 0.95f), std::min(660 * gUi, vp->WorkSize.y * 0.92f)), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Transcripts"), &gWindowOpen)) { ImGui::End(); return; }
    if (ImGui::SmallButton(U.settings ? tr("Hide settings") : tr("Show settings"))) U.settings = !U.settings;
    std::string ind;
    indicator(ind);
    const char* status = ind.empty() ? tr("Transcription is off.") : ind.c_str();
    const float statusWidth = ImGui::CalcTextSize(status).x + (ind.empty() ? 0.f : 20 * gUi);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - statusWidth));
    if (!ind.empty()) {
        ImGui::TextColored(pal::badRed(), "%s", "\xE2\x97\x8F");
        ImGui::SameLine();
        ImGui::TextColored(pal::warnAmber(), "%s", ind.c_str());
    } else ImGui::TextDisabled("%s", status);
    ImGui::Separator();

    // Draggable split: both panes keep a usable minimum width where possible.
    // When settings are hidden, the table expands to the entire available width.
    const float available = ImGui::GetContentRegionAvail().x;
    if (U.settings) {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float handle = 8.f * gUi;
        const float panes = std::max(1.f, available - handle - 2.f * gap);
        const float minimum = std::min(190.f * gUi, panes * 0.35f);
        const float left = std::clamp(panes * U.splitRatio, minimum, panes - minimum);
        ImGui::BeginChild("##txset", ImVec2(left, 0), true);
        settingsPanel(a);
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        const ImVec2 grip = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##txsplitter", ImVec2(handle, ImGui::GetContentRegionAvail().y));
        const bool dragging = ImGui::IsItemActive();
        if (ImGui::IsItemHovered() || dragging) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        const ImU32 lineColor = ImGui::GetColorU32(dragging ? ImGuiCol_SliderGrabActive : ImGuiCol_SeparatorHovered);
        const float middle = grip.x + handle * 0.5f;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(middle, grip.y + 8 * gUi),
                                            ImVec2(middle, grip.y + std::max(8.f * gUi, ImGui::GetItemRectSize().y - 8 * gUi)),
                                            lineColor, 2.f * gUi);
        if (dragging && ImGui::GetIO().MouseDelta.x != 0.f) {
            U.splitRatio = std::clamp((left + ImGui::GetIO().MouseDelta.x) / panes,
                                      minimum / panes, 1.f - minimum / panes);
        }
        if (ImGui::IsItemDeactivated()) {
            plat::basePrefs().setI("transcriptsSplitPermille", (long)std::lround(U.splitRatio * 1000.f));
            plat::basePrefs().flush();
        }
        ImGui::SameLine(0, gap);
    }
    ImGui::BeginChild("##txresults", ImVec2(0, 0), false);
    refreshRows();
    ImGui::SetNextItemWidth(std::max(90.f * gUi, ImGui::GetContentRegionAvail().x * 0.37f));
    ImGui::InputTextWithHint("##txq", tr("Search the text ..."), U.query, sizeof U.query);
    ImGui::SameLine();
    const char* scopes[] = {tr("All"), tr("Channel"), tr("Text"), tr("Aircraft")};
    ImGui::SetNextItemWidth(std::max(75.f * gUi, ImGui::GetContentRegionAvail().x * 0.20f));
    if (ImGui::BeginCombo("##txscope", scopes[U.searchIn])) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(scopes[i], U.searchIn == i)) U.searchIn = i;
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    {
        std::string cur = U.freqSel > 0 && (size_t)U.freqSel <= U.freqs.size() ? std::to_string(U.freqs[(size_t)U.freqSel - 1] / 1e6).substr(0, 7) + " MHz" : std::string(tr("All frequencies"));
        ImGui::SetNextItemWidth(std::max(90.f * gUi, ImGui::GetContentRegionAvail().x));
        if (ImGui::BeginCombo("##txf", cur.c_str())) {
            if (ImGui::Selectable(tr("All frequencies"), U.freqSel == 0)) U.freqSel = 0;
            for (size_t i = 0; i < U.freqs.size(); i++) {
                char b[32]; snprintf(b, sizeof b, "%.3f MHz", U.freqs[i] / 1e6);
                if (ImGui::Selectable(b, U.freqSel == (int)i + 1)) U.freqSel = (int)i + 1;
            }
            ImGui::EndCombo();
        }
    }
    ImGui::TextDisabled(tr("%zu of %zu"), U.rows.size(), U.total);
    if (ImGui::SmallButton(tr("Copy all shown"))) ImGui::SetClipboardText(dect2::transcriptsTxt(U.rows).c_str());
    for (const char* ext : {"txt", "srt", "json", "csv"}) {
        ImGui::SameLine();
        const std::string label = std::string(tr("Export")) + " " + ext + "##txe";
        if (ImGui::SmallButton(label.c_str())) exportRows(ext);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Delete all ..."))) U.askDelete = true;
    if (!U.note.empty()) { ImGui::TextDisabled("%s", U.note.c_str()); }

    const int count = [&] { int n = 0; for (int i = 0; i < 7; ++i) n += !!(C.columns & (1 << i)); return n; }();
    if (count && ImGui::BeginTable("##txlist", count, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        const char* names[] = {tr("Time (UTC)"), tr("Time (local)"), tr("Channel"), tr("Frequency"), tr("Text"), tr("Aircraft"), tr("Confidence")};
        for (int i = 0; i < 7; ++i) if (C.columns & (1 << i))
            ImGui::TableSetupColumn(names[i], i == 4 ? ImGuiTableColumnFlags_WidthStretch : ImGuiTableColumnFlags_WidthFixed, (i <= 1 ? 120.f : i == 2 ? 150.f : i == 3 ? 95.f : i == 5 ? 90.f : 44.f) * gUi);
        ImGui::TableHeadersRow();
        const size_t shown = std::min<size_t>(U.rows.size(), 600);
        for (size_t j = 0; j < shown; ++j) {
            const auto& e = U.rows[j];
            ImGui::PushID((int)e.id);
            ImGui::TableNextRow();
            for (int i = 0; i < 7; ++i) {
                if (!(C.columns & (1 << i))) continue;
                ImGui::TableNextColumn();
                if (i <= 1) {
                    std::tm tm{};
                    const std::time_t t = (std::time_t)e.wall;
#if defined(_WIN32)
                    if (i == 0) gmtime_s(&tm, &t); else localtime_s(&tm, &t);
#else
                    if (i == 0) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
#endif
                    char d[32]; std::strftime(d, sizeof d, "%m-%d %H:%M:%S", &tm);
                    ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(d); ImGui::PopFont();
                } else if (i == 2) {
                    ImGui::TextUnformatted(e.channel.c_str());
                    if (!e.speaker.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tr("Speaker (a guess from the wording)"), e.speaker.c_str());
                } else if (i == 3) {
                    if (e.freqHz > 0) ImGui::Text("%.3f MHz", e.freqHz / 1e6); else ImGui::TextDisabled("-");
                } else if (i == 4) {
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextUnformatted(e.text.c_str());
                    if (!e.translation.empty()) {
                        ImGui::TextDisabled("%s > EN", e.lang.empty() ? "?" : e.lang.c_str());
                        ImGui::SameLine(); ImGui::TextUnformatted(e.translation.c_str());
                    }
                    ImGui::PopTextWrapPos();
                    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(entryLine(e).c_str());
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Click to copy this line"));
                } else if (i == 5) {
                    const std::string ac = !e.callsign.empty() ? e.callsign : e.reg;
                    if (!ac.empty()) ImGui::TextUnformatted(ac.c_str()); else ImGui::TextDisabled("-");
                    if (!e.aircraft.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tr("Aircraft on the screen"), e.aircraft.c_str());
                } else if (!e.confidenceAvailable) ImGui::TextDisabled("N/A");
                else ImGui::TextDisabled("%.0f%%", e.confidence * 100.0);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
    modelPopups();
    if (U.showNotice) ImGui::OpenPopup("##transcriptsPrivacy");
    ImGui::SetNextWindowSize(ImVec2(500 * gUi, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##transcriptsPrivacy", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushTextWrapPos(470 * gUi);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Before using transcripts"));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Privacy: what is said on public aviation radio is public, but the speech engine runs here on your computer and the text is kept in your data folder. Do not use it on private conversations. It is on for what you are listening to (FM, AM, marine, amateur, CB, digital voice ...) once a speech model is installed; untick the boxes in Transcription settings to stop it. A visible line shows what it is doing."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(tr("I understand and accept"), ImVec2(-1, 0))) {
            plat::basePrefs().setB("transcriptsPrivacyAccepted", true);
            plat::basePrefs().flush();
            U.showNotice = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}


// ---------------------------------------------------------------- auto subtitles

namespace {

bool isTvStandard(int s) { return s == 0 || s == 1 || s == 3 || s == 4 || s == 6 || s == 7 || s == 8 || s == 40 || s == 47; }

dect2::CaptionTrack* trackOf(int rx) {   // gMu held by the caller
    const auto it = gTracks.find(rx);
    return it == gTracks.end() ? nullptr : it->second.get();
}

std::string capLine(const dect2::CaptionCue& c) {
    if (C.capShow == 1 && !c.translation.empty()) return c.translation;
    if (C.capShow == 2 && !c.translation.empty()) return c.text + "\n" + c.translation;
    return c.text;
}

bool writeText(const std::string& path, const std::string& body) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dect2::u8file(path)).parent_path(), ec);
    std::ofstream f(dect2::u8file(path), std::ios::binary);
    if (!f) { DIAG_DEBUG(933095, "The text file %s could not be opened for writing, so it was not saved.", path.c_str()); return false; }
    f << body;
    return (bool)f;
}

// cue lines wrapped to a width, drawn in a box on the draw list; bottom (or top) of the area
void drawLines(ImDrawList* dl, ImVec2 a0, ImVec2 asz, const std::vector<std::string>& lines, bool forceTop) {
    load();
    dect2::CaptionStyle st = dect2::CaptionStyle{C.capSize, C.capBg, C.capTop, C.capLines}.clamped();
    if (forceTop) st.top = true;   // next to the station's own subtitles, which sit at the bottom
    const float fs = ImGui::GetFontSize() * (float)st.sizePct / 100.f * std::max(1.f, asz.y / (260.f * gUi) * 0.6f);
    const float maxW = asz.x * 0.9f;
    ImFont* font = ImGui::GetFont();
    std::vector<ImVec2> sz;
    float h = 0, w = 0;
    for (const auto& l : lines) {
        const ImVec2 t = font->CalcTextSizeA(fs, FLT_MAX, maxW, l.c_str());
        sz.push_back(t); h += t.y + 4 * gUi; w = std::max(w, t.x);
    }
    if (lines.empty()) return;
    const float pad = 8 * gUi;
    float y = st.top ? a0.y + asz.y * 0.05f : a0.y + asz.y * 0.95f - h - pad;
    const float x0 = a0.x + (asz.x - w) * 0.5f;
    dl->AddRectFilled(ImVec2(x0 - pad, y - pad * 0.5f), ImVec2(x0 + w + pad, y + h + pad * 0.5f), IM_COL32(0, 0, 0, (int)(255 * st.bgAlpha / 100.f)), 6 * gUi);
    for (size_t i = 0; i < lines.size(); i++) {
        const float x = a0.x + (asz.x - sz[i].x) * 0.5f;
        dl->AddText(font, fs, ImVec2(x + 1, y + 1), IM_COL32(0, 0, 0, 200), lines[i].c_str(), nullptr, maxW);
        dl->AddText(font, fs, ImVec2(x, y), IM_COL32(255, 255, 255, 255), lines[i].c_str(), nullptr, maxW);
        y += sz[i].y + 4 * gUi;
    }
}

void drawCues(ImDrawList* dl, ImVec2 a0, ImVec2 asz, const std::vector<dect2::CaptionCue>& cues, bool forceTop = false) {
    std::vector<std::string> lines;
    for (const auto& c : cues) lines.push_back(capLine(c));
    drawLines(dl, a0, asz, lines, forceTop);
}

std::vector<dect2::CaptionCue> visibleFor(int rx) {
    std::lock_guard<std::mutex> lk(gMu);
    dect2::CaptionTrack* t = trackOf(rx);
    return t ? t->visible((size_t)std::max(1, std::min(4, C.capLines))) : std::vector<dect2::CaptionCue>();
}

std::string exportNow(App& a, int rx, bool vtt) {
    dect2::CaptionTrack* t;
    std::vector<dect2::CaptionCue> none;
    std::string body;
    {
        std::lock_guard<std::mutex> lk(gMu);
        t = trackOf(rx);
        if (!t || t->size() == 0) return "";
        body = vtt ? t->vtt(0, 0, C.capShow) : t->srt(0, 0, C.capShow);
    }
    char nm[128];
    const time_t tt = time(nullptr);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &tt);
#else
    localtime_r(&tt, &tmv);
#endif
    char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tmv);
    snprintf(nm, sizeof nm, "/captions/onair_captions_%.3fMHz_%s.%s", a.freqMhz, ts, vtt ? "vtt" : "srt");
    const std::string path = plat::dataDir() + nm;
    return writeText(path, body) ? path : "";
}

}  // namespace

bool captionsActive() { load(); return C.cap && backendReady(); }
bool captionsWanted(int standard) { load(); return C.cap && C.capModes.count(standard) && dect2::captionStandardEligible(standard); }
bool captionsIsTv(int standard) { return isTvStandard(standard); }
bool captionsShowBroadcast() { load(); return dect2::showBroadcastSubs((dect2::CaptionSource)C.capSource); }

void captionText(ImDrawList* dl, ImVec2 p0, ImVec2 size, const std::string& text) {
    std::vector<std::string> lines;
    for (size_t a = 0; a <= text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        if (b > a) lines.push_back(text.substr(a, b - a));
        a = b + 1;
    }
    drawLines(dl, p0, size, lines, false);
}

void captionsOverlay(App& a, ImDrawList* dl, ImVec2 p0, ImVec2 size, bool broadcastPresent) {
    load();
    if (!captionsActive() || !captionsWanted(a.rx.standard)) return;
    if (!dect2::showGeneratedSubs((dect2::CaptionSource)C.capSource, broadcastPresent)) return;
    drawCues(dl, p0, size, visibleFor(a.rxId), C.capSource == 2 && broadcastPresent);
}

void captionsWindow(App& a) {
    (void)a;
    load();
    if (!captionsActive()) return;
    App* h = receiverHeard();
    if (!h || !h->engine.running()) return;
    const int std_ = h->rx.standard;
    if (!captionsWanted(std_) || isTvStandard(std_)) return;
    const dect2::CaptionStyle st = dect2::CaptionStyle{C.capSize, C.capBg, C.capTop, C.capLines}.clamped();
    bool open = true;
    char id[64];
    snprintf(id, sizeof id, "%s###caption%d", tr("Captions"), h->rxId);
    firstWindowSize(560 * gUi, 110 * gUi);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetMainViewport()->Pos.x + 80 * gUi, ImGui::GetMainViewport()->Pos.y + ImGui::GetMainViewport()->Size.y - 220 * gUi), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha((float)st.bgAlpha / 100.f);
    if (ImGui::Begin(id, &open, ImGuiWindowFlags_NoCollapse)) {
        const auto cues = visibleFor(h->rxId);
        const dect2::TranscriberStats ts = gCap.stats();
        ImGui::SetWindowFontScale((float)st.sizePct / 100.f);
        ImGui::PushTextWrapPos(0);
        if (cues.empty()) { int lv = 0; const std::string sl = statusLine(&lv); ImGui::TextColored(lv == 2 ? pal::warnAmber() : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s", sl.c_str()); }
        (void)ts;
        for (const auto& c : cues) {
            ImGui::TextUnformatted(capLine(c).c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::SetWindowFontScale(1.f);
        ImGui::Separator();
        if (ImGui::SmallButton(tr("Save SRT"))) { const std::string p = exportNow(*h, h->rxId, false); h->engine.log(p.empty() ? "captions: nothing to save yet" : "captions saved to " + p); }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Save VTT"))) { const std::string p = exportNow(*h, h->rxId, true); h->engine.log(p.empty() ? "captions: nothing to save yet" : "captions saved to " + p); }
        ImGui::SameLine();
        ImGui::TextDisabled(ts.avgRtf > 1.0 ? tr("behind real time (%.1f x): the oldest speech is skipped") : tr("speed %.2f x real time"), ts.avgRtf);
        if (ts.musicSkipped) { ImGui::SameLine(); ImGui::TextDisabled(tr("music skipped %llu"), (unsigned long long)ts.musicSkipped); }
    }
    ImGui::End();
    if (!open) { C.capModes.erase(std_); save(); }
}

void captionsControls(App& a) {
    load();
    const int std_ = a.rx.standard;
    if (!dect2::captionStandardEligible(std_)) return;
    flowNext();
    bool on = C.cap && C.capModes.count(std_);
    if (ImGui::Checkbox(tr("auto captions"), &on)) {
        if (on) { C.capModes.insert(std_); C.cap = true; } else C.capModes.erase(std_);
        save(); applyConfig();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Captions made on your computer from the speech you hear, with the offline speech model (View > Transcripts > Settings). Nothing is sent anywhere."));
    if (isTvStandard(std_) && on) {
        flowNext();
        ImGui::SetNextItemWidth(230 * gUi);
        const char* names[] = {tr("Station's own subtitles"), tr("Generated from speech"), tr("Both"), tr("Generated where the station has none")};
        if (ImGui::BeginCombo("##capsrc", names[C.capSource])) {
            for (int i = 0; i < 4; i++) if (ImGui::Selectable(names[i], i == C.capSource)) { C.capSource = i; save(); }
            ImGui::EndCombo();
        }
    }
}

void captionsSettings(App& a) {
    (void)a;
    load();
    bool ch = false;

    settingsSection(tr("Caption generation"));
    ch |= ImGui::Checkbox(tr("Make captions"), &C.cap);
    ImGui::BeginDisabled(!C.cap);
    ch |= ImGui::Checkbox(tr("Save .srt and .vtt next to audio recordings"), &C.capSave);
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("Captions are generated locally with the offline speech model and shown on the receiver. They are kept in memory unless you save them. Music is skipped."));
    ImGui::PopTextWrapPos();

    settingsSection(tr("Sources"));
    int col = 0;
    for (int standard : dect2::captionStandards()) {
        bool on = C.capModes.count(standard) != 0;
        if (ImGui::Checkbox((std::string(dect2::captionStandardName(standard)) + "##capm" + std::to_string(standard)).c_str(), &on)) {
            if (on) C.capModes.insert(standard); else C.capModes.erase(standard);
            ch = true;
        }
        if (++col % 2) ImGui::SameLine();
    }
    if (col % 2) ImGui::NewLine();

    settingsSection(tr("TV subtitles"));
    const char* srcs[] = {tr("Station's own subtitles"), tr("Generated from speech"), tr("Both"), tr("Generated where the station has none")};
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##captvsource", srcs[C.capSource])) {
        for (int i = 0; i < 4; ++i) if (ImGui::Selectable(srcs[i], i == C.capSource)) { C.capSource = i; ch = true; }
        ImGui::EndCombo();
    }

    settingsSection(tr("Caption appearance"));
    const char* shows[] = {tr("As spoken"), tr("English translation"), tr("Both")};
    ImGui::TextUnformatted(tr("Caption text"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##captext", shows[C.capShow])) {
        for (int i = 0; i < 3; ++i) if (ImGui::Selectable(shows[i], i == C.capShow)) { C.capShow = i; ch = true; }
        ImGui::EndCombo();
    }
    if (C.capShow && !(C.translate && translationSupported())) {
        const ImVec4 warning = pal::warnAmber();
        processingMessage(tr("Enable Translate to English under Transcription and select a multilingual model to get translated captions."), &warning);
    }
    ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::SliderInt(tr("Caption size, percent"), &C.capSize, 60, 250);
    ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::SliderInt(tr("Background, percent"), &C.capBg, 0, 100);
    ImGui::SetNextItemWidth(160 * gUi);
    ch |= ImGui::SliderInt(tr("Lines"), &C.capLines, 1, 4);
    ch |= ImGui::Checkbox(tr("At the top of the picture"), &C.capTop);
    ImGui::EndDisabled();
    if (ch) { C.capSize = dect2::CaptionStyle{C.capSize, C.capBg, C.capTop, C.capLines}.clamped().sizePct; save(); applyConfig(); }

    settingsSection(tr("Processing"));
    const dect2::TranscriberStats ts = gCap.stats();
    std::ostringstream processing;
    processing << tr("Captions:") << ' ' << ts.segments << ' ' << tr("pieces")
               << ", " << ts.musicSkipped << ' ' << tr("music skipped")
               << ", " << ts.dropped << ' ' << tr("skipped (too slow)")
               << ", " << tr("speed") << ' ' << std::fixed << std::setprecision(2)
               << ts.avgRtf << ' ' << tr("x real time");
    const std::string processingText = processing.str();
    processingMessage(processingText.c_str());
    if (ts.avgRtf > 1.0) {
        const ImVec4 warning = pal::warnAmber();
        processingMessage(tr("The speech engine is slower than the audio: use a smaller model or fewer CPU threads. The oldest speech is skipped, never the newest."), &warning);
    }
}

void captionsRecordStart() {
    std::lock_guard<std::mutex> lk(gMu);
    if (!gCapCtx.valid) return;
    if (dect2::CaptionTrack* t = trackOf(gCapCtx.rx)) t->markOrigin();
}

void captionsRecordStop(const std::string& recordingPath) {
    load();
    if (!C.cap || !C.capSave || recordingPath.empty()) return;
    std::string srt, vtt;
    {
        std::lock_guard<std::mutex> lk(gMu);
        dect2::CaptionTrack* t = nullptr;
        if (gCapCtx.valid) t = trackOf(gCapCtx.rx);
        if (!t || t->size() == 0) return;
        const double o = t->origin();
        srt = t->srt(o, 0, C.capShow);
        vtt = t->vtt(o, 0, C.capShow);
    }
    if (srt.empty()) return;
    std::string base = recordingPath;
    const size_t dot = base.find_last_of('.'), sl = base.find_last_of("/\\");
    if (dot != std::string::npos && (sl == std::string::npos || dot > sl)) base.resize(dot);
    writeText(base + ".srt", srt);
    writeText(base + ".vtt", vtt);
}

}  // namespace tx

namespace tx {
std::string latestLanguage(double freqHz, float* probability) {
    const int64_t now = (int64_t)std::time(nullptr);
    dect2::TranscriptFilter f; f.freqHz = freqHz; f.from = now - 120;
    for (const dect2::TranscriptStore* st : {&gStore, &gCapStore}) {
        const auto v = st->find(f, 1);
        if (!v.empty() && !v[0].lang.empty()) { if (probability) *probability = v[0].confidenceAvailable ? v[0].confidence : 0.f; return v[0].lang; }
    }
    if (probability) *probability = 0;
    return "";
}
}

namespace tx {
std::vector<AirbandLine> airbandSince(int64_t since) {
    std::vector<AirbandLine> out;
    dect2::TranscriptFilter f; f.mode = "airband"; f.from = since;
    const auto v = gStore.find(f, 200);   // newest first
    for (auto it = v.rbegin(); it != v.rend(); ++it) out.push_back({it->wall, it->freqHz, it->channel, it->text});
    return out;
}
}
