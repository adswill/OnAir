#include "session_list.h"
#include "app.h"
#include "dect2/session/privacy.h"
#include "dect2/session/session_store.h"
#include "dect2/session/track_book.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "ui/scale.h"
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace kit {
namespace {

using namespace dect2::sess;

struct St {
    std::unique_ptr<CachedList> list;
    char search[80] = "";
    bool activeOnly = false, starredOnly = false, audioOnly = false;
    int sort = 0;
    uint64_t sel = 0;
    uint64_t detailFor = 0, detailGen = ~0ull;
    std::vector<SessionLink> links;
    Note note;
    char noteBuf[400] = "", tagBuf[100] = "";
    std::string exported;
    bool confirmDelete = false;
};
std::map<std::string, St>& states() { static auto* m = new std::map<std::string, St>; return *m; }

std::string when(int64_t ms) {
    if (ms <= 0) return "-";
    time_t t = (time_t)(ms / 1000);
    struct tm tmv;
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char b[32];
    strftime(b, sizeof b, "%m-%d %H:%M:%S", &tmv);
    return b;
}

std::string dur(uint32_t ms) {
    char b[32];
    const unsigned s = ms / 1000;
    if (s >= 3600) snprintf(b, sizeof b, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(b, sizeof b, "%u:%02u", s / 60, s % 60);
    return b;
}

const char* kindWord(int kind) { return kind == 2 ? "thread" : kind == 3 ? "track" : "call"; }

void exportTo(SessionStore& st, const Query& base, const char* fmt, uint16_t mode, St& s) {
    Query q = base;
    q.offset = 0;
    q.limit = 200000;
    const std::string data = st.exportAs(q, fmt, false);
    const std::string dir = plat::dataDir() + "/exports";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[96];
    snprintf(name, sizeof name, "/sessions-%u-%lld.%s", (unsigned)mode, (long long)wallNowMs(), fmt);
    const std::string path = dir + name;
    s.exported = writeOwnerOnlyFile(path, data) ? path : std::string();
    if (s.exported.empty()) s.exported = "(could not write the file)";
}

}  // namespace

const char* sessionTabName(int kind) { return kind == 2 ? "Threads" : kind == 3 ? "Tracks" : "Calls"; }

void sessionItems(uint16_t mode, int kind, const char* idc) {
    SessionStore& st = global();
    St& s = states()[std::string(idc) + std::to_string(mode)];
    if (!s.list) s.list.reset(new CachedList(st));
    const float u = gUi;
    ImGui::PushID(idc);

    // ---- header: the Logging state, counts, drops
    const bool on = st.logging(mode);
    ImGui::Text("%s: %s", tr("Logging"), on ? tr("ON") : tr("OFF"));
    ImGui::SameLine();
    bool onv = on;
    if (ImGui::Checkbox(tr("Keep history"), &onv)) st.setLogging(mode, onv);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Saves who, when and how long (never audio, never message text) so the list survives a restart. Turn off to keep only the current run."));
    const StoreStats ss = st.stats();
    if (ss.dropped) { ImGui::SameLine(); ImGui::TextColored(ImVec4(0.9f, 0.6f, 0.2f, 1), "%s: %llu", tr("dropped"), (unsigned long long)ss.dropped); }
    if (st.firstRunNoticeNeeded()) {
        ImGui::TextWrapped("%s", tr("OnAir keeps a log of what each mode hears (times, ids, quality) in its data folder, only on this computer. Pager ids are stored as short tags, message text is not stored, and audio is not recorded unless you turn it on. You can switch it off per mode or delete it in the Items tab."));
        if (ImGui::Button(tr("Got it"))) st.acknowledgeFirstRunNotice();
    }

    // ---- search and filters
    ImGui::SetNextItemWidth(std::max(120.f * u, ImGui::GetContentRegionAvail().x * 0.3f));
    ImGui::InputTextWithHint("##q", tr("Search"), s.search, sizeof s.search);
    ImGui::SameLine(); ImGui::Checkbox(tr("Active"), &s.activeOnly);
    ImGui::SameLine(); ImGui::Checkbox(tr("Kept"), &s.starredOnly);
    ImGui::SameLine(); ImGui::Checkbox(tr("With audio"), &s.audioOnly);
    static const char* const sorts[] = {"Active, newest", "Newest", "Oldest", "Longest", "Title"};
    ImGui::SameLine(); ImGui::SetNextItemWidth(130 * u);
    ImGui::Combo("##sort", &s.sort, sorts, 5);

    Query q;
    q.mode = mode;
    q.text = s.search;
    q.activeOnly = s.activeOnly;
    q.starredOnly = s.starredOnly;
    q.withAudio = s.audioOnly;
    q.sort = (Sort)s.sort;
    q.limit = 500;
    s.list->setQuery(q);
    std::shared_ptr<const Page> page = s.list->page();

    ImGui::SameLine();
    if (ImGui::Button(tr("Export"))) ImGui::OpenPopup("##exp");
    if (ImGui::BeginPopup("##exp")) {
        if (ImGui::MenuItem("CSV")) exportTo(st, q, "csv", mode, s);
        if (ImGui::MenuItem("JSON")) exportTo(st, q, "json", mode, s);
        if (ImGui::MenuItem("JSONL")) exportTo(st, q, "jsonl", mode, s);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("...")) ImGui::OpenPopup("##more");
    if (ImGui::BeginPopup("##more")) {
        if (ImGui::MenuItem(tr("Delete all history of this mode..."))) s.confirmDelete = true;
        ImGui::EndPopup();
    }
    if (s.confirmDelete) {
        ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.3f, 1), "%s", tr("Delete every saved entry of this mode? This cannot be undone."));
        ImGui::SameLine(); if (ImGui::Button(tr("Delete"))) { st.deleteMode(mode); s.confirmDelete = false; s.sel = 0; }
        ImGui::SameLine(); if (ImGui::Button(tr("Cancel"))) s.confirmDelete = false;
    }
    if (!s.exported.empty()) {
        ImGui::TextDisabled("%s %s", tr("Saved:"), s.exported.c_str());
    }
    ImGui::TextDisabled("%lld %s%s", (long long)page->total, tr(kindWord(kind)), page->total == 1 ? "" : "s");

    // ---- the list (left) and the detail (right, or below in a narrow window)
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const bool side = W >= 720 * u;
    const float listW = (s.sel && side) ? W * 0.62f : W;
    ImGui::BeginChild("##list", ImVec2(listW, (s.sel && !side) ? H * 0.55f : H), false);
    if (page->rows.empty()) {
        ImGui::Dummy(ImVec2(0, 10 * u));
        ImGui::TextWrapped("%s", tr("Nothing here yet. Entries appear while this mode hears something, and are kept between runs while Logging is ON."));
    } else if (ImGui::BeginTable("##t", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Start"), ImGuiTableColumnFlags_WidthFixed, 110 * u);
        ImGui::TableSetupColumn(tr("Name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("Length"), ImGuiTableColumnFlags_WidthFixed, 60 * u);
        ImGui::TableSetupColumn(tr("Events"), ImGuiTableColumnFlags_WidthFixed, 56 * u);
        ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_WidthFixed, 74 * u);
        ImGui::TableHeadersRow();
        ImGuiListClipper clip;
        clip.Begin((int)page->rows.size());
        while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
            const Session& r = page->rows[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushID((int)(r.id & 0x7FFFFFFF) ^ i);
            if (ImGui::Selectable(when(r.startWall).c_str(), s.sel == r.id, ImGuiSelectableFlags_SpanAllColumns)) s.sel = (s.sel == r.id) ? 0 : r.id;
            ImGui::PopID();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.title.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.active() ? "..." : dur(r.durationMs).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", r.eventCount);
            ImGui::TableNextColumn();
            if (r.active()) ImGui::TextColored(ImVec4(0.45f, 0.8f, 0.5f, 1), "%s", tr("live"));
            else if (r.verdict == (uint8_t)Verdict::Encrypted) ImGui::TextDisabled("%s", tr("encrypted"));
            else if (r.flags & FlagStarred) ImGui::TextUnformatted(tr("kept"));
            else ImGui::TextUnformatted("");
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // ---- detail
    const Session* cur = nullptr;
    for (const Session& r : page->rows) if (r.id == s.sel) { cur = &r; break; }
    if (cur) {
        if (side) ImGui::SameLine();
        ImGui::BeginChild("##detail", ImVec2(0, 0), true);
        if (s.detailFor != cur->id || s.detailGen != page->gen) {
            s.links = st.links(cur->id);
            s.note = Note();
            st.note(cur->id, s.note);
            if (s.detailFor != cur->id) {
                snprintf(s.noteBuf, sizeof s.noteBuf, "%s", s.note.note.c_str());
                snprintf(s.tagBuf, sizeof s.tagBuf, "%s", s.note.tags.c_str());
            }
            s.detailFor = cur->id;
            s.detailGen = page->gen;
        }
        ImGui::TextWrapped("%s", cur->title.c_str());
        ImGui::TextDisabled("%s  -  %s", when(cur->startWall).c_str(), cur->active() ? tr("live") : dur(cur->durationMs).c_str());
        bool kept = (cur->flags & FlagStarred) != 0;
        if (ImGui::Checkbox(tr("Keep (never delete)"), &kept)) st.setStarred(cur->id, kept);
        if (!cur->who[0].empty()) ImGui::Text("%s: %s", tr("From"), cur->who[0].c_str());
        if (!cur->who[1].empty()) ImGui::Text("%s: %s", tr("To"), cur->who[1].c_str());
        if (!cur->alias[0].empty()) ImGui::Text("%s: %s", tr("Name"), cur->alias[0].c_str());
        if (cur->freqHz) ImGui::Text("%s: %.4f MHz", tr("Frequency"), cur->freqHz / 1e6);
        if (cur->hasPosition()) ImGui::Text("%s: %.5f, %.5f%s", tr("Position"), cur->lat, cur->lon, std::isfinite(cur->altM) ? (std::string("  ") + std::to_string((int)cur->altM) + " m").c_str() : "");
        if (cur->snrDb != 0) ImGui::Text("SNR %.1f dB", cur->snrDb);
        if (cur->errRate >= 0) ImGui::Text("%s %.1f %%", tr("Errors"), cur->errRate * 100.f);
        ImGui::Text("%s: %u", tr("Events"), cur->eventCount);
        if (cur->flags & FlagEmergency) ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1), "%s", tr("Emergency"));
        for (const SessionLink& l : s.links) ImGui::TextDisabled("%s: %s", linkTypeName((LinkType)l.type), l.ref.c_str());
        ImGui::Separator();
        ImGui::SetNextItemWidth(-1);
        const bool e1 = ImGui::InputTextWithHint("##note", tr("Note"), s.noteBuf, sizeof s.noteBuf);
        ImGui::SetNextItemWidth(-1);
        const bool e2 = ImGui::InputTextWithHint("##tags", tr("Tags"), s.tagBuf, sizeof s.tagBuf);
        if (e1 || e2) st.setNote(cur->id, s.noteBuf, s.tagBuf);
        ImGui::EndChild();
    }
    ImGui::PopID();
}

}  // namespace kit
