// The Badges window and toast (see badges_ui.h). Network work runs on one background thread at a time; this file draws and starts tasks.
// The sign-in is the plugin marketplace's GitHub device flow (core/src/pstore/market.cpp): signing in or out here does it for both.
#include "badges_ui.h"
#include "../app.h"
#include "../platform/platform.h"
#include "../state/receivers.h"
#include "../ui/scale.h"
#include "../ui/theme.h"
#include "dect2/badges/badges_client.h"
#include "dect2/engine/updater.h"
#include "dect2/pstore/market.h"
#include <atomic>
#include <chrono>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace bd = dect2::badges;
namespace ps = dect2::pstore;

namespace {
constexpr const char* kPrefEarn = "mapBadges";          // Earn badges (off until the user turns it on)
constexpr const char* kPrefHome = "mapBadgesHome";      // send the station's 6-character locator square for range badges (off until chosen)

struct State {
    std::mutex mu;                                       // guards the fields the task thread writes
    std::thread th;
    std::atomic<bool> busy{false}, cancel{false};
    bd::Profile me; bool haveMe = false;
    std::string msg; bool msgBad = false;
    ps::DeviceStart ds; bool signingIn = false;
    std::unique_ptr<ps::Market> market;
    bd::Session sess; std::atomic<int64_t> sessAt{0};
    std::string replyError;                              // the last post's refused sign-in
    bool open = false, fetchedOnce = false;
    bd::Award toast; double toastUntil = 0;
    ~State() { cancel = true; if (th.joinable()) th.join(); }   // a sign-in wait ends within 0.1 s; an HTTP call within its timeout
};
State S;
bd::ToastQueue& toasts() { static bd::ToastQueue q(plat::dataDir() + "/badges-shown.txt"); return q; }

int64_t nowSec() { return (int64_t)std::time(nullptr); }
double nowS() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
std::string stateDir() { return plat::dataDir() + "/plugins/.market"; }
App& root(App& a) { if (App* r = receiverById(1)) return *r; return a; }
std::string origin(App& a) {
    std::string o = root(a).mapServer;
    if (o.empty()) o = plat::basePrefs().getS("mapServer", "");
    if (o.empty()) o = "https://map.on-air.dev";
    while (!o.empty() && o.back() == '/') o.pop_back();
    return o;
}
bool earn() { return plat::prefs().getB(kPrefEarn, false); }
const bd::Session& session() {   // the marketplace's file, read again every 15 s (a sign-in or sign-out in the plugin panel shows up here)
    const int64_t t = nowSec();
    if (t - S.sessAt >= 15 || t < S.sessAt) { S.sess = bd::readSession(stateDir(), t); S.sessAt = t; }
    return S.sess;
}
void setMsg(const std::string& m, bool bad) { std::lock_guard<std::mutex> lk(S.mu); S.msg = m; S.msgBad = bad; }
std::string ua() { return std::string("OnAir/") + ONAIR_VERSION + " (map badges)"; }

void ensureMarket() {
    if (S.market) return;
    ps::MarketConfig c;
    c.baseUrl = plat::prefs().getS("marketUrl", ps::kDefaultMarketUrl);
    const std::string k = plat::prefs().getS("marketKey", "");
    c.publicKeyHex = !k.empty() ? k : (c.baseUrl == ps::kDefaultMarketUrl ? std::string(ps::kBuiltInMarketKeyHex) : std::string());
    c.stateDir = stateDir(); c.hostVersion = ONAIR_VERSION;
    const std::string u = std::string("OnAir-Market/") + ONAIR_VERSION;
    c.http = [u](const ps::HttpCall& h) {
        const plat::HttpResult r = plat::httpCall(h.method, h.url, h.body, h.contentType, h.headers, u, h.timeoutSec, h.maxReply);
        ps::HttpReply o; o.status = r.status; o.body = r.body; o.error = r.detail;
        return o;
    };
    S.market = std::make_unique<ps::Market>(c, nullptr);   // sign-in only: no plugin manager needed
}

bool startTask(std::function<void()> fn) {
    if (S.busy.exchange(true)) return false;
    if (S.th.joinable()) S.th.join();
    S.cancel = false;
    S.th = std::thread([fn] { fn(); S.busy = false; });
    return true;
}

void fetchMe(App& a) {
    const std::string url = origin(a) + "/api/me", hdr = bd::authHeader(session(), nowSec());
    startTask([url, hdr] {
        std::vector<std::string> h; if (!hdr.empty()) h.push_back(hdr);
        const plat::HttpResult r = plat::httpCall("GET", url, "", "", h, ua(), 15, 1u << 20);
        if (r.status == 0) { setMsg(std::string(tr("Could not reach the map server")) + " (" + r.detail + ").", true); return; }
        bd::Profile p = bd::parseMe(r.body);
        std::lock_guard<std::mutex> lk(S.mu);
        S.me = std::move(p); S.haveMe = true;
        if (!S.me.ok) { S.msg = S.me.error; S.msgBad = true; } else if (!S.me.error.empty()) { S.msg = S.me.error; S.msgBad = true; } else S.msg.clear();
    });
}

void postSettings(App& a, const std::string& body) {
    const std::string url = origin(a) + "/api/me/settings", hdr = bd::authHeader(session(), nowSec()), meUrl = origin(a) + "/api/me";
    if (hdr.empty()) { setMsg(tr("Sign in with GitHub first."), true); return; }
    startTask([url, hdr, body, meUrl] {
        const plat::HttpResult r = plat::httpCall("POST", url, body, "application/json", {hdr}, ua(), 15, 1u << 16);
        if (r.status < 200 || r.status > 299) {
            const bd::Profile e = bd::parseMe(r.body);
            setMsg(!e.error.empty() ? e.error : std::string(tr("The map server did not save it")) + " (HTTP " + std::to_string(r.status) + ").", true);
            return;
        }
        const plat::HttpResult m = plat::httpCall("GET", meUrl, "", "", {hdr}, ua(), 15, 1u << 20);
        bd::Profile p = bd::parseMe(m.body);
        std::lock_guard<std::mutex> lk(S.mu);
        if (p.ok) { S.me = std::move(p); S.haveMe = true; }
        S.msg = tr("Saved."); S.msgBad = false;
    });
}

void signIn(App& a) {
    ensureMarket();
    if (!S.market->keyConfigured()) { setMsg(tr("GitHub sign-in goes through the OnAir plugin marketplace, which is not set up in this build yet."), true); return; }
    { std::lock_guard<std::mutex> lk(S.mu); S.signingIn = true; S.ds = ps::DeviceStart{}; }
    const std::string meUrl = origin(a) + "/api/me", setUrl = origin(a) + "/api/me/settings";
    startTask([meUrl, setUrl] {
        ps::DeviceStart ds;
        if (!S.market->signInStart(ds)) { setMsg(S.market->lastError(), true); std::lock_guard<std::mutex> lk(S.mu); S.signingIn = false; return; }
        { std::lock_guard<std::mutex> lk(S.mu); S.ds = ds; }
        int interval = ds.interval;
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(ds.expiresIn);
        std::string result; bool bad = true, ok = false;
        for (;;) {
            for (int i = 0; i < interval * 10 && !S.cancel; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (S.cancel) { result = tr("Sign-in cancelled."); break; }
            if (std::chrono::steady_clock::now() > until) { result = tr("The code expired. Press Sign in with GitHub again."); break; }
            int ni = interval;
            const ps::SignInState st = S.market->signInPoll(ds.deviceCode, &ni);
            if (st == ps::SignInState::Pending) continue;
            if (st == ps::SignInState::SlowDown) { interval = ni; continue; }
            if (st == ps::SignInState::Ok) { result = std::string(tr("Signed in as")) + " " + S.market->login() + "."; bad = false; ok = true; break; }
            if (st == ps::SignInState::Denied) { result = tr("The sign-in was refused on GitHub."); break; }
            if (st == ps::SignInState::Expired) { result = tr("The code expired. Press Sign in with GitHub again."); break; }
            result = S.market->lastError(); break;
        }
        if (ok) {   // signing in from the Badges window is the choice to earn them: turn them on at the server too, then show the profile
            const bd::Session s = bd::readSession(stateDir(), nowSec());
            const std::string hdr = bd::authHeader(s, nowSec());
            if (!hdr.empty()) {
                plat::httpCall("POST", setUrl, bd::settingsBody(1, -1, nullptr), "application/json", {hdr}, ua(), 15, 1u << 16);
                bd::Profile p = bd::parseMe(plat::httpCall("GET", meUrl, "", "", {hdr}, ua(), 15, 1u << 20).body);
                std::lock_guard<std::mutex> lk(S.mu);
                if (p.ok) { S.me = std::move(p); S.haveMe = true; }
            }
        }
        setMsg(result, bad);
        std::lock_guard<std::mutex> lk(S.mu);
        S.signingIn = false;
        S.sessAt = 0;   // read the new session file at once
    });
    if (!earn()) plat::prefs().setB(kPrefEarn, true);
}

ImU32 tierCol(int t) {
    switch (t) { case 1: return IM_COL32(196, 122, 61, 255); case 2: return IM_COL32(185, 194, 204, 255); case 3: return IM_COL32(230, 180, 34, 255); case 4: return IM_COL32(143, 216, 224, 255); default: return IM_COL32(89, 96, 106, 255); }
}
// A small medal (the website draws the full art): a disc in the tier's colour with a dark centre and one pip per tier.
void medal(int tier, float size) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
    dl->AddCircleFilled(c, size * 0.48f, tierCol(tier), 16);
    dl->AddCircleFilled(c, size * 0.34f, IM_COL32(20, 26, 34, 255), 16);
    for (int i = 0; i < tier; i++) dl->AddCircleFilled(ImVec2(c.x + (i - (tier - 1) * 0.5f) * size * 0.16f, c.y), size * 0.06f, IM_COL32(244, 246, 248, 255), 8);
    ImGui::Dummy(ImVec2(size, size));
}

void signInBlock(App& a) {
    bool signingIn; ps::DeviceStart ds;
    { std::lock_guard<std::mutex> lk(S.mu); signingIn = S.signingIn; ds = S.ds; }
    const bd::Session& s = session();
    if (s.valid(nowSec()) && !signingIn) {
        ImGui::TextColored(pal::okGreen(), "%s %s", tr("Signed in with GitHub as"), s.login.c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(S.busy);
        if (ImGui::SmallButton(tr("Sign out##badges"))) { ensureMarket(); S.market->signOut(); S.sessAt = 0; std::lock_guard<std::mutex> lk(S.mu); S.haveMe = false; }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Signs OnAir out of GitHub for the map badges and the plugin marketplace (one sign-in serves both)."));
        return;
    }
    if (signingIn) {
        if (ds.userCode.empty()) { ImGui::TextDisabled("%s", tr("Asking GitHub for a code...")); return; }
        ImGui::TextWrapped("%s", tr("Open GitHub, sign in there and enter this code:"));
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.6f);
        ImGui::TextUnformatted(ds.userCode.c_str());
        ImGui::PopFont();
        if (ImGui::Button(tr("Open GitHub##badges"))) dect2::openUrl(ds.verificationUri);
        ImGui::SameLine();
        if (ImGui::Button(tr("Copy code##badges"))) ImGui::SetClipboardText(ds.userCode.c_str());
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel##badgesignin"))) S.cancel = true;
        return;
    }
    ImGui::TextWrapped("%s", tr("Sign in with GitHub to earn badges for what you share to the live map, and to show them on a public profile if you want. OnAir shows a code that you enter on github.com; OnAir never sees your password and keeps no GitHub token. The same sign-in is used by the plugin marketplace."));
    ImGui::BeginDisabled(S.busy);
    if (ImGui::Button(tr("Sign in with GitHub##badges"))) signIn(a);
    ImGui::EndDisabled();
}

void drawToast(App& a) {
    const double t = nowS();
    if (S.toastUntil < t) {
        bd::Award w;
        if (toasts().pop(w)) { S.toast = w; S.toastUntil = t + 6.0; }
        else return;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 16.f * ImGui::GetFontSize() / 13.f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - pad, vp->WorkPos.y + vp->WorkSize.y - pad), ImGuiCond_Always, ImVec2(1, 1));
    ImGui::SetNextWindowBgAlpha(0.95f);
    if (ImGui::Begin("##badgetoast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings)) {
        medal(S.toast.tier, ImGui::GetFrameHeight() * 1.4f);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextUnformatted(tr("Badge earned"));
        ImGui::TextColored(pal::okGreen(), "%s", S.toast.name.c_str());
        ImGui::EndGroup();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("View##badgetoast"))) { S.open = true; S.toastUntil = 0; fetchMe(a); }
        ImGui::SameLine();
        if (ImGui::SmallButton("x##badgetoast")) S.toastUntil = 0;
    }
    ImGui::End();
}
}  // namespace

namespace badgesui {

bool& windowOpen() { return S.open; }

void openItem(bool menu) {
    const char* label = tr("Badges...##mapbadges");
    const bool hit = menu ? ImGui::MenuItem(label) : ImGui::Selectable(label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sign in with GitHub and earn badges for what you share to the live map; your profile and the leaderboard are on map.on-air.dev."));
    if (hit) S.open = true;
}

void postExtras(App& a, std::string& authHeader, std::string& home) {
    authHeader.clear(); home.clear();
    const bd::Session& s = session();
    if (!bd::sendToken(earn(), s, nowSec())) return;
    authHeader = bd::authHeader(s, nowSec());
    if (plat::prefs().getB(kPrefHome, false) && receiverLocationSet()) {
        double la = 0, lo = 0;
        receiverLocation(la, lo);
        home = bd::locator6(la, lo);
    }
    (void)a;
}

void onIngestReply(const std::string& body) {
    const bd::IngestAccount acc = bd::parseIngestReply(body);
    if (!acc.awards.empty()) toasts().push(acc.awards);
    std::lock_guard<std::mutex> lk(S.mu);
    S.replyError = acc.error;
}

void window(App& a) {
    drawToast(a);
    if (!S.open) { S.fetchedOnce = false; return; }
    if (!S.fetchedOnce) { S.fetchedOnce = true; if (!S.busy) fetchMe(a); }
    firstWindowSize(560 * gUi, 600 * gUi);
    if (!ImGui::Begin(trw("Badges"), &S.open)) { ImGui::End(); return; }
    bd::Profile me; bool haveMe; std::string msg, replyErr; bool msgBad;
    { std::lock_guard<std::mutex> lk(S.mu); me = S.me; haveMe = S.haveMe; msg = S.msg; msgBad = S.msgBad; replyErr = S.replyError; }
    const bool sess = session().valid(nowSec());
    App& r = root(a);
    const bd::PanelStep step = bd::panelStep(sess, haveMe ? &me : nullptr, earn(), r.mapSend);
    ImGui::TextWrapped("%s", tr(bd::panelStepText(step)));
    if (!replyErr.empty() && sess) ImGui::TextColored(pal::warnAmber(), "%s", replyErr.c_str());
    ImGui::Separator();
    signInBlock(a);
    if (sess) {
        ImGui::Spacing();
        bool e = earn();
        if (ImGui::Checkbox(tr("Earn badges"), &e)) { plat::prefs().setB(kPrefEarn, e); postSettings(a, bd::settingsBody(e ? 1 : 0, -1, nullptr)); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Your map posts carry your OnAir sign-in, and the map counts what you share for stats and badges. Off: posts are anonymous as before."));
        bool pub = haveMe && me.isPublic;
        ImGui::BeginDisabled(!haveMe || S.busy);
        if (ImGui::Checkbox(tr("Public profile and leaderboard"), &pub)) postSettings(a, bd::settingsBody(-1, pub ? 1 : 0, nullptr));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Shows your badges and stats at map.on-air.dev/u/<your GitHub login>, lists you on the leaderboard and puts your top badge next to your map name. Off: only you can see them."));
        bool h = plat::prefs().getB(kPrefHome, false);
        if (ImGui::Checkbox(tr("Use my area for range badges"), &h)) plat::prefs().setB(kPrefHome, h);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Sends the 6-character locator square of your receiver position (about 5 x 2.5 km) with each post, so the map can measure how far you hear. The map uses it for that post and does not store it. Off: no range badges."));
        if (h && !receiverLocationSet()) ImGui::TextDisabled("%s", tr("Set your receiver position first (ADS-B map > receiver location)."));
        if (!r.mapName.empty() && haveMe && me.by != r.mapName) {
            if (ImGui::Button(tr("Link my map name to this account"))) postSettings(a, bd::settingsBody(-1, -1, &r.mapName));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Then only stations signed in with your GitHub account can use the name, and your badge can show next to it."));
        } else if (haveMe && !me.by.empty()) ImGui::TextDisabled("%s %s", tr("Linked map name:"), me.by.c_str());
        if (!r.mapSend) {
            ImGui::TextColored(pal::warnAmber(), "%s", tr("Sharing to the map is off."));
            ImGui::SameLine();
            ImGui::TextDisabled("%s", tr("Turn it on in the Server menu."));
        }
    }
    if (!msg.empty()) { if (msgBad) ImGui::TextColored(pal::warnAmber(), "%s", msg.c_str()); else ImGui::TextDisabled("%s", msg.c_str()); }
    ImGui::Separator();
    ImGui::BeginDisabled(S.busy);
    if (ImGui::Button(tr("Refresh##badges"))) fetchMe(a);
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (haveMe && me.signedIn && ImGui::Button(tr("Open my profile"))) dect2::openUrl(bd::profileUrl(origin(a), me.login));
    ImGui::SameLine();
    if (ImGui::Button(tr("Leaderboard##badges"))) dect2::openUrl(origin(a) + "/leaders");
    if (S.busy) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("Working...")); }
    if (haveMe && me.signedIn) {
        ImGui::Spacing();
        ImGui::Text("%s %d   %s %s   %s %lld   %s %lld (%lld)", tr("Badges:"), me.earnedCount(), tr("Hours:"), bd::fmtValue(me.hours, "").c_str(),
                    tr("Days:"), (long long)me.days, tr("Streak:"), (long long)me.streak, (long long)me.bestStreak);
        ImGui::TextDisabled("%s %lld   %s %lld   %s %lld   %s %lld   %s %lld   %s %s", tr("Objects"), (long long)me.objects, tr("aircraft"), (long long)me.aircraft,
                            tr("ships"), (long long)me.ships, tr("APRS"), (long long)me.aprs, tr("countries"), (long long)me.countries, tr("range"),
                            me.rangeKm > 0 ? bd::fmtValue(me.rangeKm, "km").c_str() : "-");
        if (ImGui::BeginTable("##badgelist", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 0))) {
            ImGui::TableSetupColumn("##m", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight() * 1.3f);
            ImGui::TableSetupColumn(tr("Badge"), ImGuiTableColumnFlags_WidthStretch, 3.f);
            ImGui::TableSetupColumn(tr("Progress"), ImGuiTableColumnFlags_WidthStretch, 2.f);
            for (const auto& b : me.badges) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); medal(b.tier, ImGui::GetFrameHeight() * 1.1f);
                ImGui::TableNextColumn();
                if (b.tier) ImGui::TextUnformatted(b.name.c_str()); else ImGui::TextDisabled("%s", b.name.c_str());
                if (ImGui::IsItemHovered() && !b.desc.empty()) ImGui::SetTooltip("%s", b.desc.c_str());
                if (b.earnedAt > 0) {
                    const std::time_t tt = (std::time_t)(b.earnedAt / 1000);
                    char d[16] = {0}; std::tm tmv{};
#if defined(_WIN32)
                    gmtime_s(&tmv, &tt);
#else
                    gmtime_r(&tt, &tmv);
#endif
                    std::strftime(d, sizeof d, "%Y-%m-%d", &tmv);
                    ImGui::TextDisabled("%s %s", tr("earned"), d);
                }
                ImGui::TableNextColumn();
                ImGui::ProgressBar((float)b.frac, ImVec2(-1, 0), b.progress.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

}  // namespace badgesui
