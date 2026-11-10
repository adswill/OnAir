// MeshCore device screen (the Device view of the MeshCore entry): OnAir as a MeshCore companion node that sends and receives, built on
// dect2/mesh/meshcore_node.h. One click on Join MeshCore makes an identity on this computer (if there is none), sets the region's standard
// radio preset (EU/UK Narrow 869.618 MHz in Europe, USA 910.525 MHz, Australia 915.8 MHz), starts the receiver and queues an advert. Chat:
// channels (Public, hashtag channels, channels with a key the user adds) and contacts (from the adverts heard), each with its messages and
// their state (queued, sent, delivered = ACK received, failed after three attempts). Manual LoRa settings, the path hash size, the position
// and the identity's export / import are under Advanced.
//
// Transmit follows the Transmit window's rules: nothing goes out until the user arms it (not saved: every start begins disarmed), the
// one-time notice is asked first, the radio is claimed and given back, the map connection is paused, the HackRF amplifier stays off.
// Nothing is blocked: the airtime used in the last hour is shown against the band's duty-cycle limit (EU 868: 1 % or 10 % by sub-band) and
// a warning appears above it. A half-duplex radio (HackRF) stops the receiver while it sends; frames heard meanwhile are lost (the node's
// retries cover a lost ACK). The identity and the contacts are kept in the app's data folder (meshcore-node.txt, readable only by the
// user); nothing is uploaded. Not tested on the air against a MeshCore radio yet: interoperability was checked against the firmware's own
// code on a computer (tests/mesh/test_mesh_meshcore_node.cpp).
#include "mesh_internal.h"
#include "dect2/common/time_fmt.h"
#include "dect2/engine/device_claims.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/mesh/meshcore_node.h"
#include "dect2/mesh/meshcore_tx.h"
#include "dect2/tx/tx.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace meshui {

void mcLoad();
void mcApply(App& a, const dect2::MeshCorePresetEntry& p);
void saveCustom(const App& a);

namespace {

using namespace dect2;
using namespace dect2::meshcore;

constexpr double kTxIfHz = 250e3;   // frames are made this far above the radio's centre: its carrier leak stays off the channel

// frames from the receiver thread (setFrameCallback); never freed, so a late callback at exit finds it
struct RxQueue { std::mutex mu; std::deque<std::pair<std::vector<uint8_t>, float>> q; };
RxQueue& rxq() { static RxQueue* q = new RxQueue; return *q; }

struct Dev {
    bool loaded = false, hooked = false;
    Node node;
    bool dirty = false;
    double lastSave = 0;
    // transmit
    bool armed = false, askNotice = false;
    float txGain = 10;
    TxSession session;
    bool sending = false, claimed = false, pausedRx = false;
    std::vector<Outgoing> batch;
    std::string radioName, error;
    uint64_t framesSent = 0;
    // interface
    std::string sel = "c:Public";       // "c:<channel>" or "p:<public key hex>"
    char input[kMaxText + 1] = "";
    char name[33] = "";
    char chName[40] = "", chKey[96] = "", importHex[200] = "";
    std::string chMsg, idMsg;
    bool showKey = false, focusInput = false;
    size_t shown = 0;
    double lastAdvert = 0;
};
Dev D;

double mono() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
std::string nodeFile() { return plat::dataDir() + "/meshcore-node.txt"; }

void save() {
    const std::string path = nodeFile(), tmp = path + ".part";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << D.node.save();
    }
#ifndef _WIN32
    chmod(tmp.c_str(), 0600);   // the private key: for this user only
#endif
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
    D.dirty = false;
    D.lastSave = mono();
}

Radio radioOf(const App& a) {
    const State::Custom& c = customOf(a);
    Radio r;
    r.freqHz = a.freqMhz * 1e6;
    r.bwHz = c.bwHz; r.sf = c.sf; r.cr = c.cr; r.preamble = c.preamble;
    r.pathHashSize = (int)std::clamp<long>(plat::prefs().getI("mcHashSize", 1), 1, 3);
    return r;
}

void load(App& a) {
    if (D.loaded) return;
    D.loaded = true;
    plat::Prefs& p = plat::prefs();
    std::ifstream f(nodeFile(), std::ios::binary);
    if (f) { std::stringstream ss; ss << f.rdbuf(); D.node.load(ss.str()); }
    snprintf(D.name, sizeof D.name, "%s", p.getS("mcName", "OnAir").c_str());
    D.txGain = (float)p.getD("mcTxGain", 10);
    NodeConfig& c = D.node.config();
    c.name = D.name;
    c.repeater = p.getB("mcRepeater", false);
    c.sharePosition = p.getB("mcSharePos", false);
    c.lat = p.getD("mcLat", 0); c.lon = p.getD("mcLon", 0);
    c.radio = radioOf(a);
    D.node.log = [&a](const std::string& s) { a.engine.log(s); };
}

void hook(App& a) {
    if (D.hooked) return;
    D.hooked = true;
    a.engine.mesh().setFrameCallback([](int, const std::vector<uint8_t>& b, float snr, double) {
        RxQueue& q = rxq();
        std::lock_guard<std::mutex> lk(q.mu);
        if (q.q.size() < 256) q.q.push_back({b, snr});
    });
}

// the radio that sends, and whether the receiver uses it (as the Flipper chat)
const DeviceInfo* txRadio(const App& a, bool& same) {
    same = false;
    const std::vector<int> radios = txRadioChoices(a.devices);
    const DeviceInfo* rxDev = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? &a.devices[(size_t)a.devIdx] : nullptr;
    if (rxDev && rxDev->isRadio())
        for (int i : radios) if (deviceClaimKey(a.devices[(size_t)i]) == deviceClaimKey(*rxDev)) { same = true; return &a.devices[(size_t)i]; }
    for (int i : radios) {
        const DeviceInfo& d = a.devices[(size_t)i];
        const int h = deviceHolder(d);
        if (h == 0 || h == kTxClaimOwner || d.txFullDuplex) return &d;
    }
    return nullptr;
}

void resumeRx(App& a) {
    if (!D.pausedRx) return;
    D.pausedRx = false;
    startReceiver(a);
}

void fail(App& a, const std::string& why) {
    D.error = why;
    D.batch.clear();
    if (D.claimed) { txReleaseRadio(); D.claimed = false; mapTransmitAbort(a); }
    D.sending = false;
    resumeRx(a);
}

// every frame: see the current burst end, then start the next one with every frame that is due
void sendTick(App& a) {
    const double now = mono();
    if (D.sending) {
        if (D.session.running()) return;
        const TxSession::Stats st = D.session.stats();
        D.session.stop();
        if (D.claimed) { txReleaseRadio(); D.claimed = false; mapTransmitEnded(); }
        D.sending = false;
        if (st.failed) { fail(a, st.error.empty() ? std::string("the radio stopped") : st.error); return; }
        for (const Outgoing& o : D.batch) { D.node.noteSent(o, now); D.framesSent++; a.engine.log("MeshCore: sent " + o.what); }
        D.batch.clear();
        D.dirty = true;
    }
    if (!D.armed) {
        D.node.dropStale(now - 30);
        resumeRx(a);
        return;
    }
    if (D.node.nextDue() < 0 || D.node.nextDue() > now) { resumeRx(a); return; }
    if (D.askNotice) return;
    if (!plat::basePrefs().getB("txNoticeAccepted", false)) { D.askNotice = true; return; }
    if (txActive()) { D.error = "the Transmit window is sending: stop it first"; return; }
    bool same = false;
    const DeviceInfo* radio = txRadio(a, same);
    if (!radio) { D.error = "no radio here can transmit (a HackRF, PlutoSDR, LimeSDR, USRP or bladeRF can)"; return; }
    const TxModeSpec* mode = txModeById("meshcore");
    if (!mode) { fail(a, "this build has no MeshCore transmitter"); return; }
    // the due frames, up to about four seconds of airtime in one burst
    D.batch.clear();
    const Radio r = radioOf(a);
    double air = 0;
    Outgoing o;
    while (air < 4 && D.node.popDue(now, o)) { air += airtimeSec(r, o.bytes.size()); D.batch.push_back(std::move(o)); }
    if (D.batch.empty()) return;
    D.error.clear();
    const DeviceInfo dev = *radio;   // a copy: stopping the receiver may refresh the list
    if (same && !dev.txFullDuplex && a.engine.running()) { a.engine.stop(); D.pausedRx = true; }
    std::string err;
    if (!txClaimRadio(dev, err)) { fail(a, err); return; }
    D.claimed = true;
    std::unique_ptr<TxSink> sink = makeTxSink(dev);
    if (!sink) { const std::string why = txUnavailableReason(dev); fail(a, why.empty() ? std::string("this radio has no transmit driver in OnAir") : why); return; }
    std::vector<std::vector<uint8_t>> frames;
    for (const Outgoing& f : D.batch) frames.push_back(f.bytes);
    TxRequest q;
    q.modeId = "meshcore";
    q.payload = meshCoreTxPayload(frames);
    q.loop = true;   // the generator ends after the frames
    q.tune.centerHz = a.freqMhz * 1e6;
    q.tune.offsetHz = -kTxIfHz;
    q.tune.gainDb = dev.txGainMaxDb > dev.txGainMinDb ? std::clamp((double)D.txGain, dev.txGainMinDb, dev.txGainMaxDb) : D.txGain;
    q.tune.ampOn = false;
    q.opt["meshcore.ifHz"] = kTxIfHz;
    q.opt["meshcore.sf"] = r.sf; q.opt["meshcore.bwHz"] = r.bwHz; q.opt["meshcore.cr"] = r.cr; q.opt["meshcore.preamble"] = r.preamble;
    mapPauseForTransmit(a);
    if (!D.session.start(std::move(sink), q, TxRateLimits{dev.txMinRateHz, dev.txMaxRateHz}, err)) { fail(a, err.empty() ? std::string("the radio did not start") : err); return; }
    D.sending = true;
    D.radioName = txRadioLabel(dev);
}

const char* presetFor(int rxRegion) { return rxRegion == 1 ? "USA" : rxRegion >= 2 ? "Australia" : "EU/UK (Narrow)"; }

void join(App& a) {
    if (!D.node.identity().valid) {
        D.node.setIdentity(identityNew());
        D.dirty = true;
        a.engine.log("MeshCore: a new identity was made on this computer (public key " + hexOf(D.node.identity().pub, 4) + "...)");
    }
    if (!S.mcListed) mcLoad();
    const std::string want = presetFor(kPlans[effRegion(a)].rxRegion);
    for (const auto& p : S.mcList)
        if (p.title == want) {
            mcApply(a, p);
            S.mcPreset = p.title;
            plat::prefs().setS("meshMcPreset", p.title.c_str());
            plat::prefs().setI("mcHashSize", p.note == "2B" ? 2 : p.note == "3B" ? 3 : 1);
        }
    D.node.config().radio = radioOf(a);
    if (!a.engine.running()) startReceiver(a);
    D.node.sendAdvert(mono());
    D.lastAdvert = mono();
    D.sel = "c:Public";
}

void noticePopup() {
    if (D.askNotice && !ImGui::IsPopupOpen("##mcnotice")) ImGui::OpenPopup("##mcnotice");
    ImGui::SetNextWindowSize(ImVec2(480 * gUi, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("##mcnotice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::PushTextWrapPos(460 * gUi);
        ImGui::TextColored(pal::heading(), "%s", tr("Before you transmit"));
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Transmitting needs a licence or permission where you are. You are responsible for what you emit and for not interfering with others "
                                  "(including services that people rely on for safety). Connect an antenna, an attenuator or a dummy load to the radio before you start. "
                                  "OnAir does not limit the frequency or the power: the radio's own limits are the only ones."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::Button(tr("I understand"), ImVec2(220 * gUi, 0))) { plat::basePrefs().setB("txNoticeAccepted", true); plat::basePrefs().flush(); D.askNotice = false; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), ImVec2(-1, 0))) { D.askNotice = false; D.armed = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
}

std::string contactLabel(const Contact& c) {
    std::string s = c.name.empty() ? hexOf(c.pub, 4) : c.name;
    if (c.type == AdvRepeater) s += " (repeater)";
    else if (c.type == AdvRoom) s += " (room)";
    else if (c.type == AdvSensor) s += " (sensor)";
    return s;
}

const char* statusText(int st) {
    switch (st) {
    case Message::Queued: return tr("queued");
    case Message::Sent: return tr("sent, waiting for the ACK");
    case Message::Delivered: return tr("delivered");
    case Message::Failed: return tr("no ACK");
    default: return "";
    }
}

// ---------------------------------------------------------------- the top rows

void topRows(App& a) {
    const bool have = D.node.identity().valid;
    if (ImGui::Button(have ? tr("Join MeshCore again") : tr("Join MeshCore"), ImVec2(170 * gUi, 0))) join(a);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tr("Makes this computer's MeshCore identity (the first time), sets the region's standard radio preset, starts the receiver "
                                   "and queues an advert (it goes out once transmit is armed)"));
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Name")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(120 * gUi);
    if (ImGui::InputText("##mcname", D.name, sizeof D.name)) { plat::prefs().setS("mcName", D.name); D.node.config().name = D.name; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("The name in your adverts and in front of your channel messages"));
    flowNext(10 * gUi);
    if (have) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s %s", tr("key"), hexOf(D.node.identity().pub, 4).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", tr("Your public key (the first byte is your path hash):"), hexOf(D.node.identity().pub, 32).c_str());
        flowNext(10 * gUi);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%.3f MHz SF%d %.1f kHz 4/%d", a.freqMhz, customOf(a).sf, customOf(a).bwHz / 1e3, customOf(a).cr);
    flowEnd();

    // transmit: arm, radio, gain, duty cycle
    bool same = false;
    const DeviceInfo* radio = txRadio(a, same);
    if (D.armed) ImGui::PushStyleColor(ImGuiCol_Text, pal::badRed());
    if (ImGui::Checkbox(tr("Arm transmit"), &D.armed) && D.armed && !plat::basePrefs().getB("txNoticeAccepted", false)) D.askNotice = true;
    if (D.armed) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Nothing goes on the air until this is on. It is off at every start of OnAir."));
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    if (!radio) ImGui::TextColored(pal::warnAmber(), "%s", tr("no radio that can transmit"));
    else {
        ImGui::TextDisabled("%s %s", tr("with"), txRadioLabel(*radio).c_str());
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("TX gain")); ImGui::SameLine(0, 5 * gUi);
        const float lo = radio->txGainMaxDb > radio->txGainMinDb ? (float)radio->txGainMinDb : 0.f, hi = radio->txGainMaxDb > radio->txGainMinDb ? (float)radio->txGainMaxDb : 47.f;
        ImGui::SetNextItemWidth(120 * gUi);
        if (ImGui::SliderFloat("##mcgain", &D.txGain, lo, hi, "%.0f dB")) plat::prefs().setD("mcTxGain", D.txGain);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("The radio's own TX gain. EU 869.4 - 869.65 MHz allows 500 mW ERP, most other 868 MHz sub-bands 25 mW; "
                                       "a HackRF gives a few mW at most (its +14 dB amplifier stays off). Use the least power that works."));
        flowNext(10 * gUi);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", same && !radio->txFullDuplex ? tr("half duplex: the receiver pauses while it sends") : tr("the receiver keeps listening"));
        if (same && !radio->txFullDuplex && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("The receiver is off while a burst goes out and for about a second after it, while it starts again. An ACK that arrives then is "
                                       "missed: a message can show 'no ACK' although it arrived. A radio that receives while it sends (PlutoSDR, USRP, bladeRF, "
                                       "LimeSDR) avoids this."));
    }
    flowEnd();
    // the duty-cycle budget of the last hour
    const double now = mono();
    const DutyRule rule = dutyCycleRule(a.freqMhz * 1e6);
    const double used = D.node.duty().used(now);
    if (rule.limit > 0) {
        const double allowed = rule.limit * 3600;
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", tr("Airtime, last hour"));
        ImGui::SameLine(0, 6 * gUi);
        char ov[96];
        snprintf(ov, sizeof ov, "%.1f s of %.0f s", used, allowed);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, used > allowed ? pal::badRed() : used > 0.8 * allowed ? pal::warnAmber() : pal::okGreen());
        ImGui::ProgressBar((float)std::min(1.0, used / allowed), ImVec2(180 * gUi, 0), ov);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", rule.band.c_str(), tr("Counted by OnAir since it started; the limit is the band's, not OnAir's. Nothing is blocked."));
        if (used > allowed) { ImGui::SameLine(0, 8 * gUi); ImGui::TextColored(pal::badRed(), "%s", tr("over the duty-cycle limit of this band: stop sending")); }
    } else {
        ImGui::TextDisabled("%s %.1f s%s", tr("Airtime, last hour:"), used, tr(" (no duty-cycle limit for this frequency in OnAir's table: check your local rules)"));
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", tr("You are responsible for the rules where you are (licence, power, duty cycle). OnAir never blocks a transmission. "
                                 "Not tested on the air against a real MeshCore node yet."));
    ImGui::PopTextWrapPos();
    if (D.sending) ImGui::TextColored(ImVec4(1.f, 0.66f, 0.36f, 1), "%s %s (%zu %s)%s", tr("sending on"), D.radioName.c_str(), D.batch.size(), tr("frames"), D.pausedRx ? tr(", receiver paused") : "");
    else if (D.node.queued() && !D.armed) ImGui::TextColored(pal::warnAmber(), "%zu %s", D.node.queued(), tr("frames waiting: arm transmit to send them"));
    if (!D.error.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::badRed(), "%s", D.error.c_str()); ImGui::PopTextWrapPos(); }
}

// ---------------------------------------------------------------- Advanced

void advanced(App& a) {
    if (!ImGui::CollapsingHeader(trw("Advanced##mcadv"))) return;
    ImGui::Indent(8 * gUi);
    State::Custom& c = customOf(a);
    bool ch = false;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Frequency")); ImGui::SameLine(0, 5 * gUi);
    double mhz = a.freqMhz;
    ImGui::SetNextItemWidth(110 * gUi);
    if (ImGui::InputDouble("##mcmhz", &mhz, 0, 0, "%.4f MHz", ImGuiInputTextFlags_EnterReturnsTrue)) { a.freqMhz = mhz; c.mhz = mhz; ch = true; }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("SF"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(90 * gUi);
    ch |= ImGui::SliderInt("##mcsf", &c.sf, 5, 12);
    flowNext(10 * gUi);
    static const double kBw[] = {62500, 125000, 250000, 500000};
    static const char* kBwN[] = {"62.5 kHz", "125 kHz", "250 kHz", "500 kHz"};
    int bi = 0;
    for (int i = 0; i < 4; i++) if (std::fabs(c.bwHz - kBw[i]) < 1) bi = i;
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Bandwidth")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::Combo("##mcbw", &bi, kBwN, 4)) { c.bwHz = kBw[bi]; ch = true; }
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("CR 4/"); ImGui::SameLine(0, 2 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    ch |= ImGui::SliderInt("##mccr", &c.cr, 5, 8);
    flowNext(10 * gUi);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Preamble")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    ch |= ImGui::InputInt("##mcpre", &c.preamble, 0, 0);
    flowNext(10 * gUi);
    int hs = (int)std::clamp<long>(plat::prefs().getI("mcHashSize", 1), 1, 3);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Path hash")); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(80 * gUi);
    if (ImGui::SliderInt("##mchs", &hs, 1, 3, "%d B")) plat::prefs().setI("mcHashSize", hs);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Bytes per repeater in the path of the floods you start. 1 is the default; some regions use 2 or 3 (their preset says so)"));
    flowEnd();
    if (ch) {
        c.on = true; c.sync = 0x12;
        c.sf = std::clamp(c.sf, 5, 12); c.cr = std::clamp(c.cr, 5, 8); c.preamble = std::clamp(c.preamble, 6, 65535);
        saveCustom(a);
        S.mcPreset.clear();
    }
    D.node.config().radio = radioOf(a);

    NodeConfig& nc = D.node.config();
    if (ImGui::Checkbox(tr("Repeater mode"), &nc.repeater)) plat::prefs().setB("mcRepeater", nc.repeater);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Pass on the floods and the direct packets routed through this node, as a MeshCore repeater does. Off by default: "
                                                          "it transmits for others, so mind the duty cycle"));
    flowNext(12 * gUi);
    if (nc.repeater) { ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("%llu %s", (unsigned long long)D.node.repeated(), tr("repeated")); flowNext(12 * gUi); }
    if (ImGui::Checkbox(tr("Position in adverts"), &nc.sharePosition)) plat::prefs().setB("mcSharePos", nc.sharePosition);
    if (nc.sharePosition) {
        ImGui::SameLine(0, 6 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputDouble("##mclat", &nc.lat, 0, 0, "%.5f")) plat::prefs().setD("mcLat", nc.lat);
        ImGui::SameLine(0, 4 * gUi);
        ImGui::SetNextItemWidth(90 * gUi);
        if (ImGui::InputDouble("##mclon", &nc.lon, 0, 0, "%.5f")) plat::prefs().setD("mcLon", nc.lon);
    }
    flowEnd();

    // identity: export and import (the MeshCore apps' 64 byte private key in hex)
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", tr("Identity"));
    ImGui::SameLine(0, 6 * gUi);
    if (D.node.identity().valid) {
        ImGui::Checkbox(tr("Show the private key"), &D.showKey);
        if (D.showKey) {
            ImGui::SameLine(0, 6 * gUi);
            if (ImGui::SmallButton(tr("Copy"))) ImGui::SetClipboardText(identityPrivateHex(D.node.identity()).c_str());
            ImGui::PushFont(a.mono, 0);
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(identityPrivateHex(D.node.identity()).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
            ImGui::TextColored(pal::warnAmber(), "%s", tr("Anyone with this key can read your messages and send as you. It never leaves this computer unless you copy it."));
        }
    } else ImGui::TextDisabled("%s", tr("none yet (Join MeshCore makes one)"));
    ImGui::SetNextItemWidth(std::min(420 * gUi, std::max(160 * gUi, ImGui::GetContentRegionAvail().x - 90 * gUi)));
    ImGui::InputTextWithHint("##mcimp", tr("import: the 64 byte private key in hex"), D.importHex, sizeof D.importHex, ImGuiInputTextFlags_Password);
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button(tr("Import"))) {
        Identity id;
        std::string why;
        if (identityFromPrivateHex(D.importHex, id, &why)) { D.node.setIdentity(id); D.dirty = true; D.idMsg = tr("imported"); }
        else D.idMsg = why;
        std::memset(D.importHex, 0, sizeof D.importHex);
    }
    if (!D.idMsg.empty()) { ImGui::SameLine(0, 6 * gUi); ImGui::TextDisabled("%s", D.idMsg.c_str()); }
    ImGui::Unindent(8 * gUi);
}

// ---------------------------------------------------------------- chat

void sideList() {
    const double nowU = (double)std::time(nullptr);
    ImGui::TextColored(pal::heading(), "%s", tr("Channels"));
    for (const Channel& c : D.node.channels()) {
        ImGui::PushID(c.name.c_str());
        if (ImGui::Selectable(c.name.c_str(), D.sel == "c:" + c.name)) { D.sel = "c:" + c.name; D.focusInput = true; }
        if (ImGui::BeginPopupContextItem("##mcchctx")) {
            ImGui::TextDisabled("%s %s", tr("key"), channelKeyHex(c).c_str());
            if (c.name != "Public" && ImGui::MenuItem(tr("Remove the channel"))) { const std::string n = c.name; ImGui::EndPopup(); ImGui::PopID(); D.node.removeChannel(n); D.dirty = true; if (D.sel == "c:" + n) D.sel = "c:Public"; return; }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mcchn", tr("#name or channel name"), D.chName, sizeof D.chName);
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mcchk", tr("key (hex or base64; none for #name)"), D.chKey, sizeof D.chKey);
    if (ImGui::SmallButton(tr("Add channel"))) {
        Channel c;
        std::string why;
        if (!makeChannel(D.chName, D.chKey, c, &why)) D.chMsg = why;
        else if (!D.node.addChannel(c)) D.chMsg = tr("a channel with that name is already there");
        else { D.chMsg.clear(); D.sel = "c:" + c.name; D.chName[0] = D.chKey[0] = 0; D.dirty = true; }
    }
    if (!D.chMsg.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(pal::warnAmber(), "%s", D.chMsg.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::Spacing();
    ImGui::TextColored(pal::heading(), "%s (%zu)", tr("Contacts"), D.node.contacts().size());
    std::vector<const Contact*> cs;
    for (const auto& c : D.node.contacts()) cs.push_back(&c);
    std::sort(cs.begin(), cs.end(), [](const Contact* x, const Contact* y) { return x->heardAt > y->heardAt; });
    if (cs.empty()) ImGui::TextDisabled("%s", tr("none heard yet: a node shows here when its advert is heard"));
    for (const Contact* c : cs) {
        const std::string key = "p:" + c->pubHex();
        ImGui::PushID(key.c_str());
        char line[160];
        snprintf(line, sizeof line, "%s", contactLabel(*c).c_str());
        if (ImGui::Selectable(line, D.sel == key)) { D.sel = key; D.focusInput = true; }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s\n%s %s\n%s %d, SNR %.1f dB\n%s", c->pubHex().c_str(), tr("heard"), ago(nowU, c->heardAt).c_str(), tr("hops"), c->hops, c->snrDb,
                              c->outPathLen < 0 ? tr("no route yet: messages flood") : (std::string(tr("route: ")) + std::to_string(c->outPathLen & 63) + tr(" hops")).c_str());
        ImGui::PopID();
    }
}

void conversation(App& a) {
    const float inputH = ImGui::GetFrameHeightWithSpacing() * 2 + 4 * gUi;
    const bool isCh = D.sel.rfind("c:", 0) == 0;
    const std::string peer = D.sel.substr(2);
    const Contact* ct = nullptr;
    for (const auto& c : D.node.contacts()) if (c.pubHex() == peer) ct = &c;
    if (!isCh && !ct) { D.sel = "c:Public"; return; }
    ImGui::TextColored(pal::heading(), "%s", isCh ? peer.c_str() : contactLabel(*ct).c_str());
    if (!isCh) {
        ImGui::SameLine(0, 10 * gUi);
        ImGui::TextDisabled("%s", ct->outPathLen < 0 ? tr("no route: flood") : (std::to_string(ct->outPathLen & 63) + tr(" hops, direct")).c_str());
        ImGui::SameLine(0, 10 * gUi);
        if (ImGui::SmallButton(tr("Reset route"))) { D.node.resetPath(peer); D.dirty = true; }
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::SmallButton(tr("Remove contact"))) { D.node.removeContact(peer); D.dirty = true; D.sel = "c:Public"; return; }
    }
    ImGui::BeginChild("##mcconv", ImVec2(0, std::max(80 * gUi, ImGui::GetContentRegionAvail().y - inputH)), ImGuiChildFlags_Borders);
    size_t n = 0;
    for (const Message& m : D.node.messages()) {
        if ((m.kind == Message::Group) != isCh || m.peer != peer) continue;
        n++;
        ImGui::PushID((int)m.id);
        ImGui::TextDisabled("%s", dect2::timefmt::clock((int64_t)m.at).c_str());
        ImGui::SameLine(0, 8 * gUi);
        if (m.outgoing) ImGui::TextColored(ImVec4(1.f, 0.66f, 0.36f, 1), "%s", D.name);
        else ImGui::TextColored(pal::okGreen(), "%s", m.from.empty() ? "?" : m.from.c_str());
        ImGui::SameLine(0, 8 * gUi);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(m.text.c_str());
        ImGui::PopTextWrapPos();
        if (m.outgoing && !isCh) {
            ImGui::SameLine(0, 8 * gUi);
            const ImVec4 col = m.status == Message::Delivered ? pal::okGreen() : m.status == Message::Failed ? pal::badRed() : ImVec4(0.6f, 0.6f, 0.6f, 1);
            ImGui::TextColored(col, "%s", statusText(m.status));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%d %s%s", m.attempts, tr("attempt(s)"), m.status == Message::Delivered ? (std::string(", ") + std::to_string((int)(m.rtt * 1000)) + " ms").c_str() : "");
        } else if (m.outgoing && m.status == Message::Queued) { ImGui::SameLine(0, 8 * gUi); ImGui::TextDisabled("%s", tr("queued")); }
        else if (!m.outgoing && ImGui::IsItemHovered()) ImGui::SetTooltip("%s %d, SNR %.1f dB", tr("hops"), m.hops, m.snrDb);
        ImGui::PopID();
    }
    if (!n) ImGui::TextDisabled("%s", isCh ? tr("no messages on this channel yet") : tr("no messages with this contact yet"));
    if (D.node.messages().size() != D.shown) { D.shown = D.node.messages().size(); ImGui::SetScrollHereY(1.f); }
    ImGui::EndChild();
    const float bw = 80 * gUi;
    ImGui::SetNextItemWidth(std::max(100 * gUi, ImGui::GetContentRegionAvail().x - bw - 8 * gUi));
    if (D.focusInput) { ImGui::SetKeyboardFocusHere(); D.focusInput = false; }
    bool go = ImGui::InputTextWithHint("##mcin", tr("type a message, Enter sends"), D.input, sizeof D.input, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(0, 8 * gUi);
    ImGui::BeginDisabled(!D.input[0] || !D.node.identity().valid);
    go = ImGui::Button(tr("Send"), ImVec2(bw, 0)) || go;
    ImGui::EndDisabled();
    if (!D.node.identity().valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Join MeshCore first: it makes your identity"));
    if (go && D.input[0] && D.node.identity().valid) {
        std::string why;
        const bool ok = isCh ? D.node.sendChannel(peer, D.input, mono(), &why) : D.node.sendText(peer, D.input, mono(), &why);
        if (!ok) D.error = why;
        else D.dirty = true;
        D.input[0] = 0;
        D.focusInput = true;
        (void)a;
    }
}

}  // namespace

// every frame (also with another view open): the frames heard, retries, the transmitter, saving
void meshcoreTick(App& a) {
    load(a);
    hook(a);
    const double now = mono();
    {
        std::deque<std::pair<std::vector<uint8_t>, float>> q;
        { RxQueue& r = rxq(); std::lock_guard<std::mutex> lk(r.mu); q.swap(r.q); }
        if (!q.empty()) D.dirty = true;
        for (auto& f : q) D.node.receive(f.first.data(), f.first.size(), f.second, now);
    }
    D.node.config().radio = radioOf(a);
    D.node.tick(now);
    sendTick(a);
    if (D.dirty && now - D.lastSave > 3) save();
}

void meshcoreView(App& a) {
    meshcoreTick(a);
    topRows(a);
    advanced(a);
    ImGui::Spacing();
    if (D.node.identity().valid) {
        if (ImGui::Button(tr("Send advert"))) { D.node.sendAdvert(mono(), true); D.lastAdvert = mono(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Tell the mesh who you are (flood: repeaters pass it on). Others add you to their contacts from it"));
        ImGui::SameLine(0, 6 * gUi);
        if (ImGui::Button(tr("Advert to neighbours"))) { D.node.sendAdvert(mono(), false); D.lastAdvert = mono(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("A zero-hop advert: only the nodes that hear you directly"));
    }
    const float W = ImGui::GetContentRegionAvail().x, H = ImGui::GetContentRegionAvail().y;
    const float lw = std::min(260 * gUi, W * 0.35f);
    ImGui::BeginChild("##mcside", ImVec2(lw, H), ImGuiChildFlags_Borders);
    sideList();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##mcmain", ImVec2(0, H));
    conversation(a);
    ImGui::EndChild();
    noticePopup();
}

}  // namespace meshui
