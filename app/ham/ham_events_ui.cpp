// ham.decode for every station heard on FT8 / FT4 / FT2 / WSPR / the other WSJT modes and in the CW skimmer, with the logbook's marks.
// Only built when something listens (automations, Integrations, the message database): automation::wants(FHam).
#include "app.h"
#include "ham/ham_events_ui.h"
#include "ham/ham_ops_ui.h"
#include "adsb/records_ui.h"
#include "adsb/digest_ui.h"
#include <set>
#include "dect2/automation/engine.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_events.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include <cstdio>
#include <ctime>

namespace {
namespace au = dect2::automation;
namespace ham = dect2::ham;
ham::HamDecodeOnce gOnce;
ham::HamCallOnce gCw;
uint64_t gLastSeq = 0;
size_t gLastN = 0;

std::set<std::string> gNewDxccToday;   // countries heard today that were new (the digest)
std::string gNewDay;
void noteNew(const au::Event& ev) {
    static bool reg = false;
    if (!reg) {
        reg = true;
        digestRegister("3ham", [] {
            if (gNewDxccToday.empty()) return std::string();
            std::string s = std::to_string(gNewDxccToday.size()) + " new DXCC (";
            int n = 0;
            for (const auto& c : gNewDxccToday) { if (n++ == 3) { s += ", ..."; break; } s += (n > 1 ? ", " : "") + c; }
            return s + ")";
        });
    }
    const time_t t = time(nullptr); struct tm l = *localtime(&t); char d[16]; std::strftime(d, sizeof d, "%Y-%m-%d", &l);
    if (gNewDay != d) { gNewDay = d; gNewDxccToday.clear(); }
    auto it = ev.f.find("new_dxcc"), c = ev.f.find("country");
    if (it != ev.f.end() && it->second.b && c != ev.f.end() && !c->second.s.empty()) gNewDxccToday.insert(c->second.s);
}

HamOps& ops() {
    hamOpsLoad();
    HamOps& o = hamOps();
    if (o.workedDirty) { o.worked.build(o.log.qsos()); o.workedDirty = false; }
    return o;
}
}  // namespace

void hamFeedFtx(App& a, const dect2::HfdigFtxTelemetry& f) {
    if (f.decodes.empty()) return;
    if (a.rx.seq == gLastSeq && f.decodes.size() == gLastN) return;
    gLastSeq = a.rx.seq; gLastN = f.decodes.size();
    HamOps& o = ops();
    for (const dect2::FtxDecode& d : f.decodes) {
        if (d.early || d.call.empty()) continue;   // the early decode is replaced by the full one of the slot
        const std::string mode = dect2::ftxModeName(d.mode);
        if (!gOnce.first(d.call, mode, d.msg, d.slotUtc)) continue;
        ham::HamHeard h;
        h.call = d.call; h.grid = d.grid; h.msg = d.msg; h.mode = mode; h.cq = d.cq; h.dbm = d.dbm;
        h.freqHz = d.dialHz > 0 ? d.dialHz : a.freqMhz * 1e6;
        h.band = ham::bandNameFor(h.freqHz);
        h.country = hamCountry(d.call);
        h.utc = d.slotUtc; h.snrDb = d.snrDb; h.dt = d.dt; h.audioHz = d.hz;
        au::Event ev = ham::hamDecodeEvent(h, o.worked.mark(d.call, d.grid, h.band, ham::hamModeName(d.mode), hamDxccKey(d.call)));
        if (ev.f.count("lat") && d.grid.size() >= 4) recordsOffer(mode.c_str(), h.band, recordsDistKm(ev.f["lat"].n, ev.f["lon"].n), d.call, d.grid + " " + std::to_string((int)d.snrDb) + " dB");
        noteNew(ev);
        if (au::wants(au::FHam)) au::emit(std::move(ev));
    }
}

void hamFeedCw(App& a, const dect2::CwTelemetry& t) {
    if (!au::wants(au::FHam)) return;
    HamOps& o = ops();
    const double now = (double)time(nullptr);
    for (const auto& s : t.signals) {
        for (const std::string& c : s.calls) {
            if (!gCw.first(std::to_string(s.id) + "|" + c, now)) continue;
            ham::HamHeard h;
            h.call = c; h.mode = "CW"; h.msg = s.text.size() > 80 ? s.text.substr(s.text.size() - 80) : s.text;
            h.freqHz = a.freqMhz * 1e6 + s.offsetHz;
            h.band = ham::bandNameFor(h.freqHz);
            h.country = hamCountry(c);
            h.utc = now; h.snrDb = s.snrDb; h.cq = s.text.find("CQ") != std::string::npos;
            au::emit(ham::hamDecodeEvent(h, o.worked.mark(c, "", h.band, "CW", hamDxccKey(c))));
        }
    }
}

void hamFeedKbd(App& a, const dect2::HfdigKbdTelemetry& k, const char* mode) {
    if (!au::wants(au::FHam) || k.callsign.empty()) return;
    static ham::HamCallOnce once;
    const double now = (double)time(nullptr);
    if (!once.first(std::string(mode) + "|" + k.callsign, now)) return;
    HamOps& o = ops();
    ham::HamHeard h;
    h.call = k.callsign; h.mode = mode; h.msg = k.text.size() > 80 ? k.text.substr(k.text.size() - 80) : k.text;
    h.freqHz = a.freqMhz * 1e6;
    h.band = ham::bandNameFor(h.freqHz);
    h.country = hamCountry(k.callsign);
    h.utc = now; h.snrDb = k.snr; h.cq = h.msg.find("CQ") != std::string::npos;
    au::emit(ham::hamDecodeEvent(h, o.worked.mark(k.callsign, "", h.band, mode, hamDxccKey(k.callsign))));
}

bool hamCallInfo(const std::string& call, double freqHz, const char* mode, std::string& tag, int& level) {
    if (call.empty()) return false;
    HamOps& o = ops();
    tag = hamCountryTag(call);
    const ham::DecodeMarks m = o.worked.mark(call, "", ham::bandNameFor(freqHz), mode, hamDxccKey(call));
    level = m.newDxcc ? 3 : m.newDxccBand ? 2 : !m.workedCall ? 1 : 0;
    return true;
}

void hamCallText(const std::string& call, double freqHz, const char* mode, bool withCountry) {
    std::string tag;
    int lv = 1;
    if (!hamCallInfo(call, freqHz, mode, tag, lv)) return;
    static const ImVec4 col[4] = {ImVec4(0.62f, 0.65f, 0.68f, 1), ImVec4(0.55f, 0.80f, 1.f, 1), ImVec4(0.95f, 0.75f, 0.3f, 1), ImVec4(1.f, 0.45f, 0.35f, 1)};
    ImGui::TextColored(col[lv], "%s", call.c_str());
    if (ImGui::IsItemHovered()) {
        static const char* what[4] = {"worked before", "not worked", "new on this band", "new DXCC"};
        ImGui::SetTooltip("%s\n%s", tag.empty() ? "country not known" : tag.c_str(), what[lv]);
    }
    if (withCountry && !tag.empty()) {
        const size_t c = tag.find(',');
        ImGui::SameLine(0, 5 * gUi);
        ImGui::TextDisabled("%s", tag.substr(0, c).c_str());
    }
}
