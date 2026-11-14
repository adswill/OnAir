// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// View > LAN swarm: opt-in cooperation between OnAir nodes on this local network. Discovery (swarm_net), the swarm view, pairing, the encrypted control
// channel (swarm_ctl), pooled results and tracks, coordinated scan and memory sync. Off every time the program starts; before it is switched on the
// exact announcement is shown. The rules and the threat model are in core/include/dect2/net/swarm.h and docs/LAN-SWARM.md.
// NOT yet compiled in the app (see docs/LAN-SWARM.md, NEEDS BUILD/TEST).
#include "../app.h"
#include "../memory/memory_options.h"
#include "../ui/typography.h"
#include "dect2/common/u8file.h"
#include "dect2/msgdb/msgdb.h"
#include "dect2/net/swarm_ctl.h"
#include "dect2/net/swarm_net.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include "dect2/common/diag.h"

namespace {
using namespace dect2::swarm;
namespace mdb = dect2::msgdb;
namespace fs = std::filesystem;

double steadyNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int64_t unixMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string kindOf(const std::string& mode) {
    std::string k;
    for (unsigned char c : mode) {
        if (k.size() >= 16) break;
        if (c >= 'A' && c <= 'Z') k.push_back((char)(c + 32));
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') k.push_back((char)c);
        else if (c == ' ' || c == '/' || c == '.') k.push_back('-');
    }
    return k.empty() ? "other" : k;
}
bool sameName(const char* a, const std::string& b) {
    size_t n = strlen(a);
    if (n != b.size()) return false;
    for (size_t i = 0; i < n; i++) if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}
std::string alnum(const std::string& s, size_t maxLen) {
    std::string o;
    for (unsigned char c : s) if (std::isalnum(c) && o.size() < maxLen) o.push_back((char)c);
    return o;
}

// ---------------------------------------------------------------- the node as seen by paired peers (every call runs on the main thread)
struct AppHost : SwarmHost {
    struct Job {
        std::function<bool(App&, std::string&)> fn;
        std::mutex m;
        std::condition_variable cv;
        bool running = false, done = false, cancelled = false, ok = false;
        std::string err;
    };
    std::mutex qMu;
    std::deque<std::shared_ptr<Job>> q;
    std::atomic<double> lastTick{-1e9};
    std::atomic<bool> shutdown{false};
    std::mutex cacheMu;
    Announce cached;
    std::string statusLine = "not ready";

    bool run(const std::function<bool(App&, std::string&)>& fn, std::string& err) {
        if (shutdown) { DIAG_DEBUG(620674, "A swarm request arrived while OnAir is closing, so it was refused."); err = "the application is closing"; return false; }
        if (steadyNow() - lastTick > 1.5) { DIAG_DEBUG(800174, "A swarm request was refused because the OnAir window has not updated for 1.5 seconds."); err = "the application is not responding"; return false; }
        auto j = std::make_shared<Job>();
        j->fn = fn;
        { std::lock_guard<std::mutex> lk(qMu); if (q.size() > 32) { DIAG_DEBUG(947749, "More than 32 swarm requests are waiting, so a new one was refused."); err = "too many requests waiting"; return false; } q.push_back(j); }
        std::unique_lock<std::mutex> lk(j->m);
        if (!j->cv.wait_for(lk, std::chrono::seconds(3), [&] { return j->done || shutdown.load(); })) {
            if (!j->running) { j->cancelled = true; DIAG_DEBUG(885977, "A swarm request waited without starting, so it was cancelled."); err = "the application is busy"; return false; }
            j->cv.wait(lk, [&] { return j->done; });
        }
        if (!j->done) { DIAG_DEBUG(534917, "A swarm request was dropped because OnAir closed before it finished."); err = "the application is closing"; return false; }
        err = j->err;
        return j->ok;
    }
    void tick(App& a) {
        lastTick = steadyNow();
        std::deque<std::shared_ptr<Job>> todo;
        { std::lock_guard<std::mutex> lk(qMu); todo.swap(q); }
        for (auto& j : todo) {
            std::unique_lock<std::mutex> lk(j->m);
            if (j->cancelled) continue;
            j->running = true;
            lk.unlock();
            std::string err;
            const bool ok = j->fn(a, err);
            lk.lock();
            j->ok = ok; j->err = err; j->done = true;
            j->cv.notify_all();
        }
    }
    void abort() {
        shutdown = true;
        std::lock_guard<std::mutex> lk(qMu);
        for (auto& j : q) { std::lock_guard<std::mutex> l2(j->m); j->cancelled = true; j->cv.notify_all(); }
        q.clear();
    }

    std::string nodeInfo() override { std::lock_guard<std::mutex> g(cacheMu); return encodeAnnounce(cached).substr(9); }
    std::string status() override { std::lock_guard<std::mutex> g(cacheMu); return statusLine; }
    bool start(std::string& err) override {
        return run([](App& a, std::string& e) {
            if (a.engine.running()) { e = "already running"; return false; }
            startReceiver(a);
            if (!a.engine.running()) { e = "could not start the receiver"; return false; }
            return true;
        }, err);
    }
    bool stop(std::string& err) override { return run([](App& a, std::string&) { if (a.engine.running()) a.engine.stop(); return true; }, err); }
    bool setFrequencyHz(double hz, std::string& err) override {
        return run([hz](App& a, std::string&) { tuneFreq(a, hz / 1e6); return true; }, err);
    }
    bool setMode(const std::string& name, std::string& err) override {
        return run([name](App& a, std::string& e) {
            if (a.engine.running()) { e = "stop the receiver before switching mode"; return false; }
            for (int i = 0; i < kNumModes; i++) {
                if (!sameName(kModes[i].name, name)) continue;
                selectMode(a, kModes[i].family, kModes[i].subId);
                return a.family == kModes[i].family;
            }
            e = "no such mode";
            return false;
        }, err);
    }
    std::vector<SeqMsg> messagesSince(uint64_t since, size_t max) override {
        std::vector<SeqMsg> out;
        std::string err;
        run([&](App&, std::string&) {
            if (!mdb::global().isOpen()) return true;
            mdb::Filter f;
            f.limit = 500;
            std::vector<mdb::Record> rows = mdb::global().query(f);   // newest first
            for (auto it = rows.rbegin(); it != rows.rend() && out.size() < max; ++it) {
                if ((uint64_t)it->id <= since) continue;
                SeqMsg m;
                m.seq = (uint64_t)it->id;
                m.m.kind = kindOf(it->mode);
                m.m.text = (it->ident.empty() ? "" : it->ident + " ") + it->text;
                if (m.m.text.size() > 500) m.m.text.resize(500);
                m.m.timeMs = (int64_t)(it->time * 1000.0);
                out.push_back(std::move(m));
            }
            return true;
        }, err);
        return out;
    }
    uint64_t latestSeq() override {
        uint64_t v = 0;
        std::string err;
        run([&](App&, std::string&) {
            if (!mdb::global().isOpen()) return true;
            mdb::Filter f; f.limit = 1;
            auto r = mdb::global().query(f);
            if (!r.empty()) v = (uint64_t)r[0].id;
            return true;
        }, err);
        return v;
    }
    std::vector<TrackRow> tracks() override {
        std::vector<TrackRow> out;
        std::string err;
        run([&](App&, std::string&) {
            if (!mdb::global().isOpen()) return true;
            mdb::Filter f;
            f.posOnly = true; f.limit = 2000;
            f.since = (double)unixMs() / 1000.0 - 120.0;
            for (auto& r : mdb::global().query(f)) {
                const std::string k = kindOf(r.mode);
                if (!r.hasPos || (k != "ads-b" && k != "ais")) continue;
                TrackRow t;
                t.kind = k == "ads-b" ? "adsb" : "ais";
                t.id = alnum(r.ident, 16);
                t.lat = r.lat; t.lon = r.lon; t.timeMs = (int64_t)(r.time * 1000.0);
                if (!t.id.empty()) out.push_back(std::move(t));
            }
            return true;
        }, err);
        return out;
    }
    static std::string memKey(const dect2::Memory& m) {
        char b[64];
        snprintf(b, sizeof b, "m%llu_", (unsigned long long)m.freqHz);
        std::string k = b;
        for (unsigned char c : m.mode) if (std::isalnum(c) && k.size() < 44) k.push_back((char)c);
        return k;
    }
    std::vector<Mem> memories() override {
        std::vector<Mem> out;
        std::string err;
        run([&](App&, std::string&) {
            for (auto& m : memoryBank().all()) {
                Mem x;
                x.key = memKey(m); x.name = m.name.substr(0, 64); x.mode = m.mode.substr(0, 32); x.freqHz = (double)m.freqHz; x.note = m.notes.substr(0, 200);
                x.modifiedMs = std::max<int64_t>(m.created, m.lastUsed) * 1000;
                if (memValid(x)) out.push_back(std::move(x));
            }
            return true;
        }, err);
        return out;
    }
    int applyMemories(const std::vector<Mem>& in) override {
        int changed = 0;
        std::string err;
        run([&](App&, std::string&) {
            dect2::MemoryBank& b = memoryBank();
            for (auto& r : in) {
                if (r.deleted || r.freqHz <= 0) continue;   // deletions are not synced
                const dect2::Memory* have = b.findDuplicate((uint64_t)r.freqHz, r.mode);
                if (!have) {
                    dect2::Memory m;
                    m.name = r.name; m.freqHz = (uint64_t)r.freqHz; m.mode = r.mode; m.notes = r.note;
                    b.add(m, dect2::DupPolicy::Keep, r.modifiedMs / 1000);
                    changed++;
                } else if (r.modifiedMs > std::max<int64_t>(have->created, have->lastUsed) * 1000 && (have->name != r.name || have->notes != r.note)) {
                    dect2::Memory m = *have;
                    m.name = r.name; m.notes = r.note;
                    b.update(have->id, m);
                    changed++;
                }
            }
            if (changed) memoryChanged();
            return true;
        }, err);
        return changed;
    }
};

// ---------------------------------------------------------------- state
struct Paired { std::string nodeId, name, token; };   // nodes this one may control
struct St {
    AppHost host;
    SwarmNet net;
    std::unique_ptr<SwarmCtlServer> ctl;
    std::mutex mu;                      // everything below that threads share
    std::string ownId, err;
    bool on = false, previewing = false, loaded = false;
    std::vector<Paired> outs;
    std::vector<std::pair<std::string, std::string>> ins;   // label, token: peers allowed to control this node
    MessageMerger merger{5000, 5000};
    TrackBoard board;
    std::map<std::string, uint64_t> since;
    std::map<std::string, std::unique_ptr<SwarmClient>> clients;
    std::thread poller;
    std::atomic<bool> pollStop{false};
    std::string pairCode;               // shown on this node while it accepts pairing
    double pairUntil = 0;
    char codeIn[80] = "";
    std::string pairingWith, note;      // note: result line of the last action
    std::atomic<bool> busy{false};
    char freqIn[32] = "100.0";
    char scanLo[16] = "88", scanHi[16] = "108", scanStep[16] = "100";
    ScanOutcome scanOut;
    bool haveScan = false;
};
St& S() { static St s; return s; }

std::string filePath() { return plat::dataDir() + "/swarm.txt"; }

void save(St& s) {   // caller holds s.mu
    std::error_code ec;
    fs::create_directories(fs::path(reinterpret_cast<const char8_t*>(plat::dataDir().c_str())), ec);
    {
        std::ofstream o(dect2::u8file(filePath()));
        o << "id " << s.ownId << "\n";
        for (auto& i : s.ins) o << "in\t" << i.first << "\t" << i.second << "\n";
        for (auto& p : s.outs) o << "out\t" << p.nodeId << "\t" << p.name << "\t" << p.token << "\n";
    }
    fs::permissions(fs::path(reinterpret_cast<const char8_t*>(filePath().c_str())), fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);   // tokens are secrets
}
void load(St& s) {
    if (s.loaded) return;
    s.loaded = true;
    std::ifstream f(dect2::u8file(filePath()));
    std::string l;
    while (std::getline(f, l)) {
        if (l.compare(0, 3, "id ") == 0 && l.size() == 19) s.ownId = l.substr(3);
        std::vector<std::string> w;
        size_t p = 0;
        while (p <= l.size()) { size_t e = l.find('\t', p); w.push_back(l.substr(p, e == std::string::npos ? std::string::npos : e - p)); if (e == std::string::npos) break; p = e + 1; }
        if (w.size() == 3 && w[0] == "in") s.ins.push_back({w[1], w[2]});
        if (w.size() == 4 && w[0] == "out") s.outs.push_back({w[1], w[2], w[3]});
    }
    bool ok = s.ownId.size() == 16;
    for (char c : s.ownId) ok = ok && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    if (!ok) {
        std::random_device rd;
        char b[20];
        snprintf(b, sizeof b, "%08x%08x", (unsigned)rd(), (unsigned)rd());
        s.ownId = b;   // a stable id from now on
        save(s);
    }
}

Announce describe(App& a, const St& s) {
    Announce n;
    const bool have = !a.devices.empty();
    const DeviceInfo& d = a.devices[(size_t)std::max(0, std::min(a.devIdx, (int)a.devices.size() - 1))];
    const bool run = a.engine.running();
    std::string mode = "idle";
    if (run) for (int i = 0; i < kNumModes; i++) if (kModes[i].family == a.family && (!kModes[i].subId || subIndexOf(kModes[i].subId) == a.sub)) { mode = kModes[i].name; break; }
    n.id = s.ownId;
    n.name = "OnAir-" + s.ownId.substr(0, 4);
    n.radio = have ? d.name : "none";
    n.mode = mode;
    n.loHz = have ? std::max(0.0, d.minFreqHz) : 0;
    n.hiHz = have ? std::max(n.loHz, d.maxFreqHz) : 0;
    char b[64];
    snprintf(b, sizeof b, run ? "receiving %.3f MHz" : "stopped", a.freqMhz);
    n.summary = b;
    n.load = run ? std::max(0, std::min(100, (int)(a.engine.sampleLoss().loadPct + 0.5f))) : 0;
    n.ctlPort = s.ctl ? s.ctl->port() : 0;
    return n;
}

// ---------------------------------------------------------------- talking to other nodes
bool nodeAddress(St& s, const std::string& id, std::string& ip, int& port) {
    for (auto& n : s.net.nodes()) {
        if (n.info.id != id || n.info.ctlPort <= 0) continue;
        char b[24];
        snprintf(b, sizeof b, "%u.%u.%u.%u", n.ip >> 24, (n.ip >> 16) & 255, (n.ip >> 8) & 255, n.ip & 255);
        ip = b; port = n.info.ctlPort;
        return true;
    }
    return false;
}
std::string tokenFor(St& s, const std::string& id) {
    std::lock_guard<std::mutex> g(s.mu);
    for (auto& p : s.outs) if (p.nodeId == id) return p.token;
    return std::string();
}
Requester requester(St& s, int timeoutMs = 20000) {
    return [&s, timeoutMs](const std::string& id, const std::string& req, std::string& reply) {
        std::string ip, err, tok = tokenFor(s, id);
        int port = 0;
        if (tok.empty() || !nodeAddress(s, id, ip, port)) return false;
        std::unique_ptr<SwarmClient>* slot;
        { std::lock_guard<std::mutex> g(s.mu); slot = &s.clients[id]; if (!*slot) slot->reset(new SwarmClient); }
        SwarmClient& c = **slot;
        if (!c.connected() && !c.connect(ip, port, tok, err)) return false;
        c.setTimeoutMs(timeoutMs);
        if (!c.request(req, reply, err)) return false;
        return true;
    };
}

void pollLoop(St* s) {
    int64_t ownSince = 0;
    while (!s->pollStop) {
        for (int i = 0; i < 50 && !s->pollStop; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (s->pollStop) break;
        const int64_t now = unixMs();
        // this node's own results and tracks go into the same table
        {
            std::vector<SeqMsg> own = s->host.messagesSince((uint64_t)ownSince, 200);
            std::vector<TrackRow> ownTracks = s->host.tracks();   // before the lock: these wait for the main thread
            std::lock_guard<std::mutex> g(s->mu);
            for (auto& m : own) { m.m.nodeId = s->ownId; s->merger.add(m.m, now); ownSince = std::max<int64_t>(ownSince, (int64_t)m.seq); }
            for (auto& t : ownTracks) s->board.update(t.kind, t.id, t.lat, t.lon, s->ownId, t.timeMs, now);
            s->board.expire(now);
        }
        std::vector<std::string> ids;
        { std::lock_guard<std::mutex> g(s->mu); for (auto& p : s->outs) ids.push_back(p.nodeId); }
        Requester rq = requester(*s);
        for (auto& id : ids) {
            std::string err;
            uint64_t since;
            { std::lock_guard<std::mutex> g(s->mu); since = s->since[id]; }
            MessageMerger local(5000, 1000);
            pullResults(rq, id, since, local, now, err);
            TrackBoard tb;
            pullTracks(rq, id, tb, now, err);
            std::lock_guard<std::mutex> g(s->mu);
            s->since[id] = since;
            for (auto& r : local.rows()) { Msg m{r.kind, r.text, id, r.timeMs}; s->merger.add(m, now); }
            for (auto& t : tb.list()) s->board.update(t.kind, t.id, t.lat, t.lon, id, t.timeMs, now);
        }
    }
}

void switchOn(App& a, St& s) {
    s.err.clear();
    s.ctl.reset(new SwarmCtlServer(s.host));
    s.ctl->setOnPaired([&s](const std::string& token, const std::string& peer) {
        std::lock_guard<std::mutex> g(s.mu);
        s.ins.push_back({"paired " + peer.substr(0, peer.find(':')), token});
        save(s);
        s.note = "A node paired with this one.";
        s.pairCode.clear();
    });
    for (auto& i : s.ins) s.ctl->addToken(i.second, i.first);
    std::string e;
    if (!s.ctl->start(47832, false, e) && !s.ctl->start(0, false, e)) { s.err = e; s.ctl.reset(); return; }
    { std::lock_guard<std::mutex> g(s.host.cacheMu); s.host.cached = describe(a, s); }
    St* sp = &s;
    if (!s.net.start(s.ownId, [sp] { std::lock_guard<std::mutex> g(sp->host.cacheMu); return sp->host.cached; }, s.err)) { s.ctl->stop(); s.ctl.reset(); return; }
    s.pollStop = false;
    s.poller = std::thread(pollLoop, sp);
    s.on = true;
}
void switchOff(St& s) {
    s.pollStop = true;
    if (s.poller.joinable()) s.poller.join();
    s.net.stop();
    if (s.ctl) { s.ctl->stop(); s.ctl.reset(); }
    std::lock_guard<std::mutex> g(s.mu);
    s.clients.clear();
    s.on = false;
    s.pairCode.clear();
}

std::string groups4(const std::string& c) {
    std::string o;
    for (size_t i = 0; i < c.size(); i++) { if (i && i % 4 == 0) o += ' '; o += c[i]; }
    return o;
}
std::string timeText(int64_t ms) {
    time_t t = (time_t)(ms / 1000);
    tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    char b[16];
    strftime(b, sizeof b, "%H:%M:%S", &lt);
    return b;
}
}   // namespace

void swarmWindow(App& a) {
    if (a.rxId != 1) {   // one swarm per program: receiver 1 owns the window
        if (App* f = receiverById(1)) { f->swarmOpen = a.swarmOpen; swarmWindow(*f); a.swarmOpen = f->swarmOpen; }
        return;
    }
    St& s = S();
    s.host.tick(a);
    if (s.on) {
        std::lock_guard<std::mutex> g(s.host.cacheMu);
        s.host.cached = describe(a, s);
        s.host.statusLine = std::string("running=") + (a.engine.running() ? "1" : "0") + " " + s.host.cached.mode + " " + s.host.cached.summary;
        if (s.pairUntil && steadyNow() > s.pairUntil) { s.pairCode.clear(); s.pairUntil = 0; }
    }
    if (!a.swarmOpen) return;
    { std::lock_guard<std::mutex> g(s.mu); load(s); }
    ImGui::SetNextWindowSize(ImVec2(640 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("LAN swarm"), &a.swarmOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    ImGui::PushTextWrapPos(620 * gUi);
    ImGui::TextDisabled("%s", tr("Finds other OnAir programs on this local network and lists what they are doing. Off until you switch it on. Stays on this network (nothing goes to the internet) and sends no IQ and no audio. Controlling another node needs a one-time pairing; transmit and the voice keys can never be reached."));
    ImGui::PopTextWrapPos();

    if (!s.on) {
        if (!s.previewing) {
            if (ImGui::Button(tr("Join the swarm..."))) s.previewing = true;
        } else {
            ImGui::Separator();
            ImGui::TextUnformatted(tr("This is exactly what will be announced every 5 seconds:"));
            ImGui::PushTextWrapPos(620 * gUi);
            { std::lock_guard<std::mutex> g(s.mu); ImGui::TextUnformatted(announcePreview(describe(a, s)).c_str()); }
            ImGui::PopTextWrapPos();
            if (ImGui::Button(tr("Switch on"))) { switchOn(a, s); s.previewing = false; }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"))) s.previewing = false;
        }
        if (!s.err.empty()) ImGui::TextColored(pal::warnAmber(), "%s", s.err.c_str());
        ImGui::End();
        return;
    }

    if (ImGui::Button(tr("Leave the swarm"))) { switchOff(s); ImGui::End(); return; }
    const NetStats st = s.net.stats();
    const CtlServerStatus cs = s.ctl ? s.ctl->status() : CtlServerStatus();
    ImGui::SameLine();
    ImGui::TextDisabled(tr("%d network(s), %llu seen, %llu ignored, control port %d, %llu requests served"), st.interfaces, (unsigned long long)st.received,
                        (unsigned long long)st.rejected, cs.port, (unsigned long long)cs.requests);
    if (!s.note.empty()) ImGui::TextColored(pal::okGreen(), "%s", s.note.c_str());

    typo::separatorText(tr("Nodes"));
    const std::vector<Node> nodes = s.net.nodes();
    std::vector<Paired> outs;
    { std::lock_guard<std::mutex> g(s.mu); outs = s.outs; }
    if (nodes.empty()) ImGui::TextDisabled("%s", tr("No other nodes seen yet."));
    else if (ImGui::BeginTable("swarm", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(tr("Name")); ImGui::TableSetupColumn(tr("Radio")); ImGui::TableSetupColumn(tr("Range (MHz)")); ImGui::TableSetupColumn(tr("Mode"));
        ImGui::TableSetupColumn(tr("Load")); ImGui::TableSetupColumn(tr("Summary")); ImGui::TableSetupColumn(tr("Paired"));
        ImGui::TableHeadersRow();
        for (const Node& n : nodes) {
            const bool paired = std::any_of(outs.begin(), outs.end(), [&](const Paired& p) { return p.nodeId == n.info.id; });
            ImGui::PushID(n.info.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.info.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.info.radio.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.1f - %.1f", n.info.loHz / 1e6, n.info.hiHz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.info.mode.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d%%", n.info.load);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(n.info.summary.c_str());
            ImGui::TableNextColumn();
            if (paired) {
                if (ImGui::SmallButton(tr("Start"))) { std::string r; Requester rq = requester(s); rq(n.info.id, "start", r); s.note = n.info.name + ": " + r; }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Stop"))) { std::string r; Requester rq = requester(s); rq(n.info.id, "stop", r); s.note = n.info.name + ": " + r; }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Tune"))) {
                    std::string r; Requester rq = requester(s);
                    char b[64]; snprintf(b, sizeof b, "set_frequency %.0f", atof(s.freqIn) * 1e6);
                    rq(n.info.id, b, r); s.note = n.info.name + ": " + r;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Sync memories")) && !s.busy) {
                    s.busy = true;
                    St* sp = &s; std::string id = n.info.id, nm = n.info.name;
                    std::thread([sp, id, nm] {
                        AppHost& h = sp->host;
                        std::vector<Mem> local = h.memories();
                        MemMergeStats ms; std::string e;
                        Requester rq = requester(*sp);
                        bool ok = syncMemories(rq, id, local, ms, e);
                        if (ok) h.applyMemories(local);
                        std::lock_guard<std::mutex> g(sp->mu);
                        char b[160];
                        snprintf(b, sizeof b, ok ? "Memories synced with %s: %d new here, %d updated here." : "Memory sync with %s failed: %s", nm.c_str(), ok ? ms.added : 0, ok ? ms.updated : 0);
                        sp->note = ok ? std::string(b) : "Memory sync with " + nm + " failed: " + e;
                        sp->busy = false;
                    }).detach();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Forget"))) {
                    std::lock_guard<std::mutex> g(s.mu);
                    s.outs.erase(std::remove_if(s.outs.begin(), s.outs.end(), [&](const Paired& p) { return p.nodeId == n.info.id; }), s.outs.end());
                    s.clients.erase(n.info.id);
                    save(s);
                }
            } else if (n.info.ctlPort > 0) {
                if (ImGui::SmallButton(tr("Pair..."))) { s.pairingWith = n.info.id; s.codeIn[0] = 0; ImGui::OpenPopup("pairnode"); }
            } else ImGui::TextDisabled("-");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputText(tr("MHz for Tune"), s.freqIn, sizeof s.freqIn);
    if (ImGui::BeginPopup("pairnode")) {
        ImGui::TextUnformatted(tr("Type the pairing code that node shows (32 letters and digits):"));
        ImGui::SetNextItemWidth(420 * gUi);
        ImGui::InputText("##code", s.codeIn, sizeof s.codeIn);
        if (ImGui::Button(tr("Pair")) && !s.busy) {
            std::string ip; int port = 0, nm = 0;
            std::string id = s.pairingWith, name = id;
            for (auto& n : nodes) if (n.info.id == id) { name = n.info.name; nm = 1; }
            if (nm && nodeAddress(s, id, ip, port)) {
                s.busy = true;
                St* sp = &s; std::string code = s.codeIn;
                std::thread([sp, ip, port, id, name, code] {
                    std::string tok, e;
                    bool ok = swarmPair(ip, port, code, tok, e);
                    std::lock_guard<std::mutex> g(sp->mu);
                    if (ok) { sp->outs.push_back({id, name, tok}); save(*sp); sp->note = "Paired with " + name + "."; }
                    else sp->note = "Pairing with " + name + " failed: " + e;
                    sp->busy = false;
                }).detach();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    typo::separatorText(tr("Let another node control this one"));
    if (s.pairCode.empty()) {
        if (ImGui::Button(tr("Show a pairing code (2 minutes)")) && s.ctl) { s.pairCode = s.ctl->startPairing(); s.pairUntil = steadyNow() + 120; }
    } else {
        ImGui::TextUnformatted(groups4(s.pairCode).c_str());
        ImGui::TextDisabled("%s", tr("Type it on the other node (Pair...). Anyone who sees it can pair, so keep it private. It works once."));
        if (ImGui::Button(tr("Copy"))) ImGui::SetClipboardText(s.pairCode.c_str());
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel pairing")) && s.ctl) { s.ctl->cancelPairing(); s.pairCode.clear(); s.pairUntil = 0; }
    }
    {
        std::vector<std::pair<std::string, std::string>> ins;
        { std::lock_guard<std::mutex> g(s.mu); ins = s.ins; }
        for (auto& i : ins) {
            ImGui::PushID(i.second.c_str());
            ImGui::BulletText("%s", i.first.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Revoke")) && s.ctl) {
                s.ctl->revoke(i.first);
                std::lock_guard<std::mutex> g(s.mu);
                s.ins.erase(std::remove_if(s.ins.begin(), s.ins.end(), [&](const std::pair<std::string, std::string>& x) { return x.second == i.second; }), s.ins.end());
                save(s);
            }
            ImGui::PopID();
        }
    }

    typo::separatorText(tr("Coordinated scan"));
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputText(tr("from MHz"), s.scanLo, sizeof s.scanLo); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputText(tr("to MHz"), s.scanHi, sizeof s.scanHi); ImGui::SameLine();
    ImGui::SetNextItemWidth(70 * gUi); ImGui::InputText(tr("step kHz"), s.scanStep, sizeof s.scanStep);
    if (ImGui::Button(tr("Scan with the paired nodes")) && !s.busy) {
        std::vector<Cap> caps;
        for (auto& n : nodes) {
            if (!std::any_of(outs.begin(), outs.end(), [&](const Paired& p) { return p.nodeId == n.info.id; })) continue;
            Cap c; c.id = n.info.id; c.minHz = n.info.loHz; c.maxHz = n.info.hiHz; c.bwHz = 2e6; c.load = n.info.load;   // bandwidth is not announced yet: 2 MHz assumed
            if (c.maxHz > c.minHz) caps.push_back(c);
        }
        const double lo = atof(s.scanLo) * 1e6, hi = atof(s.scanHi) * 1e6, step = atof(s.scanStep) * 1e3;
        if (caps.empty()) s.note = "No paired node is visible to scan with.";
        else {
            s.busy = true;
            St* sp = &s;
            std::thread([sp, caps, lo, hi, step] {
                ScanOutcome out; std::string e;
                Requester rq = requester(*sp, 600000);
                bool ok = coordinateScan(rq, lo, hi, step, caps, std::max(step, 5e3), out, e);
                std::lock_guard<std::mutex> g(sp->mu);
                sp->scanOut = out; sp->haveScan = ok;
                sp->note = ok ? "Scan finished: " + std::to_string(out.hits.size()) + " hits, " + std::to_string(out.failed.size()) + " node(s) could not scan." : "Scan not started: " + e;
                sp->busy = false;
            }).detach();
        }
    }
    {
        std::lock_guard<std::mutex> g(s.mu);
        if (s.haveScan) {
            for (auto& h : s.scanOut.hits) {
                std::string tags;
                for (auto& n : h.nodes) tags += (tags.empty() ? "" : ", ") + n.substr(0, 4);
                ImGui::Text("%.4f MHz  %.1f dB  (%s)", h.freqHz / 1e6, h.level, tags.c_str());
            }
            if (!s.scanOut.uncovered.empty()) ImGui::TextDisabled(tr("%zu part(s) of the range were not covered."), s.scanOut.uncovered.size());
        }
    }

    typo::separatorText(tr("Pooled results"));
    {
        std::lock_guard<std::mutex> g(s.mu);
        ImGui::TextDisabled(tr("%zu messages, %zu tracks (from this node and the paired ones; the same message heard twice is one row)"), s.merger.rows().size(), s.board.list().size());
        if (ImGui::BeginTable("pool", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 180 * gUi))) {
            ImGui::TableSetupColumn(tr("Time")); ImGui::TableSetupColumn(tr("Kind")); ImGui::TableSetupColumn(tr("Message")); ImGui::TableSetupColumn(tr("Heard by"));
            ImGui::TableHeadersRow();
            const auto& rows = s.merger.rows();
            for (size_t i = rows.size(), shown = 0; i > 0 && shown < 200; i--, shown++) {
                const MergedRow& r = rows[i - 1];
                std::string tags;
                for (auto& n : r.nodes) tags += (tags.empty() ? "" : ", ") + n.substr(0, 4);
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(timeText(r.timeMs).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.kind.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(r.text.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(tags.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}
