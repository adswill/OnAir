// The Integrations window (View > Integrations): decoded events out as MQTT messages (optionally with Home Assistant discovery), a webhook post and a
// JSON-lines file. Everything is off until the user switches a sink on and types the broker / address; no server is ever contacted by default.
// The password and token are written to integrations/integrations.json (owner-only) and never to a log or the status text. The work is in
// core/src/integrations; this file is the window, the saved file and the web post.
#include "../app.h"
#include "dect2/automation/engine.h"
#include "dect2/common/u8file.h"
#include "dect2/integrations/integrations.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
namespace ig = dect2::integrations;
namespace au = dect2::automation;
namespace fs = std::filesystem;

struct St {
    bool started = false, dirty = false;
    ig::Settings set;
    char host[200] = "", user[120] = "", pass[200] = "", clientId[80] = "", topic[200] = "", haPrefix[80] = "", haBase[80] = "";
    char hookUrl[400] = "", hookToken[300] = "", jsonlPath[400] = "", arcFolder[400] = "";
    std::string note;
};
St S;

std::string dir() { return plat::dataDir() + "/integrations"; }

void copyTo(char* dst, size_t n, const std::string& s) { snprintf(dst, n, "%s", s.c_str()); }

void loadBuffers() {
    const ig::Settings& s = S.set;
    copyTo(S.host, sizeof S.host, s.mqtt.host);
    copyTo(S.user, sizeof S.user, s.mqtt.user);
    copyTo(S.pass, sizeof S.pass, s.mqtt.password);
    copyTo(S.clientId, sizeof S.clientId, s.mqtt.clientId);
    copyTo(S.topic, sizeof S.topic, s.mqtt.topicTemplate);
    copyTo(S.haPrefix, sizeof S.haPrefix, s.mqtt.haPrefix);
    copyTo(S.haBase, sizeof S.haBase, s.mqtt.haBase);
    copyTo(S.hookUrl, sizeof S.hookUrl, s.webhook.url);
    copyTo(S.hookToken, sizeof S.hookToken, s.webhook.token);
    copyTo(S.jsonlPath, sizeof S.jsonlPath, s.jsonl.path);
    copyTo(S.arcFolder, sizeof S.arcFolder, s.archive.folder);
}

void save() {
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(dir().c_str())), ec);
    const std::string path = dir() + "/integrations.json";
    {
        std::ofstream o(dect2::u8file(path), std::ios::binary | std::ios::trunc);
        o << ig::toJson(S.set);
    }
    fs::permissions(fs::path(reinterpret_cast<const char8_t*>(path.c_str())), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);   // the file holds the user's password and token
}

void load() {
    std::ifstream f(dect2::u8file(dir() + "/integrations.json"), std::ios::binary);
    if (!f) return;   // first run: everything off, no families ticked beyond the default set, no server
    std::stringstream ss;
    ss << f.rdbuf();
    std::string err;
    ig::Settings s;
    if (ig::fromJson(ss.str(), s, err)) S.set = s;
    else S.note = std::string(tr("The integrations file could not be read:")) + " " + err;
}

void apply() {
    ig::Hub& h = ig::global();
    h.setSettings(S.set);
    if (S.set.hookFamilies()) au::global().setPublisher([](const au::Event& e) { ig::global().submit(e); }, S.set.hookFamilies());
    else au::global().setPublisher(nullptr, 0);
}

void start() {
    S.started = true;
    S.set.families = ig::defaultFamilies();   // what goes out once a sink is switched on; nothing goes anywhere before that
    load();
    loadBuffers();
    ig::global().setPost([](const std::string& url, const std::string& body, const std::vector<std::string>& headers) {
        return plat::postRequest(url, body, "application/json", headers, std::string("OnAir/") + ONAIR_VERSION + " (integrations)");
    });
    ig::global().start();
    apply();
}

struct FamilyRow { int fam; const char* label; };
const FamilyRow kFamilies[] = {
    {au::FAdsb, N_("Aircraft (ADS-B)")}, {au::FAcars, N_("ACARS")}, {au::FVdl2, N_("VDL Mode 2")}, {au::FHfdl, N_("HFDL")}, {au::FPager, N_("Pagers")},
    {au::FAis, N_("Ships (AIS)")}, {au::FDsc, N_("DSC")}, {au::FAprs, N_("APRS")}, {au::FSonde, N_("Radiosondes")}, {au::FGnss, N_("GNSS")}, {au::FLora, N_("LoRa and mesh")},
    {au::FBle, N_("Bluetooth")}, {au::FRid, N_("Drone Remote ID")}, {au::FSignal, N_("Signals (SARSAT, beacons, ...)")}, {au::FMsg, N_("Text messages (Inmarsat-C, Aero, NAVTEX ...)")}, {au::FVoice, N_("Digital voice calls (who called whom, never audio)")},
    {au::FPlugin, N_("Plug-in events")}, {au::FSession, N_("Finished sessions")}, {au::FSatellite, N_("Satellite passes")}, {au::FTime, N_("Time signals")}, {au::FReceiver, N_("Receiver status")},
};

bool field(const char* label, const char* id, char* buf, size_t n, std::string& dst, float w = 300, const char* hint = nullptr, bool password = false) {
    ImGui::SetNextItemWidth(w * gUi);
    const ImGuiInputTextFlags fl = password ? ImGuiInputTextFlags_Password : 0;
    char lab[160];
    snprintf(lab, sizeof lab, "%s##%s", label, id);
    const bool ch = hint ? ImGui::InputTextWithHint(lab, hint, buf, n, fl) : ImGui::InputText(lab, buf, n, fl);
    if (ch) dst = buf;
    return ch;
}

void mqttTab() {
    ig::MqttSettings& m = S.set.mqtt;
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Publish to an MQTT broker"), &m.enabled);
    ImGui::TextWrapped("%s", tr("OnAir connects only to the broker you type here, and only while this is ticked. There is no default server."));
    ch |= field(tr("Broker address"), "host", S.host, sizeof S.host, m.host, 260, tr("e.g. 192.168.1.20 or broker.example.org"));
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputInt(tr("Port##mqtt"), &m.port, 0, 0)) { m.port = std::max(1, std::min(65535, m.port)); ch = true; }
    if (ImGui::Checkbox(tr("TLS (encrypted)"), &m.tls)) { if (m.tls && m.port == 1883) m.port = 8883; if (!m.tls && m.port == 8883) m.port = 1883; ch = true; }
    ch |= field(tr("User"), "user", S.user, sizeof S.user, m.user, 200, tr("optional"));
    ch |= field(tr("Password"), "pass", S.pass, sizeof S.pass, m.password, 200, tr("optional"), true);
    ch |= field(tr("Client id"), "cid", S.clientId, sizeof S.clientId, m.clientId, 200, tr("empty: made up at random"));
    ImGui::Separator();
    ch |= field(tr("Topic"), "topic", S.topic, sizeof S.topic, m.topicTemplate, 360);
    const std::string bad = ig::checkTopicTemplate(m.topicTemplate);
    if (!bad.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", bad.c_str());
    else {
        au::Event ex("adsb.aircraft");
        ex.at(0, "ADS-B", 1090e6).str("hex", "4840D6").str("callsign", "KLM1023");
        ImGui::TextDisabled("%s %s", tr("Example:"), ig::expandTopic(m.topicTemplate, ex).c_str());
    }
    ImGui::TextDisabled("%s", tr("Placeholders: {family} {type} {sub} {mode} {ident} {freq_mhz} {field:NAME}"));
    ch |= ImGui::Checkbox(tr("Retain messages (the broker keeps the last one per topic)"), &m.retain);
    int q = m.qos;
    ImGui::SetNextItemWidth(220 * gUi);
    if (ImGui::Combo(tr("Delivery"), &q, "At most once (QoS 0)\0At least once (QoS 1)\0")) { m.qos = q; ch = true; }
    ImGui::Separator();
    ch |= ImGui::Checkbox(tr("Home Assistant discovery"), &m.haDiscovery);
    if (m.haDiscovery) {
        ImGui::TextWrapped("%s", tr("Announces five sensors (last aircraft, vessel, APRS station, pager message, radiosonde) and an online / offline state. The announcements are retained, as Home Assistant expects."));
        ch |= field(tr("Discovery prefix"), "hapre", S.haPrefix, sizeof S.haPrefix, m.haPrefix, 200);
        ch |= field(tr("State topic base"), "habase", S.haBase, sizeof S.haBase, m.haBase, 200);
    }
    if (ImGui::Button(tr("Send a test message"))) { apply(); ig::global().sendTest(); }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", ig::global().stats().mqttStatus.empty() ? tr("off") : ig::global().stats().mqttStatus.c_str());
    if (ch) S.dirty = true;
}

void webTab() {
    ig::WebhookSettings& w = S.set.webhook;
    ig::JsonlSettings& j = S.set.jsonl;
    bool ch = false;
    ch |= ImGui::Checkbox(tr("Post every event to a webhook"), &w.enabled);
    ImGui::TextWrapped("%s", tr("One JSON object per event, posted to the address you give. Sent only while this is ticked."));
    ch |= field(tr("Address"), "hook", S.hookUrl, sizeof S.hookUrl, w.url, 360, "https://");
    if (!w.url.empty()) { const std::string p = ig::httpUrlProblem(w.url); if (!p.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", p.c_str()); }
    ch |= field(tr("Token"), "hooktok", S.hookToken, sizeof S.hookToken, w.token, 300, tr("optional, sent as a Bearer token"), true);
    ImGui::TextDisabled("%s", ig::global().stats().webhookStatus.c_str());
    ImGui::Separator();
    ch |= ImGui::Checkbox(tr("Append every event to a JSON-lines file"), &j.enabled);
    ch |= field(tr("File"), "jsonl", S.jsonlPath, sizeof S.jsonlPath, j.path, 360, tr("a path in a folder you can write to"));
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::InputInt(tr("Rotate at (MB, 0 = never)"), &j.maxMegabytes, 0, 0)) { j.maxMegabytes = std::max(0, j.maxMegabytes); ch = true; }
    ImGui::TextDisabled("%s", ig::global().stats().jsonlStatus.c_str());
    if (ImGui::Button(tr("Send a test message##web"))) { apply(); ig::global().sendTest(); }
    if (ch) S.dirty = true;
}

// "Save decoded messages": one toggle, a folder, a format. Every mode that decodes text or messages feeds it (ADS-B, ACARS, VDL2, HFDL, POCSAG/FLEX, APRS, AIS,
// Inmarsat-C, Aero, NAVTEX, DSC, LoRa ...). One file per day per mode: onair-<mode>-<YYYY-MM-DD>.<jsonl|json|csv>.
void saveTab() {
    ig::ArchiveSettings& a = S.set.archive;
    bool ch = false;
    if (ImGui::Checkbox(tr("Save decoded messages to files"), &a.enabled)) {
        if (a.enabled && a.folder.empty()) { a.folder = plat::dataDir() + "/messages"; copyTo(S.arcFolder, sizeof S.arcFolder, a.folder); }
        ch = true;
    }
    ImGui::TextWrapped("%s", tr("Every message OnAir decodes, in every mode, is appended to a file in the folder below: one file per day per mode (for example onair-Inmarsat-C-2026-10-31.jsonl). Each record has the UTC time, mode, sender / id, frequency, signal quality and the message text with all decoded fields."));
    ch |= field(tr("Folder"), "arcdir", S.arcFolder, sizeof S.arcFolder, a.folder, 380, tr("a folder you can write to"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Use the default"))) { a.folder = plat::dataDir() + "/messages"; copyTo(S.arcFolder, sizeof S.arcFolder, a.folder); ch = true; }
    int f = (int)a.format;
    ImGui::SetNextItemWidth(260 * gUi);
    if (ImGui::Combo(tr("Format"), &f, "JSON Lines (one message per line)\0JSON (one array per file)\0CSV (opens in a spreadsheet)\0")) { a.format = (ig::ArchiveFormat)f; ch = true; }
    const ig::Stats s = ig::global().stats();
    ImGui::TextDisabled("%s", s.archiveStatus.empty() ? tr("off") : s.archiveStatus.c_str());
    ImGui::Text("%s %llu, %s %llu", tr("saved"), (unsigned long long)s.archiveRecords, tr("failed"), (unsigned long long)s.archiveFailed);
    ImGui::TextDisabled("%s", tr("This is separate from the message database and from the UDP output (Settings > Server), which sends over the network and writes no file."));
    if (ch) S.dirty = true;
}

void whatTab() {
    bool ch = false;
    ImGui::TextWrapped("%s", tr("Choose what goes out. Voice entries are call records (who, which talkgroup), never audio. Encrypted traffic is not decoded, so nothing of it goes out."));
    for (const FamilyRow& r : kFamilies) {
        bool on = (S.set.families >> r.fam & 1) != 0;
        if (ImGui::Checkbox(tr(r.label), &on)) {
            if (on) S.set.families |= au::familyBit(r.fam); else S.set.families &= ~au::familyBit(r.fam);
            ch = true;
        }
    }
    ImGui::Separator();
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputInt(tr("At most this many events per second (0 = no limit)"), &S.set.maxPerSecond, 0, 0)) { S.set.maxPerSecond = std::max(0, S.set.maxPerSecond); ch = true; }
    if (ch) S.dirty = true;
}

void statusTab() {
    const ig::Stats s = ig::global().stats();
    ImGui::Text("MQTT: %s", s.mqttStatus.empty() ? tr("off") : s.mqttStatus.c_str());
    ImGui::Text("%s %llu, %s %llu", tr("sent"), (unsigned long long)s.mqttSent, tr("failed"), (unsigned long long)s.mqttFailed);
    ImGui::Text("Webhook: %llu %s, %llu %s", (unsigned long long)s.webhookSent, tr("sent"), (unsigned long long)s.webhookFailed, tr("failed"));
    ImGui::Text("%s: %llu", tr("Saved messages"), (unsigned long long)s.archiveRecords);
    ImGui::Text("JSON-lines: %llu %s", (unsigned long long)s.jsonlLines, tr("lines"));
    ImGui::Text("%s %llu, %s %llu", tr("Dropped (queue full):"), (unsigned long long)s.dropped, tr("over the rate limit:"), (unsigned long long)s.rateDropped);
    ImGui::Separator();
    ImGui::TextWrapped("%s", tr("The password and tokens are stored in the integrations folder of your data folder (readable by you only). They are never put into a log, a message or this window. Nothing is sent to the OnAir map by this window."));
}

void windowPanel(App& a) {
    firstWindowSize(640 * gUi, 560 * gUi);
    if (!ImGui::Begin(trw("Integrations"), &a.integrationsOpen)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", tr("Send what OnAir decodes to other programs: MQTT (Home Assistant, Node-RED ...), a webhook or a file. Everything is off until you tick it and type the address."));
    if (!S.note.empty()) ImGui::TextDisabled("%s", S.note.c_str());
    if (ImGui::BeginTabBar("igtabs")) {
        if (ImGui::BeginTabItem(trw("Save messages"))) { saveTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("MQTT"))) { mqttTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Webhook and file"))) { webTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("What to send"))) { whatTab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Status"))) { statusTab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    if (S.dirty) { S.dirty = false; apply(); save(); }
    ImGui::End();
}
}  // namespace

void integrationsWindow(App& a) {
    if (a.rxId != 1) {   // one set of integrations for the program: RX 1's window
        if (App* f = receiverById(1)) { f->integrationsOpen = a.integrationsOpen; integrationsWindow(*f); a.integrationsOpen = f->integrationsOpen; }
        return;
    }
    if (!S.started) start();
    if (a.integrationsOpen) windowPanel(a);
}
