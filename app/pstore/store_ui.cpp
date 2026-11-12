// The Store tab of View > Plugins: see store_ui.h. The network work (refresh, the plugin's page, the download and install) runs on one background
// thread at a time; this file only draws and starts tasks. The core is core/src/pstore.
#include "store_ui.h"
#include "market_ui.h"
#include "../app.h"
#include "dect2/engine/updater.h"
#include "dect2/pstore/store.h"
#include "../platform/platform.h"
#include "../ui/theme.h"
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace ps = dect2::pstore;
namespace dp = dect2::plugin;

namespace {
struct StoreUi {
    std::unique_ptr<ps::Store> store;
    std::string builtUrl, builtKey;
    std::thread th;
    std::atomic<bool> busy{false}, finished{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::mutex mu;
    std::string msg;                  // the result of the last action (guarded by mu)
    bool msgBad = false, changed = false, wantRevocations = false, triedCache = false;
    std::string sel, cat, confirmId;
    char query[96] = {0};
    char urlBuf[512] = {0}, keyBuf[80] = {0};
    bool bufInit = false, confirmTick = false, openConfirm = false;
    std::string detailTried;           // the plugin whose page was asked for once already (a failure is not retried every frame)
    std::vector<ps::Revocation> extra;  // from the community marketplace (market_ui.cpp)
    bool extraPending = false;
};
StoreUi S;

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
std::string hostOf(const std::string& url) {
    if (url.compare(0, 8, "https://") != 0) return {};
    const size_t e = url.find('/', 8);
    return url.substr(8, e == std::string::npos ? std::string::npos : e - 8);
}
void setMsg(const std::string& m, bool bad) { std::lock_guard<std::mutex> lk(S.mu); S.msg = m; S.msgBad = bad; }
void joinWorker() { if (S.th.joinable()) S.th.join(); }
void startTask(std::function<void()> fn) {
    if (S.busy.exchange(true)) return;
    joinWorker();
    S.finished = false; S.done = 0; S.total = 0;
    S.th = std::thread([fn]() { fn(); S.finished = true; S.busy = false; });
}
std::string registryUrl() { return plat::prefs().getS("storeRegistryUrl", ps::kDefaultRegistryUrl); }
std::string registryKey() {
    const std::string k = plat::prefs().getS("storeRegistryKey", "");
    if (!k.empty()) return k;
    return registryUrl() == ps::kDefaultRegistryUrl ? std::string(ps::kBuiltInRegistryKeyHex) : std::string();   // another address needs its own key
}
void ensureStore(dp::Manager& mgr) {
    if (S.busy) return;
    const std::string u = registryUrl(), k = registryKey();
    if (S.store && u == S.builtUrl && k == S.builtKey) return;
    ps::Config c;
    c.registryUrl = u; c.publicKeyHex = k; c.cacheDir = mgr.pluginsDir() + "/.store"; c.hostVersion = ONAIR_VERSION;
    c.autoRevocations = false;   // applied below on the UI thread, after the running plugin was stopped
    S.store = std::make_unique<ps::Store>(c, &mgr);
    S.store->setExtraRevocations(S.extra);
    S.builtUrl = u; S.builtKey = k; S.triedCache = false;
}
bool installedHere(dp::Manager& mgr, const std::string& id, std::string* ver = nullptr) { dp::Entry e; const bool f = mgr.find(id, e) && !e.bundled; if (f && ver) *ver = e.m.version; return f; }
const char* reviewText(const std::string& r) {
    if (r == "verified") return N_("verified");
    if (r == "reviewed") return N_("reviewed");
    return N_("listed");
}
ImVec4 reviewColour(const std::string& r) { return r == "verified" ? pal::okGreen() : r == "reviewed" ? ImVec4(0.45f, 0.70f, 0.95f, 1) : ImVec4(0.65f, 0.65f, 0.65f, 1); }
const char* reviewExplain(const std::string& r) {
    if (r == "verified") return N_("Verified: a maintainer read the source, built it and compared it with the package. This is still not a guarantee.");
    if (r == "reviewed") return N_("Reviewed: a person looked at it. This is not a guarantee.");
    return N_("Listed: it only meets the listing rules (open licence, declared permissions). Nobody has looked at its code.");
}
void openLink(const std::string& u) { if (u.compare(0, 8, "https://") == 0) dect2::openUrl(u); }
} // namespace

void pluginStoreShutdown() { pluginMarketShutdown(); joinWorker(); }

void pluginStoreSetExtraRevocations(dp::Manager& mgr, const std::vector<ps::Revocation>& r) {
    S.extra = r;
    S.extraPending = true;
    if (S.busy) return;   // applied by pluginStoreTab once the registry task is done
    ensureStore(mgr);
    if (!S.store) return;
    S.store->setExtraRevocations(S.extra);
    S.store->applyRevocations();
    S.extraPending = false;
}

namespace {
// the 3.9 signed static registry: reviewed and verified plugins
bool registryPanel(App& a, dp::Manager& mgr, const std::function<void(const std::string&)>& stopIfActive) {
    (void)a;
    bool changed = false;
    if (S.finished.exchange(false)) {   // a task ended: act on the UI thread
        joinWorker();
        if (S.wantRevocations && S.store) {
            S.wantRevocations = false;
            for (const auto& r : S.store->revocations()) stopIfActive(r.id);
            const auto hit = S.store->applyRevocations();
            if (!hit.empty()) {
                std::string who;
                for (const auto& h : hit) who += (who.empty() ? "" : ", ") + h.id;
                std::string cur; { std::lock_guard<std::mutex> lk(S.mu); cur = S.msg; }
                setMsg(std::string(tr("Switched off by the registry")) + ": " + who + "  " + cur, true);
            }
            changed = true;
        }
    }
    {
        std::lock_guard<std::mutex> lk(S.mu);
        if (S.changed) { S.changed = false; changed = true; }
    }
    ensureStore(mgr);
    if (!S.bufInit) {
        snprintf(S.urlBuf, sizeof S.urlBuf, "%s", registryUrl().c_str());
        snprintf(S.keyBuf, sizeof S.keyBuf, "%s", plat::prefs().getS("storeRegistryKey", "").c_str());
        S.bufInit = true;
    }
    if (!S.triedCache && S.store && !S.busy) {   // the list saved earlier: a file read, no network
        S.triedCache = true;
        S.store->loadCache();
        for (const auto& r : S.store->revocations()) stopIfActive(r.id);
        S.store->applyRevocations();
        changed = true;
    }
    if (!S.store) return changed;
    ps::Store& st = *S.store;

    // what this tab does with the network
    const std::string host = hostOf(S.builtUrl);
    ImGui::TextWrapped(tr("Nothing is fetched until you press Refresh, open a plugin or install one. The registry is %s; that server sees your IP address and the OnAir version, nothing else. No account, no telemetry."), host.empty() ? tr("the registry folder") : host.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
    ImGui::TextWrapped("%s", tr("Plugins are native code and run with this program's rights. There is no sandbox for untrusted plugins yet: install only plugins you trust. Reviewed means a person looked at it, not a guarantee."));
    ImGui::PopStyleColor();
    if (!st.signatureKeyConfigured()) {
        ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
        ImGui::TextWrapped("%s", tr("No signing key is set for this registry, so its list cannot be trusted and nothing is fetched. The project's key is not published yet; for another registry, set its address and key under Advanced."));
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    const bool busy = S.busy;
    ImGui::BeginDisabled(busy || !st.signatureKeyConfigured());
    if (ImGui::Button(trw("Refresh"))) { S.wantRevocations = true; S.detailTried.clear(); startTask([] { const bool ok = S.store->refresh(); setMsg(ok ? std::string(tr("The list is up to date.")) : S.store->lastError(), !ok); }); }
    ImGui::SameLine();
    if (ImGui::Button(trw("Check for updates"))) {
        S.wantRevocations = true;
        startTask([] {
            const bool ok = S.store->refresh();
            const size_t n = S.store->updates().size();
            setMsg(!ok ? S.store->lastError() : n ? std::to_string(n) + " " + tr("update(s) available. Select a plugin and press Update.") : std::string(tr("All installed plugins are up to date.")), !ok);
        });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (busy) {
        const uint64_t d = S.done, t = S.total;
        if (t) ImGui::ProgressBar((float)d / (float)t, ImVec2(160 * gUi, 0)); else ImGui::TextDisabled("%s", tr("Working..."));
    } else if (st.source() != ps::Source::None) {
        ImGui::TextDisabled("%s %s (#%d)", tr("List from"), st.generated().empty() ? "?" : st.generated().substr(0, 10).c_str(), st.serial());
    } else ImGui::TextDisabled("%s", tr("No list yet. Press Refresh."));
    { std::string m; bool bad; { std::lock_guard<std::mutex> lk(S.mu); m = S.msg; bad = S.msgBad; }
      if (!m.empty()) { ImGui::PushStyleColor(ImGuiCol_Text, bad ? pal::badRed() : pal::okGreen()); ImGui::TextWrapped("%s", m.c_str()); ImGui::PopStyleColor(); } }
    if (!st.notice().empty() && st.source() == ps::Source::Cache) ImGui::TextDisabled("%s", st.notice().c_str());
    if (st.hiddenCount()) ImGui::TextDisabled("%zu %s", st.hiddenCount(), tr("listing(s) hidden: not for this computer, this plugin interface version or this OnAir version."));

    ImGui::SetNextItemWidth(200 * gUi);
    ImGui::InputTextWithHint("##sq", tr("Search"), S.query, sizeof S.query);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::BeginCombo("##scat", S.cat.empty() ? tr("All categories") : S.cat.c_str())) {
        if (ImGui::Selectable(tr("All categories"), S.cat.empty())) S.cat.clear();
        for (const auto& c : st.categories()) if (ImGui::Selectable(c.c_str(), S.cat == c)) S.cat = c;
        ImGui::EndCombo();
    }
    ImGui::Separator();

    const auto items = st.list(S.query, S.cat);
    const auto ups = st.updates();
    ImGui::BeginChild("##slist", ImVec2(260 * gUi, -70 * gUi), true);
    if (items.empty()) ImGui::TextDisabled("%s", tr("No plugins to show."));
    for (const auto& l : items) {
        std::string ver;
        const bool inst = installedHere(mgr, l.id, &ver);
        const bool up = std::any_of(ups.begin(), ups.end(), [&](const ps::UpdateInfo& u) { return u.id == l.id; });
        if (ImGui::Selectable((l.name + "  " + l.version + "##" + l.id).c_str(), S.sel == l.id)) S.sel = l.id;
        ImGui::SameLine(); ImGui::TextColored(reviewColour(l.review), "[%s]", tr(reviewText(l.review)));
        if (up) { ImGui::SameLine(); ImGui::TextColored(pal::warnAmber(), "%s", tr("update")); }
        else if (inst) { ImGui::SameLine(); ImGui::TextColored(pal::okGreen(), "%s", tr("installed")); }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##sdet", ImVec2(0, -70 * gUi), false);
    ps::Listing l;
    if (!S.sel.empty() && st.find(S.sel, l)) {
        ImGui::Text("%s %s", l.name.c_str(), l.version.c_str());
        ImGui::TextDisabled("%s: %s    %s: %s    %s: %s", tr("Author"), l.author.c_str(), tr("Licence"), l.license.c_str(), tr("Kind"), l.kind.c_str());
        ImGui::TextColored(reviewColour(l.review), "[%s]", tr(reviewText(l.review)));
        ImGui::SameLine(); ImGui::TextWrapped("%s", tr(reviewExplain(l.review)));
        if (!l.haveDetail && !busy && S.detailTried != l.id) { const std::string id = l.id; S.detailTried = id; startTask([id] { ps::Listing x; std::string err; if (!S.store->detail(id, x, err)) setMsg(err, true); }); }
        if (!l.summary.empty()) ImGui::TextWrapped("%s", l.summary.c_str());
        if (!l.description.empty()) ImGui::TextWrapped("%s", l.description.c_str());
        ImGui::Spacing();
        if (l.permissions) {
            ImGui::TextDisabled("%s", tr("Asks for"));
            for (uint32_t b = 1; b <= dp::PermSpawn; b <<= 1) if (l.permissions & b) ImGui::BulletText("%s", tr(permLabel(b)));
        } else ImGui::TextDisabled("%s", tr("Asks for no permissions."));
        if (!l.homepage.empty() && ImGui::SmallButton(trw("Project page"))) openLink(l.homepage);
        if (!l.source.empty()) { ImGui::SameLine(); if (ImGui::SmallButton(trw("Source code"))) openLink(l.source); }
        ImGui::Spacing();
        std::string ver;
        const bool inst = installedHere(mgr, l.id, &ver);
        const bool up = std::any_of(ups.begin(), ups.end(), [&](const ps::UpdateInfo& u) { return u.id == l.id; });
        if (inst) ImGui::TextDisabled("%s: %s", tr("Installed version"), ver.c_str());
        ImGui::BeginDisabled(busy || (inst && !up) || !l.haveDetail);
        if (ImGui::Button(up ? trw("Update") : trw("Install"))) { S.confirmId = l.id; S.confirmTick = false; S.openConfirm = true; }
        ImGui::EndDisabled();
        if (inst) {
            ImGui::SameLine();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(trw("Remove"))) {
                const std::string id = l.id;
                stopIfActive(id);
                startTask([id] { const auto r = S.store->uninstall(id); setMsg(r.ok ? std::string(tr("Removed")) + ": " + id : r.error, !r.ok); std::lock_guard<std::mutex> lk(S.mu); S.changed = true; });
            }
            ImGui::EndDisabled();
        }
    } else ImGui::TextDisabled("%s", tr("Select a plugin."));
    ImGui::EndChild();

    // the confirmation: permissions in plain words and the native-code warning, before anything is installed
    if (S.openConfirm) { ImGui::OpenPopup(trw("Install plugin?")); S.openConfirm = false; }
    if (ImGui::BeginPopupModal(trw("Install plugin?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ps::Listing c;
        if (st.find(S.confirmId, c)) {
            ImGui::Text("%s %s", c.name.c_str(), c.version.c_str());
            ImGui::TextDisabled("%s: %s    %s: %s", tr("Author"), c.author.c_str(), tr("Licence"), c.license.c_str());
            ImGui::PushTextWrapPos(420 * gUi);
            ImGui::TextColored(reviewColour(c.review), "[%s]", tr(reviewText(c.review)));
            ImGui::TextWrapped("%s", tr(reviewExplain(c.review)));
            ImGui::Spacing();
            if (c.permissions) { ImGui::TextDisabled("%s", tr("It asks for")); for (uint32_t b = 1; b <= dp::PermSpawn; b <<= 1) if (c.permissions & b) ImGui::BulletText("%s", tr(permLabel(b))); }
            else ImGui::TextDisabled("%s", tr("It asks for no permissions."));
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, pal::warnAmber());
            ImGui::TextWrapped("%s", tr("This is native code. Once it runs it can do anything this program can do on your computer, whatever it asks for here; there is no sandbox yet. Install it only if you trust its author."));
            ImGui::PopStyleColor();
            ImGui::PopTextWrapPos();
            ImGui::Checkbox(trw("I understand that this plugin is native code"), &S.confirmTick);
            ImGui::BeginDisabled(!S.confirmTick);
            if (ImGui::Button(trw("Install now"))) {
                const std::string id = c.id;
                stopIfActive(id);
                startTask([id] {
                    const auto r = S.store->install(id, true, [](uint64_t d, uint64_t t) { S.done = d; S.total = t; });
                    setMsg(r.ok ? std::string(tr("Installed")) + ": " + id + " " + r.version + ". " + tr("It is off until you turn it on in the Installed tab.") : r.error, !r.ok);
                    std::lock_guard<std::mutex> lk(S.mu); S.changed = true;
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
        }
        if (ImGui::Button(trw("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // the registry address and key: another registry, or a local folder for testing
    if (ImGui::CollapsingHeader(trw("Advanced: registry address and key"))) {
        ImGui::TextWrapped("%s", tr("The registry is a folder of files on an https:// address (or a local folder). Its list is signed; OnAir only accepts a list signed with the key below (64 hex digits). Do not use a registry whose key you did not get from its owner."));
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##sru", S.urlBuf, sizeof S.urlBuf);
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##srk", tr("Public key (hex); empty = the project's key for the project's registry"), S.keyBuf, sizeof S.keyBuf);
        if (ImGui::Button(trw("Apply"))) { plat::prefs().setS("storeRegistryUrl", S.urlBuf); plat::prefs().setS("storeRegistryKey", S.keyBuf); plat::prefs().flush(); }
        ImGui::SameLine();
        if (ImGui::Button(trw("Use the project's registry"))) {
            plat::prefs().setS("storeRegistryUrl", ps::kDefaultRegistryUrl); plat::prefs().setS("storeRegistryKey", ""); plat::prefs().flush();
            snprintf(S.urlBuf, sizeof S.urlBuf, "%s", ps::kDefaultRegistryUrl); S.keyBuf[0] = 0;
        }
    }
    return changed;
}
} // namespace

// Two sources in one tab (4.0): the community marketplace (anyone can publish, unreviewed) and the reviewed registry of 3.9.
bool pluginStoreTab(App& a, dp::Manager& mgr, const std::function<void(const std::string&)>& stopIfActive) {
    static int source = -1;
    if (source < 0) source = plat::prefs().getS("storeSource", "community") == "registry" ? 1 : 0;
    if (S.extraPending && !S.busy) pluginStoreSetExtraRevocations(mgr, S.extra);
    int was = source;
    ImGui::RadioButton(tr("Community marketplace"), &source, 0);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Anyone with a GitHub account can publish here. Plugins are not reviewed."));
    ImGui::SameLine();
    ImGui::RadioButton(tr("Reviewed registry"), &source, 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A small signed list kept by the OnAir maintainers: listed, reviewed and verified plugins."));
    if (source != was) { plat::prefs().setS("storeSource", source ? "registry" : "community"); plat::prefs().flush(); }
    ImGui::Separator();
    return source == 0 ? pluginMarketPanel(a, mgr, stopIfActive) : registryPanel(a, mgr, stopIfActive);
}
