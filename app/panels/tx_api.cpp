// The transmit panel's pieces for other screens (tx_api.h): thin wrappers over the panel's own state and drawers.
#include "tx_api.h"
#include "tx_state.h"

using namespace txui;

namespace txapi {

namespace {
struct Ctx { const TxModeSpec* mode = nullptr; const DeviceInfo* radio = nullptr; int conflict = 0; };
Ctx ctx(App& a) {
    TxUi& s = ui();
    ensureTxMode(a, s);
    loadPayloadPrefs(s);
    Ctx c;
    c.mode = txModeById(s.modeId);
    c.radio = bindRadio(a, s, c.conflict);
    if (c.mode && s.pvMode != s.modeId) paramsEnter(s, *c.mode);
    if (c.mode) paramsFor(s, *c.mode);
    return c;
}
char* buf(TxUi& s, dect2::TxPField f, size_t& cap) {
    switch (f) {
    case dect2::TxPField::Callsign: cap = sizeof s.callsign; return s.callsign;
    case dect2::TxPField::Grid: cap = sizeof s.grid; return s.grid;
    case dect2::TxPField::Text: cap = sizeof s.text; return s.text;
    case dect2::TxPField::Dest: cap = sizeof s.dest; return s.dest;
    case dect2::TxPField::Path: cap = sizeof s.path; return s.path;
    case dect2::TxPField::File: cap = sizeof s.file; return s.file;
    case dect2::TxPField::Name: cap = sizeof s.name; return s.name;
    case dect2::TxPField::Network: cap = sizeof s.network; return s.network;
    default: cap = 0; return nullptr;
    }
}
} // namespace

bool selectMode(App& a, const std::string& id) {
    const TxModeSpec* m = txModeById(id);
    if (!m) return false;
    TxUi& s = ui();
    ensureTxMode(a, s);
    if (s.session.running()) return s.modeId == id;   // never changed under a running transmission
    chooseMode(a, s, *m);
    paramsEnter(s, *m);
    return true;
}
std::string modeId() { return ui().modeId; }

void setText(dect2::TxPField f, const std::string& v) {
    size_t cap = 0;
    if (char* b = buf(ui(), f, cap)) snprintf(b, cap, "%s", v.c_str());
}
std::string text(dect2::TxPField f) {
    size_t cap = 0;
    const char* b = buf(ui(), f, cap);
    return b ? std::string(b) : std::string();
}
bool setParam(const std::string& key, double v) {
    TxUi& s = ui();
    const PDef* d = dect2::txParamFind(s.params, key);
    if (!d || d->custom || d->type == dect2::TxPType::Text || d->type == dect2::TxPType::File) return false;
    s.pv[key] = v;
    return true;
}
double param(const std::string& key, double def) {
    const auto it = ui().pv.find(key);
    return it == ui().pv.end() ? def : it->second;
}
void setFrequencyMhz(double mhz) { ui().freqMhz = mhz; ui().linkRx = false; }
double frequencyMhz() { return ui().freqMhz; }

TxState state() {
    const TxUi& s = ui();
    if (s.pendingStart || s.pendingCarrier) return TxState::Preparing;
    if (s.session.running()) return s.sessionDry ? TxState::Test : TxState::OnAir;
    if (s.armed) return TxState::Armed;
    return s.doneShown ? TxState::Done : TxState::Idle;
}

bool arm(App& a) {
    const Ctx c = ctx(a);
    TxUi& s = ui();
    if (!c.mode || !canStart(a, s, c.radio, c.conflict, c.mode)) return false;
    txArm(s, true);
    return true;
}
bool transmit(App& a) {
    const Ctx c = ctx(a);
    TxUi& s = ui();
    if (!c.mode || s.session.running() || !canStart(a, s, c.radio, c.conflict, c.mode)) return false;
    if (txNeedsArm(s) && !s.armed) return false;
    s.pendingStart = true;   // the Transmit panel (or drawFlow) opens the radio on its next frame
    return true;
}
void stop(App& a, const char* why) { stopTx(a, ui(), why ? why : "by the user"); txArm(ui(), false); }

void drawFlow(App& a) {
    const Ctx c = ctx(a);
    TxUi& s = ui();
    // the start the flow asked for (the Transmit panel does the same when it is drawn: whichever runs first starts it, once)
    if (s.pendingStart && !s.session.running() && c.mode && canStart(a, s, c.radio, c.conflict, c.mode)) {
        s.pendingStart = false;
        if (scheduleConfigured(s)) scheduleArm(s); else startTx(a, s, c.radio, c.mode, false);
    }
    drawStatusHeader(a, s, c.radio, c.conflict, c.mode);
}
void drawWhatToSend(App& a) {
    const Ctx c = ctx(a);
    if (!c.mode) { ImGui::TextDisabled("%s", tr("Choose a signal first.")); return; }
    TxUi& s = ui();
    ImGui::BeginDisabled(s.session.running());
    payloadPanel(s, *c.mode, s.session.running());
    ImGui::EndDisabled();
}
void drawAdvanced(App& a) {
    const Ctx c = ctx(a);
    if (c.mode) advancedPanel(ui(), *c.mode, ui().session.running());
}
void drawPreview(App& a) {
    const Ctx c = ctx(a);
    stepSend(a, ui(), c.radio, c.conflict, c.mode, ui().session.running());
}

} // namespace txapi
