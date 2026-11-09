// View > Multi-radio trunking: one radio on the control channel, the others retuned to the voice channels as grants arrive (core/src/trunkpool, docs/TRUNKING.md).
// This file is the window, the App's receivers as the pool's radios (AppRx), and the feed from the P25 control channel. Receive only. Voice sound and
// per-call audio need the user-installed vocoder add-on; without it the calls are still logged and a JSON file is kept for each. Encrypted calls are
// never followed. The tick runs every frame from the window function, shut or not.
#include "../app.h"
#include "i18n/i18n.h"
#include "platform/platform.h"
#include "state/receivers.h"
#include "state/tune_worker.h"
#include "ui/kit/mode_panel.h"
#include "dect2/p25/trunk.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/p25p2/p25p2_tel.h"
#include "dect2/trunkpool/trunkpool.h"
#include "voice/tgdir_ui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

dect2::trunk::Controller& p25TrunkController();   // p25_ui.cpp
void p25VoiceRadioOff(bool off);

namespace {
namespace tp = dect2::tpool;

constexpr int kP25p1Std = 52, kP25p2Std = 182;   // the engine standards of P25 Phase 1 and Phase 2 (family 50 / 180)

double nowSec() { using namespace std::chrono; return duration<double>(steady_clock::now().time_since_epoch()).count(); }
bool p25Family(const App& b) { return b.family == 50 || b.family == 180; }

// One of the App's receivers as a pool radio. P25 voice channels only (Phase 1 and 2); other protocols' voice radios are listed as remaining in docs/TRUNKING.md.
class AppRx : public tp::VoiceReceiver {
public:
    explicit AppRx(int id) : id_(id) {}
    int id() const { return id_; }
    bool selfFollow = false;   // the control radio: calls inside its capture are decoded by the P25 follower itself, as slices
    App* app() const { return receiverById(id_); }
    std::string name() const override { return "RX " + std::to_string(id_); }
    bool present() const override { App* b = app(); return b && b->engine.running(); }
    double captureCentreHz() const override { App* b = app(); return b ? b->engine.captureCentreHz() : 0; }
    double captureSpanHz() const override { App* b = app(); return b && b->engine.running() ? b->engine.sampleRate() : 0; }
    int maxSlices() const override { return selfFollow ? std::max(0, p25TrunkController().follower.config().maxSlices) : 0; }
    bool tune(const tp::TuneRequest& r) override {
        App* b = app();
        if (!b || !b->engine.running()) return false;
        if (!r.control && r.proto != tp::Proto::P25p1 && r.proto != tp::Proto::P25p2) return false;   // only P25 voice is decoded by this adapter
        if (!first_) { first_ = true; prevMhz_ = b->freqMhz; prevStd_ = engineStd(*b); }
        const bool p2 = r.proto == tp::Proto::P25p2;
        if (!r.control) b->engine.setStandard(p2 ? kP25p2Std : kP25p1Std);
        else if (b->engine.running() && prevStd_ > 0) b->engine.setStandard(prevStd_);
        const double mhz = std::min(dect2::kMaxTuneMhz, std::max(0.001, r.freqHz / 1e6));
        b->engine.setDecodeShiftHz(0);
        b->freqMhz = mhz;
        b->tune.centerHz = mhz * 1e6;
        tunework::post(b->engine, b->tune, true);
        b->peak.clear();
        tuneAt_ = nowSec(); slot_ = std::max(0, r.slot) & 1; p2_ = p2; wasActive_ = false; term_ = false; busy_ = !r.control;
        return true;
    }
    tp::RxState state() const override {
        if (!present()) return tp::RxState::Failed;
        if (!busy_) return tp::RxState::Idle;
        return nowSec() - tuneAt_ < 0.8 ? tp::RxState::Tuning : tp::RxState::Locked;   // retune, settle and the receiver's own synchronisation
    }
    bool hearingVoice() const override {
        App* b = app();
        if (!b || !busy_ || nowSec() - tuneAt_ < 0.4) return false;
        if (!p2_) return b->rx.standard == kP25p1Std - 1 && b->rx.p25.now.active;
        const dect2::P25p2Telemetry& p = b->rx.p25p2;
        return b->rx.standard == kP25p2Std - 1 && p.timeSec - p.voiceSec[slot_] < 0.6;
    }
    bool terminated() override {
        const bool h = hearingVoice();
        if (h) wasActive_ = true;
        else if (wasActive_) { wasActive_ = false; return true; }
        return false;
    }
    bool encryptedHeard() const override {
        App* b = app();
        if (!b || !busy_ || nowSec() - tuneAt_ < 0.4) return false;
        if (!p2_) return b->rx.standard == kP25p1Std - 1 && b->rx.p25.now.active && b->rx.p25.now.encrypted;
        const dect2::P25p2Call& c = b->rx.p25p2.now[slot_];
        return c.active && c.cryptoKnown && c.encrypted;
    }
    void release() override {
        App* b = app();
        busy_ = false;
        if (!b || !first_) return;
        first_ = false;
        if (b->engine.running()) {
            if (prevStd_ > 0) b->engine.setStandard(prevStd_);
            if (prevMhz_ > 0) { b->freqMhz = prevMhz_; b->tune.centerHz = prevMhz_ * 1e6; tunework::post(b->engine, b->tune, true); }
        }
    }
    bool openSlice(const tp::TuneRequest&, int* s) override { if (!selfFollow) return false; *s = next_++ % std::max(1, maxSlices()); return true; }   // the follower opens it itself
    bool sliceHearing(int) const override { return true; }   // the grant repeats and the follower's own hang time keep the call as long as it runs
private:
    int id_;
    bool first_ = false, busy_ = false, p2_ = false, wasActive_ = false, term_ = false;
    double prevMhz_ = 0, tuneAt_ = 0;
    int prevStd_ = 0, slot_ = 0, next_ = 0;
};

struct State {
    std::unique_ptr<tp::TrunkPool> pool;
    std::vector<std::unique_ptr<AppRx>> rx;
    std::map<int, int> index;   // receiver id -> pool device
    bool loaded = false, on = false, tapped = false;
    char planPath[300] = "", accessPath[300] = "", logPath[300] = "";
    std::string status;
    int ruleLo = 0, ruleHi = 0, ruleKind = 0, ruleAct = 0, rulePri = 0;
    bool savedRetune = true;
};
State S;

std::string accessFile() { return plat::dataDir() + "/trunkpool_access.csv"; }
std::string planFile() { return plat::dataDir() + "/trunkpool_plan.csv"; }
std::string recordDir() { return recordingsDir() + "/trunk"; }

std::string readAll(const std::string& p) {
    std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
void writeAll(const std::string& p, const std::string& t) { std::ofstream f(std::filesystem::u8path(p), std::ios::binary); f << t; }

tp::TrunkPool& pool() {
    if (!S.pool) {
        S.pool.reset(new tp::TrunkPool());
        tp::PoolConfig c = S.pool->config();
        c.recordDir = recordDir();
        c.recordMetaOnly = true;
        S.pool->setConfig(c);
        S.pool->wallBase = (int64_t)std::time(nullptr) - (int64_t)nowSec();
    }
    return *S.pool;
}

void loadAll() {
    S.loaded = true;
    plat::Prefs& pf = plat::basePrefs();
    S.on = pf.getB("trunkpool.on", false);
    tp::PoolConfig c = pool().config();
    c.hangSec = pf.getD("trunkpool.hang", c.hangSec);
    c.maxSetupSec = pf.getD("trunkpool.setup", c.maxSetupSec);
    c.preempt = pf.getB("trunkpool.preempt", c.preempt);
    c.record = pf.getB("trunkpool.record", c.record);
    pool().setConfig(c);
    pool().access.allowByDefault = pf.getB("trunkpool.allowall", true);
    if (std::filesystem::exists(std::filesystem::u8path(accessFile()))) pool().access.importCsv(readAll(accessFile()));
    if (std::filesystem::exists(std::filesystem::u8path(planFile()))) pool().plan.importCsv(readAll(planFile()));
}

void saveCfg() {
    plat::Prefs& pf = plat::basePrefs();
    const tp::PoolConfig& c = pool().config();
    pf.setB("trunkpool.on", S.on);
    pf.setD("trunkpool.hang", c.hangSec);
    pf.setD("trunkpool.setup", c.maxSetupSec);
    pf.setB("trunkpool.preempt", c.preempt);
    pf.setB("trunkpool.record", c.record);
    pf.setB("trunkpool.allowall", pool().access.allowByDefault);
}

// The pool's radios follow the App's open receivers; the control radio is the one running P25 Phase 1 (the receiver that owns the follower)
void syncDevices() {
    tp::TrunkPool& p = pool();
    std::vector<App*> all = receiversAll();
    for (App* b : all) {
        if (S.index.count(b->rxId)) continue;
        S.rx.emplace_back(new AppRx(b->rxId));
        S.index[b->rxId] = p.addDeviceRef(S.rx.back().get());
    }
    for (auto it = S.index.begin(); it != S.index.end();) {
        bool open = false;
        for (App* b : all) open = open || b->rxId == it->first;
        if (!open) { p.removeDevice(it->second); it = S.index.erase(it); } else ++it;
    }
    int hint = -1;
    for (App* b : all) if (b->engine.running() && b->rx.standard == kP25p1Std - 1 && S.index.count(b->rxId)) { hint = S.index[b->rxId]; break; }
    for (auto& r : S.rx) r->selfFollow = S.index.count(r->id()) && S.index[r->id()] == p.controlDevice();
    if (hint >= 0 && p.controlDevice() != hint && p.setControl(hint)) {}
}

void connect() {
    dect2::trunk::Controller& c = p25TrunkController();
    if (S.on && !S.tapped) {
        S.tapped = true;
        c.grantTap = [](const dect2::trunk::Grant& g, double now) {
            tp::GrantEvent e = tp::grantFromTrunk(g);
            if (!(e.freqHz > 0) && e.channel >= 0 && !g.dmr) e.freqHz = pool().plan.p25Freq(e.channel);   // a channel plan from the user's file, before the site's own identifier table is heard
            const auto& si = p25TrunkController().sys.site();
            if (si.site >= 0) e.site = (si.rfss >= 0 ? si.rfss * 1000 : 0) + si.site;   // the pool merges the same call granted by another site
            pool().onGrant(e, now);
        };
        dect2::trunk::FollowConfig fc = c.follower.config();
        S.savedRetune = fc.allowRetune;
        fc.allowRetune = false;   // the pool owns the retuning: the control radio never leaves
        c.follower.setConfig(fc);
        p25VoiceRadioOff(true);
    } else if (!S.on && S.tapped) {
        S.tapped = false;
        c.grantTap = nullptr;
        dect2::trunk::FollowConfig fc = c.follower.config();
        fc.allowRetune = S.savedRetune;
        c.follower.setConfig(fc);
        p25VoiceRadioOff(false);
        pool().clear();
    }
}

const char* stateWord(tp::CallState s) {
    switch (s) {
    case tp::CallState::Tuning: return tr("tuning");
    case tp::CallState::Active: return tr("active");
    case tp::CallState::Hold: return tr("hold");
    default: return tr("ended");
    }
}
const char* roleWord(tp::Role r) {
    switch (r) {
    case tp::Role::Control: return tr("control");
    case tp::Role::Voice: return tr("voice");
    case tp::Role::Idle: return tr("idle");
    case tp::Role::Lost: return tr("lost");
    default: return tr("off");
    }
}

// the directory's name for a talkgroup of a protocol the directory covers (P25, DMR, TETRA); "" for the others or an unknown number
std::string tgName(tp::Proto pr, int tg) {
    if (tg < 0) return "";
    switch (pr) {
        case tp::Proto::P25p1: case tp::Proto::P25p2: return tgdirui::nameOf(dect2::TgKind::P25, (uint32_t)tg);
        case tp::Proto::DmrTier3: case tp::Proto::DmrCapPlus: case tp::Proto::DmrConnectPlus: return tgdirui::nameOf(dect2::TgKind::Dmr, (uint32_t)tg);
        case tp::Proto::Tetra: return tgdirui::nameOf(dect2::TgKind::Tetra, (uint32_t)tg);
        default: return "";
    }
}

void drawDevices() {
    tp::TrunkPool& p = pool();
    if (ImGui::BeginTable("##tp_dev", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn(tr("On"), ImGuiTableColumnFlags_WidthFixed, 34 * gUi);
        ImGui::TableSetupColumn(tr("Radio"), ImGuiTableColumnFlags_None, 1.f);
        ImGui::TableSetupColumn(tr("Role"), ImGuiTableColumnFlags_None, 1.f);
        ImGui::TableSetupColumn(tr("TG"), ImGuiTableColumnFlags_None, 1.f);
        ImGui::TableSetupColumn(tr("MHz"), ImGuiTableColumnFlags_None, 1.4f);
        ImGui::TableSetupColumn(tr("Calls"), ImGuiTableColumnFlags_None, 0.8f);
        ImGui::TableHeadersRow();
        for (const tp::DeviceInfo& d : p.devices()) {
            if (d.role == tp::Role::Off && !d.present && d.name == "(removed)") continue;
            ImGui::PushID(d.index);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool en = d.enabled;
            if (ImGui::Checkbox("##en", &en)) p.setEnabled(d.index, en);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(d.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(roleWord(d.role));
            if (d.slices > 0) { ImGui::SameLine(); ImGui::TextDisabled("+%d", d.slices); }
            ImGui::TableNextColumn();
            if (d.tg >= 0) {
                ImGui::Text("%d", d.tg);
                for (const tp::CallRecord& cr : p.calls()) if (cr.id == d.call) { const std::string nm = tgName(cr.proto, d.tg); if (!nm.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", nm.c_str()); } break; }
            } else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            if (d.freqHz > 0) ImGui::Text("%.4f", d.freqHz / 1e6); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::Text("%llu", (unsigned long long)d.calls);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void drawCalls() {
    tp::TrunkPool& p = pool();
    if (ImGui::BeginTable("##tp_calls", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerV, ImVec2(0, 200 * gUi))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("TG"), ImGuiTableColumnFlags_None, 0.8f);
        ImGui::TableSetupColumn(tr("Unit"), ImGuiTableColumnFlags_None, 0.8f);
        ImGui::TableSetupColumn(tr("MHz"), ImGuiTableColumnFlags_None, 1.2f);
        ImGui::TableSetupColumn(tr("Radio"), ImGuiTableColumnFlags_None, 0.9f);
        ImGui::TableSetupColumn(tr("State"), ImGuiTableColumnFlags_None, 0.9f);
        ImGui::TableSetupColumn(tr("Time"), ImGuiTableColumnFlags_None, 0.7f);
        ImGui::TableSetupColumn(tr("Note"), ImGuiTableColumnFlags_None, 2.5f);
        ImGui::TableHeadersRow();
        const auto& v = p.calls();
        for (size_t k = v.size(); k-- > 0 && v.size() - k <= 300;) {
            const tp::CallRecord& r = v[k];
            ImGui::PushID((int)r.id);
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%d%s", r.tg, r.emergency ? " !" : "");
            { const std::string nm = tgName(r.proto, r.tg); if (!nm.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", nm.c_str()); } }
            ImGui::TableNextColumn(); if (r.src >= 0) ImGui::Text("%d", r.src); else ImGui::TextDisabled("-");
            ImGui::TableNextColumn(); ImGui::Text("%.4f", r.freqHz / 1e6);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.deviceName.empty() ? "-" : r.deviceName.c_str());
            ImGui::TableNextColumn();
            if (r.missed) ImGui::TextDisabled("%s", tr("not followed")); else ImGui::TextUnformatted(stateWord(r.state));
            ImGui::TableNextColumn(); ImGui::Text("%.0f s", r.durationSec());
            ImGui::TableNextColumn(); ImGui::TextUnformatted((r.note.empty() ? r.endReason : r.note).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void drawAdvanced(App& a) {
    (void)a;
    tgdirui::section(dect2::TgKind::P25); tgdirui::section(dect2::TgKind::Dmr); tgdirui::section(dect2::TgKind::Tetra);   // the names shown beside the talkgroups above
    tp::TrunkPool& p = pool();
    tp::PoolConfig c = p.config();
    bool changed = false;
    ImGui::SetNextItemWidth(120 * gUi);
    float hang = (float)c.hangSec;
    if (ImGui::SliderFloat(tr("Hang time (s)##tp_hang"), &hang, 0.5f, 10.f, "%.1f")) { c.hangSec = hang; changed = true; }
    ImGui::SetNextItemWidth(120 * gUi);
    float setup = (float)c.maxSetupSec;
    if (ImGui::SliderFloat(tr("Retune wait (s)##tp_setup"), &setup, 0.5f, 10.f, "%.1f")) { c.maxSetupSec = setup; changed = true; }
    if (ImGui::Checkbox(tr("Priority talkgroups take a radio##tp_pre"), &c.preempt)) changed = true;
    if (ImGui::Checkbox(tr("Keep a record of every call##tp_rec"), &c.record)) changed = true;
    ImGui::SameLine(); kit::folderButton("tprec", recordDir());
    bool all = p.access.allowByDefault;
    if (ImGui::Checkbox(tr("Follow talkgroups that are not listed##tp_all"), &all)) { p.access.allowByDefault = all; changed = true; }
    if (changed) { p.setConfig(c); saveCfg(); }
    ImGui::TextDisabled("%s", tr("Calls are saved in the recordings folder, trunk. Sound needs the vocoder add-on."));

    ImGui::SeparatorText(tr("Talkgroup and radio rules"));
    {
        const char* kinds[] = {tr("Talkgroup"), tr("Radio ID")};
        const char* acts[] = {tr("Allow"), tr("Deny")};
        ImGui::SetNextItemWidth(110 * gUi); ImGui::Combo("##tp_rkind", &S.ruleKind, kinds, 2);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt("##tp_rlo", &S.ruleLo, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::InputInt("##tp_rhi", &S.ruleHi, 0);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi); ImGui::Combo("##tp_ract", &S.ruleAct, acts, 2);
        ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi); ImGui::InputInt("##tp_rpri", &S.rulePri, 0);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Priority 1 (highest) to 10, 0 none. A higher priority talkgroup takes a radio from a lower one."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Add##tp_radd"))) {
            tp::AccessRule r;
            r.kind = S.ruleKind ? tp::AccessRule::Unit : tp::AccessRule::Talkgroup;
            r.lo = std::max(0, S.ruleLo); r.hi = std::max(r.lo, S.ruleHi);
            r.act = S.ruleAct ? tp::AccessRule::Deny : tp::AccessRule::Allow;
            r.priority = std::min(10, std::max(0, S.rulePri));
            p.access.add(r);
            writeAll(accessFile(), p.access.exportCsv());
        }
        ImGui::TextDisabled("%s", tr("From, to (the same number for one), action, priority"));
        const auto& rules = p.access.rules();
        for (size_t i = 0; i < rules.size(); i++) {
            const tp::AccessRule& r = rules[i];
            ImGui::PushID((int)i);
            ImGui::Text("%s %d-%d %s %d", r.kind == tp::AccessRule::Unit ? tr("Radio ID") : tr("Talkgroup"), r.lo, r.hi, r.act == tp::AccessRule::Allow ? tr("Allow") : tr("Deny"), r.priority);
            ImGui::SameLine();
            if (ImGui::SmallButton("x##rm")) { p.access.remove(i); writeAll(accessFile(), p.access.exportCsv()); ImGui::PopID(); break; }
            ImGui::PopID();
        }
        ImGui::SetNextItemWidth(260 * gUi);
        ImGui::InputText("##tp_apath", S.accessPath, sizeof S.accessPath);
        ImGui::SameLine();
        if (ImGui::Button(tr("Import rules##tp_aimp"))) {
            std::string err; int sk = 0;
            const int n = p.access.importCsv(readAll(S.accessPath), &err, &sk);
            S.status = std::to_string(n) + " " + tr("rules read") + (sk ? ", " + std::to_string(sk) + " " + tr("skipped") + ". " + err : std::string());
            writeAll(accessFile(), p.access.exportCsv());
        }
    }

    ImGui::SeparatorText(tr("Channel plan"));
    ImGui::TextDisabled("%s", tr("System, identifier table (iden_up) and channel numbers from a CSV file"));
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText("##tp_ppath", S.planPath, sizeof S.planPath);
    ImGui::SameLine();
    if (ImGui::Button(tr("Import plan##tp_pimp"))) {
        std::string err; int sk = 0;
        const int n = p.plan.importCsv(readAll(S.planPath), &err, &sk);
        S.status = std::to_string(n) + " " + tr("rows read") + (sk ? ", " + std::to_string(sk) + " " + tr("skipped") + ". " + err : std::string());
        writeAll(planFile(), p.plan.exportCsv());
    }
    ImGui::Text("%s: %zu, %s: %zu", tr("Identifiers"), p.plan.idenCount(), tr("Channels"), p.plan.channelCount());

    ImGui::SeparatorText(tr("Call log"));
    ImGui::SetNextItemWidth(260 * gUi);
    ImGui::InputText("##tp_lpath", S.logPath, sizeof S.logPath);
    ImGui::SameLine();
    if (ImGui::Button(tr("Export log##tp_lexp"))) {
        const std::string path = S.logPath[0] ? std::string(S.logPath) : plat::dataDir() + "/trunk_calls.csv";
        writeAll(path, p.callLogCsv());
        S.status = std::string(tr("Saved")) + " " + path;
    }
}

}  // namespace

void trunkPoolTick(App& a) {
    (void)a;
    if (!S.loaded) loadAll();
    connect();
    if (!S.on) return;
    syncDevices();
    tp::TrunkPool& p = pool();
    const double now = nowSec();
    dect2::trunk::Controller& c = p25TrunkController();
    if (c.status(now).controlLocked) p.onControlSeen(now);
    if (c.follower.currentControlHz() > 0) p.setControlChannel(c.follower.currentControlHz());
    p.tick(now);
}

void trunkPoolWindow(App& a) {
    trunkPoolTick(a);
    if (!a.trunkPoolOpen) return;
    firstWindowSize(760 * gUi, 560 * gUi);
    if (!ImGui::Begin(trw("Multi-radio trunking"), &a.trunkPoolOpen)) { ImGui::End(); return; }
    tp::TrunkPool& p = pool();
    ImGui::TextWrapped("%s", tr("One radio listens to the control channel of a trunked system. When it announces a call, another radio is tuned to the voice channel, and the call is logged. With one radio, calls inside its capture are followed and the rest are not."));
    ImGui::Spacing();
    if (ImGui::Checkbox(tr("Follow calls with several radios##tp_on"), &S.on)) saveCfg();
    ImGui::SameLine();
    if (ImGui::Button(tr("Auto-assign devices##tp_auto"))) {
        S.on = true; saveCfg(); connect(); syncDevices();
        int hint = -1;
        for (App* b : receiversAll()) if (b->engine.running() && b->rx.standard == kP25p1Std - 1 && S.index.count(b->rxId)) { hint = S.index[b->rxId]; break; }
        p.autoAssign(hint);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The radio on the control channel stays there; every other open receiver becomes a voice radio. Open more receivers with + at the bottom."));
    ImGui::TextUnformatted(S.on ? p.statusLine(nowSec()).c_str() : tr("Off"));
    if (!S.status.empty()) ImGui::TextDisabled("%s", S.status.c_str());
    ImGui::Separator();
    if (!S.on) ImGui::TextDisabled("%s", tr("Start a P25 receiver on the control channel, then press Auto-assign devices."));
    else {
        drawDevices();
        ImGui::Spacing();
        const dect2::tpool::PoolStats& st = p.stats();
        ImGui::TextDisabled(tr("Calls %llu, followed %llu, retune %.2f s (max %.2f s)"), (unsigned long long)st.calls, (unsigned long long)st.followed, st.retuneAvgSec, st.retuneMaxSec);
        drawCalls();
        if (!p.messages.empty()) {
            ImGui::TextDisabled("%s", p.messages.back().c_str());
        }
    }
    if (!modepanel::easyModeOn()) {
        ImGui::Separator();
        if (ImGui::CollapsingHeader(trw("Advanced##tp_adv"))) drawAdvanced(a);
    }
    ImGui::End();
}
