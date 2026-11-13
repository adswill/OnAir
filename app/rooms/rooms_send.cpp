// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Server > Go live: opt-in "Live rooms". Creates a room on the rooms server, then POSTs a small batch every 2 s:
// one waterfall line a second (512 bins, one byte each), and decoded ACARS text only when the room was started with that box ticked.
// Audio is not sent: the repo has no Opus encoder. The host token goes in the JSON body, never in a URL. The room page shows the viewers.
#include "dect2/common/thread_name.h"
#include "dect2/net/map_guard.h"
#include "rooms.h"
#include "rooms_net.h"
#include "../app.h"
#include "../map/pos_predict.h"
#include "../platform/platform.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#ifndef ONAIR_VERSION
#define ONAIR_VERSION "dev"
#endif

namespace {

constexpr int kBins = 512;
constexpr int kPushMs = 2000;
constexpr int kMaxFails = 5;
enum Phase { kOff = 0, kStarting, kLive, kFailed };

std::atomic<int> gPhase{kOff};
std::atomic<bool> gStop{false};
std::atomic<int> gViewers{0};
std::thread gTh;
std::mutex gMu;                       // guards everything below
std::string gTitle, gMode, gId, gToken, gPin, gUrl;
std::atomic<int> gErr{0};            // a roomsnet::Fail (as int) when the room could not be created, kLost when the connection was lost
constexpr int kLost = 1000;
std::string gErrDetail;               // technical reason of the last failure (under gMu)
roomsnet::AvailCache gAvail;          // under gMu
std::thread gChk;                     // the availability check (never on the UI thread)
std::atomic<bool> gChecking{false}, gCheckDone{false};
bool gWantConsent = false;            // the user clicked Go live while the check ran (UI thread only)
double gFreq = 0;
bool gShareText = false;
std::vector<std::string> gLines;      // base64 waterfall lines not yet sent
std::vector<std::string> gTexts;
std::vector<float> gAcc;              // max level per bin since the last line
std::chrono::steady_clock::time_point gLastLine;
bool gOpenConsent = false;

std::string origin() {
    const char* e = std::getenv("ONAIR_ROOMS_ORIGIN");   // a test server
    return e && *e ? e : "https://rooms.on-air.dev";
}

std::string esc(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) o += ' ';
        else o += (char)c;
    }
    return o;
}

std::string b64(const unsigned char* p, size_t n) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        o += T[v >> 18 & 63]; o += T[v >> 12 & 63];
        o += i + 1 < n ? T[v >> 6 & 63] : '='; o += i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

// "key":"value" in a small flat JSON answer (the server's own; values have no escapes)
std::string jstr(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":\"";
    const size_t p = j.find(k);
    if (p == std::string::npos) return {};
    const size_t e = j.find('"', p + k.size());
    return e == std::string::npos ? std::string() : j.substr(p + k.size(), e - p - k.size());
}
int jint(const std::string& j, const char* key) {
    const std::string k = std::string("\"") + key + "\":";
    const size_t p = j.find(k);
    return p == std::string::npos ? -1 : std::atoi(j.c_str() + p + k.size());
}

std::string userAgent() { return std::string("OnAir/") + ONAIR_VERSION + " (live rooms)"; }

roomsnet::Transport transport() {
    return [](const std::string& m, const std::string& url, const std::string& body, int to) { return plat::httpRequest(m, url, body, userAgent(), to); };
}

void rlog(const std::string& s) { std::fprintf(stderr, "live rooms: %s\n", s.c_str()); }

void checkWorker() {
    const std::string o = origin();
    const roomsnet::Avail a = roomsnet::checkAvailable(o, transport(), 3);
    rlog(std::string(a.ok ? "server reachable: " : "server not available: ") + a.detail);
    {
        std::lock_guard<std::mutex> lk(gMu);
        gAvail.store(o, a, roomsnet::AvailCache::Clock::now());
    }
    gChecking = false;
    gCheckDone = true;
}

void startCheck() {
    if (gChecking.exchange(true)) return;
    if (gChk.joinable()) gChk.join();
    gCheckDone = false;
    gChk = std::thread(checkWorker);
}

void worker() {
    const std::string ua = userAgent();
    std::string reply, body;
    {
        std::lock_guard<std::mutex> lk(gMu);
        char f[48]; std::snprintf(f, sizeof f, "%.6f", gFreq);
        body = "{\"consent\":true,\"title\":\"" + esc(gTitle) + "\",\"mode\":\"" + esc(gMode) + "\",\"freq\":" + f + ",\"transcript\":" + (gShareText ? "true" : "false") + "}";
    }
    {
        const std::string url = origin() + "/api/create";
        const plat::HttpResult r = plat::httpRequest("POST", url, body, ua, 12);
        roomsnet::Fail f = roomsnet::classify(r);
        reply = r.body;
        if (f == roomsnet::Fail::None && (jstr(reply, "id").empty() || jstr(reply, "hostToken").empty())) f = roomsnet::Fail::BadReply;
        if (f != roomsnet::Fail::None) {
            const std::string d = roomsnet::describe(url, r);
            rlog("could not create the room: " + d);
            {
                std::lock_guard<std::mutex> lk(gMu);
                gErrDetail = d;
                if (roomsnet::serverMissing(f)) {
                    roomsnet::Avail a; a.ok = false; a.why = f; a.detail = d;
                    gAvail.store(origin(), a, roomsnet::AvailCache::Clock::now());
                }
            }
            gErr = (int)f;
            gPhase = kFailed;
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lk(gMu);
        gId = jstr(reply, "id"); gToken = jstr(reply, "hostToken"); gPin = jstr(reply, "pin"); gUrl = jstr(reply, "viewUrl");
        gLines.clear(); gTexts.clear();
    }
    gPhase = kLive;
    int fails = 0;
    while (!gStop) {
        for (int i = 0; i < kPushMs / 100 && !gStop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (gStop) break;
        std::string id, tok;
        std::vector<std::string> lines, texts;
        std::string mode; double freq;
        {
            std::lock_guard<std::mutex> lk(gMu);
            id = gId; tok = gToken; mode = gMode; freq = gFreq;
            lines.swap(gLines); texts.swap(gTexts);
        }
        if (lines.size() > 4) lines.erase(lines.begin(), lines.end() - 4);   // the server takes 4 lines per push
        if (dect2::mapguard::holdingNow()) texts.clear();   // decoded text is not uploaded while transmitting and for 10 minutes after (net/map_guard.h)
        if (texts.size() > 20) texts.erase(texts.begin(), texts.end() - 20);
        char f[48]; std::snprintf(f, sizeof f, "%.6f", freq);
        std::string b = "{\"tok\":\"" + tok + "\",\"mode\":\"" + esc(mode) + "\",\"freq\":" + f + ",\"wf\":[";
        for (size_t i = 0; i < lines.size(); i++) b += (i ? ",\"" : "\"") + lines[i] + "\"";
        b += "],\"msgs\":[";
        for (size_t i = 0; i < texts.size(); i++) b += (i ? ",\"" : "\"") + esc(texts[i]) + "\"";
        b += "]}";
        if (plat::postJsonReply(origin() + "/api/r/" + id + "/push", b, ua, reply)) {
            fails = 0;
            const int v = jint(reply, "viewers");
            if (v >= 0) gViewers = v;
        } else if (++fails >= kMaxFails) {
            gErr = kLost;
            { std::lock_guard<std::mutex> lk(gMu); gErrDetail = "the room did not accept data for " + std::to_string(kMaxFails) + " pushes in a row"; }
            gPhase = kFailed;
            break;
        }
    }
    std::string id, tok;
    { std::lock_guard<std::mutex> lk(gMu); id = gId; tok = gToken; }
    plat::postJsonReply(origin() + "/api/r/" + id + "/end", "{\"tok\":\"" + tok + "\"}", ua, reply);
    if (gPhase == kLive) gPhase = kOff;
}

void stopLive() {
    gStop = true;
    if (gTh.joinable()) gTh.join();
    gPhase = kOff;
    gViewers = 0;
    std::lock_guard<std::mutex> lk(gMu);
    gToken.clear(); gPin.clear(); gId.clear(); gUrl.clear(); gLines.clear(); gTexts.clear();
}

void startLive() {
    if (gTh.joinable()) gTh.join();
    gStop = false;
    gErr = 0;
    gPhase = kStarting;
    gTh = dect2::namedThread("onair-rooms", worker);
}

const char* failText(int f) {
    switch ((roomsnet::Fail)f) {
        case roomsnet::Fail::Dns:
        case roomsnet::Fail::NotFound:
        case roomsnet::Fail::Unavailable: return tr("The live rooms server is not available yet.");
        case roomsnet::Fail::Offline: return tr("No internet connection.");
        case roomsnet::Fail::Timeout: return tr("The live rooms server did not answer in time.");
        case roomsnet::Fail::Full: return tr("The live rooms server is full right now. Try again later.");
        case roomsnet::Fail::RateLimited: return tr("Too many rooms were started from here. Try again in a few minutes.");
        case roomsnet::Fail::Refused: return tr("The live rooms server refused the request.");
        case roomsnet::Fail::BadReply: return tr("The live rooms server sent an answer OnAir does not understand.");
        default: return tr("Could not create the room");
    }
}

void curMode(App& a, std::string& mode, double& freq) {
    mode.clear();
    for (const auto& m : allModes()) if (m.family == a.family) { mode = m.name; break; }
    freq = a.freqMhz;
}

}  // namespace

bool roomsLive() { return gPhase.load() == kLive; }

void roomsFeedRow(const std::vector<float>& dbfs, float minDb, float maxDb) {
    if (gPhase.load() != kLive || dbfs.empty() || !(maxDb > minDb)) return;
    std::lock_guard<std::mutex> lk(gMu);
    if (gAcc.size() != (size_t)kBins) gAcc.assign(kBins, -1e9f);
    const size_t n = dbfs.size();
    for (size_t i = 0; i < n; i++) {
        float& d = gAcc[std::min<size_t>(kBins - 1, i * kBins / n)];
        d = std::max(d, dbfs[i]);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - gLastLine < std::chrono::milliseconds(1000)) return;
    gLastLine = now;
    unsigned char row[kBins];
    for (int i = 0; i < kBins; i++) {
        const float v = (gAcc[i] - minDb) / (maxDb - minDb);
        row[i] = (unsigned char)(std::max(0.f, std::min(1.f, v)) * 255.f);
        gAcc[i] = -1e9f;
    }
    gLines.push_back(b64(row, kBins));
    if (gLines.size() > 8) gLines.erase(gLines.begin());
}

void roomsFeedText(const std::string& t) {
    if (gPhase.load() != kLive || t.empty()) return;
    std::lock_guard<std::mutex> lk(gMu);
    if (!gShareText) return;
    gTexts.push_back(t.substr(0, 300));
    if (gTexts.size() > 40) gTexts.erase(gTexts.begin());
}

void roomsShutdown() {
    stopLive();
    if (gChk.joinable()) gChk.join();
}

void roomsMenu(App& a, bool menu) {
    const int ph = gPhase.load();
    if (ph == kLive || ph == kStarting) {
        char b[96];
        std::snprintf(b, sizeof b, "%s  (%d)", tr("Live: stop the room"), gViewers.load());
        const bool hit = menu ? ImGui::MenuItem(b) : ImGui::Button(b);
        if (hit) stopLive();
        std::string url, pin;
        { std::lock_guard<std::mutex> lk(gMu); url = gUrl; pin = gPin; }
        if (!url.empty()) {
            ImGui::TextDisabled("%s", url.c_str());
            if (ImGui::SmallButton(tr("Copy room link"))) ImGui::SetClipboardText(url.c_str());
            ImGui::TextDisabled("%s %s", tr("Host PIN (to moderate on the room page):"), pin.c_str());
        }
        return;
    }
    if (ph == kFailed) {
        std::string d;
        { std::lock_guard<std::mutex> lk(gMu); d = gErrDetail; }
        ImGui::TextDisabled("%s", gErr.load() == kLost ? tr("Lost the connection to the rooms server") : failText(gErr.load()));
        if (ImGui::IsItemHovered() && !d.empty()) ImGui::SetTooltip("%s", d.c_str());
    }
    bool missing = false;
    std::string detail;
    int why = 0;
    {
        std::lock_guard<std::mutex> lk(gMu);
        const auto now = roomsnet::AvailCache::Clock::now();
        missing = gAvail.fresh(origin(), now) && !gAvail.value().ok;
        if (!gAvail.value().ok) { detail = gAvail.value().detail; why = (int)gAvail.value().why; }
    }
    const bool checking = gChecking.load();
    if (checking) {
        ImGui::BeginDisabled();
        menu ? ImGui::MenuItem(tr("Checking the live rooms server...")) : ImGui::Button(tr("Checking the live rooms server..."));
        ImGui::EndDisabled();
        (void)a;
        return;
    }
    if (missing) {
        ImGui::BeginDisabled();
        menu ? ImGui::MenuItem(tr("Go live (Live rooms)...")) : ImGui::Button(tr("Go live (Live rooms)..."));
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(failText(why));
            if (!detail.empty()) ImGui::TextDisabled("%s", detail.c_str());
            ImGui::EndTooltip();
        }
        const bool again = menu ? ImGui::MenuItem(tr("Check the live rooms server again")) : ImGui::SmallButton(tr("Check the live rooms server again"));
        if (again) {
            { std::lock_guard<std::mutex> lk(gMu); gAvail.reset(); }
            gWantConsent = false;
            startCheck();
        }
        (void)a;
        return;
    }
    if (why != 0 && !detail.empty() && ph != kFailed) {   // the last check failed in a way that is not kept (offline, timeout)
        ImGui::TextDisabled("%s", failText(why));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", detail.c_str());
    }
    const bool hit = menu ? ImGui::MenuItem(tr("Go live (Live rooms)...")) : ImGui::Button(tr("Go live (Live rooms)..."));
    if (hit) {
        bool ok = false;
        {
            std::lock_guard<std::mutex> lk(gMu);
            ok = gAvail.fresh(origin(), roomsnet::AvailCache::Clock::now()) && gAvail.value().ok;
        }
        if (ok) gOpenConsent = true;
        else { gWantConsent = true; startCheck(); }
    }
    (void)a;
}

void roomsPopups(App& a) {
    if (gPhase.load() == kLive) {
        std::lock_guard<std::mutex> lk(gMu);
        curMode(a, gMode, gFreq);
    }
    if (gCheckDone.exchange(false)) {
        if (gChk.joinable()) gChk.join();
        bool ok;
        { std::lock_guard<std::mutex> lk(gMu); ok = gAvail.value().ok; }
        if (gWantConsent && ok) gOpenConsent = true;
        gWantConsent = false;
    }
    if (gOpenConsent) { ImGui::OpenPopup("##roomsconsent"); gOpenConsent = false; }
    if (!ImGui::BeginPopupModal("##roomsconsent", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    static char title[64] = "";
    static bool share = false;
    ImGui::PushTextWrapPos(480 * gUi);
    ImGui::TextUnformatted(tr("Live rooms: while you are live, anyone with the room link can watch this receiver's waterfall, the mode and the frequency you are tuned to, and chat with each other. The room list on the web also shows its title. Nothing is sent before you press Go live, and nothing is stored after the room ends. Your location, your name and the audio are never sent."));
    ImGui::PopTextWrapPos();
    ImGui::Dummy(ImVec2(0, 4 * gUi));
    ImGui::SetNextItemWidth(300 * gUi);
    ImGui::InputTextWithHint("##roomtitle", tr("Room title"), title, sizeof title);
    const bool canText = a.mapSend && a.mapMsgs;
    if (!canText) share = false;
    ImGui::BeginDisabled(!canText);
    ImGui::Checkbox(tr("Also show decoded ACARS text in the room (public)"), &share);
    ImGui::EndDisabled();
    if (!canText) ImGui::TextDisabled("%s", tr("Needs Server > Send data to map server and Also share interesting ACARS messages."));
    ImGui::Dummy(ImVec2(0, 6 * gUi));
    if (ImGui::Button(tr("Go live"), fitButton(tr("Go live"), 160 * gUi))) {
        {
            std::lock_guard<std::mutex> lk(gMu);
            gTitle = title[0] ? title : "OnAir receiver";
            gShareText = share;
            curMode(a, gMode, gFreq);
            gAcc.clear();
        }
        startLive();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Cancel"), fitButton(tr("Cancel"), 120 * gUi)) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
