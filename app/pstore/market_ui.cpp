// The community marketplace panel (see market_ui.h). Network work runs on one background thread at a time; this file draws and starts
// tasks. The client is core/src/pstore/market.cpp, the server tools/marketplace/server.
#include "market_ui.h"
#include "store_ui.h"
#include "../app.h"
#include "dect2/engine/updater.h"
#include "dect2/pstore/market.h"
#include "../platform/platform.h"
#include "../ui/theme.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

namespace ps = dect2::pstore;
namespace dp = dect2::plugin;

namespace {
const char* kSortIds[] = {"relevance", "likes", "downloads", "newest"};
const char* kSortNames[] = {N_("Relevance"), N_("Most liked"), N_("Most downloaded"), N_("Newest")};
const char* kReasonIds[] = {"malware", "broken", "licence", "spam", "other"};
const char* kReasonNames[] = {N_("Harmful (malware, steals data)"), N_("Does not work"), N_("Licence problem"), N_("Spam or fake"), N_("Something else")};
constexpr size_t kMaxUploadBytes = 25u << 20;

struct MarketUi {
    std::unique_ptr<ps::Market> m;
    std::thread th;
    std::atomic<bool> busy{false}, finished{false}, cancel{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::mutex mu;                                     // guards everything a task writes
    std::string msg;
    bool msgBad = false, changed = false, wantRevocations = false, searched = false;
    std::vector<ps::MarketItem> items;
    int totalItems = 0, page = 1;
    std::vector<std::pair<std::string, int>> tags;
    ps::MarketItem detail;
    std::string sel, detailTried, tag;
    char query[100] = {0};
    int sort = 0;
    bool otherSystems = false;
    // install and report
    bool openConfirm = false, confirmTick = false, openReport = false;
    std::string confirmId;
    int reportReason = 0;
    char reportText[600] = {0};
    // sign-in
    ps::DeviceStart ds;
    bool signingIn = false;
    // upload
    bool showUpload = false, pendingUpload = false;
    std::vector<uint8_t> pkg;
    std::string pkgPath;
    ps::UploadPreview pv;
    std::vector<ps::MarketItem> mine;
    bool newVersion = false;
    char title[81] = {0}, desc[4001] = {0}, repo[201] = {0}, tagsBuf[200] = {0};
};
MarketUi U;

void setMsg(const std::string& s, bool bad) { std::lock_guard<std::mutex> lk(U.mu); U.msg = s; U.msgBad = bad; }
void join() { if (U.th.joinable()) U.th.join(); }
bool startTask(std::function<void()> fn) {
    if (U.busy.exchange(true)) return false;
    join();
    U.finished = false; U.done = 0; U.total = 0; U.cancel = false;
    U.th = std::thread([fn]() { fn(); U.finished = true; U.busy = false; });
    return true;
}
std::string marketUrl() { return plat::prefs().getS("marketUrl", ps::kDefaultMarketUrl); }
std::string marketKey() {
    const std::string k = plat::prefs().getS("marketKey", "");
    if (!k.empty()) return k;
    return marketUrl() == ps::kDefaultMarketUrl ? std::string(ps::kBuiltInMarketKeyHex) : std::string();
}
void ensure(dp::Manager& mgr) {
    if (U.m || U.busy) return;
    ps::MarketConfig c;
    c.baseUrl = marketUrl(); c.publicKeyHex = marketKey(); c.stateDir = mgr.pluginsDir() + "/.market"; c.hostVersion = ONAIR_VERSION;
    const std::string ua = std::string("OnAir-Market/") + ONAIR_VERSION;
    c.http = [ua](const ps::HttpCall& h) {
        const plat::HttpResult r = plat::httpCall(h.method, h.url, h.body, h.contentType, h.headers, ua, h.timeoutSec, h.maxReply);
        ps::HttpReply o; o.status = r.status; o.body = r.body; o.error = r.detail;
        return o;
    };
    U.m = std::make_unique<ps::Market>(c, &mgr);
}
std::string permText(uint32_t bit) {
    switch (bit) {
    case dp::PermNetwork: return tr("Network: talk to other computers");
    case dp::PermFilesystem: return tr("Files: read and write files");
    case dp::PermAudioOut: return tr("Sound: play audio through the program");
    case dp::PermRecord: return tr("Recording: save audio or IQ");
    case dp::PermSpawn: return tr("Programs: start other programs");
    default: return "";
    }
}
void unreviewedBadge() {
    ImGui::TextColored(pal::warnAmber(), "[%s]", tr("unreviewed"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Nobody has checked this plugin. It runs with this program's full rights."));
}
std::string joinTags(const std::vector<std::string>& t) { std::string s; for (const auto& x : t) s += (s.empty() ? "#" : "  #") + x; return s; }

// ---------------------------------------------------------------- tasks
void searchTask(bool withTags) {
    ps::MarketQuery q;
    q.q = U.query; q.sort = kSortIds[std::clamp(U.sort, 0, 3)]; q.tag = U.tag; q.page = U.page;
    q.platform = U.otherSystems ? std::string() : U.m->platform();
    startTask([q, withTags] {
        std::vector<ps::MarketItem> items; int total = 0;
        const bool ok = U.m->search(q, items, total);
        std::vector<std::pair<std::string, int>> tags;
        const bool tagsOk = ok && withTags && U.m->tags(tags);
        std::lock_guard<std::mutex> lk(U.mu);
        if (ok) {
            if (q.page > 1) U.items.insert(U.items.end(), items.begin(), items.end()); else U.items = std::move(items);
            U.totalItems = total;
            if (tagsOk) U.tags = std::move(tags);
            U.wantRevocations = true;
            U.msg.clear();
        } else { U.msg = U.m->lastError(); U.msgBad = true; }
        U.searched = true;
    });
}
void detailTask(const std::string& id) {
    startTask([id] {
        ps::MarketItem it;
        if (U.m->page(id, it)) { std::lock_guard<std::mutex> lk(U.mu); U.detail = std::move(it); }
        else setMsg(U.m->lastError(), true);
    });
}
void signInTask() {
    { std::lock_guard<std::mutex> lk(U.mu); U.signingIn = true; U.ds = ps::DeviceStart{}; }
    startTask([] {
        ps::DeviceStart ds;
        if (!U.m->signInStart(ds)) { setMsg(U.m->lastError(), true); std::lock_guard<std::mutex> lk(U.mu); U.signingIn = false; return; }
        { std::lock_guard<std::mutex> lk(U.mu); U.ds = ds; }
        int interval = ds.interval;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(ds.expiresIn);
        std::string result; bool bad = true;
        for (;;) {
            for (int i = 0; i < interval * 10 && !U.cancel; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (U.cancel) { result = tr("Sign-in cancelled."); break; }
            if (std::chrono::steady_clock::now() > until) { result = tr("The code expired. Press Sign in with GitHub again."); break; }
            int ni = interval;
            const ps::SignInState s = U.m->signInPoll(ds.deviceCode, &ni);
            if (s == ps::SignInState::Pending) continue;
            if (s == ps::SignInState::SlowDown) { interval = ni; continue; }
            if (s == ps::SignInState::Ok) { result = std::string(tr("Signed in as")) + " " + U.m->login() + "."; bad = false; std::vector<ps::MarketItem> mine; U.m->mine(mine); std::lock_guard<std::mutex> lk(U.mu); U.mine = std::move(mine); break; }
            if (s == ps::SignInState::Denied) { result = tr("The sign-in was refused on GitHub."); break; }
            if (s == ps::SignInState::Expired) { result = tr("The code expired. Press Sign in with GitHub again."); break; }
            result = U.m->lastError(); break;
        }
        setMsg(result, bad);
        std::lock_guard<std::mutex> lk(U.mu);
        U.signingIn = false;
    });
}

// the sign-in block (upload window and report dialog)
void signInBlock() {
    bool signingIn; ps::DeviceStart ds;
    { std::lock_guard<std::mutex> lk(U.mu); signingIn = U.signingIn; ds = U.ds; }
    if (U.m->signedIn()) {
        ImGui::TextColored(pal::okGreen(), "%s %s", tr("Signed in with GitHub as"), U.m->login().c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(U.busy);
        if (ImGui::SmallButton(tr("Sign out"))) U.m->signOut();
        ImGui::EndDisabled();
        return;
    }
    if (signingIn) {
        if (ds.userCode.empty()) { ImGui::TextDisabled("%s", tr("Asking GitHub for a code...")); return; }
        ImGui::TextWrapped("%s", tr("Open GitHub, sign in there and enter this code:"));
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f);
        ImGui::TextUnformatted(ds.userCode.c_str());
        ImGui::PopFont();
        if (ImGui::Button(tr("Open GitHub"))) dect2::openUrl(ds.verificationUri);
        ImGui::SameLine();
        if (ImGui::Button(tr("Copy code"))) ImGui::SetClipboardText(ds.userCode.c_str());
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##signin"))) U.cancel = true;
        ImGui::TextDisabled("%s %s", tr("Waiting for GitHub at"), ds.verificationUri.c_str());
        return;
    }
    ImGui::TextWrapped("%s", tr("Uploading and reporting need a GitHub account. OnAir shows a code, you enter it on github.com; OnAir never sees your password and keeps no GitHub token. Only your GitHub login name is stored, as the author."));
    ImGui::BeginDisabled(U.busy);
    if (ImGui::Button(tr("Sign in with GitHub"))) signInTask();
    ImGui::EndDisabled();
}

void loadPackage(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> d;
    if (f) d.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    U.pkg.clear(); U.pkgPath = path; U.pv = ps::UploadPreview{};
    if (!f) { U.pv.error = tr("Cannot open the file."); return; }
    if (d.size() > kMaxUploadBytes) { U.pv.error = tr("The file is larger than 25 MiB."); return; }
    U.pv = ps::Market::preview(d, ONAIR_VERSION);
    if (!U.pv.ok) return;
    U.pkg = std::move(d);
    // fill in from the manifest what the user has not typed yet
    const auto& man = U.pv.manifest;
    if (!U.title[0]) snprintf(U.title, sizeof U.title, "%s", man.name.c_str());
    if (!U.desc[0]) snprintf(U.desc, sizeof U.desc, "%s", man.description.c_str());
    if (!U.repo[0] && man.homepage.compare(0, 8, "https://") == 0) snprintf(U.repo, sizeof U.repo, "%s", man.homepage.c_str());
    std::lock_guard<std::mutex> lk(U.mu);
    U.newVersion = std::any_of(U.mine.begin(), U.mine.end(), [&](const ps::MarketItem& x) { return x.id == man.id; });
}

void uploadWindow() {
    firstWindowSize(560 * gUi, 640 * gUi);
    if (!ImGui::Begin(trw("Upload plugin"), &U.showUpload)) { ImGui::End(); return; }
    ImGui::TextDisabled("%s", tr("1. Sign in"));
    signInBlock();
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("2. Choose the plugin file (.onairplugin, up to 25 MiB)"));
    if (ImGui::Button(tr("Choose file..."))) { const std::string f = plat::openFileDialog(); if (!f.empty()) loadPackage(f); }
    if (!U.pkgPath.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", U.pkgPath.c_str()); }
    if (!U.pv.ok && !U.pv.error.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed()); ImGui::TextWrapped("%s", U.pv.error.c_str()); ImGui::PopStyleColor(); }
    bool newVersion; { std::lock_guard<std::mutex> lk(U.mu); newVersion = U.newVersion; }
    if (U.pv.ok) {
        const auto& m = U.pv.manifest;
        std::string plats; for (const auto& p : U.pv.platforms) plats += (plats.empty() ? "" : ", ") + p;
        ImGui::TextColored(pal::okGreen(), "%s %s %s", m.id.c_str(), m.version.c_str(), tr("checks out"));
        ImGui::TextDisabled("%s: %s    %s: %s    %s: %s", tr("Kind"), m.kind.c_str(), tr("Licence"), m.license.c_str(), tr("Systems"), plats.c_str());
        if (m.permissions) { std::string p; for (uint32_t b = 1; b <= dp::PermSpawn; b <<= 1) if (m.permissions & b) p += (p.empty() ? "" : ", ") + std::string(dp::permissionName(b)); ImGui::TextDisabled("%s: %s", tr("Permissions"), p.c_str()); }
        if (newVersion) ImGui::TextColored(pal::okGreen(), "%s", tr("You published this plugin before: this upload is its new version."));
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("3. Describe it"));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mutitle", tr("Title (required)"), U.title, sizeof U.title);
    ImGui::TextDisabled("%s", tr("Description (required): what it does, which radios and modes, how to use it."));
    ImGui::InputTextMultiline("##mudesc", U.desc, sizeof U.desc, ImVec2(-1, 120 * gUi));
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##murepo", tr("Link to the source repository (optional, https://...)"), U.repo, sizeof U.repo);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mutags", tr("Tags (optional, up to 8): ft8, aprs, map"), U.tagsBuf, sizeof U.tagsBuf);
    ImGui::Separator();
    ImGui::TextDisabled("%s", tr("4. Preview"));
    ImGui::BeginChild("##muprev", ImVec2(-1, 90 * gUi), true);
    ImGui::Text("%s", U.title[0] ? U.title : tr("(no title)"));
    ImGui::SameLine(); unreviewedBadge();
    ImGui::TextDisabled("%s %s  -  %s %s", tr("by"), U.m->signedIn() ? U.m->login().c_str() : "?", U.pv.ok ? U.pv.manifest.version.c_str() : "", U.tagsBuf);
    ImGui::TextWrapped("%.*s", 200, U.desc);
    ImGui::EndChild();
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::warnAmber(), "%s", tr("It is published at once and marked unreviewed. Your GitHub login name is shown as the author. The moderators can remove it at any time."));
    ImGui::PopTextWrapPos();
    const bool trimmedRepo = !U.repo[0] || strncmp(U.repo, "https://", 8) == 0;
    const bool ready = U.m->signedIn() && U.pv.ok && (newVersion || (U.title[0] && strlen(U.desc) >= 10)) && trimmedRepo;
    if (!trimmedRepo) ImGui::TextColored(pal::badRed(), "%s", tr("The link must start with https://"));
    ImGui::BeginDisabled(!ready || U.busy);
    if (ImGui::Button(newVersion ? tr("Publish new version") : tr("Publish"))) {
        ps::UploadForm f;
        f.package = U.pkg; f.title = U.title; f.description = U.desc; f.repo = U.repo; f.tags = U.tagsBuf;
        if (newVersion) f.newVersionOf = U.pv.manifest.id;
        startTask([f] {
            std::string id, ver;
            if (U.m->upload(f, id, ver)) {
                setMsg(std::string(tr("Published")) + ": " + id + " " + ver + ". " + tr("It is listed now (unreviewed)."), false);
                std::lock_guard<std::mutex> lk(U.mu);
                U.pendingUpload = true;
            } else setMsg(U.m->lastError(), true);
        });
    }
    ImGui::EndDisabled();
    { std::string m; bool bad; { std::lock_guard<std::mutex> lk(U.mu); m = U.msg; bad = U.msgBad; }
      if (!m.empty()) { ImGui::SameLine(); ImGui::PushTextWrapPos(0); ImGui::TextColored(bad ? pal::badRed() : pal::okGreen(), "%s", m.c_str()); ImGui::PopTextWrapPos(); } }
    ImGui::End();
}
} // namespace

void pluginMarketOpenUpload() { U.showUpload = true; }
void pluginMarketShutdown() { U.cancel = true; join(); }

bool pluginMarketPanel(App& a, dp::Manager& mgr, const std::function<void(const std::string&)>& stopIfActive) {
    (void)a;
    bool changed = false;
    ensure(mgr);
    if (!U.m) return false;
    ps::Market& m = *U.m;
    if (U.finished.exchange(false)) {
        join();
        bool rev, ch, up;
        { std::lock_guard<std::mutex> lk(U.mu); rev = U.wantRevocations; ch = U.changed; up = U.pendingUpload; U.wantRevocations = U.changed = U.pendingUpload = false; }
        if (rev) {   // plugins the moderators withdrew that were installed from here: stop them, then the kill switch turns them off
            const auto pulled = m.pulledInstalled();
            for (const auto& r : pulled) stopIfActive(r.id);
            pluginStoreSetExtraRevocations(mgr, pulled);
            changed = true;
        }
        if (ch) changed = true;
        if (up) { U.page = 1; searchTask(true); }
    }
    const bool busy = U.busy;
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::warnAmber(), "%s", tr("Community plugins: anyone with a GitHub account can publish here, and nobody reviews them first. A plugin runs with this program's full rights: install only what you trust."));
    ImGui::TextDisabled("%s", tr("Opening this panel fetches the list from plugins.on-air.dev; searching, opening a plugin, liking and installing fetch more. That server sees your IP address; likes and download counts use a random id of this OnAir installation, stored there only as a salted hash."));
    ImGui::PopTextWrapPos();
    if (!m.keyConfigured()) {
        ImGui::TextColored(pal::badRed(), "%s", tr("The community marketplace is not set up in this build yet (no signing key), so nothing is fetched."));
        return changed;
    }
    // search line
    if (!U.searched && !busy) searchTask(true);   // the first look at the panel lists the most relevant plugins
    ImGui::SetNextItemWidth(220 * gUi);
    const bool enter = ImGui::InputTextWithHint("##mq", tr("Search plugins"), U.query, sizeof U.query, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * gUi);
    bool again = enter;
    if (ImGui::BeginCombo("##msort", tr(kSortNames[U.sort]))) {
        for (int i = 0; i < 4; i++) if (ImGui::Selectable(tr(kSortNames[i]), U.sort == i)) { U.sort = i; again = true; }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr("Search"))) again = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("Other systems too"), &U.otherSystems)) again = true;
    ImGui::SameLine();
    if (ImGui::Button(tr("Upload plugin"))) {
        U.showUpload = true;
        if (m.signedIn() && !busy) startTask([] { std::vector<ps::MarketItem> mine; if (U.m->mine(mine)) { std::lock_guard<std::mutex> lk(U.mu); U.mine = std::move(mine); } });
    }
    // tag chips
    std::vector<std::pair<std::string, int>> tags;
    { std::lock_guard<std::mutex> lk(U.mu); tags = U.tags; }
    if (!tags.empty()) {
        float x = 0;
        const float maxX = ImGui::GetContentRegionAvail().x;
        for (size_t i = 0; i < tags.size(); i++) {
            const std::string label = "#" + tags[i].first + "##mt" + std::to_string(i);
            const float w = ImGui::CalcTextSize(label.c_str(), nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
            if (i && x + w < maxX) ImGui::SameLine(); else x = 0;
            x += w;
            const bool on = U.tag == tags[i].first;
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::SmallButton(label.c_str())) { U.tag = on ? std::string() : tags[i].first; again = true; }
            if (on) ImGui::PopStyleColor();
        }
    }
    if (again && !busy) { U.page = 1; searchTask(false); }
    // status line
    if (busy) { const uint64_t d = U.done, t = U.total; if (t) ImGui::ProgressBar((float)d / (float)t, ImVec2(200 * gUi, 0)); else ImGui::TextDisabled("%s", tr("Working...")); }
    { std::string ms; bool bad; { std::lock_guard<std::mutex> lk(U.mu); ms = U.msg; bad = U.msgBad; }
      if (!ms.empty() && !U.showUpload) { ImGui::PushTextWrapPos(0); ImGui::TextColored(bad ? pal::badRed() : pal::okGreen(), "%s", ms.c_str()); ImGui::PopTextWrapPos(); } }
    ImGui::Separator();

    // the list (cards)
    std::vector<ps::MarketItem> items; int total;
    { std::lock_guard<std::mutex> lk(U.mu); items = U.items; total = U.totalItems; }
    ImGui::BeginChild("##mlist", ImVec2(300 * gUi, -10 * gUi), true);
    if (items.empty()) ImGui::TextDisabled("%s", busy ? tr("Loading...") : tr("No plugins found."));
    const float lh = ImGui::GetTextLineHeightWithSpacing();
    for (const auto& it : items) {
        ImGui::PushID(it.id.c_str());
        const ImVec2 p = ImGui::GetCursorPos();
        if (ImGui::Selectable("##card", U.sel == it.id, 0, ImVec2(0, lh * 3 - ImGui::GetStyle().ItemSpacing.y))) { U.sel = it.id; U.detailTried.clear(); }
        ImGui::SetCursorPos(ImVec2(p.x + 4 * gUi, p.y));
        ImGui::TextUnformatted(it.title.c_str());
        ImGui::SameLine(); unreviewedBadge();
        dp::Entry e;
        if (mgr.find(it.id, e) && !e.bundled) { ImGui::SameLine(); ImGui::TextColored(pal::okGreen(), "%s", tr("installed")); }
        ImGui::SetCursorPosX(p.x + 4 * gUi);
        ImGui::TextDisabled("%s %s  -  %lld %s  -  %lld %s", tr("by"), it.owner.c_str(), (long long)it.likes, tr("likes"), (long long)it.downloads, tr("downloads"));
        ImGui::SetCursorPosX(p.x + 4 * gUi);
        ImGui::TextDisabled("%s", joinTags(it.tags).c_str());
        ImGui::PopID();
    }
    if ((int)items.size() < total) {
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Show more"))) { U.page++; searchTask(false); }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // the plugin page
    ImGui::BeginChild("##mdet", ImVec2(0, -10 * gUi), false);
    ps::MarketItem d;
    { std::lock_guard<std::mutex> lk(U.mu); d = U.detail; }
    if (!U.sel.empty() && d.id != U.sel && !busy && U.detailTried != U.sel) { U.detailTried = U.sel; detailTask(U.sel); }
    if (U.sel.empty()) ImGui::TextDisabled("%s", tr("Select a plugin."));
    else if (d.id != U.sel) ImGui::TextDisabled("%s", tr("Loading..."));
    else {
        ImGui::Text("%s  %s", d.title.c_str(), d.version.c_str());
        ImGui::SameLine(); unreviewedBadge();
        ImGui::TextDisabled("%s %s (GitHub)    %s: %s    %s: %s", tr("by"), d.owner.c_str(), tr("Licence"), d.license.c_str(), tr("Kind"), d.kind.c_str());
        ImGui::TextDisabled("%lld %s    %lld %s    %s", (long long)d.likes, tr("likes"), (long long)d.downloads, tr("downloads"), joinTags(d.tags).c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(pal::warnAmber(), "%s", tr("Unreviewed: nobody has checked this plugin. It runs with this program's full rights."));
        ImGui::Spacing();
        ImGui::TextUnformatted(d.description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (d.permissions) {
            ImGui::TextDisabled("%s", tr("It says it needs"));
            for (uint32_t b = 1; b <= dp::PermSpawn; b <<= 1) if (d.permissions & b) ImGui::BulletText("%s", permText(b).c_str());
        } else ImGui::TextDisabled("%s", tr("It says it needs no permissions."));
        if (!d.repo.empty()) { if (ImGui::SmallButton(tr("Source repository"))) dect2::openUrl(d.repo); ImGui::SameLine(); ImGui::TextDisabled("%s", d.repo.c_str()); }
        ImGui::Spacing();
        const std::string why = m.incompatibility(d);
        if (!why.empty()) ImGui::TextColored(pal::badRed(), "%s: %s", tr("Cannot be installed here"), why.c_str());
        dp::Entry e;
        const bool inst = mgr.find(d.id, e) && !e.bundled;
        const bool newer = inst && dp::compareVersions(d.version, e.m.version) > 0;
        if (inst) ImGui::TextDisabled("%s: %s%s", tr("Installed version"), e.m.version.c_str(), m.installedFromHere(d.id) ? "" : tr(" (not from the marketplace)"));
        ImGui::BeginDisabled(busy || !why.empty() || (inst && !newer));
        if (ImGui::Button(newer ? tr("Update") : tr("Install"))) { U.confirmId = d.id; U.confirmTick = false; U.openConfirm = true; }
        ImGui::EndDisabled();
        if (inst) {
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Remove"))) {
                std::string err;
                stopIfActive(d.id);
                if (mgr.remove(d.id, err)) { m.forgetInstalled(d.id); setMsg(std::string(tr("Removed")) + ": " + d.id, false); changed = true; }
                else setMsg(err, true);
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(d.liked ? tr("Unlike") : tr("Like"))) {
            const std::string id = d.id; const bool on = !d.liked;
            startTask([id, on] {
                int64_t likes = 0;
                if (!U.m->setLike(id, on, likes)) { setMsg(U.m->lastError(), true); return; }
                std::lock_guard<std::mutex> lk(U.mu);
                if (U.detail.id == id) { U.detail.likes = likes; U.detail.liked = on; }
                for (auto& x : U.items) if (x.id == id) x.likes = likes;
            });
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Report"))) { U.openReport = true; U.reportReason = 0; U.reportText[0] = 0; }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // the extra confirmation before an unreviewed plugin is installed
    if (U.openConfirm) { ImGui::OpenPopup(trw("Install an unreviewed plugin?")); U.openConfirm = false; }
    if (ImGui::BeginPopupModal(trw("Install an unreviewed plugin?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(440 * gUi);
        ImGui::Text("%s %s", d.title.c_str(), d.version.c_str());
        ImGui::TextDisabled("%s %s (GitHub)    %s: %s", tr("by"), d.owner.c_str(), tr("Licence"), d.license.c_str());
        ImGui::Spacing();
        ImGui::TextColored(pal::warnAmber(), "%s", tr("This plugin is unreviewed: anyone with a GitHub account can publish in the community marketplace and nobody has checked this one. It is native code. Once it runs it can do anything this program can do on your computer (read, change or delete your files, use the network), whatever it says it needs; there is no sandbox. Install it only if you trust its author, ideally after reading its source."));
        if (!d.repo.empty()) { if (ImGui::SmallButton(tr("Read the source first"))) dect2::openUrl(d.repo); }
        ImGui::Spacing();
        ImGui::PopTextWrapPos();
        ImGui::Checkbox(tr("I understand: this unreviewed plugin runs with this program's full rights"), &U.confirmTick);
        ImGui::BeginDisabled(!U.confirmTick || busy);
        if (ImGui::Button(tr("Install anyway"))) {
            const std::string id = U.confirmId;
            stopIfActive(id);
            startTask([id] {
                const auto r = U.m->install(id, true, [](uint64_t dn, uint64_t t) { U.done = dn; U.total = t; });
                setMsg(r.ok ? std::string(tr("Installed")) + ": " + id + " " + r.version + ". " + tr("It is off until you turn it on in the Installed tab.") : r.error, !r.ok);
                std::lock_guard<std::mutex> lk(U.mu);
                U.changed = true;
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // report
    if (U.openReport) { ImGui::OpenPopup(trw("Report this plugin")); U.openReport = false; }
    if (ImGui::BeginPopupModal(trw("Report this plugin"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PushTextWrapPos(440 * gUi);
        ImGui::Text("%s %s", d.title.c_str(), d.version.c_str());
        ImGui::TextDisabled("%s", tr("The moderators read every report and can remove the plugin. Your GitHub login name is sent with the report."));
        ImGui::PopTextWrapPos();
        signInBlock();
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::BeginCombo("##mrr", tr(kReasonNames[U.reportReason]))) {
            for (int i = 0; i < 5; i++) if (ImGui::Selectable(tr(kReasonNames[i]), U.reportReason == i)) U.reportReason = i;
            ImGui::EndCombo();
        }
        ImGui::InputTextMultiline("##mrt", U.reportText, sizeof U.reportText, ImVec2(440 * gUi, 90 * gUi));
        ImGui::BeginDisabled(!m.signedIn() || busy);
        if (ImGui::Button(tr("Send report"))) {
            const std::string id = d.id, reason = kReasonIds[U.reportReason], text = U.reportText;
            startTask([id, reason, text] { const bool ok = U.m->report(id, reason, text); setMsg(ok ? std::string(tr("Report sent. Thank you.")) : U.m->lastError(), !ok); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##report"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (U.showUpload) uploadWindow();
    return changed;
}
