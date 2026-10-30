// The actions of the Automations inside OnAir: notification, sound (respects Mute), recording, tuning, memories, screenshot, webhook, run.
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "../memory/memory_options.h"
#include "automation_internal.h"
#include "dect2/audio/audio_rec.h"
#include "dect2/audio/audioout.h"
#include "dect2/session/record_rules.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>

std::string gAutomationShotPath;

namespace automationapp {
namespace {
using dect2::automation::ActionSink;
namespace fs = std::filesystem;

struct Pending {
    std::string kind;
    std::map<std::string, std::string> p;
    std::shared_ptr<std::promise<std::pair<bool, std::string>>> done;
};

struct Banner { std::string title, body; double until = 0; };

std::mutex gMu;
std::deque<Pending> gQ;
Banner gBanner;
double gIqStop = 0, gAudioStop = 0;
std::string gHookStatus;
dect2::AudioOut* gBeep = nullptr;

// webhooks: one worker, one request at a time
std::mutex gHookMu;
std::condition_variable gHookCv;
std::deque<std::pair<std::string, std::string>> gHookQ;
std::thread gHookTh;

double nowS() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void hookWorker() {
    for (;;) {
        std::pair<std::string, std::string> job;
        {
            std::unique_lock<std::mutex> lk(gHookMu);
            gHookCv.wait(lk, [] { return !gHookQ.empty(); });
            job = gHookQ.front();
            gHookQ.pop_front();
        }
        const bool ok = plat::postJson(job.first, job.second, std::string("OnAir/") + ONAIR_VERSION + " (automation)");
        char b[240];
        std::string host = job.first.substr(0, 60);
        snprintf(b, sizeof b, "%s %s", ok ? "webhook sent:" : "webhook failed:", host.c_str());
        std::lock_guard<std::mutex> lk(gMu);
        gHookStatus = b;
    }
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> o;
    size_t i = 0;
    while (!s.empty() && i <= s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        o.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    return o;
}

// A WAV file (PCM 16 bit or float, mono or stereo) as stereo float at 48 kHz, 2 seconds at most
bool readWav(const std::string& path, std::vector<float>& out) {
    std::ifstream f(fs::path(reinterpret_cast<const char8_t*>(path.c_str())), std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 44 || d.size() > (32u << 20) || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
    int fmt = 0, ch = 0, rate = 0, bits = 0;
    size_t dataAt = 0, dataLen = 0;
    for (size_t i = 12; i + 8 <= d.size();) {
        const uint32_t n = d[i + 4] | d[i + 5] << 8 | d[i + 6] << 16 | (uint32_t)d[i + 7] << 24;
        if (!memcmp(&d[i], "fmt ", 4) && i + 8 + 16 <= d.size()) { fmt = d[i + 8] | d[i + 9] << 8; ch = d[i + 10] | d[i + 11] << 8; rate = d[i + 12] | d[i + 13] << 8 | d[i + 14] << 16; bits = d[i + 22] | d[i + 23] << 8; }
        if (!memcmp(&d[i], "data", 4)) { dataAt = i + 8; dataLen = std::min<size_t>(n, d.size() - dataAt); break; }
        i += 8 + n + (n & 1);
    }
    if (!dataAt || (ch != 1 && ch != 2) || rate < 8000 || rate > 192000 || !((fmt == 1 && bits == 16) || (fmt == 3 && bits == 32))) return false;
    const size_t bytes = (size_t)(bits / 8) * ch;
    const size_t frames = std::min<size_t>(dataLen / bytes, (size_t)rate * 2);
    std::vector<float> src(frames * 2);
    for (size_t k = 0; k < frames; k++)
        for (int c = 0; c < 2; c++) {
            const uint8_t* p = &d[dataAt + k * bytes + (size_t)(ch == 2 ? c : 0) * (bits / 8)];
            src[k * 2 + (size_t)c] = bits == 16 ? (int16_t)(p[0] | p[1] << 8) / 32768.f : [&] { float v; memcpy(&v, p, 4); return v; }();
        }
    out.clear();
    const size_t n48 = (size_t)((double)frames * 48000 / rate);
    for (size_t k = 0; k < n48; k++) {   // nearest sample: good enough for an alert
        const size_t s = std::min<size_t>(frames - 1, (size_t)((double)k * rate / 48000));
        out.push_back(src[s * 2]); out.push_back(src[s * 2 + 1]);
    }
    return !out.empty();
}

void playSound(App& a, const std::string& file, std::string& note) {
    if (a.muted || a.volume <= 0.001f) { note = "muted: no sound"; return; }
    std::vector<float> pcm;
    if (!file.empty()) {
        if (!readWav(file, pcm)) { note = "cannot read " + file + " (a 16 bit or float WAV file is needed)"; return; }
    } else {   // three short beeps
        const int rate = 48000;
        for (int b = 0; b < 3; b++) {
            for (int i = 0; i < rate * 12 / 100; i++) {
                const float env = std::min(1.f, std::min(i, rate * 12 / 100 - i) / 240.f);
                const float v = 0.5f * env * std::sin(2.f * 3.14159265f * 880.f * i / rate);
                pcm.push_back(v); pcm.push_back(v);
            }
            pcm.insert(pcm.end(), (size_t)rate * 8 / 100 * 2, 0.f);
        }
    }
    if (!gBeep) gBeep = new dect2::AudioOut();   // kept for the life of the program
    if (!gBeep->running() && !gBeep->start(48000)) { note = "no sound output"; return; }
    gBeep->setVolume(std::min(1.f, a.volume));
    gBeep->setMuted(false);
    gBeep->write(pcm.data(), (int)(pcm.size() / 2));
}

std::pair<bool, std::string> doOnUi(App& a, const Pending& p) {
    auto get = [&](const char* k) { auto it = p.p.find(k); return it == p.p.end() ? std::string() : it->second; };
    const double now = nowS();
    if (p.kind == "notify") {
        const std::string title = get("title").empty() ? "OnAir" : get("title"), body = get("body");
        {
            std::lock_guard<std::mutex> lk(gMu);
            gBanner = {title, body, now + 10};
        }
        const bool ok = plat::notify(title, body);
        return {true, ok ? "shown" : "shown in OnAir (the system could not show it)"};
    }
    if (p.kind == "sound") {
        std::string note;
        playSound(a, get("file"), note);
        return {note.empty(), note.empty() ? "played" : note};
    }
    if (p.kind == "record_iq") {
        int sec = std::max(1, std::min(3600, atoi(get("seconds").c_str())));
        if (!a.engine.running()) return {false, "the receiver is not running"};
        if (a.engine.recordingStats().active) { gIqStop = std::max(gIqStop, now + sec); return {true, "already recording: extended"}; }
        const dect2::FileFormat f = a.recFormat >= 1 ? dect2::FileFormat::CF32 : dect2::FileFormat::CS8;
        const std::string path = plat::dataDir() + "/recordings/" + (a.recFormat == 2 ? dect2::blueRecordingName(a.engine.recordingName(f)) : a.engine.recordingName(f));
        std::string err;
        if (!a.engine.startRecording(path, f, err)) { a.engine.log("automation: recording failed: " + err); return {false, err}; }
        a.recDonePath.clear();
        gIqStop = now + sec;
        return {true, "recording for " + std::to_string(sec) + " s"};
    }
    if (p.kind == "save_rewind") {
        std::string note;
        const bool ok = timeMachineSaveNow(a, std::max(0, std::min(600, atoi(get("seconds").c_str()))), note);
        return {ok, note};
    }
    if (p.kind == "record_audio") {
        int sec = std::max(1, std::min(3600, atoi(get("seconds").c_str())));
        auto& rec = dect2::AudioRecorder::instance();
        if (rec.active()) { gAudioStop = std::max(gAudioStop, now + sec); return {true, "already recording: extended"}; }
        const std::string path = plat::dataDir() + "/recordings/" + dect2::AudioRecorder::fileName("automation", a.freqMhz * 1e6, dect2::AudioRecFormat::Wav);
        std::string err;
        dect2::AudioRecOptions o;
        if (!rec.start(path, o, err)) return {false, err};
        gAudioStop = now + sec;
        return {true, "recording audio for " + std::to_string(sec) + " s"};
    }
    if (p.kind == "tune") {
        const double mhz = atof(get("freq_mhz").c_str());
        if (!(mhz > 0.01 && mhz < 60000)) return {false, "bad frequency"};
        const std::string mode = get("mode");
        if (!mode.empty()) {
            dect2::Memory m;
            m.name = "automation";
            m.freqHz = (uint64_t)std::llround(mhz * 1e6);
            m.mode = mode;
            std::string note;
            if (!memoryRecallApp(a, m, true, note)) return {false, note.empty() ? "cannot switch to mode '" + mode + "'" : note};
            return {true, "tuned"};
        }
        a.freqMhz = mhz;
        if (a.engine.running()) { a.tune.centerHz = mhz * 1e6; a.engine.retune(a.tune); }
        return {true, "tuned"};
    }
    if (p.kind == "memory") {
        const std::string want = get("name");
        auto low = [](std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; };
        for (const dect2::Memory& m : memoryBank().all()) {
            if (low(m.name) != low(want)) continue;
            std::string note;
            if (memoryRecallApp(a, m, true, note)) return {true, "recalled"};
            return {false, note};
        }
        return {false, "no memory called '" + want + "'"};
    }
    if (p.kind == "screenshot") {
        std::error_code ec;
        fs::create_directories(fs::path(reinterpret_cast<const char8_t*>((plat::dataDir() + "/screenshots").c_str())), ec);
        char b[64];
        const time_t t = time(nullptr);
        strftime(b, sizeof b, "onair_%Y%m%d-%H%M%S.png", localtime(&t));
        gAutomationShotPath = plat::dataDir() + "/screenshots/" + b;
        return {true, "saved as " + gAutomationShotPath};
    }
    return {false, "unknown action " + p.kind};
}

class AppSink : public ActionSink {
public:
    bool perform(const std::string& kind, const std::map<std::string, std::string>& p, std::string& detail) override {
        if (kind == "webhook") {
            auto u = p.find("url"), b = p.find("body");
            if (u == p.end() || b == p.end()) { detail = "no address"; return false; }
            {
                std::lock_guard<std::mutex> lk(gHookMu);
                if (!gHookTh.joinable()) gHookTh = dect2::namedThread("onair-hook", hookWorker);
                if (gHookQ.size() >= 50) { detail = "too many webhooks waiting"; return false; }
                gHookQ.push_back({u->second, b->second});
            }
            gHookCv.notify_one();
            detail = "sent in the background";
            return true;
        }
        if (kind == "run") {
            auto prog = p.find("program"), args = p.find("args");
            std::string err;
            const bool ok = plat::spawnProgram(prog == p.end() ? "" : prog->second, lines(args == p.end() ? "" : args->second), err);
            detail = ok ? "started" : err;
            return ok;
        }
        Pending q{kind, p, std::make_shared<std::promise<std::pair<bool, std::string>>>()};
        auto fut = q.done->get_future();
        {
            std::lock_guard<std::mutex> lk(gMu);
            if (gQ.size() >= 100) { detail = "too many actions waiting"; return false; }
            gQ.push_back(std::move(q));
        }
        if (fut.wait_for(std::chrono::seconds(3)) != std::future_status::ready) { detail = "queued (the window is not drawing)"; return true; }
        const auto r = fut.get();
        detail = r.second;
        return r.first;
    }
};
}  // namespace

ActionSink& sink() {
    static AppSink s;
    return s;
}

// ---- auto-record (core/include/dect2/session/record_rules.h): the rules' IQ clips, and the audio of modes that have no voice monitor of their own.
// The store's writer thread only queues; the clip is started here, on the interface thread, with the same actions the automations use.
namespace {
struct ArReq { bool iq; int sec; };
std::mutex gArMu;
std::deque<ArReq> gArQ;
bool voiceMonitorMode(uint16_t m) { return m == 9 || m == 38 || m == 50 || m == 45 || m == 33 || m == 128 || m == 46 || m == 31; }
}  // namespace

void autoRecordInstall() {
    dect2::sess::rules().setHandlers(
        [](const dect2::sess::Session& s, const dect2::sess::RecordDecision& d) {
            std::lock_guard<std::mutex> lk(gArMu);
            if (gArQ.size() > 32) return;
            if (d.iq) gArQ.push_back({true, (int)std::max<uint32_t>(1, d.iqSeconds)});
            if (d.audio && !voiceMonitorMode(s.mode)) gArQ.push_back({false, (int)(d.maxSeconds ? d.maxSeconds : 3600)});
        },
        [](uint64_t) {});
}

void uiTick(App& a) {
    for (;;) {
        ArReq r;
        {
            std::lock_guard<std::mutex> lk(gArMu);
            if (gArQ.empty()) break;
            r = gArQ.front();
            gArQ.pop_front();
        }
        Pending p;
        p.kind = r.iq ? "record_iq" : "record_audio";
        p.p["seconds"] = std::to_string(r.sec);
        const auto res = doOnUi(a, p);
        a.engine.log(std::string("auto-record ") + p.kind + ": " + res.second);
    }
    for (;;) {
        Pending p;
        {
            std::lock_guard<std::mutex> lk(gMu);
            if (gQ.empty()) break;
            p = std::move(gQ.front());
            gQ.pop_front();
        }
        p.done->set_value(doOnUi(a, p));
    }
    const double now = nowS();
    if (gIqStop > 0 && now >= gIqStop) { gIqStop = 0; if (a.engine.recordingStats().active) a.engine.stopRecording(); }
    if (gAudioStop > 0 && now >= gAudioStop) { gAudioStop = 0; dect2::AudioRecorder::instance().stop(); }
}

void drawBanner() {
    Banner b;
    {
        std::lock_guard<std::mutex> lk(gMu);
        b = gBanner;
    }
    if (nowS() > b.until) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const std::string text = b.body.empty() ? b.title : b.title + ": " + b.body;
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float wrap = std::min(disp.x - 40 * gUi, 520 * gUi);
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str(), nullptr, false, wrap);
    const float pad = 10 * gUi, x = (disp.x - ts.x) * 0.5f, y = 40 * gUi;
    dl->AddRectFilled(ImVec2(x - pad, y - pad * 0.6f), ImVec2(x + ts.x + pad, y + ts.y + pad * 0.6f), IM_COL32(20, 22, 24, 235), 6.f);
    dl->AddRect(ImVec2(x - pad, y - pad * 0.6f), ImVec2(x + ts.x + pad, y + ts.y + pad * 0.6f), ImGui::GetColorU32(pal::accent()), 6.f);
    dl->AddText(nullptr, 0.f, ImVec2(x, y), IM_COL32(235, 235, 235, 255), text.c_str(), nullptr, wrap);
}

std::string webhookStatus() {
    std::lock_guard<std::mutex> lk(gMu);
    return gHookStatus;
}

}  // namespace automationapp
