// The app side of user-installed speech codecs (see speech_codec_ui.h and docs/CODECS.md).
#include "speech_codec_ui.h"
#include "../app.h"
#include "../ui/scale.h"
#include "../platform/platform.h"
#include "dect2/codec/speech_codec.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/engine/updater.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/p25/p25_imbe.h"
#include "dect2/hfdig/hfdig_freedv.h"
#include <filesystem>
#include <cstdio>
#include <chrono>
#include <map>
#include <set>

namespace appcodec {

namespace {

// Codecs whose notice the user chose "Not now" for in this run: the dialog is not reopened until the next start.
std::set<std::string> gDismissed;
std::map<std::string, bool> acceptedAt;

std::map<std::string, dect2::codec::CodecLookup> last;
std::map<std::string, double> lastAt;
std::map<std::string, std::string> lastLogged;
std::set<std::string> gRegistered;

std::string prefKey(const std::string& id) { return "codec_notice_" + id; }

} // namespace

std::string codecsFolder() { return plat::dataDir() + "/codecs"; }

dect2::codec::CodecLookup speechCodecPoll(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out) {
    const std::string codec = id;
    // Not ready yet: look again at most once a second (the plug-in file may be dropped in while running), and keep the last verdict between looks.
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto it = last.find(codec);
    const bool accepted = plat::basePrefs().getB(prefKey(codec).c_str(), false);
    if (it != last.end() && it->second.status != dect2::codec::CodecStatus::Ready && now - lastAt[codec] < 1.0 && accepted == acceptedAt[codec])
        return it->second;
    acceptedAt[codec] = accepted;
    lastAt[codec] = now;
    const dect2::codec::CodecLookup r = dect2::codec::loadSpeechCodec(codec, codecsFolder(), accepted, out);
    last[codec] = r;
    // Log the reason whenever it changes, so a user with no audio can send the line.
    if (lastLogged[codec] != r.message) {
        lastLogged[codec] = r.message;
        fprintf(stderr, "[codec %s] %s\n", id, r.message.c_str());
    }
    return r;
}

bool speechCodecGate(const char* id, std::unique_ptr<dect2::codec::SpeechCodec>& out) {
    const std::string codec = id;
    const std::string folder = codecsFolder();
    const dect2::codec::CodecLookup r = speechCodecPoll(id, out);

    if (r.status == dect2::codec::CodecStatus::Ready) return true;

    if (r.status == dect2::codec::CodecStatus::NotInstalled) {
        ImGui::TextWrapped("%s", r.message.c_str());
        if (ImGui::Button("Open codecs folder")) {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(folder), ec);
            dect2::openUrl(folder);
        }
        ImGui::SameLine();
        if (ImGui::Button("Full instructions")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS.md");
        return false;
    }

    if (r.status == dect2::codec::CodecStatus::NoticeRequired) {
        const std::string popup = "Speech codec notice: " + codec;
        if (!gDismissed.count(codec) && !ImGui::IsPopupOpen(popup.c_str())) ImGui::OpenPopup(popup.c_str());
        if (ImGui::BeginPopupModal(popup.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
            ImGui::TextWrapped("%s", dect2::codec::speechCodecNotice().c_str());
            ImGui::PopTextWrapPos();
            if (ImGui::Button("I accept")) {
                plat::basePrefs().setB(prefKey(codec).c_str(), true);
                plat::basePrefs().flush();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Not now")) {
                gDismissed.insert(codec);
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::TextWrapped("%s", r.message.c_str());
        return false;
    }

    // LoadFailed: say why, so the user can fix the file.
    ImGui::TextWrapped("%s", r.message.c_str());
    return false;
}

void codecNoticePrompt(const char* id) {
    std::unique_ptr<dect2::codec::SpeechCodec> tmp;
    const dect2::codec::CodecLookup r = speechCodecPoll(id, tmp);
    if (r.status == dect2::codec::CodecStatus::NoticeRequired) speechCodecGate(id, tmp);
}

void voiceCodecLine(const char* id, const char* name, const std::string& libPath) {
    voiceCodecTick(id);
    std::string p;
    const bool plug = dect2::codec::pluginLoaded(id, &p);
    const bool ok = plug || !libPath.empty();
    ImGui::AlignTextToFramePadding();
    if (ok) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "Voice codec loaded: %s %s (%s)", name, plug ? "plug-in" : "libmbe", plug ? p.c_str() : libPath.c_str());
    else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Voice codec missing: %s. Install the plug-in (see ?). Calls are still found and logged.", name);
    codecNoticePrompt(id);
}

void speechCodecRescan(const char* id) {
    last.erase(id);
    lastAt.erase(id);
}

bool voiceCodecTick(const char* id) {
    const std::string codec = id;
    if (gRegistered.count(codec)) return true;
    std::unique_ptr<dect2::codec::SpeechCodec> probe;
    const dect2::codec::CodecLookup r = speechCodecPoll(id, probe);
    if (r.status != dect2::codec::CodecStatus::Ready) return false;
    probe.reset();
    const std::string folder = codecsFolder();
    // every decoder (one per slot / speaker) is a new instance of the plug-in; the notice was accepted, or it would not be Ready
    dect2::codec::setPluginFactory(codec, [codec, folder]() {
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        dect2::codec::loadSpeechCodec(codec, folder, true, c);
        return c;
    }, r.path);
    gRegistered.insert(codec);
    return true;
}

void codecInstallHelper(const char* id) {
    const std::string codec = id;
    voiceCodecTick(id);
    const std::string folder = codecsFolder();
    std::string path;
    if (dect2::codec::pluginLoaded(codec, &path)) {
        ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "Codec plug-in loaded: %s", path.c_str());
    } else {
        std::unique_ptr<dect2::codec::SpeechCodec> tmp;
        const dect2::codec::CodecLookup r = speechCodecPoll(id, tmp);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", r.status == dect2::codec::CodecStatus::NotInstalled
            ? dect2::codec::pluginStatusLine(codec, folder).c_str() : r.message.c_str());
        ImGui::PopTextWrapPos();
        if (r.status == dect2::codec::CodecStatus::NoticeRequired) speechCodecGate(id, tmp);
    }
    ImGui::TextDisabled("Codecs folder: %s", folder.c_str());
    if (ImGui::Button("Open codecs folder")) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(folder), ec);
        dect2::openUrl(folder);
    }
    ImGui::SameLine();
    if (ImGui::Button("Build steps")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS.md#speech-codec-plug-ins-voice-modes");
    ImGui::SameLine();
    if (ImGui::Button("Build helper page")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS-BUILD-HELPER.md");
    ImGui::SameLine();
    if (ImGui::Button("Rescan for plug-in")) speechCodecRescan(id);
    ImGui::TextDisabled("Easiest way: run the build helper script (see the page below)");
    ImGui::TextDisabled("Upstream mbelib project page: https://github.com/lwvmobile/mbelib. OnAir downloads nothing for you.");
}

void voiceCodecsWindow(App& a) {
    if (!a.voiceCodecsOpen) return;
    ImGui::SetNextWindowSize(ImVec2(760 * gUi, 460 * gUi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Voice codecs", &a.voiceCodecsOpen)) { ImGui::End(); return; }
    for (const char* id : {"ambe2", "ambe", "imbe"}) voiceCodecTick(id);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("OnAir never ships a patented voice codec. Patented codecs are loaded at run time from a plug-in (or library) that you install yourself in the codecs folder; open codecs need nothing. Encrypted calls are never decoded. Check the patent and radio rules where you live.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    const std::string folder = codecsFolder();
    namespace cd = dect2::codec;
    if (ImGui::BeginTable("##vcodecs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Codec"); ImGui::TableSetupColumn("Modes"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("Status");
        ImGui::TableHeadersRow();
        int row = 0;
        for (const auto& ci : cd::codecTable()) {
            ImGui::PushID(row++);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", ci.name);
            ImGui::TableNextColumn(); ImGui::TextWrapped("%s", ci.modes);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ci.kind == cd::CodecKind::Open ? "open" : ci.kind == cd::CodecKind::Patented ? "patented, plug-in only" : "not supported");
            ImGui::TableNextColumn();
            const std::string id = ci.id;
            if (ci.kind == cd::CodecKind::Open) {
                std::string p, v;
                if (std::string(ci.name) == "Codec 2") {
                    if (dect2::hfdigFreedvLibrary(&p, &v)) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "loaded: %s", p.c_str());
                    else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "libcodec2 missing: reinstall OnAir");
                } else ImGui::TextUnformatted(ci.note);
            } else if (ci.kind == cd::CodecKind::Unsupported) {
                ImGui::TextWrapped("%s", ci.note);
            } else {
                std::string p;
                const bool plug = cd::pluginLoaded(id, &p);
                const bool mbe = !plug && (id == "ambe2" ? dect2::nxdnAmbeAvailable(&p) : id == "ambe" ? dect2::dstarAmbeAvailable(&p) : id == "imbe" ? dect2::p25ImbeAvailable(&p) : false);
                std::error_code ec;
                const bool onDisk = std::filesystem::is_regular_file(std::filesystem::path(folder) / cd::speechCodecFileName(id), ec);
                if (plug) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "plug-in loaded: %s", p.c_str());
                else if (mbe) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "libmbe loaded: %s", p.c_str());
                else if (onDisk) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "plug-in file found: accept the notice or start the mode to load it");
                else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "missing: %s", cd::speechCodecFileName(id).c_str());
                if (ImGui::SmallButton("Install helper")) ImGui::OpenPopup("##vch");
                if (ImGui::BeginPopup("##vch")) {
                    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                    codecInstallHelper(ci.id);
                    ImGui::PopTextWrapPos();
                    ImGui::EndPopup();
                }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Codecs folder: %s", folder.c_str());
    if (ImGui::Button("Open codecs folder")) {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(folder), ec);
        dect2::openUrl(folder);
    }
    ImGui::SameLine();
    if (ImGui::Button("Instructions")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS.md");
    ImGui::End();
}

} // namespace appcodec
