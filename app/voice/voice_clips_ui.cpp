#include "voice_clips_ui.h"
#include "../app.h"
#include "../platform/platform.h"
#include "../i18n/i18n.h"
#include "dect2/audio/audioout.h"
#include "dect2/engine/updater.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>

namespace voiceclips {
namespace vc = dect2::voice;
namespace fs = std::filesystem;
namespace {

struct Player {
    std::unique_ptr<dect2::AudioOut> out;
    std::vector<float> pcm;   // mono, 48 kHz
    size_t pos = 0;
    std::string path;
    bool playing() const { return !path.empty(); }
} P;

struct State {
    std::map<std::string, std::unique_ptr<vc::ClipHistory>> hist;
    bool loaded = false;
    vc::ClipPolicy policy;
    double lastScan = -1e9;
    std::string note;
} S;

void loadPrefs() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.policy.keepLast = int(d.getI("voiceClipKeep", 20));
    S.policy.maxAgeDays = int(d.getI("voiceClipDays", 0));
    S.policy.maxMB = int(d.getI("voiceClipMB", 0));
    S.policy = vc::ClipPolicy::clamped(S.policy);
}

void savePrefs(vc::ClipHistory& h) {
    plat::Prefs& d = plat::prefs();
    d.setI("voiceClipKeep", S.policy.keepLast);
    d.setI("voiceClipDays", S.policy.maxAgeDays);
    d.setI("voiceClipMB", S.policy.maxMB);
    d.setS(("voiceClipPins_" + h.mode()).c_str(), h.savePins());
}

int64_t unixNow() { return int64_t(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

std::string clockText(int64_t t) {
    if (t <= 0) return "-";
    time_t tt = time_t(t);
    struct tm l;
#ifdef _WIN32
    localtime_s(&l, &tt);
#else
    localtime_r(&tt, &l);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &l);
    return b;
}

void play(const vc::Clip& c) {
    std::vector<int16_t> pcm; int rate = 0;
    if (!vc::wavReadMono16(c.path, pcm, rate) || pcm.empty()) { S.note = "That clip could not be read."; return; }
    P.pcm.clear();
    const double ratio = 48000.0 / rate;
    const size_t n = size_t(std::lround(double(pcm.size()) * ratio));
    P.pcm.reserve(n);
    for (size_t i = 0; i < n; i++) {   // linear interpolation to 48 kHz
        const double x = double(i) / ratio;
        const size_t a = std::min(pcm.size() - 1, size_t(x)), b = std::min(pcm.size() - 1, a + 1);
        const double fr = x - double(a);
        P.pcm.push_back(float((pcm[a] * (1 - fr) + pcm[b] * fr) / 32768.0));
    }
    if (!P.out) { P.out = std::make_unique<dect2::AudioOut>(); P.out->start(48000); P.out->setStartThreshold(48000 / 10); }
    P.out->flush();
    P.pos = 0;
    P.path = c.path;
    S.note.clear();
}

void tickPlayer(App& a) {
    if (!P.playing() || !P.out) return;
    P.out->setVolume(a.volume); P.out->setMuted(a.muted);
    while (P.pos < P.pcm.size() && P.out->bufferedFrames() < 24000) {
        const size_t n = std::min<size_t>(4800, P.pcm.size() - P.pos);
        std::vector<float> st(n * 2);
        for (size_t i = 0; i < n; i++) st[2 * i] = st[2 * i + 1] = P.pcm[P.pos + i];
        P.out->write(st.data(), int(n));
        P.pos += n;
    }
    if (P.pos >= P.pcm.size() && P.out->bufferedFrames() <= 0) P.path.clear();   // finished
}

} // namespace

vc::ClipHistory& history(const std::string& mode) {
    loadPrefs();
    auto& p = S.hist[mode];
    if (!p) {
        p = std::make_unique<vc::ClipHistory>(mode);
        p->setPolicy(S.policy);
        p->loadPins(plat::prefs().getS(("voiceClipPins_" + mode).c_str(), ""));
    }
    return *p;
}

void stopPlayback() {
    P.path.clear();
    if (P.out) P.out->flush();
}

void panel(App& a, const char* mode, const char* id, const std::string& folder, const std::string& prefix, const std::vector<vc::Clip>& fresh) {
    vc::ClipHistory& h = history(mode);
    h.setPolicy(S.policy);
    const double now = ImGui::GetTime();
    if (now - S.lastScan > 15.0 || h.size() == 0) { S.lastScan = now; if (!folder.empty()) h.scanFolder(folder, prefix); }
    for (const auto& c : fresh) h.add(c);
    h.prune(unixNow());
    tickPlayer(a);

    ImGui::PushID(id);
    ImGui::SeparatorText(tr("Recent voice clips"));
    const auto list = h.clips();
    if (S.policy.keepLast > 0) ImGui::TextDisabled("%s", tr("The newest clips are kept; older ones are deleted automatically. A starred clip is always kept."));
    else ImGui::TextDisabled("%s", tr("All clips are kept (no count limit). A starred clip is never deleted by the other limits."));
    if (ImGui::SmallButton(tr("Open folder")) && !folder.empty()) { std::error_code ec; fs::create_directories(folder, ec); dect2::openUrl(folder); }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu %s, %.1f MB", list.size(), tr("clips"), double(h.totalBytes()) / 1048576.0);
    if (ImGui::CollapsingHeader(trw("Advanced"))) {
        bool ch = false;
        int k = S.policy.keepLast, days = S.policy.maxAgeDays, mb = S.policy.maxMB;
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("Keep the last N clips (0 = keep all)"), &k)) { S.policy.keepLast = k; ch = true; }
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("Delete clips older than N days (0 = never)"), &days)) { S.policy.maxAgeDays = days; ch = true; }
        ImGui::SetNextItemWidth(110 * gUi);
        if (ImGui::InputInt(tr("Total size cap in MB (0 = none)"), &mb)) { S.policy.maxMB = mb; ch = true; }
        ImGui::TextDisabled("%s", tr("Applies to every voice mode. Starred clips are exempt from all three limits."));
        if (ch) { S.policy = vc::ClipPolicy::clamped(S.policy); h.setPolicy(S.policy); savePrefs(h); }
    }
    if (!S.note.empty()) ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.2f, 1), "%s", S.note.c_str());
    const float rowH = ImGui::GetFrameHeightWithSpacing();
    if (ImGui::BeginTable("##clips", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit, ImVec2(0, std::min(220.f * gUi, rowH * float(list.size() + 2))))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Source"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Length")); ImGui::TableSetupColumn(tr("Play")); ImGui::TableSetupColumn(tr("Keep")); ImGui::TableSetupColumn(tr("File"));
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < list.size(); i++) {
            const vc::Clip& c = list[i];
            ImGui::PushID(int(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clockText(c.wallStart).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.label.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f s", c.seconds);
            ImGui::TableNextColumn();
            const bool isPlaying = P.path == c.path;
            if (ImGui::SmallButton(isPlaying ? tr("Stop") : tr("Play"))) { if (isPlaying) stopPlayback(); else play(c); }
            ImGui::TableNextColumn();
            bool pin = c.pinned;
            if (ImGui::Checkbox("##pin", &pin)) { h.setPinned(c.path, pin); savePrefs(h); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Star: keep this clip for good"));
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(tr("Save as..."))) {
                const std::string name = fs::path(c.path).filename().string();
                const std::string to = plat::saveFileDialog(name.c_str());
                if (!to.empty()) { std::error_code ec; fs::copy_file(c.path, to, fs::copy_options::overwrite_existing, ec); S.note = ec ? std::string(tr("Could not save: ")) + ec.message() : ""; }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Show"))) dect2::openUrl(fs::path(c.path).parent_path().string());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Delete"))) { if (isPlaying) stopPlayback(); h.remove(c.path, true, true); savePrefs(h); }
            ImGui::PopID();
        }
        if (list.empty()) { ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextDisabled("%s", tr("no clips saved yet")); }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

} // namespace voiceclips
