// The Watchlists window (View > Watchlists): lists of aircraft, ships, APRS call signs, frequencies and words, and how to be told when one is heard
// (desktop notification, sound, ntfy, Telegram bot, webhook), with a rate limit and quiet hours. The matching is in core/src/watchlist; this file is
// the window, the saved file and the sink that shows the alerts and sends the web requests. Tokens are written to the file only, never to a log.
#include "dect2/common/time_fmt.h"
#include "dect2/common/thread_name.h"
#include "../app.h"
#include "../panels/automation_internal.h"
#include "dect2/automation/engine.h"
#include "dect2/common/u8file.h"
#include "dect2/watchlist/watchlist.h"
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
namespace wl = dect2::watchlist;
namespace au = dect2::automation;
namespace fs = std::filesystem;

constexpr size_t kEntriesBuf = 16384;

struct St {
    bool started = false;
    std::vector<wl::List> lists;
    wl::Settings set;
    int sel = -1;
    char name[80] = "";
    char entries[kEntriesBuf] = "";
    char sound[300] = "";
    char ntfyServer[200] = "", ntfyTopic[80] = "", ntfyToken[300] = "";
    char tgToken[120] = "", tgChat[80] = "";
    char hookUrl[400] = "", hookToken[300] = "";
    double lastTz = 0;
    std::string note;
    bool dirty = false;
};
St S;

// web requests of the push channels: one worker, one request at a time; what it reports never holds an address or a token
std::mutex gMu;
std::condition_variable gCv;
std::deque<wl::Request> gQ;
std::thread gTh;
std::string gStatus;

void worker() {
    for (;;) {
        wl::Request r;
        {
            std::unique_lock<std::mutex> lk(gMu);
            gCv.wait(lk, [] { return !gQ.empty(); });
            r = std::move(gQ.front());
            gQ.pop_front();
        }
        const bool ok = plat::postRequest(r.url, r.body, r.contentType, r.headers, std::string("OnAir/") + ONAIR_VERSION + " (watchlist)");
        std::lock_guard<std::mutex> lk(gMu);
        gStatus = r.label + (ok ? ": sent" : ": failed");
    }
}

class AppSink : public wl::Sink {
public:
    void notify(const std::string& title, const std::string& body) override {
        std::string d;
        automationapp::sink().perform("notify", {{"title", title}, {"body", body}}, d);
    }
    void sound(const std::string& file) override {
        std::string d;
        automationapp::sink().perform("sound", {{"file", file}}, d);
    }
    void post(const wl::Request& r) override {
        {
            std::lock_guard<std::mutex> lk(gMu);
            if (!gTh.joinable()) gTh = dect2::namedThread("onair-watch", worker);
            if (gQ.size() >= 50) return;
            gQ.push_back(r);
        }
        gCv.notify_one();
    }
};
AppSink gSink;

std::string dir() { return plat::dataDir() + "/watchlists"; }

int tzOffset() {
    const time_t t = time(nullptr);
    struct tm l = *localtime(&t), g = *gmtime(&t);
    l.tm_isdst = 0; g.tm_isdst = 0;
    return (int)difftime(mktime(&l), mktime(&g));
}

std::string newId() {
    for (int i = 1;; i++) {
        const std::string id = "w" + std::to_string(i);
        bool used = false;
        for (const wl::List& l : S.lists) if (l.id == id) used = true;
        if (!used) return id;
    }
}

void apply() {
    S.set.tzOffsetSec = tzOffset();
    wl::Watcher& w = wl::global();
    w.setLists(S.lists);
    w.setSettings(S.set);
    bool any = false;
    for (const wl::List& l : S.lists) if (l.enabled && !l.values.empty()) any = true;
    // the decoders build events for a family only when somebody wants them: hook in only while a list can match
    const au::FamilyMask mask = au::kAllFamilies & ~(au::familyBit(au::FTime) | au::familyBit(au::FReceiver) | au::familyBit(au::FSatellite) | au::familyBit(au::FVoice) | au::familyBit(au::FSession));
    if (S.set.enabled && any) au::global().setWatcher([](const au::Event& e) { wl::global().submit(e); }, mask);
    else au::global().setWatcher(nullptr, 0);
}

void save() {
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    const std::string path = dir() + "/watchlists.json";
    {
        std::ofstream o(dect2::u8file(path), std::ios::binary | std::ios::trunc);
        o << wl::toJson(S.lists, S.set);
    }
    fs::permissions(fs::path(reinterpret_cast<const char8_t*>(path.c_str())), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);   // the file holds the user's tokens
}

void load() {
    std::ifstream f(dect2::u8file(dir() + "/watchlists.json"), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    std::vector<wl::List> l;
    wl::Settings s;
    if (wl::fromJson(ss.str(), l, s, err)) { S.lists = l; S.set = s; }
    else S.note = std::string(tr("The watchlist file could not be read:")) + " " + err;
}

void copyTo(char* dst, size_t n, const std::string& s) { snprintf(dst, n, "%s", s.c_str()); }

void loadBuffers() {
    const wl::Channels& c = S.set.ch;
    copyTo(S.sound, sizeof S.sound, c.soundFile);
    copyTo(S.ntfyServer, sizeof S.ntfyServer, c.ntfyServer);
    copyTo(S.ntfyTopic, sizeof S.ntfyTopic, c.ntfyTopic);
    copyTo(S.ntfyToken, sizeof S.ntfyToken, c.ntfyToken);
    copyTo(S.tgToken, sizeof S.tgToken, c.telegramToken);
    copyTo(S.tgChat, sizeof S.tgChat, c.telegramChat);
    copyTo(S.hookUrl, sizeof S.hookUrl, c.webhookUrl);
    copyTo(S.hookToken, sizeof S.hookToken, c.webhookToken);
}

void select(int i) {
    S.sel = i;
    if (i >= 0 && i < (int)S.lists.size()) {
        copyTo(S.name, sizeof S.name, S.lists[(size_t)i].name);
        copyTo(S.entries, sizeof S.entries, wl::joinValues(S.lists[(size_t)i].values));
    }
}

void start() {
    S.started = true;
    load();
    loadBuffers();
    wl::global().setSink(&gSink);
    wl::global().start();
    apply();
    if (!S.lists.empty()) select(0);
}

const char* const kKinds[] = {N_("Aircraft"), N_("Ships"), N_("APRS call signs"), N_("Frequencies"), N_("Words in messages")};
const wl::Kind kKindOf[] = {wl::Kind::Aircraft, wl::Kind::Ship, wl::Kind::Aprs, wl::Kind::Frequency, wl::Kind::Words};

const char* kindHint(wl::Kind k) {
    switch (k) {
    case wl::Kind::Aircraft: return N_("One per line: an ICAO hex address (4840D6), a registration (A6-EDA) or a call sign (KLM1023). A trailing * matches the start (KLM*). Heard by ADS-B, ACARS, VDL2 and HFDL.");
    case wl::Kind::Ship: return N_("One per line: an MMSI number, a ship name or a call sign. A trailing * matches the start (2190*). Heard by AIS and DSC. The entry DSC distress alerts on every DSC distress call.");
    case wl::Kind::Aprs: return N_("One per line: a call sign. Without an SSID (LY1ABC) every SSID matches; with one (LY1ABC-9) only that one.");
    case wl::Kind::Frequency: return N_("One per line: a frequency in MHz (162.025), with a unit (162025 kHz) or a range (144-146). Anything decoded on it counts.");
    case wl::Kind::Words: return N_("One per line: a word or a phrase (not case sensitive). A trailing * matches word starts (fire*). Searched in pager, ACARS, APRS and other decoded text.");
    }
    return "";
}

std::string when(double t) {
    return dect2::timefmt::show((int64_t)t, dect2::timefmt::Style::DateClock);
}

bool minuteField(const char* label, const char* idBase, int& minutes) {
    int h = minutes / 60, m = minutes % 60;
    bool ch = false;
    ImGui::TextUnformatted(label);
    ImGui::SameLine(150 * gUi);
    ImGui::SetNextItemWidth(60 * gUi);
    char id[32];
    snprintf(id, sizeof id, "##h%s", idBase);
    ch |= ImGui::InputInt(id, &h, 0, 0);
    ImGui::SameLine();
    ImGui::TextUnformatted(":");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60 * gUi);
    snprintf(id, sizeof id, "##m%s", idBase);
    ch |= ImGui::InputInt(id, &m, 0, 0);
    if (ch) minutes = std::max(0, std::min(23, h)) * 60 + std::max(0, std::min(59, m));
    return ch;
}

void listsTab() {
    ImGui::BeginChild("lists", ImVec2(200 * gUi, 0), true);
    for (size_t i = 0; i < S.lists.size(); i++) {
        wl::List& l = S.lists[i];
        ImGui::PushID((int)i);
        if (!l.enabled) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (ImGui::Selectable(l.name.empty() ? tr("(no name)") : l.name.c_str(), S.sel == (int)i)) select((int)i);
        if (!l.enabled) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##addkind", tr("New list..."))) {
        for (int k = 0; k < 5; k++) {
            if (!ImGui::Selectable(tr(kKinds[k]))) continue;
            wl::List l;
            l.id = newId();
            l.kind = kKindOf[k];
            l.name = tr(kKinds[k]);
            S.lists.push_back(l);
            select((int)S.lists.size() - 1);
            S.dirty = true;
        }
        ImGui::EndCombo();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("edit", ImVec2(0, 0), false);
    if (S.sel < 0 || S.sel >= (int)S.lists.size()) {
        ImGui::TextWrapped("%s", tr("Make a list on the left: aircraft, ships, APRS call signs, frequencies or words. OnAir tells you when something on it is heard."));
        ImGui::EndChild();
        return;
    }
    wl::List& l = S.lists[(size_t)S.sel];
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::InputText(tr("Name"), S.name, sizeof S.name)) { l.name = S.name; S.dirty = true; }
    ImGui::SameLine();
    if (ImGui::Checkbox(tr("On"), &l.enabled)) S.dirty = true;
    int ki = 0;
    for (int k = 0; k < 5; k++) if (kKindOf[k] == l.kind) ki = k;
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo(tr("Kind"), tr(kKinds[ki]))) {
        for (int k = 0; k < 5; k++) if (ImGui::Selectable(tr(kKinds[k]), k == ki)) { l.kind = kKindOf[k]; S.dirty = true; }
        ImGui::EndCombo();
    }
    ImGui::TextWrapped("%s", tr(kindHint(l.kind)));
    if (l.kind == wl::Kind::Frequency) {
        float khz = (float)(l.tolHz / 1000.0);
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::InputFloat(tr("Tolerance (kHz)"), &khz, 0, 0, "%.1f")) { l.tolHz = std::max(0.0, std::min(10000.0, (double)khz * 1000.0)); S.dirty = true; }
    }
    if (ImGui::InputTextMultiline("##entries", S.entries, sizeof S.entries, ImVec2(-1, std::max(120.f, ImGui::GetContentRegionAvail().y - 60 * gUi)))) {
        l.values = wl::splitValues(S.entries);
        S.dirty = true;
    }
    ImGui::TextDisabled("%d %s", (int)l.values.size(), tr("entries"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Delete list"))) {
        S.lists.erase(S.lists.begin() + S.sel);
        select(S.lists.empty() ? -1 : std::min(S.sel, (int)S.lists.size() - 1));
        S.dirty = true;
    }
    ImGui::EndChild();
}

void alertsTab() {
    wl::Channels& c = S.set.ch;
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Desktop notification"), &c.notify);
    ch |= ImGui::Checkbox(tr("Sound (follows Mute)"), &c.sound);
    if (c.sound) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260 * gUi);
        if (ImGui::InputTextWithHint("##snd", tr("WAV file, empty = three beeps"), S.sound, sizeof S.sound)) { c.soundFile = S.sound; ch = true; }
    }
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr("Push alerts go to a service you choose, with the address and token you give. They are stored in your data folder and never written to a log."));
    ch |= ImGui::Checkbox("ntfy", &c.ntfy);
    if (c.ntfy) {
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputText(tr("Server##ntfy"), S.ntfyServer, sizeof S.ntfyServer)) { c.ntfyServer = S.ntfyServer; ch = true; }
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputText(tr("Topic##ntfy"), S.ntfyTopic, sizeof S.ntfyTopic)) { c.ntfyTopic = S.ntfyTopic; ch = true; }
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputTextWithHint(tr("Token##ntfy"), tr("optional"), S.ntfyToken, sizeof S.ntfyToken, ImGuiInputTextFlags_Password)) { c.ntfyToken = S.ntfyToken; ch = true; }
    }
    ch |= ImGui::Checkbox("Telegram", &c.telegram);
    if (c.telegram) {
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputTextWithHint(tr("Bot token"), "123456:ABC...", S.tgToken, sizeof S.tgToken, ImGuiInputTextFlags_Password)) { c.telegramToken = S.tgToken; ch = true; }
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputTextWithHint(tr("Chat id"), "-1001234567890", S.tgChat, sizeof S.tgChat)) { c.telegramChat = S.tgChat; ch = true; }
    }
    ch |= ImGui::Checkbox(tr("Webhook"), &c.webhook);
    if (c.webhook) {
        ImGui::SetNextItemWidth(400 * gUi);
        if (ImGui::InputTextWithHint(tr("Address##hook"), "https://", S.hookUrl, sizeof S.hookUrl)) { c.webhookUrl = S.hookUrl; ch = true; }
        ImGui::SetNextItemWidth(300 * gUi);
        if (ImGui::InputTextWithHint(tr("Token##hook"), tr("optional, sent as a Bearer token"), S.hookToken, sizeof S.hookToken, ImGuiInputTextFlags_Password)) { c.webhookToken = S.hookToken; ch = true; }
    }
    if (ch) S.dirty = true;
    ImGui::Separator();
    if (ImGui::Button(tr("Send a test alert"))) dect2::namedThread("onair-wltest", [] { wl::global().sendTest(); }).detach();   // a thread: the sound and the banner are made on this window's thread
    std::string st;
    {
        std::lock_guard<std::mutex> lk(gMu);
        st = gStatus;
    }
    if (!st.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", st.c_str()); }
}

void limitsTab() {
    wl::Settings& s = S.set;
    bool ch = false;
    ImGui::SetNextItemWidth(100 * gUi);
    ch |= ImGui::InputInt(tr("Most alerts per hour (0 = no limit)"), &s.maxPerHour, 0, 0);
    ImGui::SetNextItemWidth(100 * gUi);
    int minutes = s.cooldownSec / 60;
    if (ImGui::InputInt(tr("Same station again after (minutes)"), &minutes, 0, 0)) { s.cooldownSec = std::max(0, minutes) * 60; ch = true; }
    s.maxPerHour = std::max(0, std::min(10000, s.maxPerHour));
    ImGui::Separator();
    ch |= ImGui::Checkbox(tr("Quiet hours (no alerts, still listed in the history)"), &s.quietOn);
    if (s.quietOn) {
        ch |= minuteField(tr("From"), "a", s.quietStartMin);
        ch |= minuteField(tr("Until"), "b", s.quietEndMin);
        ImGui::TextDisabled("%s", tr("Local time of this computer."));
    }
    if (ch) S.dirty = true;
}

void historyTab() {
    const std::vector<wl::Hit> h = wl::global().history();
    ImGui::TextDisabled("%llu %s", (unsigned long long)wl::global().alertsSent(), tr("alerts sent"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Clear"))) wl::global().clearHistory();
    if (ImGui::BeginTable("hist", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Time"), ImGuiTableColumnFlags_WidthFixed, 150 * gUi);
        ImGui::TableSetupColumn(tr("List"), ImGuiTableColumnFlags_WidthFixed, 120 * gUi);
        ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(tr("What happened"), ImGuiTableColumnFlags_WidthFixed, 200 * gUi);
        ImGui::TableHeadersRow();
        for (size_t i = h.size(); i-- > 0;) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(when(h[i].time).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h[i].list.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(h[i].summary.c_str());
            ImGui::TableNextColumn();
            if (h[i].delivered) ImGui::TextUnformatted(h[i].result.c_str());
            else ImGui::TextDisabled("%s", h[i].why == "quiet hours" ? tr("held back: quiet hours") : tr("held back: rate limit"));
        }
        ImGui::EndTable();
    }
}

void windowPanel(App& a) {
    firstWindowSize(820 * gUi, 520 * gUi);
    if (!ImGui::Begin(trw("Watchlists"), &a.watchlistOpen)) { ImGui::End(); return; }
    if (ImGui::Checkbox(tr("Watch for these"), &S.set.enabled)) S.dirty = true;
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }
    if (ImGui::BeginTabBar("wltabs")) {
        if (ImGui::BeginTabItem(trw("Lists"))) { listsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Alerts"))) { alertsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Limits"))) { limitsTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("History"))) { historyTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (S.dirty) { S.dirty = false; apply(); save(); }
    ImGui::End();
}
}  // namespace

void watchlistWindow(App& a) {
    if (a.rxId != 1) {   // one set of lists for the program: RX 1's window
        if (App* f = receiverById(1)) { f->watchlistOpen = a.watchlistOpen; watchlistWindow(*f); a.watchlistOpen = f->watchlistOpen; }
        return;
    }
    if (!S.started) start();
    if (a.watchlistOpen) windowPanel(a);
    else if (ImGui::GetTime() - S.lastTz > 600) { S.lastTz = ImGui::GetTime(); apply(); }   // keeps the local-time offset of the quiet hours current
}
