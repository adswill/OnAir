// Round-5 smart engines for TV in the mode screens (docs/SMART5-tv-3.9.md items 11-15, and item 7 for TV; docs/SMART5-UI-3.9.md):
// service off air at the source (per-PID rates) and fault or reception (per service) from the transport stream of DVB, ATSC,
// ISDB and DTMB; the OFDM interferer locator from the per-carrier SNR (DVB-T, DVB-T2, ISDB-T); the transmitter from the cell ID
// (TPS / L1 cell_id with the NIT cell list and the user's book); the rain-fade and margin log of DVB-S/S2; the VITS analyser
// on the blanking lines of analogue TV.
#include "smart5_ui.h"
#include "dect2/dvbs/dvbs_s2.h"
#include "dect2/smart5/cell_tx.h"
#include "dect2/smart5/fault_split.h"
#include "dect2/smart5/ofdm_interf.h"
#include "dect2/smart5/pid_offair.h"
#include "dect2/smart5/sat_fade.h"
#include "dect2/smart5/vits.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

namespace s5 = dect2::smart5;

namespace {

bool tsMode(const App& a) { return !a.fmMode && !a.dabMode && (a.family == 0 || a.family == 1 || a.family == 4 || a.family == 7 || a.family == 6); }
bool ofdmMode(const App& a) { return !a.fmMode && !a.dabMode && (a.family == 0 || a.family == 4) && a.rx.standard != 2; }

// ------------------------------------------------------------------------------------------------ transport stream
struct Ts {
    s5::PidOffAir off;
    s5::FaultSplit split;
    std::map<int, std::pair<uint64_t, uint64_t>> was;   // pid -> packets, cc errors
    uint64_t total = 0, cc = 0, tei = 0;
    std::set<std::pair<unsigned, int>> mapped;
    int tsid = -1;
    double last = 0;
    std::set<unsigned> offSaid, faultSaid;
    std::map<unsigned, std::string> names;
} T;

void tsTick(App& a) {
    if (!tsMode(a) || !a.engine.running()) return;
    if (!s5ui::every(T.last, 1.0)) return;
    const TsSnapshot& ts = a.ts;
    if (ts.totalPackets == 0) return;
    if (ts.tsid != T.tsid || ts.totalPackets < T.total) {   // another multiplex, or the receiver started again
        T = Ts();
        T.tsid = ts.tsid; T.total = ts.totalPackets; T.cc = ts.ccErrors; T.tei = ts.teiPackets;
        for (const auto& p : ts.pids) T.was[p.pid] = {p.packets, p.ccErrors};
        T.last = ImGui::GetTime();
        return;
    }
    const double now = s5ui::wall();
    std::map<int, unsigned> svcOf;
    for (const auto& s : ts.services) {
        T.names[(unsigned)s.id] = s.name;
        for (const auto& st : s.streams) {
            if (st.kind != "video" && st.kind != "audio") continue;
            svcOf[st.pid] = (unsigned)s.id;
            if (T.mapped.insert({(unsigned)s.id, st.pid}).second) T.off.mapPid((unsigned)s.id, (uint16_t)st.pid, st.kind == "video");
        }
    }
    std::map<unsigned, std::pair<unsigned, unsigned>> svc;   // good, bad packets per service
    for (const auto& p : ts.pids) {
        auto& w = T.was[p.pid];
        const unsigned dp = p.packets >= w.first ? (unsigned)(p.packets - w.first) : 0, dc = p.ccErrors >= w.second ? (unsigned)(p.ccErrors - w.second) : 0;
        w = {p.packets, p.ccErrors};
        if (dp > dc) T.off.packets((uint16_t)p.pid, dp - dc);
        for (unsigned k = 0; k < std::min(dc, 1000u); k++) T.off.packet((uint16_t)p.pid, true);
        auto s = svcOf.find(p.pid);
        if (s != svcOf.end()) { svc[s->second].first += dp > dc ? dp - dc : 0; svc[s->second].second += dc; }
    }
    // the multiplex signalling: every packet against the transport errors (TEI) of the demodulator
    const unsigned dt = (unsigned)(ts.totalPackets - T.total), dtei = ts.teiPackets >= T.tei ? (unsigned)(ts.teiPackets - T.tei) : 0;
    T.total = ts.totalPackets; T.tei = ts.teiPackets; T.cc = ts.ccErrors;
    T.off.tick(now, 1.0);
    T.split.common(now, dt > dtei ? dt - dtei : 0, dtei);
    for (const auto& kv : svc) T.split.service(kv.first, kv.second.first, kv.second.second);
    T.split.tick(now);
    std::set<unsigned> offNow;
    for (const auto& e : T.off.offAir()) {
        offNow.insert(e.service);
        if (!T.offSaid.count(e.service)) s5ui::post("tv", "service_off_air", 1, std::to_string(ts.tsid) + " " + std::to_string(e.service), T.names[e.service] + ": " + T.off.text(e));
    }
    T.offSaid = offNow;
    std::set<unsigned> faultNow;
    for (const auto& f : T.split.verdict())
        if (f.kind == s5::FaultKind::ServiceFeed) {
            faultNow.insert(f.id);
            if (!T.faultSaid.count(f.id)) s5ui::post("tv", "service_fault", 1, std::to_string(ts.tsid) + " " + std::to_string(f.id), T.split.text(f.id, T.names[f.id]));
        }
    T.faultSaid = faultNow;
}

// ------------------------------------------------------------------------------------------------ OFDM interferers
struct Ofdm { std::vector<s5::Interferer> found; double last = 0; std::set<long long> said; } O;

void ofdmTick(App& a) {
    if (!ofdmMode(a) || !a.engine.running()) return;
    if (!s5ui::every(O.last, 2.0)) return;
    const RxTelemetry& rx = a.rx;
    if (rx.snrDb.size() < 16 || rx.fftN <= 0 || rx.nativeRate <= 0 || rx.snrStep <= 0) { O.found.clear(); return; }
    const double bin = rx.nativeRate / rx.fftN;
    const double c0 = a.freqMhz * 1e6 - (rx.chCarriers - 1) / 2.0 * bin;
    O.found = s5::locateInterferers(rx.snrDb, c0, bin * rx.snrStep);
    for (const auto& i : O.found) {
        const long long k = (long long)std::lround(i.centreHz / 50e3);
        if (O.said.insert(k).second) s5ui::post("tv", "ofdm_interferer", 1, std::to_string(k), s5::interfText(i));
    }
}

// ------------------------------------------------------------------------------------------------ cell ID
struct Cell { s5::CellTxBook book; bool loaded = false; int cell = -1; double last = 0; size_t nitBytes = 0; char name[64] = ""; } C;

int cellNow(const App& a) {
    if (a.family != 0) return -1;
    if (a.rx.standard == 1) return a.rx.dvbt.tpsOk && a.rx.dvbt.cellId > 0 ? a.rx.dvbt.cellId : -1;   // 0: no cell id sent (TPS length 0x17)
    if (a.rx.standard == 0) return a.rx.l1preOk ? (int)a.rx.l1pre.cellId : -1;
    return -1;
}

void cellTick(App& a) {
    if (a.family != 0 || !a.engine.running()) return;
    if (!s5ui::every(C.last, 2.0)) return;
    if (!C.loaded) { C.loaded = true; C.book.load(s5ui::readData("dvbt_cells.tsv")); }
    if (receiverLocationSet()) { double la, lo; receiverLocation(la, lo); C.book.setUser(la, lo); }
    if (a.ts.si) {
        size_t bytes = 0;
        for (const auto& n : a.ts.si->networks) for (const auto& t : n.transponders) bytes += t.cellList.size();
        if (bytes != C.nitBytes) {
            C.nitBytes = bytes;
            for (const auto& n : a.ts.si->networks)
                for (const auto& t : n.transponders) if (!t.cellList.empty()) C.book.addNit(s5::parseCellList(t.cellList.data(), t.cellList.size()));
        }
    }
    const int c = cellNow(a);
    if (c >= 0 && c != C.cell) {
        if (C.cell >= 0) s5ui::post("tv", "cell_changed", 0, std::to_string(c), C.book.text((uint16_t)c));
        C.cell = c;
    }
}

// ------------------------------------------------------------------------------------------------ DVB-S/S2 fade
struct Fade { s5::SatFadeLog log; double last = 0; size_t fadesSeen = 0, mcSeen = 0; } S;

double qefOf(const DvbsTelemetry& t) {
    if (t.standard >= 2) {
        const int r = dect2::dvbs::s2RateFromName(t.modulation, t.codeRate.c_str(), t.frameSize == 2);
        if (r < 0) return NAN;
        const double q = dect2::dvbs::s2QefEsN0(t.modulation, r, t.frameSize == 2);
        return q >= 90 ? NAN : q;
    }
    int n = 0, d = 0;
    if (t.standard == 1 && sscanf(t.codeRate.c_str(), "%d/%d", &n, &d) == 2) return s5::s1QefEsN0(n, d);
    return NAN;
}

void fadeTick(App& a) {
    if (a.family != 6 || !a.engine.running()) return;
    if (!s5ui::every(S.last, 1.0)) return;
    const DvbsTelemetry& t = a.rx.dvbs;
    const double q = qefOf(t);
    if (t.state == 0 || std::isnan(q)) return;
    S.log.sample(s5ui::wall(), t.snrDb, t.modcod, q, t.lockFec);
    const auto& f = S.log.fades();
    for (; S.fadesSeen < f.size(); S.fadesSeen++) {
        char b[160];
        snprintf(b, sizeof b, "%.0f MHz: fade of %.1f dB for %.0f s at %s UTC%s", a.freqMhz, f[S.fadesSeen].depthDb, f[S.fadesSeen].length, s5ui::utc(f[S.fadesSeen].start).c_str(), f[S.fadesSeen].lostLock ? ", lock lost" : "");
        s5ui::post("tv", "sat_fade", f[S.fadesSeen].lostLock ? 2 : 1, std::to_string((long long)f[S.fadesSeen].start), b);
    }
}

// ------------------------------------------------------------------------------------------------ VITS
struct Vits { std::map<int, s5::VitsResult> lines; double last = 0; bool said = false; } V;

void vitsTick(App& a) {
    if (a.family != 8 || !a.engine.running()) return;
    if (!s5ui::every(V.last, 2.0)) return;
    const AtvTelemetry& t = a.rx.atv;
    if (t.vitsLines.empty() || t.vitsRate <= 0) return;
    for (size_t i = 0; i < t.vitsLines.size() && i < t.vitsLineNo.size(); i++) {
        const auto& l = t.vitsLines[i];
        std::vector<float> x;
        double fs = t.vitsRate;
        if (fs < 13.5e6) {   // the analyser wants 13.5 MHz or more: linear interpolation (the content stays limited by the input rate)
            const double r = 13.5e6 / fs;
            x.resize((size_t)(l.size() * r));
            for (size_t k = 0; k < x.size(); k++) {
                const double p = k / r;
                const size_t i0 = std::min(l.size() - 2, (size_t)p);
                const double fr = p - i0;
                x[k] = (float)(l[i0] * (1 - fr) + l[i0 + 1] * fr);
            }
            fs = 13.5e6;
        } else x = l;
        const s5::VitsResult r = s5::analyseVitsLine(x, fs);
        if (r.any()) V.lines[t.vitsLineNo[i]] = r;
    }
    if (!V.lines.empty() && !V.said) {
        V.said = true;
        s5ui::post("tv", "vits_found", 0, std::to_string(V.lines.begin()->first), "Test lines found on line " + std::to_string(V.lines.begin()->first) + ": " + V.lines.begin()->second.text());
    }
}

}  // namespace

void smart5TvTick(App& a) {
    tsTick(a);
    ofdmTick(a);
    cellTick(a);
    fadeTick(a);
    vitsTick(a);
}

// the transport stream view of DVB, ATSC, ISDB, DTMB and DVB-S (analysis_tabs.cpp)
void smart5TvCard(App& a) {
    if (!tsMode(a) || !s5ui::card("s5tv")) return;
    int bad = 0;
    for (const auto& e : T.off.offAir()) { s5ui::line(1, T.names[e.service] + ": " + T.off.text(e)); bad++; }
    for (const auto& f : T.split.verdict()) if (f.kind == s5::FaultKind::ServiceFeed) { s5ui::line(1, T.split.text(f.id, T.names[f.id])); bad++; }
    if (!bad) s5ui::line(0, T.split.overall() == s5::FaultKind::Reception ? tr("All services fail together: reception, not the stations") : tr("Services: every one on air, no fault at a station's feed"));
    const int c = cellNow(a);
    if (c >= 0) s5ui::line(0, C.book.text((uint16_t)c));
    if (ofdmMode(a)) {
        if (O.found.empty()) ImGui::TextDisabled("%s", tr("Carriers: no interferer found in the channel"));
        else for (const auto& i : O.found) s5ui::line(1, s5::interfText(i));
    }
    if (s5ui::details("s5tvd")) {
        for (const auto& e : T.off.log()) ImGui::TextDisabled("%s  %s", s5ui::utc(e.since, true).c_str(), (T.names[e.service] + ": " + T.off.text(e)).c_str());
        if (c >= 0) {
            ImGui::SetNextItemWidth(180 * gUi);
            ImGui::InputTextWithHint("##s5cell", tr("Transmitter site"), C.name, sizeof C.name);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Name this cell")) && C.name[0]) { C.book.name((uint16_t)c, C.name); s5ui::writeData("dvbt_cells.tsv", C.book.save()); C.name[0] = 0; }
        }
        ImGui::TextDisabled("%s", tr("Off air at the source: a video or audio PID stops while the multiplex is fine. Fault split: one service failing while the others are clean."));
        ImGui::TreePop();
    }
}

void smart5DvbsCard(App& a) {
    if (a.family != 6 || !s5ui::card("s5dvbs")) return;
    if (S.log.clearSkyDb() == 0 && S.log.fades().empty()) { ImGui::TextDisabled("%s", tr("Rain fade log: waiting for a lock with a known MODCOD")); return; }
    s5ui::line(S.log.fading() ? 1 : S.log.marginDb() < 1 ? 1 : 0, S.log.text());
    if (s5ui::details("s5dvbsd")) {
        for (auto it = S.log.fades().rbegin(); it != S.log.fades().rend(); ++it)
            ImGui::TextDisabled(tr("%s  fade %.1f dB, %.0f s, lowest margin %.1f dB%s"), s5ui::utc(it->start, true).c_str(), it->depthDb, it->length, it->minMarginDb, it->lostLock ? tr(", lock lost") : "");
        for (const auto& m : S.log.modcodChanges()) ImGui::TextDisabled(tr("%s  MODCOD %d -> %d"), s5ui::utc(m.t, true).c_str(), m.from, m.to);
        ImGui::TreePop();
    }
    (void)a;
}

void smart5AtvCard(App& a) {
    if (a.family != 8 || !s5ui::card("s5atv")) return;
    if (V.lines.empty()) { ImGui::TextDisabled("%s", tr("Test lines (VITS): none found in the blanking interval yet")); return; }
    for (const auto& kv : V.lines) s5ui::line(0, std::string(tr("Line")) + " " + std::to_string(kv.first) + ": " + kv.second.text());
    if (s5ui::details("s5atvd")) {
        for (const auto& kv : V.lines) {
            const auto& r = kv.second;
            if (r.multiburst) {
                std::string s;
                for (const auto& b : r.bursts) { char x[48]; snprintf(x, sizeof x, "%s%.1f MHz %+.1f dB", s.empty() ? "" : ", ", b.freqHz / 1e6, b.relDb); s += x; }
                ImGui::TextDisabled(tr("Line %d multiburst: %s"), kv.first, s.c_str());
            }
        }
        ImGui::TextDisabled("%s", tr("Line numbers are counted from the field sync and may be one off."));
        ImGui::TreePop();
    }
}

void smart5InsightTv(App& a) {
    ImGui::SeparatorText(tr("TV"));
    if (T.tsid < 0) ImGui::TextDisabled("%s", tr("Receive a TV multiplex: off-air services, fault split and interferers are checked by themselves."));
    else {
        ImGui::Text(tr("Multiplex %d: %zu services off air, %s"), T.tsid, T.off.offAir().size(), s5::FaultSplit::name(T.split.overall()));
        const int c = cellNow(a);
        if (c >= 0) ImGui::TextUnformatted(C.book.text((uint16_t)c).c_str());
    }
    for (const auto& i : O.found) ImGui::BulletText("%s", s5::interfText(i).c_str());
    if (S.log.clearSkyDb() != 0) ImGui::TextUnformatted(S.log.text().c_str());
    for (const auto& kv : V.lines) ImGui::BulletText("%s %d: %s", tr("Line"), kv.first, kv.second.text().c_str());
}
