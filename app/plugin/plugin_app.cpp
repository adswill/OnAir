// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Plugins in the program: the plugin manager (View > Plugins, and the "Plugins" mode), the generic screen every plugin mode gets (status lamps, tables,
// plots, images, map points, log and the settings the plugin declared), and the glue to the host (core/src/plugin): discovery at start, a numbered row in
// the mode list for every plugin mode, the IQ from the engine to the running plugin, and the automation events. docs/plugins.md explains the model.
#include "app.h"
#include "dect2/audio/audioout.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/plugin/host.h"
#include "platform/platform.h"
#include "pstore/store_ui.h"
#include "ui/theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace dp = dect2::plugin;

namespace {

std::string exeDir() {
    std::string exe;
#if defined(_WIN32)
    char p[MAX_PATH * 2] = {0};
    if (GetModuleFileNameA(nullptr, p, (DWORD)sizeof p - 1)) exe = p;
#elif defined(__APPLE__)
    char p[4096]; uint32_t n = sizeof p;
    if (_NSGetExecutablePath(p, &n) == 0) exe = p;
#else
    char p[4096]; const ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n > 0) exe.assign(p, (size_t)n);
#endif
    const size_t cut = exe.find_last_of("/\\");
    return cut == std::string::npos ? std::string(".") : exe.substr(0, cut);
}

// The sound of the plugins (audio_out): one 48 kHz output per plugin on the default device. Declared before P so that it outlives the
// Manager (a plugin thread may still play while the Manager stops it at exit).
struct PluginSound {
    std::mutex mu;              // one per plugin: opening one plugin's device does not hold up the others
    std::unique_ptr<dect2::AudioOut> out;
    double rate = 0, pos = 0;   // pos: the next output frame, in input samples after `prev`
    float prev = 0;             // the last input sample of the previous block
    std::vector<float> st;
};
std::mutex gSoundMu;            // guards gSounds only, never held while a device is opened or closed
std::map<std::string, std::shared_ptr<PluginSound>> gSounds;
std::atomic<float> gSoundVolume{1.f};   // the app's volume and mute (pluginFrame), applied to every plugin output
std::atomic<bool> gSoundMuted{false};

// Mono PCM at any rate to the plugin's output: linear interpolation to 48 kHz stereo (as the voice modes do), across block boundaries.
int playPluginSound(const std::string& id, const float* pcm, size_t n, double rate) {
    if (!pcm || n == 0 || !(rate >= 1000 && rate <= 400000)) return -1;   // the limits of docs/plugins.md (also checked by the host)
    std::shared_ptr<PluginSound> sp;
    {
        std::lock_guard<std::mutex> lk(gSoundMu);
        auto& e = gSounds[id];
        if (!e) e = std::make_shared<PluginSound>();
        sp = e;
    }
    PluginSound& s = *sp;
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.out) {
        s.out = std::make_unique<dect2::AudioOut>();
        if (!s.out->start(48000)) { s.out.reset(); return -1; }
        s.out->setStartThreshold(48000 / 5);
    }
    s.out->setVolume(gSoundVolume.load()); s.out->setMuted(gSoundMuted.load());
    auto clean = [](float v) { return std::isfinite(v) ? v : 0.f; };
    if (s.rate != rate) { s.rate = rate; s.pos = 0; s.prev = clean(pcm[0]); }
    const double step = rate / 48000.0;   // input samples per output frame
    s.st.clear();
    // position t: -1 is `prev`, 0..n-1 the block
    for (; s.pos <= (double)(n - 1); s.pos += step) {
        const double t = s.pos;
        const long i = (long)std::floor(t);
        const float a = i < 0 ? s.prev : clean(pcm[i]);
        const float b = clean(pcm[std::min<long>(i + 1, (long)n - 1)]);
        const float x = a + (b - a) * (float)(t - (double)i);
        s.st.push_back(x); s.st.push_back(x);
    }
    s.pos -= (double)n;
    s.prev = clean(pcm[n - 1]);
    if (s.out->bufferedFrames() > 48000 * 3 / 2) s.out->flush();   // more than 1.5 s behind: start again
    s.out->write(s.st.data(), (int)(s.st.size() / 2));
    return 0;
}

// Closes the outputs of the plugins `keep` says no to. `keep` runs without gSoundMu (it may ask the Manager), and the devices are closed
// after gSoundMu is released.
template <class Keep> void dropPluginSounds(Keep keep) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(gSoundMu);
        for (const auto& kv : gSounds) ids.push_back(kv.first);
    }
    std::vector<std::shared_ptr<PluginSound>> gone;
    for (const auto& id : ids) {
        if (keep(id)) continue;
        std::lock_guard<std::mutex> lk(gSoundMu);
        auto it = gSounds.find(id);
        if (it == gSounds.end()) continue;
        gone.push_back(std::move(it->second));
        gSounds.erase(it);
    }
}

void stopPluginSound(const std::string& id) {
    dropPluginSounds([&](const std::string& k) { return k != id; });
}

struct PluginApp {
    std::unique_ptr<dp::Manager> mgr;
    bool inited = false;
    std::mutex sinkMu;
    dp::Runner* active = nullptr;                // the running plugin mode (guarded by sinkMu while it changes)
    std::string activeId, activeMode;
    std::atomic<double> centreHz{0};
    std::atomic<uint64_t> counter{0};
    std::string error;                           // why the selected plugin mode is not running
    double lastTick = 0, lastRate = 0, lastCentre = 0, askedHz = 0;
    std::string selId, installMsg;
    uint64_t imgSeq = ~0ull;
    std::map<std::string, gfx::Image*> images;
    int curFamily = 0;
    bool startFailed = false, hostAvail = false;
};
PluginApp P;

const char* permLabel(uint32_t bit) {
    switch (bit) {
    case dp::PermNetwork: return N_("Network: talk to other computers");
    case dp::PermFilesystem: return N_("Files: read and write its own data folder");
    case dp::PermAudioOut: return N_("Sound: play audio through the program");
    case dp::PermRecord: return N_("Recording: save audio or IQ");
    case dp::PermSpawn: return N_("Programs: start other programs");
    default: return "";
    }
}

void openPath(const std::string& p) {
    std::string err;
#if defined(_WIN32)
    plat::spawnProgram("explorer.exe", {p}, err);
#elif defined(__APPLE__)
    plat::spawnProgram("/usr/bin/open", {p}, err);
#else
    plat::spawnProgram("xdg-open", {p}, err);
#endif
}

// One row in the mode list and one tuning for every mode of every valid plugin (also the permanent "Plugins" row, family 999).
void registerModes() {
    dect2::registerPluginTuning(999 + 2, "plugins", "Plugins", 0.001, 6000, 100, 2e6, 1);
    for (const auto& r : P.mgr->modeRows()) {
        dp::Entry e;
        if (!P.mgr->find(r.pluginId, e)) continue;
        std::string blurb = e.m.description.empty() ? e.m.name : e.m.description.substr(0, 200);
        addPluginModeRow(r.number, r.name, blurb, "Plugin: " + e.m.name);
        const double rate = r.sampleRates.empty() ? 2e6 : r.sampleRates.back() >= 2e6 ? 2e6 : r.sampleRates.back();
        dect2::registerPluginTuning(r.number + 2, "plugin-" + r.pluginId + "-" + r.modeId, r.name, 0.001, 6000, r.defaultHz / 1e6, rate, r.bandwidthHz / 1e6);
    }
}

void init(App& a) {
    P.inited = true;
    dp::ManagerConfig c;
    c.pluginsDir = plat::dataDir() + "/plugins";
    const std::string ed = exeDir();
    for (const char* rel : {"/plugins", "/../Resources/plugins", "/../lib/onair/plugins", "/../plugins"}) {
        std::error_code ec;
        if (fs::is_directory(ed + rel, ec)) { c.bundledDir = (fs::path(ed + rel)).lexically_normal().string(); break; }
    }
#if defined(_WIN32)
    const char* hostName = "onair-plugin-host.exe";
#else
    const char* hostName = "onair-plugin-host";
#endif
    for (const char* rel : {"/", "/../Resources/", "/../lib/onair/"}) {
        std::error_code ec;
        const fs::path h = fs::path(ed + rel + hostName).lexically_normal();
        if (fs::exists(h, ec)) { c.hostExe = h.string(); break; }
    }
    P.hostAvail = !c.hostExe.empty();
    c.hostVersion = ONAIR_VERSION;
    P.mgr = std::make_unique<dp::Manager>(c);
    P.mgr->setAudioSink(playPluginSound);
    P.mgr->scan();
    P.mgr->beginSession();
    if (plat::prefs().getB("pluginsSafeMode", false)) P.mgr->forceSafeMode(true);
    registerModes();
    P.mgr->attachAutomation();
    if (!P.mgr->safeMode()) P.mgr->startActions();
    a.engine.setPluginSink([](const float* iq, size_t n, double rate) {
        if (customModeSink(iq, n, rate)) return;   // a custom mode of the Mode Builder runs (modebuilder_ui.cpp)
        std::lock_guard<std::mutex> lk(P.sinkMu);
        if (!P.active) return;
        dp::IqMeta m;
        m.centreHz = P.centreHz.load(); m.sampleRate = rate;
        m.timestamp = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        m.firstSample = P.counter.fetch_add(n);
        P.active->feedIq(iq, n, m);
    });
}

void stopActive() {
    std::string id = P.activeId, mode = P.activeMode;
    {
        std::lock_guard<std::mutex> lk(P.sinkMu);
        P.active = nullptr;
    }
    if (!id.empty()) { P.mgr->stop(id, mode); stopPluginSound(id); }
    P.activeId.clear(); P.activeMode.clear();
}

dp::Entry entryOf(const std::string& id) { dp::Entry e; P.mgr->find(id, e); return e; }

// The prompt shown the first time a plugin that asks for permissions is turned on.
bool permissionPrompt(const dp::Entry& e) {
    if (!e.needsPrompt()) return false;
    ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
    ImGui::TextWrapped(tr("%s asks for these permissions:"), e.m.name.c_str());
    ImGui::PopStyleColor();
    static std::map<std::string, uint32_t> pick;
    uint32_t& m = pick.emplace(e.m.id, e.m.permissions).first->second;
    for (uint32_t b = 1; b <= dp::PermSpawn; b <<= 1) if (e.m.permissions & b) {
        bool on = m & b;
        if (ImGui::Checkbox(trw(permLabel(b)), &on)) m = on ? (m | b) : (m & ~b);
    }
    ImGui::TextDisabled("%s", tr("Permissions are what OnAir hands the plugin. A plugin running inside OnAir can still use the system itself: install plugins you trust, or run them in a helper process."));
    std::string err;
    if (ImGui::Button(trw("Allow"))) P.mgr->setGranted(e.m.id, m, err);
    ImGui::SameLine();
    if (ImGui::Button(trw("Do not allow"))) P.mgr->setGranted(e.m.id, 0, err);
    return true;
}

void settingsControls(const dp::Entry& e) {
    auto sv = P.mgr->settings(e.m.id);
    if (!sv) return;
    for (const auto& d : e.m.settings) {
        ImGui::PushID(d.key.c_str());
        if (d.type == "slider") {
            float v = (float)sv->number(d.key, d.def);
            ImGui::SetNextItemWidth(220 * gUi);
            if (ImGui::SliderFloat(d.label.c_str(), &v, (float)d.min, (float)d.max, "%.4g")) { char b[64]; snprintf(b, sizeof b, "%.8g", v); P.mgr->setSetting(e.m.id, d.key, b); }
        } else if (d.type == "checkbox") {
            bool v = sv->flag(d.key, false);
            if (ImGui::Checkbox(d.label.c_str(), &v)) P.mgr->setSetting(e.m.id, d.key, v ? "1" : "0");
        } else if (d.type == "combo") {
            int cur = (int)sv->number(d.key, 0);
            cur = std::max(0, std::min(cur, (int)d.options.size() - 1));
            ImGui::SetNextItemWidth(220 * gUi);
            if (ImGui::BeginCombo(d.label.c_str(), d.options[(size_t)cur].c_str())) {
                for (size_t i = 0; i < d.options.size(); i++) if (ImGui::Selectable(d.options[i].c_str(), (int)i == cur)) P.mgr->setSetting(e.m.id, d.key, std::to_string(i));
                ImGui::EndCombo();
            }
        } else {
            char buf[512];
            snprintf(buf, sizeof buf, "%s", sv->text(d.key).c_str());
            ImGui::SetNextItemWidth(260 * gUi);
            if (ImGui::InputText(d.label.c_str(), buf, sizeof buf)) P.mgr->setSetting(e.m.id, d.key, buf);
        }
        ImGui::PopID();
    }
}

// ---- the generic results screen ----
const char* colTypeText(const dp::Cell& c, std::string& tmp) {
    char b[64];
    switch (c.type) {
    case 1: snprintf(b, sizeof b, "%lld", (long long)c.i); tmp = b; return tmp.c_str();
    case 2: snprintf(b, sizeof b, "%.4g", c.d); tmp = b; return tmp.c_str();
    case 3: if (c.d >= 1e6) snprintf(b, sizeof b, "%.6f MHz", c.d / 1e6); else snprintf(b, sizeof b, "%.1f Hz", c.d); tmp = b; return tmp.c_str();
    case 4: { const time_t t = (time_t)c.d; struct tm tmv;
#ifdef _WIN32
        gmtime_s(&tmv, &t);
#else
        gmtime_r(&t, &tmv);
#endif
        strftime(b, sizeof b, "%H:%M:%S", &tmv); tmp = b; return tmp.c_str(); }
    case 5: return c.i ? "yes" : "no";
    default: return c.s.c_str();
    }
}

void drawPlot(const dp::Plot& p) {
    const size_t n = p.data.size();
    if (!n) return;
    const bool constel = p.kind == 2;
    if (plt::BeginPlot((std::string("##pp") + p.id).c_str(), ImVec2(-1, (constel ? 240 : 170) * gUi), plt::Flags_NoLegend)) {
        if (constel) {
            std::vector<double> xs, ys;
            const size_t pts = std::min<size_t>(n / 2, 4000);
            for (size_t i = 0; i < pts; i++) { xs.push_back(p.data[2 * i]); ys.push_back(p.data[2 * i + 1]); }
            plt::SetupAxes("I", "Q");
            plt::PlotScatter("", xs.data(), ys.data(), (int)xs.size());
        } else {
            // at most ~1500 points: the largest value of each group, so peaks stay
            const size_t step = std::max<size_t>(1, n / 1500);
            std::vector<double> xs, ys;
            for (size_t i = 0; i < n; i += step) {
                float m = p.data[i];
                for (size_t k = i; k < std::min(n, i + step); k++) m = std::max(m, p.data[k]);
                xs.push_back(p.x0 + p.dx * (double)i); ys.push_back(m);
            }
            plt::SetupAxes(p.kind == 0 ? "Hz" : "x", p.kind == 0 ? "dB" : "");
            plt::PlotLine("", xs.data(), ys.data(), (int)xs.size());
        }
        plt::EndPlot();
    }
}

void drawImages(const dp::Snapshot& s) {
    for (const auto& im : s.images) {
        std::vector<uint32_t> px;
        int w = 0, h = 0;
        if (im.format == 0) { w = (int)im.w; h = (int)im.h; px.resize((size_t)w * h); memcpy(px.data(), im.bytes.data(), px.size() * 4); }
        else {
            const std::string f = plat::cacheDir() + "/plugin-image.png";
            FILE* fp = fopen(f.c_str(), "wb");
            if (fp) { fwrite(im.bytes.data(), 1, im.bytes.size(), fp); fclose(fp); }
            if (!plat::decodeImage(f, w, h, px) || w <= 0 || h <= 0 || w > 4096 || h > 4096) continue;
        }
        gfx::Image*& tex = P.images[im.id];
        static std::map<std::string, std::pair<int, int>> dims;
        auto& d = dims[im.id];
        if (!tex || d.first != w || d.second != h) { delete tex; tex = gGfx->createImage(w, h, 0xFF000000u); d = {w, h}; }
        static std::map<std::string, uint64_t> seen;
        const uint64_t key = s.seq;
        if (seen[im.id] != key) { tex->update(0, 0, w, h, px.data()); seen[im.id] = key; }
        const float scale = std::min(1.0f, ImGui::GetContentRegionAvail().x / (float)w);
        ImGui::Image(tex->texture(), ImVec2((float)w * scale, (float)h * scale));
    }
}

void resultsPanel(App& a, const std::string& pid, const std::string& mid) {
    auto res = P.mgr->results(pid, mid);
    const dp::Snapshot s = res->snapshot();
    for (const auto& st : s.status) {
        lamp("", st.lamp);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", st.key.c_str());
        ImGui::SameLine(140 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(st.text.c_str()); ImGui::PopFont();
    }
    for (const auto& p : s.plots) { ImGui::TextDisabled("%s", p.title.c_str()); drawPlot(p); }
    for (const auto& t : s.tables) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s  (%llu)", t.title.c_str(), (unsigned long long)t.rowsTotal);
        const float h = std::min(260.0f * gUi, (float)(t.rows.size() + 2) * ImGui::GetTextLineHeightWithSpacing() + 6);
        if (ImGui::BeginTable((std::string("##pt") + t.id).c_str(), (int)t.cols.size(), ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, ImVec2(-1, h))) {
            for (const auto& c : t.cols) ImGui::TableSetupColumn(c.name.c_str());
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            std::string tmp;
            for (size_t r = t.rows.size(); r-- > 0;) {   // newest first
                ImGui::TableNextRow();
                for (size_t c = 0; c < t.cols.size(); c++) { ImGui::TableSetColumnIndex((int)c); ImGui::TextUnformatted(colTypeText(t.rows[r][c], tmp)); }
            }
            ImGui::EndTable();
        }
    }
    drawImages(s);
    if (!s.points.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s (%d)", tr("Map points"), (int)s.points.size());
        for (size_t i = 0; i < s.points.size() && i < 12; i++) ImGui::Text("%s  %.5f, %.5f", s.points[i].label.c_str(), s.points[i].lat, s.points[i].lon);
    }
    if (!s.log.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Log"));
        ImGui::BeginChild("##plog", ImVec2(-1, 130 * gUi), true);
        ImGui::PushFont(a.mono, 0);
        for (const auto& l : s.log) ImGui::TextUnformatted(l.c_str());
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
        ImGui::PopFont();
        ImGui::EndChild();
    }
    if (s.refused) ImGui::TextDisabled("%s: %llu", tr("Results the plugin sent beyond the limits"), (unsigned long long)s.refused);
}

// ---- the manager ----
void managerPanel(App& a) {
    if (!P.inited) { ImGui::TextDisabled("%s", tr("Plugins are not available on this receiver.")); return; }
    std::string err;
    if (P.mgr->safeMode()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
        ImGui::TextWrapped("%s %s", tr("Safe mode: all plugins are switched off."), P.mgr->safeModeReason().c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button(trw("Try plugins again"))) { P.mgr->leaveSafeMode(); plat::prefs().setB("pluginsSafeMode", false); plat::prefs().flush(); }
        ImGui::Separator();
    }
    if (ImGui::Button(trw("Install from file..."))) {
        const std::string f = plat::openFileDialog();
        if (!f.empty()) {
            std::string id;
            if (P.mgr->installFile(f, id, err)) { P.installMsg = std::string(tr("Installed")) + ": " + id; P.selId = id; registerModes(); }
            else P.installMsg = std::string(tr("Not installed")) + ": " + err;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(trw("Open folder"))) { std::error_code ec; fs::create_directories(P.mgr->pluginsDir(), ec); openPath(P.mgr->pluginsDir()); }
    ImGui::SameLine();
    if (ImGui::Button(trw("Refresh"))) { P.mgr->scan(); registerModes(); }
    ImGui::SameLine();
    if (ImGui::Button(trw("Community plugins"))) openPath("https://github.com/adswill/OnAir/wiki/Plugins");
    ImGui::SameLine();
    bool sm = plat::prefs().getB("pluginsSafeMode", false);
    if (ImGui::Checkbox(trw("Safe mode (no plugins)"), &sm)) { plat::prefs().setB("pluginsSafeMode", sm); plat::prefs().flush(); if (sm) P.mgr->forceSafeMode(true); else P.mgr->leaveSafeMode(); }
    if (!P.installMsg.empty()) ImGui::TextDisabled("%s", P.installMsg.c_str());
    ImGui::Separator();
    auto es = P.mgr->entries();
    if (es.empty()) ImGui::TextWrapped("%s", tr("No plugins installed. Use Install from file, or copy a plugin folder into the plugins folder. A plugin is a .onairplugin file."));
    ImGui::BeginChild("##pl", ImVec2(250 * gUi, 0), true);
    for (const auto& e : es) {
        std::string label = e.m.name + "  " + e.m.version + (e.bundled ? "  (example)" : "") + "##" + e.m.id;
        if (ImGui::Selectable(label.c_str(), P.selId == e.m.id)) P.selId = e.m.id;
        if (!e.valid) { ImGui::SameLine(); ImGui::TextColored(pal::badRed(), "!"); }
        else if (e.enabled) { ImGui::SameLine(); ImGui::TextColored(pal::okGreen(), "*"); }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##pd", ImVec2(0, 0), false);
    dp::Entry e;
    if (!P.selId.empty() && P.mgr->find(P.selId, e)) {
        ImGui::PushFont(a.mono, 0); ImGui::Text("%s %s", e.m.name.c_str(), e.m.version.c_str()); ImGui::PopFont();
        ImGui::TextDisabled("%s: %s    %s: %s    %s: %s", tr("Author"), e.m.author.c_str(), tr("Licence"), e.m.license.c_str(), tr("Kind"), e.m.kind.c_str());
        if (!e.m.homepage.empty()) ImGui::TextDisabled("%s", e.m.homepage.c_str());
        if (!e.m.description.empty()) ImGui::TextWrapped("%s", e.m.description.c_str());
        if (!e.valid) { ImGui::TextColored(pal::badRed(), "%s", e.error.c_str()); }
        else {
            ImGui::Spacing();
            if (e.m.permissions) {
                ImGui::TextDisabled("%s", tr("Asks for"));
                for (uint32_t b = 1; b <= dp::PermSpawn; b <<= 1) if (e.m.permissions & b) ImGui::BulletText("%s%s", tr(permLabel(b)), e.permsAnswered ? ((e.granted & b) ? "  [allowed]" : "  [refused]") : "");
            } else ImGui::TextDisabled("%s", tr("Asks for no permissions."));
            if (!permissionPrompt(e)) {
                bool en = e.enabled;
                if (ImGui::Checkbox(trw("Enabled"), &en)) { P.mgr->setEnabled(e.m.id, en, err); if (!en) { P.mgr->stop(e.m.id, ""); if (P.activeId == e.m.id) stopActive(); } else if (e.m.kind == "action" && !P.mgr->safeMode()) P.mgr->startActions(); registerModes(); }
                if (!e.m.modes.empty() || e.m.kind == "action") {
                    ImGui::SameLine();
                    bool oop = P.mgr->outOfProcess(e.m.id);
                    ImGui::BeginDisabled(!P.hostAvail);
                    if (ImGui::Checkbox(trw("Run in a helper process"), &oop)) P.mgr->setOutOfProcess(e.m.id, oop);
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("A crash or a hang of the plugin then only ends its helper process. Takes effect the next time the plugin starts."));
                }
            }
            if (!e.m.modes.empty()) {
                ImGui::TextDisabled("%s", tr("Modes (in the mode list, group PLUGINS)"));
                for (const auto& r : P.mgr->modeRows()) if (r.pluginId == e.m.id) {
                    ImGui::BulletText("%s  (#%d)", r.name.c_str(), r.number);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!r.available);
                    ImGui::PushID(r.number);
                    if (ImGui::SmallButton(trw("Open"))) selectMode(a, r.number, nullptr);
                    ImGui::PopID();
                    ImGui::EndDisabled();
                }
            }
            if (!e.m.settings.empty()) { ImGui::Spacing(); ImGui::TextDisabled("%s", tr("Settings")); settingsControls(e); }
            if (dp::Runner* r = P.mgr->running(e.m.id, "")) ImGui::TextDisabled("%s: %s", tr("State"), dp::runStateName(r->state()));
        }
        ImGui::Spacing();
        if (!e.bundled) {
            if (ImGui::Button(trw("Remove"))) { if (P.activeId == e.m.id) stopActive(); if (P.mgr->remove(e.m.id, err)) { P.selId.clear(); registerModes(); } else P.installMsg = err; }
            ImGui::SameLine();
        }
        if (ImGui::Button(trw("Open its folder"))) openPath(e.dir);
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Plugin log"));
        ImGui::BeginChild("##pglog", ImVec2(-1, 0), true);
        ImGui::PushFont(a.mono, 0);
        for (const auto& l : P.mgr->log(e.m.id, 200)) ImGui::TextUnformatted(l.c_str());
        ImGui::PopFont();
        ImGui::EndChild();
    } else ImGui::TextDisabled("%s", tr("Select a plugin."));
    ImGui::EndChild();
}

// the manager with its two tabs: what is installed, and the Store (docs/PLUGIN-MARKETPLACE-3.9.md)
static bool gWantStoreTab = false;   // Tools > Plugin store: open the window on the Store tab (once)

void managerTabs(App& a) {
    if (!P.inited) { managerPanel(a); return; }
    if (ImGui::BeginTabBar("##plugtabs")) {
        if (ImGui::BeginTabItem(trw("Installed"))) { managerPanel(a); ImGui::EndTabItem(); }
        const bool pick = gWantStoreTab; gWantStoreTab = false;
        if (ImGui::BeginTabItem(trw("Store"), nullptr, pick ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (pluginStoreTab(a, *P.mgr, [](const std::string& id) { if (P.activeId == id) stopActive(); })) registerModes();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// the main tab of the "Plugins" row and of every plugin mode
void tab(App& a) {
    if (!P.inited) return;
    if (a.family == 999) { managerTabs(a); return; }
    std::string pid, mid;
    if (!P.mgr->modeFromNumber(a.family, pid, mid)) { ImGui::TextDisabled("%s", tr("This plugin mode is not installed any more.")); return; }
    dp::Entry e;
    if (!P.mgr->find(pid, e)) { ImGui::TextDisabled("%s", tr("This plugin mode is not installed any more.")); return; }
    ImGui::PushFont(a.mono, 0); ImGui::Text("%s - %s", e.m.name.c_str(), e.m.version.c_str()); ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("%s %s", tr("by"), e.m.author.c_str());
    if (a.rxId != 1) { ImGui::TextWrapped("%s", tr("Plugin modes run on receiver 1.")); return; }
    if (P.mgr->safeMode()) { ImGui::TextColored(pal::warnAmber(), "%s", tr("Safe mode is on: plugins are switched off. Open the Plugins mode to leave it.")); return; }
    if (!e.valid) { ImGui::TextColored(pal::badRed(), "%s", e.error.c_str()); return; }
    if (permissionPrompt(e)) return;
    if (!e.enabled) {
        ImGui::TextWrapped("%s", tr("This plugin is off."));
        if (ImGui::Button(trw("Turn it on"))) { std::string err; P.mgr->setEnabled(pid, true, err); registerModes(); }
        return;
    }
    if (!P.error.empty()) ImGui::TextColored(pal::badRed(), "%s", P.error.c_str());
    else if (dp::Runner* r = P.mgr->running(pid, mid)) {
        ImGui::TextDisabled("%s: %s    %s: %llu    %s: %llu", tr("State"), dp::runStateName(r->state()), tr("blocks"), (unsigned long long)r->processedBlocks(), tr("dropped"), (unsigned long long)r->droppedBlocks());
    }
    if (!e.m.settings.empty()) { ImGui::Spacing(); settingsControls(e); }
    ImGui::Spacing();
    resultsPanel(a, pid, mid);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    if (a.family == 999) { l1 = "Plugins"; l2 = P.inited ? std::to_string(P.mgr->entries().size()) + " installed" : ""; return; }
    std::string pid, mid;
    if (P.inited && P.mgr->modeFromNumber(a.family, pid, mid)) {
        dp::Entry e;
        if (P.mgr->find(pid, e)) { l1 = e.m.name; l2 = e.m.version + (P.activeId == pid ? "  running" : ""); }
    }
}

void pluginStatus(App&) {
    if (!P.inited) return;
    ImGui::TextDisabled("%s", P.error.empty() ? (P.active ? tr("Plugin running") : tr("Plugin idle")) : P.error.c_str());
}

} // namespace

extern const ModeUi kPluginUi;
const ModeUi kPluginUi = {
    .sideTitle = "PLUGINS",
    .tabName = "Plugin",
    .tabIcon = Ic::Link,
    .tab = tab,
    .status = pluginStatus,
    .summary = summary,
};

void pluginRegister(App& a) { if (a.rxId == 1 && !P.inited) init(a); }   // first thing in the frame: the plugin rows have a tuning before anything asks for it (#465)

void pluginFrame(App& a) {
    if (a.rxId != 1) return;
    pluginRegister(a);
    gSoundVolume = a.volume; gSoundMuted = a.muted;   // the sound controls are shared with the other modes
    const double now = ImGui::GetTime();
    if (now - P.lastTick > 1.0) {
        P.lastTick = now;
        P.mgr->tick(std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count());
        // close the outputs of plugins that do not run any more (switched off, removed, failed): only the active mode and actions play
        dropPluginSounds([](const std::string& id) { return id == P.activeId || P.mgr->running(id, "") != nullptr; });
        if (P.active && P.activeId.size()) {
            dp::Runner* r = P.mgr->running(P.activeId, P.activeMode);
            if (!r) { P.error = tr("The plugin was switched off after a failure. See its log in the Plugins mode."); stopActive(); }
        }
    }
    P.centreHz = a.freqMhz * 1e6;
    const bool pluginMode = a.family >= dp::kFirstModeNumber && !isCustomFamily(a.family) && a.family != kCustomFamilyBase - 1;   // not the Mode Builder's
    if (!pluginMode) {
        if (P.active) stopActive();
        P.error.clear();
        return;
    }
    std::string pid, mid;
    if (!P.mgr->modeFromNumber(a.family, pid, mid)) return;
    if (P.curFamily != a.family) { stopActive(); P.error.clear(); P.startFailed = false; P.curFamily = a.family; }
    if (!P.active) {
        dp::Entry e;
        const bool startable = P.mgr->find(pid, e) && e.valid && e.enabled && !e.needsPrompt() && !P.mgr->safeMode();
        if (!startable) { P.startFailed = false; P.error.clear(); return; }
        if (P.startFailed) return;   // the user turns the plugin off and on again to retry
        std::string err;
        dp::Runner* r = P.mgr->start(pid, mid, err);
        if (!r) { P.error = err; P.startFailed = true; return; }
        { std::lock_guard<std::mutex> lk(P.sinkMu); P.active = r; }
        P.activeId = pid; P.activeMode = mid; P.lastRate = 0;
    }
    const double rate = a.engine.sampleRate();
    if (rate != P.lastRate || std::fabs(P.centreHz - P.lastCentre) > 1) {
        P.lastRate = rate; P.lastCentre = P.centreHz;
        P.active->configure(P.centreHz, rate);
    }
    double hz = 0, sr = 0;
    if (P.mgr->wantedIq(pid, hz, sr) && hz > 0 && std::fabs(hz - P.askedHz) > 1) { P.askedHz = hz; tuneFreq(a, hz / 1e6); }
}

void pluginShutdown() {
    pluginStoreShutdown();
    if (!P.inited) return;
    stopActive();
    P.mgr->stopAll();
    P.mgr->endSession();
    for (auto& kv : P.images) delete kv.second;
    P.images.clear();
}

void pluginStoreOpen(App& a) { a.pluginsOpen = true; gWantStoreTab = true; }

void pluginManagerWindow(App& a) {
    if (!a.pluginsOpen || a.rxId != 1) return;
    if (!P.inited) init(a);
    firstWindowSize(820 * gUi, 520 * gUi);
    if (!ImGui::Begin(trw("Plugins"), &a.pluginsOpen)) { ImGui::End(); return; }
    managerTabs(a);
    ImGui::End();
}
