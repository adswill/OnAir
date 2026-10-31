// The app side of user-installed speech codecs (see speech_codec_ui.h and docs/CODECS.md).
#include "speech_codec_ui.h"
#include "../app.h"
#include "dect2/tv/avsplus_dec.h"
#include "../ui/scale.h"
#include "../platform/platform.h"
#include "dect2/codec/speech_codec.h"
#include "dect2/codec/codec_registry.h"
#include "dect2/codec/codec_paths.h"
#include "dect2/codec/codec_arch.h"
#include "dect2/engine/updater.h"
#include "dect2/dstar/dstar_ambe.h"
#include "dect2/nxdn/nxdn_ambe.h"
#include "dect2/p25/p25_imbe.h"
#include "dect2/hfdig/hfdig_freedv.h"
#include "dect2/cdr/cdr_dra.h"
#include "dect2/voice/codec_setup.h"
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

namespace {
const char* kExtraKey = "codec_extra_dir";
// The extra folder lives in the prefs; the core search list reads it from codec_paths (loaded once, then kept in step by setExtraFolder).
void syncExtra() {
    static bool done = false;
    if (done) return;
    done = true;
    dect2::codec::setExtraCodecDir(plat::basePrefs().getS(kExtraKey, ""));
}
bool fileExists(const std::string& p) { std::error_code ec; return std::filesystem::is_regular_file(std::filesystem::path(p), ec); }
void openFolder(const std::string& folder) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(folder), ec);
    dect2::openUrl(folder);
}
} // namespace

std::string codecsFolder() {
    const std::string u = dect2::codec::userCodecDir();
    return u.empty() ? plat::dataDir() + "/codecs" : u;
}

std::vector<dect2::codec::CodecDir> codecSearchFolders() {
    syncExtra();
    auto d = dect2::codec::systemCodecDirs();
    if (d.empty()) d.push_back({codecsFolder(), "user"});
    return d;
}

std::vector<std::string> codecSearchPaths() {
    std::vector<std::string> v;
    for (const auto& d : codecSearchFolders()) v.push_back(d.path);
    return v;
}

void setExtraFolder(const std::string& dir) {
    syncExtra();
    plat::basePrefs().setS(kExtraKey, dir);
    plat::basePrefs().flush();
    dect2::codec::setExtraCodecDir(dir);
    last.clear();
    lastAt.clear();
}

bool openCodecFolderButtons() {
    const auto dirs = codecSearchFolders();
    if (ImGui::Button("Open codecs folder")) openFolder(codecsFolder());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(your user folder; made if it does not exist)", codecsFolder().c_str());
    for (const auto& d : dirs) {
        if (d.source != "program") continue;
        ImGui::SameLine();
        if (ImGui::Button("Open folder next to OnAir")) openFolder(d.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n(made if it does not exist)", d.path.c_str());
        break;
    }
    for (const auto& d : dirs) {
        if (d.source != "extra") continue;
        ImGui::SameLine();
        if (ImGui::Button("Open extra folder")) openFolder(d.path);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.path.c_str());
        break;
    }
    return true;
}

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
    const dect2::codec::CodecLookup r = dect2::codec::loadSpeechCodecInDirs(codec, codecSearchPaths(), accepted, out);
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
    const dect2::codec::CodecLookup r = speechCodecPoll(id, out);

    if (r.status == dect2::codec::CodecStatus::Ready) return true;

    if (r.status == dect2::codec::CodecStatus::NotInstalled) {
        ImGui::TextWrapped("%s", r.message.c_str());
        openCodecFolderButtons();
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
    const std::string folder = r.folder;   // the folder the plug-in was found in
    // every decoder (one per slot / speaker) is a new instance of the plug-in; the notice was accepted, or it would not be Ready
    dect2::codec::setPluginFactory(codec, [codec, folder]() {
        std::unique_ptr<dect2::codec::SpeechCodec> c;
        dect2::codec::loadSpeechCodec(codec, folder, true, c);
        return c;
    }, r.path);
    gRegistered.insert(codec);
    return true;
}

void codecFoldersList(const std::string& codec) {
    const auto dirs = codecSearchFolders();
    const std::string file = dect2::codec::speechCodecFileName(codec);
    const auto hits = dect2::codec::dirsWithFile(dirs, file, fileExists);
    ImGui::TextDisabled("Folders searched, in this order (the first one that has the file wins)");
    for (const auto& d : dirs) {
        const char* mark = "";
        for (size_t h = 0; h < hits.size(); ++h)
            if (dect2::codec::samePath(hits[h].path, d.path)) mark = h == 0 ? "  <- found here" : "  <- also here (not used)";
        ImGui::BulletText("%s  [%s]%s", d.path.c_str(), d.source.c_str(), mark);
    }
    if (hits.size() > 1) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Warning: %s is in %d folders. OnAir uses the one in %s and ignores the others. Delete the extra copy to avoid confusion.",
                           file.c_str(), (int)hits.size(), hits[0].path.c_str());
        ImGui::PopTextWrapPos();
    }
}

// The processor OnAir runs as, the Rosetta notice (the Intel build on an Apple Silicon Mac cannot load arm64 plug-ins) and, for the
// build steps, which architecture to build a plug-in for.
void codecArchLines(bool buildTip) {
    namespace cd = dect2::codec;
    if (cd::runningUnderRosetta()) {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", cd::rosettaNotice().c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Open download page")) dect2::openUrl("https://github.com/adswill/OnAir/releases/latest");
    }
    ImGui::TextDisabled("OnAir runs as %s.", cd::processArch());
#if defined(__APPLE__)
    if (buildTip)
        ImGui::TextDisabled("Build plug-ins for %s (uname -m in a Terminal shows the Mac's own), or universal: add -arch arm64 -arch x86_64 to the cc line.", cd::processArch());
#else
    (void)buildTip;
#endif
}

void codecInstallHelper(const char* id) {
    const std::string codec = id;
    voiceCodecTick(id);
    const auto dirs = codecSearchFolders();
    std::string folder;
    for (const auto& d : dirs) folder += (folder.empty() ? "" : "; ") + d.path;
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
    codecArchLines(true);
    codecFoldersList(codec);
    openCodecFolderButtons();
    ImGui::SameLine();
    if (ImGui::Button("Build steps")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS.md#speech-codec-plug-ins-voice-modes");
    ImGui::SameLine();
    if (ImGui::Button("Rescan for plug-in")) speechCodecRescan(id);
    ImGui::TextDisabled("Upstream mbelib project page: https://github.com/lwvmobile/mbelib. OnAir downloads nothing for you.");
}


// The Codec Setup assistant: one status line per patented voice codec, Open codecs folder / Rescan, and the steps for each OS.
static void codecSetupAssistant() {
    namespace cd = dect2::codec;
    namespace vv = dect2::voice;
    vv::SetupProbe pr;
    pr.fileExists = fileExists;
    pr.archOfFile = [](const std::string& f) {
#if defined(__APPLE__)
        return cd::machoArchOfFile(f);
#else
        (void)f; return std::string();
#endif
    };
    pr.loaded = [](const std::string& id, std::string* path) { return cd::pluginLoaded(id, path); };
    pr.processArch = cd::processArch();
    const auto items = vv::codecSetupStatus(codecSearchFolders(), pr);
    const bool done = vv::setupComplete(items);
    ImGui::SetNextItemOpen(!done, ImGuiCond_Once);
    if (!ImGui::CollapsingHeader(("Codec setup: " + vv::setupSummary(items) + "###codecsetup").c_str())) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("Voice for DMR, P25, NXDN, dPMR, D-STAR, Yaesu Fusion and TETRA is silent until the codec plug-ins are installed. The calls, IDs and data work without them. OnAir does not include, download or build these codecs: a small helper script you run yourself does it, on this computer.");
    ImGui::PopTextWrapPos();
    if (ImGui::BeginTable("##csetup", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Codec"); ImGui::TableSetupColumn("File OnAir looks for"); ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& it : items) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%s", it.name.c_str()); ImGui::TextDisabled("%s", it.modes.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(it.fileName.c_str());
            ImGui::TableNextColumn();
            const bool good = it.state == vv::SetupState::Loaded || it.state == vv::SetupState::Ready;
            ImGui::TextColored(good ? ImVec4(0.5f, 0.9f, 0.6f, 1) : ImVec4(0.95f, 0.75f, 0.3f, 1), "%s", it.state == vv::SetupState::Loaded ? "working" : it.state == vv::SetupState::Ready ? "installed" : it.state == vv::SetupState::WrongArch ? "wrong processor" : "missing");
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 330 * gUi);
            ImGui::TextDisabled("%s", it.detail.c_str());
            if (!it.where.empty()) ImGui::TextDisabled("%s", it.where.c_str());
            for (const auto& ig : it.ignoredCopies) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "also in %s (ignored)", ig.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndTable();
    }
    openCodecFolderButtons();
    ImGui::SameLine();
    if (ImGui::Button("Rescan all")) for (const auto& it : items) speechCodecRescan(it.id.c_str());
    ImGui::SameLine();
    if (ImGui::Button("Copy build command")) ImGui::SetClipboardText(vv::setupBuildCommand(vv::currentSetupOs()).c_str());
    ImGui::SameLine();
    if (ImGui::Button("Open build helper page")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS-BUILD-HELPER.md");
    ImGui::Spacing();
    ImGui::TextUnformatted("How to install (the steps for your computer first):");
    const vv::SetupOs mine = vv::currentSetupOs();
    int oi = 0;
    for (vv::SetupOs os : {mine, vv::SetupOs::MacOS, vv::SetupOs::Windows, vv::SetupOs::Linux}) {
        const bool isMine = oi++ == 0;
        if (!isMine && os == mine) continue;
        const auto steps = vv::setupSteps(os);
        const std::string head = std::string(vv::setupOsName(os)) + (isMine ? " (this computer)" : "") + "##cs" + (isMine ? "m" : "o") + std::to_string((int)os);
        ImGui::SetNextItemOpen(isMine, ImGuiCond_Once);
        if (ImGui::TreeNode(head.c_str())) {
            ImGui::PushTextWrapPos(0);
            for (size_t i = 0; i < steps.size(); ++i) ImGui::Text("%zu. %s", i + 1, steps[i].c_str());
            ImGui::PopTextWrapPos();
            ImGui::TreePop();
        }
    }
    ImGui::Spacing();
}

void voiceCodecsWindow(App& a) {
    if (!a.voiceCodecsOpen) return;
    firstWindowSize(760 * gUi, 460 * gUi);
    if (!ImGui::Begin("Voice codecs", &a.voiceCodecsOpen)) { ImGui::End(); return; }
    for (const char* id : {"ambe2", "ambe", "imbe", "tetrapol"}) voiceCodecTick(id);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted("OnAir never ships a patented voice codec. Patented codecs are loaded at run time from a plug-in (or library) that you install yourself in the codecs folder; open codecs need nothing. Encrypted calls are never decoded. Check the patent and radio rules where you live.");
    ImGui::PopTextWrapPos();
    codecArchLines(false);
    ImGui::Spacing();
    codecSetupAssistant();
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
                const auto hits = cd::dirsWithFile(codecSearchFolders(), cd::speechCodecFileName(id), fileExists);
                const bool onDisk = !hits.empty();
                if (plug) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "plug-in loaded from %s", p.c_str());
                else if (mbe) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "libmbe loaded: %s", p.c_str());
                else if (onDisk && plat::basePrefs().getB(prefKey(id).c_str(), false)) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "plug-in file found, notice already accepted: it loads when a voice mode starts");
                else if (onDisk) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "plug-in file found: accept the notice or start the mode to load it");
                else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "missing: %s", cd::speechCodecFileName(id).c_str());
                if (!plug && onDisk) ImGui::TextDisabled("found in %s", hits[0].path.c_str());
#if defined(__APPLE__)
                if (onDisk) {   // the plug-in file's processor next to OnAir's: an arm64 file cannot load into the Intel OnAir and back
                    const std::string arch = cd::machoArchOfFile(cd::joinPath(hits[0].path, cd::speechCodecFileName(id)));
                    if (!arch.empty()) {
                        const bool fits = arch.find(cd::processArch()) != std::string::npos;
                        if (fits) ImGui::TextDisabled("built for %s", arch.c_str());
                        else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "built for %s, OnAir runs as %s: it cannot load", arch.c_str(), cd::processArch());
                    }
                }
#endif
                if (hits.size() > 1) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "Warning: also in %d other folder(s); %s wins:", (int)hits.size() - 1, hits[0].path.c_str());
                    for (size_t h = 1; h < hits.size(); ++h) ImGui::BulletText("%s (ignored)", hits[h].path.c_str());
                }
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
    {   // the DRA add-on (CDR radio and DTMB TV sound in China): not a speech codec, but the same kind of thing the user installs
        std::string p, prob;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("DRA / DRA+ (CDR radio and DTMB TV sound, patented, add-on only):");
        ImGui::SameLine();
        if (dect2::cdr::draAvailable(&p, &prob)) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "add-on loaded: %s", p.c_str());
        else if (!prob.empty()) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "add-on found but not usable: %s", prob.c_str());
        else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "missing: %s", dect2::cdr::draFileNames().empty() ? "libonair_dra" : dect2::cdr::draFileNames().front().c_str());
        ImGui::SameLine();
        codecHelpButton(CodecHelp::Dra);
    }
    {   // the AVS+ video add-on (DTMB TV in China): an FFmpeg the user builds, not a speech codec, but installed the same way
        const dect2::AvsPlusLibInfo li = dect2::avsPlusLibrary();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("AVS+ video (DTMB TV in China, patented, add-on only):");
        ImGui::SameLine();
        if (li.hasDecoder) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.6f, 1), "add-on loaded: %s", li.path.c_str());
        else if (li.found) ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "found but not usable: %s", li.message.c_str());
        else ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1), "missing: an FFmpeg libavcodec with AVS+");
        ImGui::SameLine();
        codecHelpButton(CodecHelp::AvsPlus);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Folders searched, in this order (the first one that has a plug-in wins)");
    for (const auto& d : codecSearchFolders()) ImGui::BulletText("%s  [%s]", d.path.c_str(), d.source.c_str());
    ImGui::TextDisabled("Also: set ONAIR_CODEC_DIR to a folder before starting OnAir, or type an extra folder below.");
    static char extraBuf[1024];
    static bool extraInit = false;
    if (!extraInit) { extraInit = true; snprintf(extraBuf, sizeof extraBuf, "%s", plat::basePrefs().getS("codec_extra_dir", "").c_str()); }
    ImGui::SetNextItemWidth(420 * gUi);
    ImGui::InputText("Extra folder##codecextra", extraBuf, sizeof extraBuf);
    ImGui::SameLine();
    if (ImGui::Button("Apply##codecextra")) setExtraFolder(extraBuf);
    openCodecFolderButtons();
    ImGui::SameLine();
    if (ImGui::Button("Instructions")) dect2::openUrl("https://github.com/adswill/OnAir/blob/main/docs/CODECS.md");
    ImGui::End();
}

} // namespace appcodec
