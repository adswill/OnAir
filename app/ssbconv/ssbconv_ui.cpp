// The Conversations window (View > Conversations): SSB voice found on the captured span and organised into conversations to listen to. The finding and the
// grouping are in core/src/ssbconv (docs/SSB-CONVERSATIONS.md); this file is the window, the sample tap, the worker thread, Listen / auto-listen through the
// Multi-VFO channel (the radio is not retuned while the signal is inside the span), the clip files and the events for the automations / integrations.
#include "../app.h"
#include "dect2/automation/engine.h"
#include "dect2/common/thread_name.h"
#include "dect2/common/u8file.h"
#include "dect2/engine/sig_ident.h"
#include "dect2/ssbconv/ssbconv.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace {
namespace sc = dect2::ssbconv;
namespace au = dect2::automation;
namespace fs = std::filesystem;

constexpr int kListenId = 9100;   // the Multi-VFO row this window owns

struct Block { std::vector<sc::cf> x; double rate = 0, centre = 0, t = 0; };

struct St {
    std::mutex smu;                      // the scanner (the worker feeds it; the window changes its options). Order: smu, then mu
    std::mutex mu;                       // book, auto-listen, recorder
    sc::ConversationBook book;
    sc::Scanner scanner;
    sc::AutoListen autoListen;
    std::unique_ptr<sc::ClipRecorder> rec;
    std::thread worker;
    std::mutex qmu;
    std::condition_variable qcv;
    std::deque<Block> q;
    size_t qSamples = 0;
    std::atomic<bool> stop{false}, started{false};
    std::atomic<double> lastRate{0}, lastCentre{0}, lastData{0}, busy{0};
    std::atomic<bool> tapSet{false};
    // settings (window)
    bool scanOn = true, autoRecord = false, sweepOn = false, demo = false, demoBusy = false;
    bool favOnly = false, activeOnly = false, hideEnded = false, showMuted = false;
    int hideShort = 0, minS = 0, sortBy = 0, bandSel = 0, sideSel = 0;
    double timelineHours = 1;
    float gapSec = 30, overGap = 2.0f, tolHz = 100;
    int tab = 0;
    std::string note;
    double demoNow = 0;                  // the frozen clock of the demo
    uint32_t listening = 0, lastListened = 0;
    std::unique_ptr<sc::SweepPlanner> sweep;
    double sweepSpan = 0, sweepT = 0;
    std::vector<sc::Detection> lastDets;
    bool configDirty = true;
    std::string clipDir;
};
St S;

double nowT() {
    if (S.demo && S.demoNow > 0) return S.demoNow;
    return au::wallNow();
}

void publishEvents(const std::vector<sc::BookEvent>& ev) {
    if (ev.empty() || !au::wants(au::FSignal)) return;
    for (const sc::BookEvent& e : ev) {
        const sc::Conversation& c = e.snapshot;
        au::Event x(e.type == sc::EventType::New ? "signal.ssb_conversation_new" : "signal.ssb_conversation_ended");
        x.at(e.time, "SSB conversations", c.dialHz);
        x.num("id", c.id).num("freq_hz", c.dialHz).str("sideband", sc::sideName(c.side)).str("band", c.band).str("service", c.service);
        x.num("overs", c.overs).num("speakers", c.speakers).num("snr_db", c.snrDb).str("kind", sc::kindName(c.kind)).num("talk_s", c.talkSec);
        au::emit(std::move(x));
    }
}

void processBlock(Block& b) {
    // the scanner has its own state; the book is shared with the window
    std::vector<sc::Detection> dets;
    {
        std::lock_guard<std::mutex> sl(S.smu);
        dets = S.scanner.feed(b.x.data(), b.x.size(), b.rate, b.centre, b.t);
        S.busy = S.scanner.busy();
    }
    std::lock_guard<std::mutex> lk(S.mu);
    std::vector<sc::BookEvent> ev = S.book.update(b.t, dets);
    if (dets.empty()) { std::vector<sc::BookEvent> e2 = S.book.tick(b.t); ev.insert(ev.end(), e2.begin(), e2.end()); }
    for (const sc::BookEvent& e : ev) {
        if (e.type == sc::EventType::New && S.autoRecord) S.book.setRecording(e.id, true);
        if (e.type == sc::EventType::Ended && S.rec) { if (const sc::Conversation* c = S.book.find(e.id)) S.rec->finish(*c, e.time); }
    }
    if (S.rec) for (const sc::Detection& d : dets) {
        if (!d.voiceNow || d.audio.empty()) continue;
        const uint32_t id = S.book.convForTrack(d.trackId);
        const sc::Conversation* c = id ? S.book.find(id) : nullptr;
        if (c && c->recording) S.rec->append(*c, d.audio.data(), d.audio.size(), d.audioRate, b.t);
    }
    publishEvents(ev);
}

void workerLoop() {
    dect2::setThreadName("ssbconv");
    while (!S.stop) {
        Block b;
        {
            std::unique_lock<std::mutex> lk(S.qmu);
            S.qcv.wait_for(lk, std::chrono::milliseconds(300), [] { return !S.q.empty() || S.stop; });
            if (S.stop) break;
            if (S.q.empty()) continue;
            b = std::move(S.q.front());
            S.q.pop_front();
            S.qSamples -= b.x.size();
        }
        processBlock(b);
    }
}

// A second opinion on a band that is not confirmed as voice yet: the signal identifier. Only a clear (>= 75 %) HF data / utility mode vetoes it.
bool digitalVeto(const sc::cf* x, size_t n, double rate, double centreOffsetHz) {
    if (n < 8192) return false;
    const size_t m = std::min(n, (size_t)rate);   // one second is plenty
    std::vector<dect2::cf32> v(x, x + m);
    dect2::sigid::Options o;
    o.rfHz = S.lastCentre.load() + centreOffsetHz; o.verify = false;
    const dect2::sigid::Result r = dect2::sigid::identify(v.data(), v.size(), rate, o);
    return r.verdict == dect2::sigid::VerdictLikely && !r.cands.empty() && r.cands[0].family == "HF and utility" && r.cands[0].confidence >= 0.75;
}

void ensureStarted() {
    if (S.started.exchange(true)) return;
    {
        std::lock_guard<std::mutex> sl(S.smu);
        sc::ScannerOptions so = S.scanner.options();
        so.digitalVeto = digitalVeto;
        S.scanner.setOptions(so);
    }
    S.clipDir = plat::dataDir() + "/ssb-conversations";
    S.rec.reset(new sc::ClipRecorder(S.clipDir));
    S.worker = std::thread(workerLoop);
    S.worker.detach();
}

// the tap runs on the radio's thread: copy and hand over, drop when the worker falls more than a few seconds behind
void tapFn(const std::complex<float>* x, size_t n, double rate) {
    S.lastRate = rate; S.lastData = au::wallNow();
    if (!S.scanOn || S.demo) return;
    std::lock_guard<std::mutex> lk(S.qmu);
    if (S.qSamples > (size_t)(rate * 3)) return;
    Block b; b.x.assign(x, x + n); b.rate = rate; b.centre = S.lastCentre.load(); b.t = au::wallNow();
    S.qSamples += n;
    S.q.push_back(std::move(b));
    S.qcv.notify_one();
}

void applyOptions() {
    std::lock_guard<std::mutex> sl(S.smu);
    std::lock_guard<std::mutex> lk(S.mu);
    sc::BookOptions bo = S.book.options();
    bo.conversationGapSec = S.demo ? 10.0 : S.gapSec; bo.overGapSec = S.overGap; bo.freqTolHz = S.tolHz;
    S.book.setOptions(bo);
    sc::ScannerOptions so = S.scanner.options();
    so.freqTolHz = S.tolHz;
    S.scanner.setOptions(so);
}

void startDemo() {
    ensureStarted();
    {
        std::lock_guard<std::mutex> sl(S.smu);
        std::lock_guard<std::mutex> lk(S.mu);
        S.book.clear(); S.scanner.reset(); S.demo = true; S.demoBusy = true; S.demoNow = 0;
        sc::BookOptions bo = S.book.options(); bo.conversationGapSec = 10; S.book.setOptions(bo);
    }
    std::thread([] {
        dect2::setThreadName("ssbconv-demo");
        const sc::synth::Demo d = sc::synth::demo();
        std::vector<sc::cf> iq = sc::synth::make(d.rateHz, d.seconds, d.stations, d.noisePower, 3);
        const double t0 = au::wallNow() - d.seconds;
        const size_t blk = 8192;
        for (size_t i = 0; i < iq.size() && !S.stop; i += blk) {
            Block b; const size_t n = std::min(blk, iq.size() - i);
            b.x.assign(iq.begin() + (long)i, iq.begin() + (long)(i + n)); b.rate = d.rateHz; b.centre = d.centreHz; b.t = t0 + (double)(i + n) / d.rateHz;
            processBlock(b);
            S.lastRate = d.rateHz; S.lastCentre = d.centreHz;
        }
        std::lock_guard<std::mutex> lk(S.mu);
        S.book.tick(t0 + d.seconds);
        S.demoNow = t0 + d.seconds; S.demoBusy = false;
    }).detach();
}

void stopDemo() {
    std::lock_guard<std::mutex> sl(S.smu);
    std::lock_guard<std::mutex> lk(S.mu);
    S.demo = false; S.demoNow = 0; S.book.clear(); S.scanner.reset();
}

// ---- listening
void removeListenRow(App& a) {
    a.vfoRows.erase(std::remove_if(a.vfoRows.begin(), a.vfoRows.end(), [](const VfoRow& r) { return r.id == kListenId; }), a.vfoRows.end());
}

bool listenTo(App& a, uint32_t id) {
    sc::Conversation c;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        const sc::Conversation* p = S.book.find(id);
        if (!p) return false;
        c = *p;
    }
    if (S.demo) { S.note = tr("Demo signals: there is nothing to tune. Start a radio on the shortwave bands to listen."); S.listening = id; return false; }
    if (!a.engine.running()) { S.note = tr("Start the radio first."); return false; }
    const double rate = S.lastRate.load();
    double centre = a.engine.captureCentreHz();
    if (std::fabs(c.dialHz - centre) > rate / 2 - 3500) {   // outside the captured span: the radio has to move
        const double nc = rate >= 150000 ? c.dialHz - 25000 : c.dialHz;
        a.freqMhz = nc / 1e6; a.tune.centerHz = nc;
        a.engine.retune(a.tune);
        centre = nc;
        S.note = tr("The conversation was outside the captured span: the radio was retuned.");
    } else S.note.clear();
    VfoRow r; r.id = kListenId; r.freqMhz = std::round(c.dialHz / 100.0) / 1e4; r.mode = (int)(c.side == sc::Side::Lsb ? dect2::VfoMode::Lsb : dect2::VfoMode::Usb);
    r.bwKhz = 2.8; r.sqDb = -150; r.vol = 1; r.on = true;
    bool found = false;
    for (VfoRow& x : a.vfoRows) if (x.id == kListenId) { x = r; found = true; }
    if (!found) a.vfoRows.push_back(r);
    S.listening = id;
    return true;
}

void stopListening(App& a) { removeListenRow(a); S.listening = 0; }

// ---- drawing helpers
ImVec4 colGood() { return ImVec4(0.30f, 0.85f, 0.40f, 1); }
ImVec4 colWarn() { return ImVec4(0.95f, 0.75f, 0.25f, 1); }

std::string ageText(double s) {
    char b[32];
    if (s < 0) s = 0;
    if (s < 90) snprintf(b, sizeof b, "%ds", (int)s);
    else if (s < 5400) snprintf(b, sizeof b, "%dm %02ds", (int)(s / 60), (int)std::fmod(s, 60));
    else snprintf(b, sizeof b, "%dh %02dm", (int)(s / 3600), (int)(std::fmod(s, 3600) / 60));
    return b;
}
std::string clockOf(double t) { const std::string f = sc::ConversationBook::fmtTime(t); return f.size() >= 19 ? f.substr(11, 8) : f; }

bool writeFile(const std::string& path, const std::string& text) {
    std::error_code ec; fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream o(dect2::u8file(path), std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o << text;
    return (bool)o;
}

const char* kBands[] = {"All bands", "160 m", "80 m", "60 m", "40 m", "30 m", "20 m", "17 m", "15 m", "12 m", "10 m", "CB", "2 MHz", "4 MHz", "6 MHz", "8 MHz", "12 MHz", "16 MHz", "3 MHz", "5 MHz", "HF"};
const char* kSortNames[] = {"Activity", "Strength", "Newest", "Frequency"};

sc::Filter makeFilter() {
    sc::Filter f;
    if (S.bandSel > 0) f.band = kBands[S.bandSel];
    f.minSnrDb = S.minS > 0 ? (S.minS - 1.0) * 6.0 : -1e9;
    f.activeOnly = S.activeOnly; f.hideEnded = S.hideEnded; f.favoritesOnly = S.favOnly; f.showMuted = S.showMuted;
    f.hideShortSec = S.hideShort;
    f.side = S.sideSel == 1 ? sc::Side::Usb : S.sideSel == 2 ? sc::Side::Lsb : sc::Side::Unknown;
    return f;
}

void listTab(App& a, double now) {
    std::vector<sc::Conversation> rows;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        rows = S.book.list(now, makeFilter(), (sc::SortBy)S.sortBy);
    }
    if (rows.empty()) {
        ImGui::Spacing();
        if (!S.demo && !a.engine.running()) ImGui::TextWrapped("%s", tr("Start a radio on a shortwave band (a wide span shows the most), or press Demo to see how it looks. SSB voice that is found appears here by itself."));
        else if (S.demoBusy) ImGui::TextDisabled("%s", tr("Preparing the demo signals..."));
        else ImGui::TextWrapped("%s", tr("Listening to the whole span for SSB voice. Conversations show up here as soon as someone talks; nothing found yet or the filters hide everything."));
        return;
    }
    const ImGuiTableFlags fl = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX;
    if (!ImGui::BeginTable("ssbconv_list", 13, fl, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(" ", ImGuiTableColumnFlags_WidthFixed, 24 * gUi);
    ImGui::TableSetupColumn(tr("Status"), ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn(tr("Frequency"), ImGuiTableColumnFlags_WidthFixed, 100 * gUi);
    ImGui::TableSetupColumn(tr("SB"), ImGuiTableColumnFlags_WidthFixed, 36 * gUi);
    ImGui::TableSetupColumn(tr("Service"), ImGuiTableColumnFlags_WidthFixed, 110 * gUi);
    ImGui::TableSetupColumn(tr("Signal"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
    ImGui::TableSetupColumn(tr("Started"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    ImGui::TableSetupColumn(tr("Length"), ImGuiTableColumnFlags_WidthFixed, 70 * gUi);
    ImGui::TableSetupColumn(tr("Overs"), ImGuiTableColumnFlags_WidthFixed, 44 * gUi);
    ImGui::TableSetupColumn(tr("Voices"), ImGuiTableColumnFlags_WidthFixed, 50 * gUi);
    ImGui::TableSetupColumn(tr("Kind"), ImGuiTableColumnFlags_WidthFixed, 80 * gUi);
    ImGui::TableSetupColumn(tr("Heard"), ImGuiTableColumnFlags_WidthFixed, 90 * gUi);
    ImGui::TableSetupColumn(tr("Actions"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (const sc::Conversation& c : rows) {
        ImGui::TableNextRow();
        ImGui::PushID((int)c.id);
        const bool live = c.active && !c.ended;
        ImGui::TableNextColumn();
        if (ImGui::SmallButton(c.favorite ? "*" : "-")) { std::lock_guard<std::mutex> lk(S.mu); S.book.setFavorite(c.id, !c.favorite); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Favourite: starred conversations are preferred by auto-listen"));
        ImGui::TableNextColumn();
        if (live) ImGui::TextColored(colGood(), "%s", tr("ACTIVE now"));
        else if (c.ended) ImGui::TextDisabled("%s", ("ended " + ageText(now - c.lastVoiceT)).c_str());
        else ImGui::TextColored(colWarn(), "%s", tr("quiet"));
        ImGui::TableNextColumn();
        if (S.listening == c.id) ImGui::TextColored(colGood(), "> %.4f", c.dialHz / 1e6); else ImGui::Text("%.4f", c.dialHz / 1e6);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(sc::sideName(c.side));
        ImGui::TableNextColumn(); ImGui::TextUnformatted((c.band.empty() ? c.service : c.band + (c.service.empty() ? "" : " " + c.service)).c_str());
        ImGui::TableNextColumn();
        {
            const char* arrow = c.snrTrendDbPerMin > 1.5 ? " ^" : c.snrTrendDbPerMin < -1.5 ? " v" : "";
            const ImVec4 col = c.snrDb >= 30 ? colGood() : c.snrDb >= 15 ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : colWarn();
            ImGui::TextColored(col, "%s%s", sc::sText(c.snrDb).c_str(), arrow);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(tr("Estimated from the signal over the noise floor of the capture (%.0f dB; peak %.0f dB; trend %+.1f dB/min). Not calibrated."), c.snrDb, c.snrMaxDb, c.snrTrendDbPerMin);
        }
        ImGui::TableNextColumn(); ImGui::TextUnformatted(clockOf(c.startT).c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(ageText((c.ended || !live ? c.lastVoiceT : now) - c.startT).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%d", c.overs);
        ImGui::TableNextColumn(); ImGui::Text("%d", c.speakers);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(sc::kindName(c.kind));
        ImGui::TableNextColumn();
        if (c.clues.empty()) ImGui::TextDisabled("-");
        else { std::string s; for (size_t i = 0; i < c.clues.size() && i < 3; i++) s += (i ? " " : "") + c.clues[i]; ImGui::TextUnformatted(s.c_str()); }
        ImGui::TableNextColumn();
        const bool me = S.listening == c.id;
        if (!c.ended) {
            if (ImGui::SmallButton(me ? tr("Listening") : tr("Listen"))) { if (me) { stopListening(a); S.autoListen.select(0, now); } else { if (listenTo(a, c.id)) S.autoListen.select(c.id, now); } }
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(c.recording ? tr("Rec on") : tr("Rec"))) { std::lock_guard<std::mutex> lk(S.mu); S.book.setRecording(c.id, !c.recording); if (c.recording && S.rec) S.rec->finish(c, now); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Record this conversation's audio to a WAV file with a JSON note in the data folder (off by default)"));
        ImGui::SameLine();
        if (ImGui::SmallButton(c.muted ? tr("Unmute") : tr("Ignore"))) { std::lock_guard<std::mutex> lk(S.mu); S.book.setMuted(c.id, !c.muted); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Ignore this frequency: later activity within the tolerance is not listed"));
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void timelineTab(double now) {
    std::vector<sc::ConversationBook::TimelineRow> rows;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        rows = S.book.timeline(now, S.timelineHours);
    }
    static const char* spans[] = {"Last 15 minutes", "Last hour", "Last 6 hours", "Last 24 hours"};
    static const double hours[] = {0.25, 1, 6, 24};
    static int sel = 1;
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::Combo("##tlspan", &sel, spans, 4)) S.timelineHours = hours[sel];
    const double h = hours[sel];
    if (rows.empty()) { ImGui::TextDisabled("%s", tr("Nothing in this period yet.")); return; }
    const float labelW = 150 * gUi, rowH = 22 * gUi;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = std::max(200.f, ImGui::GetContentRegionAvail().x - labelW - 8);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const double t0 = now - h * 3600;
    const ImU32 grid = ImGui::GetColorU32(ImGuiCol_Border), txt = ImGui::GetColorU32(ImGuiCol_Text), dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    // time axis
    const int ticks = h <= 0.3 ? 5 : h <= 1 ? 6 : h <= 6 ? 6 : 8;
    for (int i = 0; i <= ticks; i++) {
        const float x = p0.x + labelW + w * (float)i / (float)ticks;
        dl->AddLine(ImVec2(x, p0.y + 16 * gUi), ImVec2(x, p0.y + 16 * gUi + rowH * (float)rows.size()), grid);
        const std::string s = clockOf(t0 + (now - t0) * i / ticks).substr(0, 5);
        dl->AddText(ImVec2(x - 12 * gUi, p0.y), dim, s.c_str());
    }
    float y = p0.y + 16 * gUi;
    for (const auto& r : rows) {
        dl->AddText(ImVec2(p0.x, y + 3 * gUi), txt, r.label.c_str());
        dl->AddRectFilled(ImVec2(p0.x + labelW, y + 2), ImVec2(p0.x + labelW + w, y + rowH - 2), ImGui::GetColorU32(ImGuiCol_FrameBg));
        for (const auto& b : r.bars) {
            const float x0 = p0.x + labelW + w * (float)((b.t0 - t0) / (now - t0)), x1 = p0.x + labelW + w * (float)((b.t1 - t0) / (now - t0));
            const ImU32 col = b.favorite ? IM_COL32(240, 190, 60, 230) : b.side == sc::Side::Lsb ? IM_COL32(235, 140, 60, 230) : IM_COL32(70, 160, 235, 230);
            dl->AddRectFilled(ImVec2(x0, y + 4), ImVec2(std::max(x1, x0 + 2), y + rowH - 4), col, 2.f);
        }
        y += rowH;
    }
    ImGui::Dummy(ImVec2(labelW + w, 16 * gUi + rowH * (float)rows.size() + 4));
    ImGui::TextDisabled("%s", tr("Blue: USB, orange: LSB, gold: favourite. One row per frequency."));
}

void settingsTab(App& a) {
    bool dirty = false;
    ImGui::TextWrapped("%s", tr("Conversations are found automatically. These settings only matter when the defaults do not fit."));
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat(tr("A conversation ends after silence of (s)"), &S.gapSec, 5, 300, "%.0f")) dirty = true;
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat(tr("A new over starts after a pause of (s)"), &S.overGap, 0.5f, 6, "%.1f")) dirty = true;
    ImGui::SetNextItemWidth(160 * gUi);
    if (ImGui::SliderFloat(tr("Same frequency within (Hz)"), &S.tolHz, 30, 300, "%.0f")) dirty = true;
    if (dirty) applyOptions();
    ImGui::Separator();
    if (ImGui::Checkbox(tr("Record every new conversation automatically"), &S.autoRecord)) {}
    ImGui::TextDisabled("%s", (std::string(tr("Clips go to ")) + S.clipDir).c_str());
    ImGui::Separator();
    if (ImGui::Checkbox(tr("Scan bands (a radio that sees only a few kHz: step through the HF bands)"), &S.sweepOn)) S.sweep.reset();
    ImGui::TextDisabled("%s", tr("Uses the same detector; it stays longer on a step where a conversation is active."));
    ImGui::Separator();
    if (ImGui::Button(tr("Export list (CSV)"))) {
        std::string csv; { std::lock_guard<std::mutex> lk(S.mu); csv = S.book.toCsv(nowT()); }
        const std::string p = S.clipDir + "/ssb_conversations_" + std::to_string((long long)au::wallNow()) + ".csv";
        S.note = writeFile(p, csv) ? std::string(tr("Saved ")) + p : tr("Could not save the file.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Export list (JSON)"))) {
        std::string js; { std::lock_guard<std::mutex> lk(S.mu); js = S.book.toJson(nowT()); }
        const std::string p = S.clipDir + "/ssb_conversations_" + std::to_string((long long)au::wallNow()) + ".json";
        S.note = writeFile(p, js) ? std::string(tr("Saved ")) + p : tr("Could not save the file.");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear the list"))) { std::lock_guard<std::mutex> sl(S.smu); std::lock_guard<std::mutex> lk(S.mu); S.book.clear(); S.scanner.reset(); }
    (void)a;
}

void conversationsWindow(App& a) {
    firstWindowSize(1080 * gUi, 560 * gUi);
    if (!ImGui::Begin(trw("Conversations"), &a.ssbConvOpen)) { ImGui::End(); return; }
    const double now = nowT();
    // header: scan switch, status, auto-listen
    ImGui::Checkbox(tr("Find SSB conversations"), &S.scanOn);
    ImGui::SameLine();
    if (S.demo) ImGui::TextColored(colWarn(), "%s", tr("DEMO: synthetic signals, nothing is received"));
    else if (!a.engine.running()) ImGui::TextDisabled("%s", tr("no radio running"));
    else {
        const double rate = S.lastRate.load();
        size_t nb = 0; { std::lock_guard<std::mutex> sl(S.smu); nb = S.scanner.trackCount(); }
        ImGui::TextDisabled(tr("scanning %.3f MHz, span %.0f kHz, %zu tracked, %.0f%% of one core"), a.engine.captureCentreHz() / 1e6, rate / 1e3, nb, S.busy.load() * 100);
    }
    ImGui::SameLine();
    if (ImGui::Button(S.demo ? tr("Stop demo") : tr("Demo"))) { if (S.demo) stopDemo(); else startDemo(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show the panel with synthetic SSB signals (a QSO, a net, an LSB conversation, data decoys)"));
    // auto-listen row
    bool al = S.autoListen.enabled;
    if (ImGui::Checkbox(tr("Auto-listen to the best active conversation"), &al)) { S.autoListen.enabled = al; if (!al && !S.listening) {} }
    ImGui::SameLine();
    bool hold = S.autoListen.hold;
    if (ImGui::Checkbox(tr("Hold"), &hold)) S.autoListen.hold = hold;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Stay on the current conversation while it lasts"));
    ImGui::SameLine();
    if (ImGui::Button(tr("Skip"))) { S.autoListen.skip(S.listening); stopListening(a); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Leave this conversation and go to the next best one"));
    ImGui::SameLine();
    if (S.listening && ImGui::Button(tr("Stop listening"))) { stopListening(a); S.autoListen.select(0, now); }
    if (!S.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.note.c_str()); }
    ImGui::Separator();
    // filters
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::Combo("##band", &S.bandSel, kBands, (int)(sizeof kBands / sizeof kBands[0]));
    ImGui::SameLine(); ImGui::SetNextItemWidth(70 * gUi);
    static const char* sides[] = {"USB+LSB", "USB", "LSB"};
    ImGui::Combo("##side", &S.sideSel, sides, 3);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderInt("##mins", &S.minS, 0, 9, S.minS ? "min S%d" : "any signal");
    ImGui::SameLine(); ImGui::Checkbox(tr("Active only"), &S.activeOnly);
    ImGui::SameLine(); ImGui::Checkbox(tr("Hide ended"), &S.hideEnded);
    ImGui::SameLine(); ImGui::Checkbox(tr("Favourites"), &S.favOnly);
    ImGui::SameLine(); ImGui::Checkbox(tr("Show ignored"), &S.showMuted);
    ImGui::SameLine(); ImGui::SetNextItemWidth(110 * gUi);
    ImGui::SliderInt("##short", &S.hideShort, 0, 120, S.hideShort ? "hide < %d s" : "show all lengths");
    ImGui::SameLine(); ImGui::SetNextItemWidth(100 * gUi);
    ImGui::Combo("##sort", &S.sortBy, kSortNames, 4);
    ImGui::Separator();
    if (ImGui::BeginTabBar("ssbtabs")) {
        if (ImGui::BeginTabItem(trw("Conversations"))) { listTab(a, now); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Timeline"))) { timelineTab(now); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem(trw("Settings"))) { settingsTab(a); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::End();
}
}  // namespace

// every frame: the tap, auto-listen, the sweep; the window when open
void ssbConvWindow(App& a) {
    if (a.rxId != 1) return;   // one list for the program: RX 1's window
    static bool opened = false;
    if (a.ssbConvOpen) {
        ensureStarted();
        if (!opened) { opened = true; applyOptions(); }
    }
    const bool want = a.ssbConvOpen && S.scanOn && !S.demo && a.engine.running();
    if (want) {
        S.lastCentre = a.engine.captureCentreHz();
        const double now = au::wallNow();
        if (!S.tapSet || (S.lastData.load() > 0 && now - S.lastData.load() > 3.0) || S.lastData.load() == 0) {   // another feature (the Doctor) may have taken the tap
            a.engine.setIqTap(tapFn); S.tapSet = true; S.lastData = now;
        }
        if (S.sweepOn) {
            const double rate = S.lastRate.load();
            if (rate > 0 && (!S.sweep || S.sweepSpan != rate)) { S.sweep.reset(new sc::SweepPlanner(sc::defaultSweepRanges(), rate * 0.9, 6, 40)); S.sweepSpan = rate; S.sweepT = 0; }
            if (S.sweep && now - S.sweepT > 1.0) {
                S.sweepT = now;
                int act = 0; { std::lock_guard<std::mutex> lk(S.mu); sc::Filter f; f.activeOnly = true; act = (int)S.book.list(now, f).size(); }
                if (S.sweep->update(now, act)) { a.freqMhz = S.sweep->centreHz() / 1e6; a.tune.centerHz = S.sweep->centreHz(); a.engine.retune(a.tune); }
            }
        }
    } else if (S.tapSet) {
        if (a.engine.running()) a.engine.setIqTap(nullptr);
        S.tapSet = false;
    }
    if (!S.started) return;
    // age the conversations even when no samples arrive, and drive auto-listen
    const double now = nowT();
    if (!S.demo || S.demoNow > 0) {
        std::vector<sc::BookEvent> ev;
        { std::lock_guard<std::mutex> lk(S.mu); if (!S.demo) ev = S.book.tick(now); }
        for (const sc::BookEvent& e : ev) if (e.type == sc::EventType::Ended && S.rec) { std::lock_guard<std::mutex> lk(S.mu); if (const sc::Conversation* c = S.book.find(e.id)) S.rec->finish(*c, e.time); }
        publishEvents(ev);
    }
    if (S.autoListen.enabled || S.listening) {
        uint32_t cur = 0;
        { std::lock_guard<std::mutex> lk(S.mu); cur = S.autoListen.update(S.book, now); }
        if (S.autoListen.enabled && cur != S.listening) {
            if (cur) { if (!listenTo(a, cur)) S.listening = S.demo ? cur : 0; }
            else if (S.listening) stopListening(a);
        }
        if (S.listening && !S.demo) for (VfoRow& r : a.vfoRows) if (r.id == kListenId) r.vol = (float)std::min(1.0, std::max(0.0, S.autoListen.enabled ? S.autoListen.gain(now) : 1.0));
    }
    if (a.ssbConvOpen) conversationsWindow(a);
}

void ssbConvDevOpen(App& a, bool demo) {
    a.ssbConvOpen = true;
    if (demo) startDemo();
}
