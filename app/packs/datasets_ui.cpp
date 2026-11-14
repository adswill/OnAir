// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The Datasets window (Tools > Datasets): every reference dataset OnAir uses, with source, licence, version, size, status and the actions
// import (file or folder, validated with a preview first), export, reset to bundled, and an opt-in update check.
// Nothing is fetched by itself: "Check for updates" and "Install" are clicks, only from sources the user listed (shown right above the button),
// never in offline mode, and every download is checked against the sha256 and size of the signed-off manifest entry. Logic: core/src/packs/datasets.cpp.
#include "../app.h"
#include "../ui/ui_common.h"
#include "dect2/common/u8file.h"
#include "dect2/packs/datasets.h"
#include "dect2/packs/packs.h"
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <ctime>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include "dect2/common/diag.h"

using namespace dect2::datasets;

namespace {

constexpr const char* kExampleSource = "https://datasets.example.invalid/onair";

struct Net {   // the update check, run on a worker thread
    std::mutex mu;
    std::future<void> job;
    std::atomic<bool> cancel{false};
    std::atomic<bool> running{false};
    std::string status;
    bool statusBad = false;
    std::vector<UpdateOffer> offers;
    std::vector<std::string> rejected;
    std::vector<std::pair<std::string, std::string>> done;   // id, message of an installed update
} net;

struct U {
    bool loaded = false;
    std::vector<Descriptor> inv;
    std::string filter, note, selected, confirmReset;
    bool noteBad = false;
    // import preview
    std::string pendId, pendPath, pendText;
    bool pendFolder = false;
    Validation pendVal;
    char pathBuf[512] = {};
    // update feed
    bool updatesOn = false, offline = false;
    std::vector<std::string> sources;
    char srcBuf[300] = {};
    std::map<std::string, std::string> newer;   // id -> offered version
} u;

std::string sizeText(unsigned long long b) {
    char s[32];
    if (b >= (1u << 20)) snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
    else if (b >= 1024) snprintf(s, sizeof s, "%.0f KB", b / 1024.0);
    else snprintf(s, sizeof s, "%llu B", b);
    return s;
}

std::string dateText(long long t) {
    if (t <= 0) return "-";
    time_t tt = (time_t)t;
    tm m{};
#ifdef _WIN32
    localtime_s(&m, &tt);
#else
    localtime_r(&tt, &m);
#endif
    char s[32];
    strftime(s, sizeof s, "%Y-%m-%d", &m);
    return s;
}

void saveSources() {
    std::string t;
    for (const std::string& s : u.sources) t += s + "\n";
    plat::prefs().setS("datasetSources", t);
    plat::prefs().flush();
}

void load() {
    if (u.loaded) return;
    u.loaded = true;
    setUserDir(plat::dataDir() + "/datasets");
    u.inv = inventory();
    u.updatesOn = plat::prefs().getB("datasetUpdatesOn", false);
    u.offline = plat::prefs().getB("offlineMode", false);
    std::istringstream ss(plat::prefs().getS("datasetSources", ""));
    for (std::string l; std::getline(ss, l);) if (validAllowListEntry(l)) u.sources.push_back(l);
}

const Descriptor* find(const std::string& id) { for (const Descriptor& d : u.inv) if (d.id == id) return &d; return nullptr; }

const char* statusText(Status s) {
    switch (s) {
        case Status::Bundled: return tr("bundled");
        case Status::UserInstalled: return tr("user-installed");
        case Status::Outdated: return tr("outdated");
        case Status::Download: return tr("download pack");
    }
    return "";
}

// ---- the update check (worker thread)
bool readFile(const std::string& p, std::string& o, size_t max) {
    std::ifstream f(dect2::u8file(p), std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    o = ss.str();
    return o.size() <= max;
}

void setStatus(const std::string& s, bool bad) { std::lock_guard<std::mutex> l(net.mu); net.status = s; net.statusBad = bad; }

void checkWorker(std::vector<std::string> sources, std::map<std::string, std::string> installed) {
    const auto fetch = dect2::packs::curlFetcher();
    std::vector<UpdateOffer> all;
    std::vector<std::string> rej;
    std::string problems;
    for (const std::string& src : sources) {
        if (net.cancel) break;
        std::string base = src;
        while (base.size() > 8 && base.back() == '/') base.pop_back();
        const std::string url = base + "/manifest.json", tmp = plat::cacheDir() + "/datasets_manifest.json";
        setStatus(std::string(tr("Fetching ")) + url, false);
        const int rc = fetch(url, tmp, 4u << 20, net.cancel, [] {});
        std::string body, err;
        Manifest m;
        if (rc != 0) { problems += url + ": " + (rc == -1 ? "cancelled" : "could not be fetched") + "\n"; continue; }
        if (!readFile(tmp, body, 4u << 20) || !parseManifest(body, m, err)) { problems += url + ": " + (err.empty() ? "unreadable" : err) + "\n"; continue; }
        for (UpdateOffer& o : selectUpdates(m, installed, {src}, &rej)) {
            const Descriptor* d = find(o.entry.id);
            if (d && d->replaceable) all.push_back(std::move(o));   // an id OnAir does not know is ignored
        }
    }
    std::lock_guard<std::mutex> l(net.mu);
    net.offers = std::move(all);
    net.rejected = std::move(rej);
    net.status = problems.empty() ? (net.offers.empty() ? std::string(tr("Everything is up to date.")) : std::string(tr("Updates available.")))
                                  : problems;
    net.statusBad = !problems.empty();
    net.running = false;
}

void installWorker(UpdateOffer o, std::vector<std::string> sources) {
    const auto fetch = dect2::packs::curlFetcher();
    std::string err, msg;
    bool ok = false;
    do {
        const Descriptor* d = find(o.entry.id);
        if (!d || !d->replaceable) { DIAG_DEBUG(763022, "A dataset download was refused because dataset %s is not one that can be replaced.", o.entry.id.c_str()); err = "unknown dataset"; break; }
        if (!urlAllowed(o.entry.url, sources)) { DIAG(852662, "The dataset address %s is not in your list of sources, so it was not downloaded.", o.entry.url.c_str()); err = "the address is not in your list of sources"; break; }
        const std::string tmp = plat::cacheDir() + "/dataset_" + o.entry.id + ".part";
        const int rc = fetch(o.entry.url, tmp, o.entry.size + 1, net.cancel, [] {});
        if (rc != 0) { DIAG(159662, "Downloading the dataset %s failed (code %d), so nothing was installed.", o.entry.url.c_str(), rc); err = rc == -1 ? "cancelled" : "download failed"; break; }
        std::string body;
        if (!readFile(tmp, body, 64u << 20)) { DIAG(573759, "The downloaded dataset file %s could not be read back, so nothing was installed.", tmp.c_str()); err = "unreadable download"; break; }
        std::remove(tmp.c_str());
        if (!verifyDownload(o.entry, body, err)) { DIAG(142327, "The downloaded dataset %s failed its checksum check (%s), so nothing was installed.", o.entry.id.c_str(), err.c_str()); err = "checksum: " + err + " (nothing was installed)"; break; }
        Validation v;
        if (!importText(*d, body, o.entry.url, o.entry.version, v, err)) break;
        ok = true;
        msg = o.entry.id + " " + o.entry.version + (v.issues.empty() ? "" : " (with warnings)");
    } while (false);
    std::lock_guard<std::mutex> l(net.mu);
    if (ok) net.done.push_back({o.entry.id, msg}); else { net.status = o.entry.id + ": " + err; net.statusBad = true; }
    net.running = false;
}

void reap() {
    std::lock_guard<std::mutex> l(net.mu);
    for (auto& d : net.done) {
        u.note = std::string(tr("Installed update: ")) + d.second;
        u.noteBad = false;
        u.newer.erase(d.first);
        for (size_t i = 0; i < net.offers.size(); i++) if (net.offers[i].entry.id == d.first) { net.offers.erase(net.offers.begin() + (long)i); break; }
    }
    net.done.clear();
}

// ---- the import preview
void beginImport(const Descriptor& d, const std::string& path, bool folder) {
    u.pendId = d.id; u.pendPath = path; u.pendFolder = folder; u.pendVal = Validation(); u.note.clear();
    std::string text;
    if (folder) {   // the same reading importFolder does, for the preview only
        Validation v;
        std::string err;
        // validate by a dry run into a scratch id is not possible; read the files here
        std::error_code ec;
        std::vector<std::filesystem::path> files;
        for (const auto& e : std::filesystem::directory_iterator(dect2::u8file(path), ec)) {
            std::string x = e.path().extension().string();
            for (char& c : x) c = (char)tolower((unsigned char)c);
            if (e.is_regular_file() && (x == ".tsv" || x == ".csv" || x == ".txt" || x == ".json")) files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& p : files) { std::string t; if (readFile(p.string(), t, 64u << 20)) { text += t; if (text.empty() || text.back() != '\n') text += '\n'; } }
        if (files.empty()) { u.note = tr("No .tsv, .csv, .txt or .json file in that folder."); u.noteBad = true; u.pendId.clear(); return; }
    } else if (!readFile(path, text, 64u << 20)) { u.note = tr("Cannot read that file (or it is larger than 64 MB)."); u.noteBad = true; u.pendId.clear(); return; }
    u.pendText = text;
    u.pendVal = validateText(d.schema, text);
}

void previewPanel() {
    const Descriptor* d = find(u.pendId);
    if (!d) return;
    ImGui::Separator();
    ImGui::Text(tr("Import into: %s"), d->name.c_str());
    ImGui::TextDisabled("%s", u.pendPath.c_str());
    const Validation& v = u.pendVal;
    ImGui::Text(tr("%zu record(s), %zu error(s), %zu warning(s)"), v.records, v.errors(), v.issues.size() - v.errors());
    if (!v.preview.empty() && ImGui::TreeNodeEx(trw("Preview"), ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const std::string& l : v.preview) ImGui::TextDisabled("%s", l.c_str());
        ImGui::TreePop();
    }
    size_t shown = 0;
    for (const Issue& i : v.issues) {
        if (shown++ >= 30) { ImGui::TextDisabled(tr("... %zu more"), v.issues.size() - 30); break; }
        const std::string at = i.line > 0 ? std::string(tr("line ")) + std::to_string(i.line) + ": " : std::string();
        ImGui::TextColored(i.error ? pal::badRed() : pal::okGreen(), "%s%s", at.c_str(), i.msg.c_str());
    }
    ImGui::BeginDisabled(!v.ok());
    if (ImGui::Button(tr("Import"))) {
        Validation done;
        std::string err;
        const bool ok = importText(*d, u.pendText, u.pendPath, "", done, err);
        u.note = ok ? std::string(tr("Imported: ")) + d->name : err;
        u.noteBad = !ok;
        u.pendId.clear(); u.pendText.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"))) { u.pendId.clear(); u.pendText.clear(); }
    if (!v.ok()) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("A file with errors is never imported. Fix the lines above and try again.")); }
}

void table(float h) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable("##datasets", 7, tf, ImVec2(0, h))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(tr("Dataset"), ImGuiTableColumnFlags_WidthStretch, 3.f);
    ImGui::TableSetupColumn(tr("Source"), ImGuiTableColumnFlags_WidthStretch, 2.5f);
    ImGui::TableSetupColumn(tr("Licence"), ImGuiTableColumnFlags_WidthStretch, 2.f);
    ImGui::TableSetupColumn(tr("Version"), ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn(tr("Size"), ImGuiTableColumnFlags_WidthFixed, 130 * gUi);
    ImGui::TableSetupColumn(tr("Status / updated"), ImGuiTableColumnFlags_WidthFixed, 130 * gUi);
    ImGui::TableSetupColumn(tr("Actions"), ImGuiTableColumnFlags_WidthFixed, 330 * gUi);
    ImGui::TableHeadersRow();
    for (const Descriptor& d : u.inv) {
        if (!u.filter.empty()) {
            std::string hay = d.name + " " + d.source + " " + d.licence + " " + d.id, f = u.filter;
            for (char& c : hay) c = (char)tolower((unsigned char)c);
            for (char& c : f) c = (char)tolower((unsigned char)c);
            if (hay.find(f) == std::string::npos) continue;
        }
        ImGui::PushID(d.id.c_str());
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", d.name.c_str());
        if (!d.note.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", d.note.c_str());
        ImGui::TableNextColumn(); ImGui::TextWrapped("%s", d.source.c_str());
        ImGui::TableNextColumn();
        ImGui::TextWrapped("%s", d.licence.c_str());
        if (!d.attribution.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s: %s", tr("Credit"), d.attribution.c_str());
        UserMeta um;
        const bool mine = hasUserCopy(d.id) && readMeta(d.id, um);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(mine && !um.version.empty() ? um.version.c_str() : d.version.c_str());
        ImGui::TableNextColumn();
        const unsigned long long bundledBytes = d.bundledText ? d.bundledText().size() : 0;
        if (bundledBytes || mine) {
            std::string s = sizeText(bundledBytes);
            if (mine) s += " + " + sizeText(userCopyBytes(d.id));
            ImGui::TextUnformatted(s.c_str());
        } else ImGui::TextDisabled("-");
        ImGui::TableNextColumn();
        const Status st = statusOf(d, u.newer);
        ImGui::Text("%s", statusText(st));
        if (mine) ImGui::TextDisabled("%s", dateText(um.when).c_str());
        ImGui::TableNextColumn();
        if (d.downloadable) {
            ImGui::TextDisabled("%s", tr("View > Data packs"));
        } else {
            ImGui::BeginDisabled(!d.replaceable);
            if (ImGui::SmallButton(tr("Import file..."))) { const std::string p = plat::openFileDialog(); if (!p.empty()) beginImport(d, p, false); }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Folder..."))) { const std::string p = plat::openFolderDialog(); if (!p.empty()) beginImport(d, p, true); }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!d.bundledText);
            if (ImGui::SmallButton(tr("Export..."))) {
                const std::string p = plat::saveFileDialog((d.id + ".txt").c_str());
                std::string err;
                if (!p.empty()) { const bool ok = exportEffective(d, p, err); u.note = ok ? std::string(tr("Exported to ")) + p : err; u.noteBad = !ok; }
            }
            ImGui::EndDisabled();
            if (mine) {
                ImGui::SameLine();
                if (u.confirmReset == d.id) {
                    if (ImGui::SmallButton(tr("Really reset"))) { std::string e; resetToBundled(d.id, e); u.confirmReset.clear(); u.note = std::string(tr("Back to the bundled data: ")) + d.name; u.noteBad = false; }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Keep"))) u.confirmReset.clear();
                } else if (ImGui::SmallButton(tr("Reset to bundled"))) u.confirmReset = d.id;
            }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void updatesPanel() {
    if (!ImGui::CollapsingHeader(trw("Updates (off by default)"))) return;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("OnAir never fetches datasets by itself. With this switched on, the buttons below fetch a small manifest (manifest.json) from the sources you list, and only from them. Each file is checked against the sha256 and size in the manifest before it is installed. The data you imported yourself is never replaced without your click."));
    ImGui::PopTextWrapPos();
    if (ImGui::Checkbox(tr("Allow update checks (my click only)"), &u.updatesOn)) { plat::prefs().setB("datasetUpdatesOn", u.updatesOn); plat::prefs().flush(); }
    ImGui::SameLine(0, 16 * gUi);
    if (ImGui::Checkbox(tr("Offline mode"), &u.offline)) { plat::prefs().setB("offlineMode", u.offline); plat::prefs().flush(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("No network access from this window while on"));
    ImGui::Text("%s", tr("Sources (a manifest is read from <source>/manifest.json):"));
    if (u.sources.empty()) ImGui::TextDisabled("%s  (%s)", tr("none"), kExampleSource);
    for (size_t i = 0; i < u.sources.size(); i++) {
        ImGui::PushID((int)i);
        ImGui::BulletText("%s", u.sources[i].c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Remove"))) { u.sources.erase(u.sources.begin() + (long)i); saveSources(); ImGui::PopID(); break; }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(360 * gUi);
    ImGui::InputTextWithHint("##src", kExampleSource, u.srcBuf, sizeof u.srcBuf);
    ImGui::SameLine();
    const std::string cand = u.srcBuf;
    ImGui::BeginDisabled(!validAllowListEntry(cand));
    if (ImGui::Button(tr("Add source"))) {
        if (std::find(u.sources.begin(), u.sources.end(), cand) == u.sources.end()) u.sources.push_back(cand);
        saveSources();
        u.srcBuf[0] = 0;
    }
    ImGui::EndDisabled();
    if (!cand.empty() && !validAllowListEntry(cand)) ImGui::TextColored(pal::badRed(), "%s", tr("A source is https://host[/path], with no user name, query or spaces."));
    const bool busy = net.running;
    ImGui::BeginDisabled(!u.updatesOn || u.offline || u.sources.empty() || busy);
    if (ImGui::Button(tr("Check for updates"))) {
        net.cancel = false; net.running = true; net.offers.clear(); net.rejected.clear();
        std::map<std::string, std::string> inst;
        for (const Descriptor& d : u.inv) if (d.replaceable) { UserMeta m; inst[d.id] = hasUserCopy(d.id) && readMeta(d.id, m) ? m.version : std::string("0"); }
        net.job = std::async(std::launch::async, checkWorker, u.sources, inst);
    }
    ImGui::EndDisabled();
    if (busy) { ImGui::SameLine(); if (ImGui::Button(tr("Cancel"))) net.cancel = true; }
    if (!u.updatesOn) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("switch on 'Allow update checks' first")); }
    else if (u.offline) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("offline mode is on")); }
    std::vector<UpdateOffer> offers;
    std::string status;
    bool bad;
    {
        std::lock_guard<std::mutex> l(net.mu);
        offers = net.offers; status = net.status; bad = net.statusBad;
        u.newer.clear();
        for (const UpdateOffer& o : net.offers) u.newer[o.entry.id] = o.entry.version;
    }
    if (!status.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(bad ? pal::badRed() : pal::okGreen(), "%s", status.c_str()); ImGui::PopTextWrapPos(); }
    for (const UpdateOffer& o : offers) {
        ImGui::PushID(o.entry.id.c_str());
        ImGui::Text("%s  %s -> %s  (%s, %s)", o.entry.id.c_str(), o.installed.empty() || o.installed == "0" ? tr("bundled") : o.installed.c_str(), o.entry.version.c_str(),
                    sizeText(o.entry.size).c_str(), o.entry.licence.c_str());
        ImGui::TextDisabled("%s  sha256 %.16s...", o.entry.url.c_str(), o.entry.sha256.c_str());
        ImGui::BeginDisabled(busy || u.offline || !u.updatesOn);
        if (ImGui::SmallButton(tr("Install"))) {
            net.cancel = false; net.running = true;
            net.job = std::async(std::launch::async, installWorker, o, u.sources);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (!net.rejected.empty()) {
        std::lock_guard<std::mutex> l(net.mu);
        std::string r;
        for (const std::string& s : net.rejected) r += s + " ";
        ImGui::TextColored(pal::badRed(), "%s %s", tr("Ignored (address outside your sources):"), r.c_str());
    }
    ImGui::TextDisabled("%s", tr("Manifest format (JSON): {\"format\":\"onair-datasets-1\",\"entries\":[{\"id\",\"version\",\"url\",\"sha256\",\"licence\",\"size\"}]}"));
}

void body() {
    load();
    reap();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("The reference data OnAir uses. Your own file layered over a dataset wins over the bundled one (by key for lists), and Reset to bundled removes it again. Map tiles, satellite elements and station lists that you download are in View > Data packs."));
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(180 * gUi);
    char fb[64];
    snprintf(fb, sizeof fb, "%s", u.filter.c_str());
    if (ImGui::InputTextWithHint("##dsfilter", tr("Search"), fb, sizeof fb)) u.filter = fb;
    if (!u.note.empty()) { ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextColored(u.noteBad ? pal::badRed() : pal::okGreen(), "%s", u.note.c_str()); ImGui::PopTextWrapPos(); }
    table(ImGui::GetContentRegionAvail().y * (u.pendId.empty() ? 0.6f : 0.35f));
    previewPanel();
    updatesPanel();
    ImGui::TextDisabled("%s", tr("Credits and licences of every dataset: Help > Credits (THIRD_PARTY.md). Data you import stays on your computer."));
}

}  // namespace

void datasetsWindow(App& a) {
    if (!a.datasetsOpen) return;
    firstWindowSize(1100 * gUi, 620 * gUi);
    if (!ImGui::Begin(trw("Datasets"), &a.datasetsOpen)) { ImGui::End(); return; }
    body();
    ImGui::End();
}
