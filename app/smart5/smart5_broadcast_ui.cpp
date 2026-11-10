// Round-5 smart engines for broadcast radio in the mode screens (docs/SMART5-broadcast-3.9.md items 1-10, docs/SMART5-UI-3.9.md):
// FM station watchdog, BS.412 multiplex power (measured in the FM receiver), transmitter fingerprint; AM split carriers and
// modulation health (one click: a 24 s look at the listened channel); DAB ensemble change log and fault-or-reception; HD Radio
// sideband level; DRM best hours and fault-or-reception. The HD Radio blend alignment needs the analogue host's audio, which the
// HD Radio receiver does not demodulate, so it is not shown (docs/SMART5-UI-3.9.md).
#include "smart5_ui.h"
#include "dect2/smart5/am_carriers.h"
#include "dect2/smart5/am_health.h"
#include "dect2/smart5/drm_besthour.h"
#include "dect2/smart5/ens_changes.h"
#include "dect2/smart5/fault_split.h"
#include "dect2/smart5/fm_txprint.h"
#include "dect2/smart5/fm_watchdog.h"
#include "dect2/smart5/hd_sideband.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>
#include <cstdio>
#include <mutex>

namespace s5 = dect2::smart5;

namespace {

// ---------------------------------------------------------------------------------------------------------------- FM
struct Fm {
    s5::FmWatchdog dog;
    s5::FmTxPrint print;
    bool printLoaded = false;
    uint64_t seq = 0;
    size_t evSeen = 0;
    int lastPi = 0, txNow = 0;
    bool txChanged = false, overLimit = false;
    double lastSave = 0;
} F;

void fmTick(App& a) {
    if (!a.fmMode || !a.engine.running()) return;
    const FmTelemetry& t = a.rx.fm;
    if (t.seq == F.seq) return;
    F.seq = t.seq;
    if (!F.printLoaded) { F.printLoaded = true; F.print.load(s5ui::readData("fm_txprint.txt")); }
    const double now = s5ui::wall();
    F.dog.update(s5::fmWatchInput(t), now);
    const auto& ev = F.dog.events();
    for (; F.evSeen < ev.size(); F.evSeen++) {
        const auto& e = ev[F.evSeen];
        if (e.cleared) continue;
        char key[48];
        snprintf(key, sizeof key, "%04X %.1f", (unsigned)e.pi, a.freqMhz);
        std::string kind = "fm_";   // "dead air" -> fm_dead_air, "over-deviation" -> fm_over_deviation
        for (char c : e.kind) kind += std::isalnum((unsigned char)c) ? (char)std::tolower((unsigned char)c) : '_';
        s5ui::post("broadcast", kind.c_str(), 1, key, e.text);
    }
    if (t.piCode && t.carrier && t.snrDb > 25) {
        s5::FmPrintSample s;
        s.cfoHz = t.cfoHz; s.pilotPct = t.pilotPct; s.mpxDbr = t.mpx412Valid ? t.mpxPowerDbr : -99;
        const int before = F.print.current(t.piCode, a.freqMhz);
        F.txNow = F.print.observe(t.piCode, a.freqMhz, s);
        const bool ch = F.print.changed(t.piCode, a.freqMhz);
        if (ch && !F.txChanged && before > 0) {
            char key[48], b[160];
            snprintf(key, sizeof key, "%04X %.1f", (unsigned)t.piCode, a.freqMhz);
            snprintf(b, sizeof b, "%.1f MHz %s (PI %04X): a different transmitter of this network now (signature %d, was %d)", a.freqMhz, t.psName.c_str(), (unsigned)t.piCode, F.txNow, before);
            s5ui::post("broadcast", "fm_other_tx", 0, key, b);
        }
        F.txChanged = ch;
        if (now - F.lastSave > 300) { F.lastSave = now; s5ui::writeData("fm_txprint.txt", F.print.save()); }
    }
    if (t.mpx412Valid) {
        char key[48], b[140];
        snprintf(key, sizeof key, "%04X %.1f", (unsigned)t.piCode, a.freqMhz);
        snprintf(b, sizeof b, "%.1f MHz %s: multiplex power %+.1f dBr over 60 s (ITU-R BS.412 limit +0 dBr)", a.freqMhz, t.psName.c_str(), t.mpxPowerDbr);
        s5ui::edge(F.overLimit, t.mpxPowerDbr > 0.5f, "broadcast", "fm_mpx_power", 1, key, b);
    }
}

// ---------------------------------------------------------------------------------------------------------------- AM
// One click: 24 s of the listened channel from the radio's samples, mixed to the carrier and decimated to about 12 kHz
struct Am {
    std::mutex mu;
    bool capturing = false, have = false;
    double offHz = 0, inRate = 0, outRate = 0, startT = 0;
    int D = 1, k = 0;
    std::complex<double> acc{0, 0}, ph{1, 0}, rot{1, 0};
    std::vector<std::complex<float>> buf;
    size_t want = 0;
    double khz = 0;
    std::vector<s5::AmCarrier> carriers;
    s5::AmHealthReport health;
    std::string note;
    char name[64] = "";
    int nameIdx = -1;
} A;

std::vector<s5::AmBookEntry> amBook(double khz) {
    std::vector<s5::AmBookEntry> v;
    const std::string s = s5ui::readData("am_carrier_book.tsv");   // kHz <TAB> offset Hz <TAB> name
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find('\n', p);
        if (e == std::string::npos) e = s.size();
        const std::string l = s.substr(p, e - p);
        p = e + 1;
        double k = 0, off = 0; char nm[128] = "";
        if (sscanf(l.c_str(), "%lf\t%lf\t%127[^\n]", &k, &off, nm) == 3 && std::fabs(k - khz) < 0.05) { s5::AmBookEntry b; b.offsetHz = off; b.name = nm; v.push_back(b); }
    }
    return v;
}

void amStart(App& a) {
    if (!a.engine.running()) { A.note = tr("Start the receiver first."); return; }
    const double listen = generalListenHz(a);
    std::lock_guard<std::mutex> l(A.mu);
    A.offHz = listen - radioCenterMhz(a) * 1e6;
    A.khz = listen / 1e3;
    A.buf.clear(); A.have = false; A.capturing = true; A.inRate = 0; A.note.clear();
    A.startT = ImGui::GetTime();
    a.engine.setIqTap([](const std::complex<float>* x, size_t n, double rate) {
        std::lock_guard<std::mutex> l(A.mu);
        if (!A.capturing) return;
        if (A.inRate != rate) {
            A.inRate = rate;
            A.D = std::max(1, (int)std::lround(rate / 12000.0));
            A.outRate = rate / A.D;
            A.want = (size_t)(A.outRate * 24);
            A.buf.reserve(A.want);
            const double w = -2 * 3.14159265358979323846 * A.offHz / rate;
            A.rot = {std::cos(w), std::sin(w)}; A.ph = {1, 0}; A.acc = {0, 0}; A.k = 0;
        }
        for (size_t i = 0; i < n && A.buf.size() < A.want; i++) {
            A.acc += std::complex<double>(x[i].real(), x[i].imag()) * A.ph;
            A.ph *= A.rot;
            if (++A.k == A.D) {
                A.buf.push_back(std::complex<float>((float)(A.acc.real() / A.D), (float)(A.acc.imag() / A.D)));
                A.acc = {0, 0}; A.k = 0;
                A.ph /= std::abs(A.ph);
            }
        }
    });
}

void amTick(App& a) {
    bool done = false;
    {
        std::lock_guard<std::mutex> l(A.mu);
        if (!A.capturing) return;
        done = A.want > 0 && A.buf.size() >= A.want;
        if (!done && ImGui::GetTime() - A.startT > 60) { A.capturing = false; A.note = tr("No samples arrived."); }
        if (done) A.capturing = false;
    }
    if (!A.capturing) a.engine.setIqTap(nullptr);
    if (!done) return;
    std::vector<std::complex<float>> x;
    double rate;
    { std::lock_guard<std::mutex> l(A.mu); x.swap(A.buf); rate = A.outRate; }
    s5::AmCarrierAnalyser an;
    s5::AmCarrierParams p;
    p.averages = 4;
    an.configure(rate, p, 0);
    an.setBook(amBook(A.khz));
    an.push(x.data(), x.size(), (double)x.size() / rate);
    A.carriers = an.carriers();
    s5::AmHealth h;
    h.configure(rate);
    std::vector<float> env(x.size());
    for (size_t i = 0; i < x.size(); i++) env[i] = std::abs(x[i]);
    h.push(env.data(), env.size());
    A.health = h.report();
    A.have = true;
    char key[32];
    snprintf(key, sizeof key, "%.1f kHz", A.khz);
    if (A.health.overModulated || A.health.carrierShift || A.health.deadAir)
        s5ui::post("broadcast", "am_health", 1, key, std::string(key) + ": " + A.health.summary());
    if (A.carriers.size() > 1) {
        char b[160];
        snprintf(b, sizeof b, "%.1f kHz: %zu carriers on the channel, het %.2f Hz", A.khz, A.carriers.size(), A.carriers[1].hetHz);
        s5ui::post("broadcast", "am_carriers", 0, key, b);
    }
}

// ---------------------------------------------------------------------------------------------------------------- DAB
struct Dab {
    s5::EnsChangeLog log;
    s5::FaultSplit split;
    std::string ens;
    size_t evSeen = 0;
    uint64_t fibOk = 0, fibBad = 0, auOk = 0, auBad = 0;
    int sub = -2;
    double last = 0;
    bool faultSaid = false;
} D;

std::vector<s5::EnsService> dabList(const DabEnsemble& e) {
    std::vector<s5::EnsService> v;
    for (const auto& kv : e.services) {
        s5::EnsService s;
        s.id = kv.second.sid; s.label = kv.second.label;
        for (const auto& c : kv.second.comps) {
            if (!c.primary) continue;
            s.subch = c.subId;
            s.kind = c.tmid != 0 ? "data" : c.ascty == 63 ? "DAB+" : "DAB";
            auto sc = e.subs.find(c.subId);
            if (sc != e.subs.end()) {
                s.bitrateKbps = sc->second.bitrate;
                char b[24];
                if (sc->second.eep) snprintf(b, sizeof b, "EEP %d-%c", sc->second.level + 1, sc->second.option ? 'B' : 'A');
                else snprintf(b, sizeof b, "UEP %d", sc->second.level + 1);
                s.protection = b;
            }
            break;
        }
        v.push_back(s);
    }
    return v;
}

void dabTick(App& a) {
    if (!a.dabMode || !a.engine.running()) return;
    if (!s5ui::every(D.last, 1.0)) return;
    const double now = s5ui::wall();
    const DabEnsemble e = a.engine.dabEnsemble();
    if (e.services.empty()) return;
    if (e.label != D.ens) { D.ens = e.label; D.log.reset(); D.evSeen = 0; D.split = s5::FaultSplit(); D.faultSaid = false; }
    D.log.update(dabList(e), now);
    const auto& ev = D.log.events();
    for (; D.evSeen < ev.size(); D.evSeen++) s5ui::post("broadcast", "dab_change", 0, D.ens + " " + std::to_string(ev[D.evSeen].id) + " " + ev[D.evSeen].what, D.ens + ": " + ev[D.evSeen].text);
    // fault or reception: the FIC is the multiplex signalling, the selected sub-channel's access units the service
    const DabTelemetry& t = a.rx.dab;
    auto d = [](uint64_t now, uint64_t& was) { const unsigned v = now >= was ? (unsigned)(now - was) : 0; was = now; return v; };
    const unsigned fo = d(t.fibOk, D.fibOk), fb = d(t.fibBad, D.fibBad);
    if (t.audio.sub != D.sub) { D.sub = t.audio.sub; D.auOk = t.audio.auOk; D.auBad = t.audio.auBad; }
    const unsigned ao = d(t.audio.auOk, D.auOk), ab = d(t.audio.auBad, D.auBad);
    D.split.common(now, fo, fb);
    unsigned sid = 0; std::string label;
    for (const auto& kv : e.services) for (const auto& c : kv.second.comps) if (c.subId == t.audio.sub) { sid = kv.second.sid; label = kv.second.label; }
    if (sid) D.split.service(sid, ao, ab);
    D.split.tick(now);
    if (sid) {
        bool feed = false;
        for (const auto& f : D.split.verdict()) if (f.id == sid && (f.kind == s5::FaultKind::ServiceFeed || f.kind == s5::FaultKind::NoData)) feed = true;
        s5ui::edge(D.faultSaid, feed, "broadcast", "service_fault", 1, D.ens + " " + label, D.split.text(sid, label));
    }
}

// ---------------------------------------------------------------------------------------------------------------- DRM
struct Drm {
    s5::DrmBestHour book;
    s5::FaultSplit split;
    bool loaded = false, faultSaid = false;
    double last = 0, lastSave = 0;
    uint64_t facOk = 0, facBad = 0, auOk = 0, auBad = 0;
    std::string station;
} R;

std::string drmStation(const DrmTelemetry& t, unsigned* id) {
    for (const auto& s : t.services) if (s.audio && !s.label.empty()) { if (id) *id = s.id; return s.label; }
    if (!t.services.empty()) { if (id) *id = t.services[0].id; char b[16]; snprintf(b, sizeof b, "%06X", (unsigned)t.services[0].id); return b; }
    return {};
}

void drmTick(App& a) {
    if (a.family != 10 || !a.engine.running()) return;
    if (!R.loaded) { R.loaded = true; R.book.load(s5ui::readData("drm_besthour.txt")); }
    const double now = s5ui::wall();
    const double dt = R.last > 0 ? ImGui::GetTime() - R.last : 0;
    if (!s5ui::every(R.last, 5.0)) return;
    const DrmTelemetry& t = a.rx.drm;
    unsigned id = 0;
    const std::string st = drmStation(t, &id);
    auto d = [](uint64_t now, uint64_t& was) { const unsigned v = now >= was ? (unsigned)(now - was) : 0; was = now; return v; };
    const unsigned fo = d(t.facOk, R.facOk), fb = d(t.facBad, R.facBad), ao = d(t.blocksOk, R.auOk), ab = d(t.blocksBad, R.auBad);
    if (st.empty()) return;
    if (st != R.station) { R.station = st; R.split = s5::FaultSplit(); R.faultSaid = false; return; }
    if (dt > 0 && dt < 30) R.book.add(st, (int)std::lround(a.freqMhz * 1e3), now, dt, t.audioState == 2 && ab * 10 <= ao + ab);
    R.split.common(now, fo, fb);
    R.split.service(id, ao, ab);
    R.split.tick(now);
    bool feed = false;
    for (const auto& f : R.split.verdict()) if (f.id == id && (f.kind == s5::FaultKind::ServiceFeed || f.kind == s5::FaultKind::NoData)) feed = true;
    s5ui::edge(R.faultSaid, feed, "broadcast", "service_fault", 1, "DRM " + st, R.split.text(id, st));
    if (now - R.lastSave > 300) { R.lastSave = now; s5ui::writeData("drm_besthour.txt", R.book.save()); }
}

// ---------------------------------------------------------------------------------------------------------------- HD Radio
struct Hd { s5::HdSidebandReport rep; double last = 0; bool asymSaid = false; } H;

void hdTick(App& a) {
    if (a.family != 21 || !a.engine.running() || a.spec.dbfs.empty()) return;
    if (!s5ui::every(H.last, 1.0)) return;
    const double fs = a.engine.sampleRate();
    const int n = (int)a.spec.dbfs.size();
    if (fs <= 0 || n < 64) return;
    const double bin = fs / n, off = a.freqMhz * 1e6 - a.engine.captureCentreHz();
    if (std::fabs(off) + 260e3 > fs * 0.48) return;   // the sidebands are not all in the capture
    const int k0 = (int)std::floor(n / 2 + (off - 260e3) / bin), k1 = (int)std::ceil(n / 2 + (off + 260e3) / bin);
    if (k0 < 0 || k1 >= n) return;
    std::vector<float> v(a.spec.dbfs.begin() + k0, a.spec.dbfs.begin() + k1 + 1);
    H.rep = s5::hdSidebandLevelsDb(v, (k0 - n / 2) * bin - off, bin);
    if (H.rep.valid) {
        char key[32];
        snprintf(key, sizeof key, "%.1f MHz", a.freqMhz);
        s5ui::edge(H.asymSaid, H.rep.asymmetric, "broadcast", "hd_sideband", 1, key, std::string(key) + ": " + H.rep.summary());
    }
}

}  // namespace

void smart5BroadcastTick(App& a) {
    fmTick(a);
    amTick(a);
    dabTick(a);
    drmTick(a);
    hdTick(a);
}

// ---------------------------------------------------------------------------------------------------------------- cards
void smart5FmCard(App& a) {
    if (!s5ui::card("s5fm")) return;
    const FmTelemetry& t = a.rx.fm;
    const auto& st = F.dog.status();
    const std::string sum = st.summary();
    s5ui::line(sum.empty() ? 0 : 1, sum.empty() ? tr("Station watchdog: nothing wrong") : sum);
    if (t.mpx412Valid || t.mpxRunningDbr > -98) {
        char b[200];
        if (t.mpx412Valid) snprintf(b, sizeof b, tr("Multiplex power %+.1f dBr (BS.412, 60 s), peaks %.0f / %.0f kHz (99 / 99.9 %%), above 75 kHz %.2f %%"), t.mpxPowerDbr, t.devP99Khz, t.devP999Khz, t.over75Pct);
        else snprintf(b, sizeof b, tr("Multiplex power %+.1f dBr so far (BS.412 needs 60 s)"), t.mpxRunningDbr);
        s5ui::line(t.mpx412Valid && t.mpxPowerDbr > 0.5f ? 1 : 0, b);
    }
    if (t.piCode && F.txNow > 0) {
        const auto* sig = F.print.signatures(t.piCode, a.freqMhz);
        std::string nm;
        if (sig) for (const auto& g : *sig) if (g.id == F.txNow) nm = g.label;
        char b[160];
        snprintf(b, sizeof b, tr("Transmitter %d of PI %04X%s%s%s"), F.txNow, (unsigned)t.piCode, nm.empty() ? "" : " (", nm.c_str(), nm.empty() ? "" : ")");
        s5ui::line(F.txChanged ? 1 : 0, std::string(b) + (F.txChanged ? tr(": a different transmitter of this network") : ""));
    }
    if (s5ui::details("s5fmd")) {
        ImGui::TextDisabled(tr("Peak deviation 50 / 99 / 99.9 %%: %.1f / %.1f / %.1f kHz"), t.devP50Khz, t.devP99Khz, t.devP999Khz);
        if (st.clockSamples > 0) ImGui::TextDisabled(tr("RDS clock %+.0f s against this computer, drift %+.1f s a day (%d clock times)"), st.clockErrS, st.clockDriftSPerDay, st.clockSamples);
        const auto* sig = t.piCode ? F.print.signatures(t.piCode, a.freqMhz) : nullptr;
        if (sig && !sig->empty() && ImGui::BeginTable("##s5fmsig", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn(tr("Carrier offset")); ImGui::TableSetupColumn(tr("Pilot")); ImGui::TableSetupColumn(tr("MPX power")); ImGui::TableSetupColumn(tr("Name"));
            ImGui::TableHeadersRow();
            for (const auto& g : *sig) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%d%s", g.id, g.id == F.txNow ? " *" : "");
                ImGui::TableNextColumn(); ImGui::Text("%+.0f Hz", g.cfoHz);
                ImGui::TableNextColumn(); ImGui::Text("%.1f %%", g.pilotPct);
                ImGui::TableNextColumn(); if (g.mpxDbr > -98) ImGui::Text("%+.1f dBr", g.mpxDbr); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn(); ImGui::TextUnformatted(g.label.c_str());
            }
            ImGui::EndTable();
            static char nm[64] = "";
            ImGui::SetNextItemWidth(180 * gUi);
            ImGui::InputTextWithHint("##s5fmname", tr("Site name"), nm, sizeof nm);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Name the transmitter on air")) && nm[0] && F.txNow > 0) {
                F.print.setLabel(t.piCode, a.freqMhz, F.txNow, nm);
                s5ui::writeData("fm_txprint.txt", F.print.save());
                nm[0] = 0;
            }
        }
        ImGui::TextDisabled("%s", tr("The carrier offset includes your radio's own error: compare signatures from the same radio."));
        ImGui::TreePop();
    }
}

void smart5AmCard(App& a) {
    if (!s5ui::card("s5am")) return;
    bool cap;
    { std::lock_guard<std::mutex> l(A.mu); cap = A.capturing; }
    if (cap) {
        size_t got, want;
        { std::lock_guard<std::mutex> l(A.mu); got = A.buf.size(); want = std::max<size_t>(A.want, 1); }
        ImGui::ProgressBar((float)got / (float)want, ImVec2(220 * gUi, 0), tr("Listening to the carriers..."));
    } else if (ImGui::Button(tr("Check carriers and modulation"))) amStart(a);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("24 seconds on the listened channel: every carrier on it to about 0.1 Hz (split channels, hets), over-modulation, carrier shift and dead air."));
    if (!A.note.empty()) ImGui::TextDisabled("%s", A.note.c_str());
    if (!A.have) return;
    const std::string hs = A.health.summary();
    s5ui::line(A.health.overModulated || A.health.carrierShift || A.health.deadAir ? 1 : 0, hs.empty() ? tr("Modulation: healthy") : hs);
    if (A.carriers.empty()) ImGui::TextDisabled("%s", tr("No carrier found on the channel."));
    else {
        char b[160];
        snprintf(b, sizeof b, tr("%.1f kHz: %zu carrier(s); the strongest %+.2f Hz off the channel"), A.khz, A.carriers.size(), A.carriers[0].offsetHz);
        s5ui::line(0, b);
    }
    if (s5ui::details("s5amd")) {
        ImGui::TextDisabled(tr("Modulation peaks +%.0f / -%.0f %%, clipped %.2f %%, carrier shift %.1f %%"), A.health.posPeakPct, A.health.negPeakPct, A.health.clipPct, A.health.carrierShiftPct);
        if (!A.carriers.empty() && ImGui::BeginTable("##s5amc", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn(tr("Offset")); ImGui::TableSetupColumn(tr("Level")); ImGui::TableSetupColumn("SNR"); ImGui::TableSetupColumn(tr("Het")); ImGui::TableSetupColumn(tr("Name"));
            ImGui::TableHeadersRow();
            for (int i = 0; i < (int)A.carriers.size(); i++) {
                const auto& c = A.carriers[(size_t)i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); ImGui::Text("%+.2f Hz", c.offsetHz);
                ImGui::TableNextColumn(); ImGui::Text("%+.1f dB", c.levelDb);
                ImGui::TableNextColumn(); ImGui::Text("%.0f dB", c.snrDb);
                ImGui::TableNextColumn(); if (i) ImGui::Text("%.2f Hz", c.hetHz); else ImGui::TextDisabled("-");
                ImGui::TableNextColumn();
                if (!c.name.empty()) ImGui::TextUnformatted(c.name.c_str());
                else { ImGui::PushID(i); if (ImGui::SmallButton(tr("Name"))) { A.nameIdx = i; A.name[0] = 0; } ImGui::PopID(); }
            }
            ImGui::EndTable();
        }
        if (A.nameIdx >= 0 && A.nameIdx < (int)A.carriers.size()) {
            ImGui::SetNextItemWidth(180 * gUi);
            ImGui::InputTextWithHint("##s5amname", tr("Station"), A.name, sizeof A.name);
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Add to the carrier book")) && A.name[0]) {
                char l[200];
                snprintf(l, sizeof l, "%.1f\t%.2f\t%s\n", A.khz, A.carriers[(size_t)A.nameIdx].offsetHz, A.name);
                s5ui::writeData("am_carrier_book.tsv", s5ui::readData("am_carrier_book.tsv") + l);
                A.carriers[(size_t)A.nameIdx].name = A.name;
                A.nameIdx = -1;
            }
        }
        ImGui::TextDisabled("%s", tr("Offsets include your radio's frequency error; a GNSS-locked or calibrated radio gives the real ones."));
        ImGui::TreePop();
    }
}

void smart5DabCard(App& a) {
    if (!s5ui::card("s5dab")) return;
    const auto& ev = D.log.events();
    if (ev.empty()) ImGui::TextDisabled(tr("Ensemble: %zu services, no change seen yet"), D.log.services());
    else s5ui::line(0, s5ui::utc(ev.back().t) + " UTC  " + ev.back().text);
    for (const auto& f : D.split.verdict())
        if (f.kind != s5::FaultKind::Ok) {
            std::string label;
            const DabEnsemble e = a.engine.dabEnsemble();
            auto it = e.services.find(f.id);
            if (it != e.services.end()) label = it->second.label;
            s5ui::line(1, D.split.text(f.id, label));
        }
    if (s5ui::details("s5dabd")) {
        for (auto it = ev.rbegin(); it != ev.rend() && it - ev.rbegin() < 40; ++it) ImGui::TextDisabled("%s  %s", s5ui::utc(it->t, true).c_str(), it->text.c_str());
        if (!ev.empty() && ImGui::SmallButton(tr("Copy the change log (CSV)"))) ImGui::SetClipboardText(D.log.csv().c_str());
        ImGui::TreePop();
    }
}

void smart5DrmCard(App& a) {
    if (!s5ui::card("s5drm")) return;
    if (R.station.empty()) { ImGui::TextDisabled("%s", tr("Best hours: waiting for a station name.")); return; }
    const int khz = (int)std::lround(a.freqMhz * 1e3);
    const auto best = R.book.bestHours(R.station, khz);
    const double nx = R.book.nextGood(R.station, khz, s5ui::wall());
    if (best.empty()) ImGui::TextDisabled(tr("Best hours for %s on %d kHz: still learning (5 minutes of listening per hour)"), R.station.c_str(), khz);
    else {
        char b[200];
        snprintf(b, sizeof b, tr("%s on %d kHz decodes best at %02d:00 UTC"), R.station.c_str(), khz, best[0]);
        std::string s = b;
        if (nx > 0) s += std::string(", ") + tr("next good hour") + " " + s5ui::utc(nx);
        s5ui::line(0, s);
    }
    for (const auto& f : R.split.verdict()) if (f.kind != s5::FaultKind::Ok) s5ui::line(1, R.split.text(f.id, R.station));
    if (s5ui::details("s5drmd")) {
        const auto* h = R.book.hours(R.station, khz);
        if (h) for (int i = 0; i < 24; i++) if ((*h)[(size_t)i].listenS > 0) ImGui::TextDisabled("%02d UTC  %3.0f %%  (%.0f min)", i, 100 * (*h)[(size_t)i].rate(), (*h)[(size_t)i].listenS / 60);
        ImGui::TreePop();
    }
}

void smart5HdCard(App& a) {
    (void)a;
    if (!s5ui::card("s5hd")) return;
    if (!H.rep.valid) { ImGui::TextDisabled("%s", tr("Sideband level: needs the whole HD Radio signal (+/- 200 kHz) in the capture.")); return; }
    s5ui::line(H.rep.asymmetric ? 1 : 0, H.rep.summary());
    if (s5ui::details("s5hdd")) {
        ImGui::TextDisabled(tr("Lower %.1f dBc, upper %.1f dBc, total %.1f dBc; nearest step %d dBc"), H.rep.lowerDbc, H.rep.upperDbc, H.rep.totalDbc, H.rep.stepDbc);
        ImGui::TextDisabled("%s", tr("Blend alignment is not measured: the HD Radio receiver does not demodulate the analogue FM host."));
        ImGui::TreePop();
    }
}

void smart5InsightBroadcast(App& a) {
    (void)a;
    ImGui::SeparatorText(tr("FM"));
    const std::string s = F.dog.status().summary();
    ImGui::TextUnformatted(F.seq ? (s.empty() ? tr("Station watchdog: nothing wrong") : s.c_str()) : tr("Tune an FM station: watchdog, BS.412 power and transmitter fingerprint run by themselves."));
    ImGui::SeparatorText(tr("AM"));
    if (A.have) ImGui::Text(tr("%.1f kHz: %zu carrier(s); %s"), A.khz, A.carriers.size(), A.health.summary().empty() ? tr("modulation healthy") : A.health.summary().c_str());
    else ImGui::TextDisabled("%s", tr("Check carriers and modulation from an AM station's screen."));
    ImGui::SeparatorText(tr("DAB"));
    if (!D.ens.empty()) ImGui::Text(tr("%s: %zu services, %zu changes logged"), D.ens.c_str(), D.log.services(), D.log.events().size());
    else ImGui::TextDisabled("-");
    ImGui::SeparatorText(tr("DRM"));
    ImGui::Text(tr("%zu station and frequency entries in the best-hour book"), R.book.entries());
    ImGui::SeparatorText(tr("HD Radio"));
    if (H.rep.valid) ImGui::TextUnformatted(H.rep.summary().c_str()); else ImGui::TextDisabled("-");
}
