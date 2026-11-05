// The recording library: the recordings in the recordings folder, with what their sidecars say (length, rate, centre frequency and the mode
// they were made in) and two actions per row. Open makes the recording the IQ file source; Re-analyse does the same and then opens the
// chosen mode (by default the one the recording was made in). Opened from the "Library" button in the diagnostics row. Lists only the
// sidecars and file headers, never the samples; deletes and moves nothing.
#include "../app.h"
#include "../i18n/i18n.h"
#include "dect2/engine/updater.h"
#include "dect2/source/iq_file.h"
#include "dect2/source/source.h"
#include "../state/rec_tags.h"
#include <cstdio>
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace {

struct LibState {
    bool open = false;
    std::string dir;
    std::vector<RecordingListing> rows;   // read from the folder when the window opens and on Refresh
    rectags::Store tags;                  // tags and notes by file name (the folder's .onair-tags.tsv)
    char search[128] = "";                // the filter: words of the file name, a tag or the note, "#tag" for an exact tag
    std::string editFile;                 // the recording whose tags and note are being edited
    char editTags[256] = "", editNote[1024] = "";
    bool openEdit = false;
    std::map<std::string, int> pick;      // per recording (by path): the mode index for Re-analyse; -1 = the mode in use
};
LibState gLib;

void reload(const std::string& dir) {
    gLib.dir = dir;
    gLib.rows = listRecordingFiles(dir);
    rectags::load(dir, gLib.tags);
}

// the mode a recording names in its sidecar, as an index into kModes; -1 when there is none or this build has no such mode
int defaultPick(const RecordingListing& r) { return r.mode.empty() ? -1 : scanModeIndex(r.mode); }

int pickFor(const RecordingListing& r) {
    auto it = gLib.pick.find(r.path);
    return it != gLib.pick.end() ? it->second : defaultPick(r);
}

std::string dateText(int64_t t) {
    if (t <= 0) return "";
    const std::time_t tt = (std::time_t)t;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

std::string lengthText(double s) {
    if (s <= 0) return "";
    const int t = (int)(s + 0.5);
    char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

// The recording becomes the IQ file source, with the same settings the source's Open button sets. False (and the reason in the log) when
// the receiver runs or the file cannot be read.
bool openAsSource(App& a, const RecordingListing& r) {
    if (a.engine.running()) { a.engine.log("stop the receiver before opening a recording"); return false; }
    int fileIdx = -1;
    for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].kind == DeviceInfo::File) { fileIdx = i; break; }
    if (fileIdx < 0) { a.engine.log("no IQ file source in this build"); return false; }
    IqFileInfo info; std::string err;
    a.file.offsetBytes = 0; a.file.endBytes = 0; a.file.bigEndian = false; a.file.centreHz = 0;
    if (probeIqFile(r.path, info, err)) {   // a SigMF or WAV: its own rate, centre, sample format and start
        a.file.path = info.dataPath; a.file.format = info.format; a.file.bigEndian = info.bigEndian;
        a.file.offsetBytes = info.offsetBytes; a.file.endBytes = info.endBytes;
        if (info.sampleRate > 0) a.file.sampleRate = info.sampleRate;
        restoreRecordingMode(a, info.description, r.path);
        if (info.centerHz > 0) a.freqMhz = info.centerHz / 1e6;
    } else if (!err.empty()) { a.engine.log(err); return false; }
    else {   // a raw file without a sidecar: the rate and the format its name says
        a.file.path = r.path; a.file.format = guessFormat(r.path);
        const double g = guessSampleRate(r.path);
        if (g > 0) a.file.sampleRate = g;
    }
    a.devIdx = fileIdx;
    a.engine.log("IQ file: " + r.fileName);
    return true;
}

void reanalyse(App& a, const RecordingListing& r, int pick) {
    if (!openAsSource(a, r)) return;
    if (pick >= 0 && pick < kNumModes) {
        selectMode(a, kModes[pick].family, kModes[pick].subId);
        a.engine.log(std::string("recording set to re-analyse in ") + kModes[pick].name + ": press Start");
    } else a.engine.log("recording set to re-analyse in the mode in use: press Start");
}

} // namespace

// A recording by path as the IQ file source (the recordings manager's Open). A sample range (first < end) plays only that part: the file source
// starts at the first sample and stops at the end one.
bool recordingOpenPath(App& a, const std::string& path, uint64_t firstSample, uint64_t endSample) {
    RecordingListing r;
    r.path = path;
    const size_t sl = path.find_last_of("/\\");
    r.fileName = sl == std::string::npos ? path : path.substr(sl + 1);
    if (!openAsSource(a, r)) return false;
    if (endSample > firstSample) {
        const uint64_t bps = a.file.format == FileFormat::CF32 ? 8 : (a.file.format == FileFormat::CS8 || a.file.format == FileFormat::CU8) ? 2 : 4;
        const uint64_t base = a.file.offsetBytes;
        a.file.offsetBytes = base + firstSample * bps;
        a.file.endBytes = base + endSample * bps;
    }
    return true;
}

void recordingLibraryDevOpen() { gLib.open = true; reload(recordingsDir()); }

// The button in the diagnostics row, and its window while it is open.
void recordingLibraryUi(App& a) {
    const std::string dir = recordingsDir();
    if (ImGui::SmallButton("Library")) {
        gLib.open = !gLib.open;
        if (gLib.open) reload(dir);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "The recordings in the recordings folder: open one as the source, or re-analyse it in a mode");
    if (!gLib.open) return;
    firstWindowSize(900 * gUi, 440 * gUi);
    if (!ImGui::Begin("Recording library###reclib", &gLib.open)) { ImGui::End(); return; }
    if (ImGui::Button("Refresh")) reload(dir);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", dir.c_str());
    ImGui::SetNextItemWidth(340 * gUi);
    ImGui::InputTextWithHint("##libsearch", tr("Search name, tag or note (#tag = exact tag)"), gLib.search, sizeof gLib.search);
    if (ImGui::CollapsingHeader(trw("Advanced##libadv"))) {   // the tags in use as one-click filters
        const auto counts = rectags::tagCounts(gLib.tags);
        if (counts.empty()) ImGui::TextDisabled("%s", tr("No tags yet: use Edit on a recording to add tags and a note."));
        for (const auto& tc : counts) {
            char b[96]; snprintf(b, sizeof b, "#%s (%d)##tagchip", tc.first.c_str(), tc.second);
            if (ImGui::SmallButton(b)) snprintf(gLib.search, sizeof gLib.search, "#%s", tc.first.c_str());
            ImGui::SameLine();
        }
        ImGui::NewLine();
        if (ImGui::SmallButton(tr("Clear filter"))) gLib.search[0] = 0;
    }
    std::vector<size_t> shown;
    for (size_t i = 0; i < gLib.rows.size(); i++) {
        auto it = gLib.tags.find(gLib.rows[i].fileName);
        if (rectags::matches(gLib.rows[i].fileName, it == gLib.tags.end() ? nullptr : &it->second, gLib.search)) shown.push_back(i);
    }
    if (gLib.rows.empty()) {
        ImGui::TextDisabled("%s", "No recordings in this folder yet. Record IQ from the diagnostics row, or put a SigMF or WAV file here.");
    } else if (ImGui::BeginTable("##reclib", 11, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY)) {
        const auto fx = ImGuiTableColumnFlags_WidthFixed;
        ImGui::TableSetupColumn("File", fx, 210 * gUi);
        ImGui::TableSetupColumn("Date", fx, 120 * gUi);
        ImGui::TableSetupColumn("Length", fx, 55 * gUi);
        ImGui::TableSetupColumn("Rate", fx, 80 * gUi);
        ImGui::TableSetupColumn("Centre", fx, 90 * gUi);
        ImGui::TableSetupColumn("Recorded in", fx, 90 * gUi);
        ImGui::TableSetupColumn(tr("Tags"), fx, 130 * gUi);
        ImGui::TableSetupColumn(tr("Note"), fx, 200 * gUi);
        ImGui::TableSetupColumn("Re-analyse in", fx, 170 * gUi);
        ImGui::TableSetupColumn("", fx, 50 * gUi);
        ImGui::TableSetupColumn("", fx, 150 * gUi);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        for (size_t i : shown) {
            const RecordingListing& r = gLib.rows[i];
            ImGui::TableNextRow();
            ImGui::PushID((int)i);
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(r.fileName.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", r.path.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(dateText(r.modified).c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(lengthText(r.seconds).c_str());
            ImGui::TableSetColumnIndex(3);
            if (r.sampleRate > 0) { char b[32]; snprintf(b, sizeof b, "%.3f Msps", r.sampleRate / 1e6); ImGui::TextUnformatted(b); }
            ImGui::TableSetColumnIndex(4);
            if (r.centerHz > 0) { char b[32]; snprintf(b, sizeof b, "%.3f MHz", r.centerHz / 1e6); ImGui::TextUnformatted(b); }
            ImGui::TableSetColumnIndex(5);
            ImGui::TextUnformatted(r.mode.c_str());
            ImGui::TableSetColumnIndex(6);
            {
                auto tg = gLib.tags.find(r.fileName);
                const rectags::Entry* te = tg == gLib.tags.end() ? nullptr : &tg->second;
                ImGui::TextUnformatted(te ? rectags::joinTags(te->tags).c_str() : "");
                ImGui::TableSetColumnIndex(7);
                ImGui::TextUnformatted(te ? te->note.c_str() : "");
                if (te && !te->note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", te->note.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Edit##rectag"))) {
                    gLib.editFile = r.fileName; gLib.openEdit = true;
                    snprintf(gLib.editTags, sizeof gLib.editTags, "%s", te ? rectags::joinTags(te->tags).c_str() : "");
                    snprintf(gLib.editNote, sizeof gLib.editNote, "%s", te ? te->note.c_str() : "");
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tags and a note for this recording"));
            }
            ImGui::TableSetColumnIndex(8);
            const int pick = pickFor(r);
            ImGui::SetNextItemWidth(160 * gUi);
            const std::string label = pick >= 0 && pick < kNumModes ? kModes[pick].name : "(mode in use)";
            if (ImGui::BeginCombo("##pick", label.c_str())) {
                if (ImGui::Selectable("(mode in use)", pick < 0)) gLib.pick[r.path] = -1;
                for (int m = 0; m < kNumModes; m++) if (ImGui::Selectable(kModes[m].name, m == pick)) gLib.pick[r.path] = m;
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(9);
            if (ImGui::SmallButton("Open")) openAsSource(a, r);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Makes this recording the IQ file source (press Start to play it)");
            ImGui::TableSetColumnIndex(10);
            if (ImGui::SmallButton("Re-analyse")) reanalyse(a, r, pickFor(r));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", "Opens this recording as the source in the chosen mode (press Start to analyse it)");
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Share##rec"))) {   // the folder opens with the file, and the path of the file to send goes to the clipboard (a SigMF recording: the .sigmf-meta and its data go together)
                ImGui::SetClipboardText(r.path.c_str());
                dect2::openUrl(r.path.substr(0, r.path.find_last_of("/\\")));
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Opens the folder with this recording and copies its path. Send the file (for SigMF the .sigmf-meta and the .sigmf-data together): it replays with its own frequency, rate and mode."));
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (gLib.openEdit) { ImGui::OpenPopup("###rectagedit"); gLib.openEdit = false; }
    if (ImGui::BeginPopupModal("###rectagedit", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("%s", gLib.editFile.c_str());
        ImGui::SetNextItemWidth(380 * gUi);
        ImGui::InputTextWithHint("##edtags", tr("Tags, separated by commas"), gLib.editTags, sizeof gLib.editTags);
        ImGui::InputTextMultiline("##ednote", gLib.editNote, sizeof gLib.editNote, ImVec2(380 * gUi, 80 * gUi));
        if (ImGui::Button(tr("Save##rectag"))) {
            rectags::set(gLib.tags, gLib.editFile, gLib.editTags, gLib.editNote);
            if (!rectags::save(dir, gLib.tags)) a.engine.log("could not write the tags file in the recordings folder");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##rectag"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}
