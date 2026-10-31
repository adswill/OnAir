// View > Antenna rotator / mount: GS-232, Hamlib rotctld or ASCOM Alpaca mount, calibration offsets, limits, park, satellite pass tracking.
#include "../app.h"
#include "../ui/typography.h"
#include "dect2/geo/passes.h"
#include "dect2/source/rotator.h"
#include "dect2/source/mount_driver.h"
#include "dect2/source/mount_track.h"
#include "dect2/specan/rot_scan.h"
#include "imgui.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

struct State {
    bool loaded = false;
    int linkUi = 0;   // 0 off, 1 serial, 2 tcp
    char serialDev[128] = "/dev/tty.usbserial";
    int baud = 9600;
    char tcpHost[128] = "127.0.0.1";
    int tcpPort = 4533;
    int protocol = 0;          // TCP link: 0 GS-232, 1 Hamlib rotctld, 2 ASCOM Alpaca
    int alpacaDev = 0;
    bool passDrive = false;    // follow the tracked satellite pass (passes_ui)
    bool passPark = true;      // park after LOS (otherwise just stop)
    bool prepDone = false, tracking = false;
    double lastTrackT = 0, lastAz = -999, lastEl = -999;
    std::vector<std::string> found;   // Alpaca servers answering discovery
    std::string discoverMsg;
    float cmdAz = 0.f, cmdEl = 0.f;
    bool autoConnect = false;
    bool showMechanical = false;
    float azOff = 0.f, elOff = 0.f;
    float azMin = 0.f, azMax = 360.f;
    float elMin = 0.f, elMax = 90.f;
    float parkAz = 0.f, parkEl = 0.f;
    bool shortPath = true;
    bool limitAz = false, limitEl = false;
    float calTrueAz = 0.f, calTrueEl = 0.f;
    int geoPick = 0;
    std::string calMsg;
    struct {
        bool on = false;
        double trueAz = 0, trueEl = 0, mechEl = 0;
        double az = 0, azEnd = 0, step = 2;
        int i = 0, n = 0;
        double settleUntil = 0;
        std::vector<double> levels;   // signal level at each scan step (dB), for the parabolic peak
    } peak;
};
State S;

// Test button: the probe talks to the mount on its own thread so the window never waits for a dead host.
struct ProbeJob {
    std::mutex mu;
    bool running = false, done = false, ok = false;
    std::string msg;
};
std::shared_ptr<ProbeJob> gProbe = std::make_shared<ProbeJob>();

static void saveUiPrefs();

static double normAz360(double a) {
    a = std::fmod(a, 360.0);
    if (a < 0) a += 360.0;
    return a;
}

// Ku (narrow) and L-band (wider): DVB-S/S2 MER when locked; otherwise band power in the spectrum centre.
static float signalScoreForPeakCal(const App& a) {
    if (!a.engine.running()) return -120.f;
    if (a.rx.standard == 7) {
        const dect2::DvbsTelemetry& d = a.rx.dvbs;
        if (d.lockFec || d.tsLock) return std::max(d.merDb, d.snrDb);
        if (d.lockSpectrum || d.lockCarrier || d.lockTiming) return d.snrDb;
        if (d.snrDb > 0.f) return d.snrDb - 8.f;
    }
    if (a.smooth.size() >= 32) {
        const size_t n = a.smooth.size();
        const size_t i0 = n * 9 / 20, i1 = n * 11 / 20;
        float mx = -120.f;
        for (size_t j = i0; j <= i1 && j < n; j++) mx = std::max(mx, a.smooth[j]);
        return mx;
    }
    return -120.f;
}

static void startPeakAzCal(double trueAz, double trueEl, double mechAz, double mechEl) {
    constexpr double halfSpan = 10.0;   // +-10 degrees in 1 degree steps around the current reading
    S.peak.on = true;
    S.peak.trueAz = trueAz;
    S.peak.trueEl = trueEl;
    S.peak.mechEl = mechEl;
    S.peak.az = normAz360(mechAz - halfSpan);
    S.peak.azEnd = normAz360(mechAz + halfSpan);
    S.peak.step = 1.0;
    S.peak.i = 0;
    S.peak.n = (int)std::ceil((halfSpan * 2.0) / S.peak.step) + 1;
    S.peak.settleUntil = 0;
    S.peak.levels.clear();
}

static void stepPeakAzCal(App& a, dect2::Rotator& rot) {
    if (!S.peak.on) return;
    const double now = ImGui::GetTime();
    if (now < S.peak.settleUntil) return;
    const dect2::RotatorStatus st = rot.status();
    if (S.peak.settleUntil > 0 && st.moving && now < S.peak.settleUntil + 2.5) return;

    if (S.peak.settleUntil > 0) {   // the level at the step commanded last
        S.peak.levels.push_back((double)signalScoreForPeakCal(a));
        S.peak.i++;
    }

    if (S.peak.i >= S.peak.n) {
        S.peak.on = false;
        double peakOff = 0;
        const dect2::ScanPeakResult r = dect2::scanPeakParabolic(S.peak.levels, S.peak.step, peakOff);
        if (r == dect2::ScanPeakResult::Ok) {
            const double peakMech = dect2::scanPeakMechAz(S.peak.az, peakOff);
            S.azOff = (float)dect2::rotatorAzOffsetForTrueBearing(S.peak.trueAz, peakMech);
            char b[160];
            snprintf(b, sizeof b, "Auto az offset %.1f° (peak at mech %.1f°)", S.azOff, peakMech);
            S.calMsg = b;
            saveUiPrefs();
        } else {
            S.calMsg = "No clear peak within 10 degrees: check the signal and the dish, then try again";
        }
        return;
    }

    const double cmdAz = normAz360(S.peak.az + S.peak.i * S.peak.step);
    rot.gotoMechanicalAzEl(cmdAz, S.peak.mechEl);
    rot.requestPoll();
    S.peak.settleUntil = now + 0.45;
    char b[80];
    snprintf(b, sizeof b, "Peaking azimuth… %d / %d", S.peak.i + 1, S.peak.n);
    S.calMsg = b;
}

struct GeoSlot { const char* name; double lonEast; };
static const GeoSlot kGeoSlots[] = {
    {"Astra 19.2°E", 19.2},   {"Hotbird 13°E", 13.0},   {"Eutelsat 16°E", 16.0},   {"Astra 23.5°E", 23.5},
    {"Astra 28.2°E", 28.2},   {"Eutelsat 9°E", 9.0},    {"Eutelsat 5°W", -5.0},    {"Thor 0.8°W", -0.8},
    {"Turksat 42°E", 42.0},   {"Express 36°E", 36.0},   {"ArabSat 26°E", 26.0},    {"ABS-2A 75°E", 75.0},
    {"Intelsat 18°W", -18.0}, {"Galaxy 30°W", -30.0},   {"Galaxy 91°W", -91.0},    {"Galaxy 97°W", -97.0},
    {"Galaxy 103°W", -103.0}, {"Galaxy 123°W", -123.0},
};
static constexpr int kGeoSlotCount = (int)(sizeof kGeoSlots / sizeof kGeoSlots[0]);

struct GeoVis { int slot = 0; float az = 0, el = 0; };

static std::vector<GeoVis> visibleGeoSlots(double lat, double lon, double altKm) {
    dect2::Observer o;
    o.latDeg = lat;
    o.lonDeg = lon;
    o.altKm = altKm;
    std::vector<GeoVis> v;
    for (int i = 0; i < kGeoSlotCount; i++) {
        dect2::Look l;
        if (!dect2::geoLookAngles(o, kGeoSlots[i].lonEast, l) || l.elDeg < 5.0) continue;
        v.push_back({i, (float)l.azDeg, (float)l.elDeg});
    }
    std::sort(v.begin(), v.end(), [](const GeoVis& a, const GeoVis& b) { return a.el > b.el; });
    return v;
}

void pushCalibration() {
    dect2::RotatorCalibration c;
    c.azOffsetDeg = S.azOff;
    c.elOffsetDeg = S.elOff;
    c.limitAz = S.limitAz;
    c.azMin = S.azMin;
    c.azMax = S.azMax;
    c.limitEl = S.limitEl;
    c.elMin = S.elMin;
    c.elMax = S.elMax;
    c.parkAz = S.parkAz;
    c.parkEl = S.parkEl;
    c.shortestPathAz = S.shortPath;
    dect2::Rotator::get().setCalibration(c);
}

void load() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    S.linkUi = (int)d.getI("rotLink", 0);
    S.baud = (int)d.getI("rotBaud", 9600);
    S.tcpPort = (int)d.getI("rotTcpPort", 4533);
    S.protocol = (int)d.getI("rotProto", 0);
    S.alpacaDev = (int)d.getI("rotAlpacaDev", 0);
    S.passDrive = d.getB("rotPass", false);
    S.passPark = d.getB("rotPassPark", true);
    S.autoConnect = d.getB("rotAuto", false);
    S.showMechanical = d.getB("rotShowMech", false);
    S.azOff = (float)d.getD("rotAzOff", 0);
    S.elOff = (float)d.getD("rotElOff", 0);
    S.azMin = (float)d.getD("rotAzMin", 0);
    S.azMax = (float)d.getD("rotAzMax", 360);
    S.elMin = (float)d.getD("rotElMin", 0);
    S.elMax = (float)d.getD("rotElMax", 90);
    S.parkAz = (float)d.getD("rotParkAz", 0);
    S.parkEl = (float)d.getD("rotParkEl", 0);
    S.shortPath = d.getB("rotShortPath", true);
    S.limitAz = d.getB("rotAzLim", false);
    S.limitEl = d.getB("rotElLim", false);
    snprintf(S.serialDev, sizeof S.serialDev, "%s", d.getS("rotSerial", S.serialDev).c_str());
    snprintf(S.tcpHost, sizeof S.tcpHost, "%s", d.getS("rotHost", S.tcpHost).c_str());
    pushCalibration();
}

dect2::RotatorConfig buildCfg() {
    dect2::RotatorConfig c;
    if (S.linkUi == 1) {
        c.link = dect2::RotatorLink::Serial;
        c.serialDevice = S.serialDev;
        c.baud = S.baud;
    } else if (S.linkUi == 2) {
        c.link = dect2::RotatorLink::Tcp;
        c.host = S.tcpHost;
        c.tcpPort = std::max(1, std::min(65535, S.tcpPort));
        c.protocol = S.protocol == 1 ? dect2::RotatorProtocol::Rotctld : S.protocol == 2 ? dect2::RotatorProtocol::Alpaca : dect2::RotatorProtocol::Gs232;
        c.alpacaDevice = std::max(0, S.alpacaDev);
    }
    return c;
}

void saveUiPrefs() {
    plat::Prefs& d = plat::prefs();
    d.setI("rotLink", S.linkUi);
    d.setI("rotBaud", S.baud);
    d.setI("rotTcpPort", S.tcpPort);
    d.setI("rotProto", S.protocol);
    d.setI("rotAlpacaDev", S.alpacaDev);
    d.setB("rotPass", S.passDrive);
    d.setB("rotPassPark", S.passPark);
    d.setB("rotAuto", S.autoConnect);
    d.setB("rotShowMech", S.showMechanical);
    d.setD("rotAzOff", S.azOff);
    d.setD("rotElOff", S.elOff);
    d.setD("rotAzMin", S.azMin);
    d.setD("rotAzMax", S.azMax);
    d.setD("rotElMin", S.elMin);
    d.setD("rotElMax", S.elMax);
    d.setD("rotParkAz", S.parkAz);
    d.setD("rotParkEl", S.parkEl);
    d.setB("rotShortPath", S.shortPath);
    d.setB("rotAzLim", S.limitAz);
    d.setB("rotElLim", S.limitEl);
    d.setS("rotSerial", S.serialDev);
    d.setS("rotHost", S.tcpHost);
    pushCalibration();
}

void applyConnect() {
    saveUiPrefs();
    dect2::Rotator::get().configure(buildCfg());
}

} // namespace

void rotatorLoadPrefs() {
    load();
    if (S.autoConnect && S.linkUi != 0) dect2::Rotator::get().configure(buildCfg());
}

void rotatorSavePrefs() { saveUiPrefs(); }

void rotatorPointBearing(double bearingDeg, double elDeg) {
    load();
    if (S.linkUi == 0) return;
    dect2::Rotator& r = dect2::Rotator::get();
    if (!r.connected()) applyConnect();
    r.gotoAzEl(bearingDeg, elDeg);
}

// ---- satellite pass tracking (called from passes_ui trackTick / endPass)
bool rotatorPassDrive() { load(); return S.passDrive && S.linkUi != 0; }

bool rotatorPassWanted() { load(); return S.passDrive; }

void rotatorPassSetDrive(bool on) {
    load();
    S.passDrive = on;
    saveUiPrefs();
}

void rotatorPassStep(const dect2::MountPlan& m) {
    if (!rotatorPassDrive()) return;
    dect2::Rotator& r = dect2::Rotator::get();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (m.phase == dect2::MountPhase::Preposition) {
        if (S.prepDone) return;
        if (!r.connected()) applyConnect();
        r.gotoAzEl(m.azDeg, 0.0);   // wait at the rise point
        S.prepDone = true;
    } else if (m.phase == dect2::MountPhase::Track) {
        const double every = r.supportsRates() ? 0.5 : 1.0;   // a speed order lapses after 3 s, so it is repeated
        if (now - S.lastTrackT < every) return;
        const bool rates = r.supportsRates();
        if (!rates && std::fabs(dect2::azDelta(S.lastAz, m.azDeg)) < 0.5 && std::fabs(m.elDeg - S.lastEl) < 0.5) return;   // goto mounts: only for a real move
        if (!r.connected()) applyConnect();
        r.trackAzEl(m.azDeg, m.elDeg, m.azRateDegS, m.elRateDegS);
        S.lastTrackT = now; S.lastAz = m.azDeg; S.lastEl = m.elDeg;
        S.tracking = true;
    }
}

void rotatorPassEnd() {
    load();
    if (!S.tracking && !S.prepDone) return;
    S.tracking = S.prepDone = false;
    S.lastAz = S.lastEl = -999;
    if (S.linkUi == 0) return;
    dect2::Rotator& r = dect2::Rotator::get();
    if (S.passPark) r.park(); else r.stop();
}

void rotatorTick(App& a) {
    if (a.rxId != 1) return;
    load();
    if (!S.peak.on) return;
    dect2::Rotator& rot = dect2::Rotator::get();
    if (!rot.connected() && S.linkUi != 0) applyConnect();
    stepPeakAzCal(a, rot);
}

void rotatorWindow(App& a) {
    if (a.rxId != 1) {
        if (App* f = receiverById(1)) { f->rotatorOpen = a.rotatorOpen; rotatorWindow(*f); a.rotatorOpen = f->rotatorOpen; }
        return;
    }
    if (!a.rotatorOpen) return;
    load();
    ImGui::SetNextWindowSize(ImVec2(460 * gUi, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin(trw("Antenna rotator"), &a.rotatorOpen, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }

    dect2::Rotator& rot = dect2::Rotator::get();
    const dect2::RotatorStatus st = rot.status();
    const bool canAz = st.connected && st.azValid;
    const bool canEl = st.connected && st.elValid;
    bool changed = false;

    ImGui::PushTextWrapPos(440 * gUi);
    ImGui::TextDisabled("%s", tr("Antenna rotator or telescope mount: GS-232 / Yaesu, Hamlib rotctld (works with almost any rotator) or ASCOM Alpaca. Corrected az/el include your offsets below."));
    ImGui::PopTextWrapPos();

    changed |= ImGui::RadioButton(tr("Off"), &S.linkUi, 0);
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("Serial"), &S.linkUi, 1);
    ImGui::SameLine();
    changed |= ImGui::RadioButton(tr("TCP"), &S.linkUi, 2);

    if (S.linkUi == 1) {
        ImGui::SetNextItemWidth(260 * gUi);
        changed |= ImGui::InputText(tr("Device"), S.serialDev, sizeof S.serialDev);
        ImGui::SetNextItemWidth(120 * gUi);
        changed |= ImGui::InputInt(tr("Baud"), &S.baud, 0, 0);
        S.baud = std::max(1200, std::min(115200, S.baud));
    } else if (S.linkUi == 2) {
        const int oldProto = S.protocol;
        bool pc = ImGui::RadioButton(tr("GS-232"), &S.protocol, 0);
        ImGui::SameLine();
        pc |= ImGui::RadioButton(tr("Hamlib rotctld"), &S.protocol, 1);
        ImGui::SameLine();
        pc |= ImGui::RadioButton(tr("ASCOM Alpaca"), &S.protocol, 2);
        if (pc && oldProto != S.protocol) {   // the usual port of each
            if (S.protocol == 2 && (S.tcpPort == 4533 || S.tcpPort == 4533)) S.tcpPort = 11111;
            else if (S.protocol != 2 && S.tcpPort == 11111) S.tcpPort = 4533;
            changed = true;
        }
        if (S.protocol == 1) ImGui::TextDisabled("%s", tr("Start rotctld (Hamlib) for your rotator, then connect here. Goto mounts."));
        if (S.protocol == 2) ImGui::TextDisabled("%s", tr("Alpaca runs over HTTP on every OS. On Windows, reach an ASCOM driver through the ASCOM Remote server. Takes Alt/Az positions, and axis speeds when the mount allows it."));
        ImGui::SetNextItemWidth(180 * gUi);
        changed |= ImGui::InputText(tr("Host"), S.tcpHost, sizeof S.tcpHost);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(90 * gUi);
        changed |= ImGui::InputInt(tr("Port"), &S.tcpPort, 0, 0);
        S.tcpPort = std::max(1, std::min(65535, S.tcpPort));
        if (S.protocol == 2) {
            ImGui::SetNextItemWidth(90 * gUi);
            changed |= ImGui::InputInt(tr("Telescope number"), &S.alpacaDev, 0, 0);
            S.alpacaDev = std::max(0, S.alpacaDev);
            if (ImGui::Button(tr("Find Alpaca servers"))) {
                S.found = dect2::alpacaDiscover(1200);
                S.discoverMsg = S.found.empty() ? tr("No Alpaca server answered on this network.") : "";
            }
            for (const std::string& f : S.found) {
                if (ImGui::Selectable(f.c_str())) {
                    const size_t c = f.rfind(':');
                    snprintf(S.tcpHost, sizeof S.tcpHost, "%s", f.substr(0, c).c_str());
                    S.tcpPort = std::atoi(f.c_str() + c + 1);
                    changed = true;
                }
            }
            if (!S.discoverMsg.empty()) ImGui::TextDisabled("%s", S.discoverMsg.c_str());
        }
        if (S.protocol != 0) {
            std::unique_lock<std::mutex> pl(gProbe->mu);
            const bool busy = gProbe->running;
            pl.unlock();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(tr("Test connection"))) {
                std::shared_ptr<ProbeJob> job = gProbe;
                const dect2::RotatorConfig cfg = buildCfg();
                { std::lock_guard<std::mutex> lk(job->mu); job->running = true; job->done = false; job->msg.clear(); }
                std::thread([job, cfg] {
                    dect2::MountPos pos;
                    std::string msg;
                    const bool ok = dect2::probeMount(cfg, pos, msg);
                    std::lock_guard<std::mutex> lk(job->mu);
                    job->ok = ok; job->msg = msg; job->running = false; job->done = true;
                }).detach();
            }
            ImGui::EndDisabled();
            std::lock_guard<std::mutex> lk(gProbe->mu);
            if (gProbe->running) { ImGui::SameLine(); ImGui::TextDisabled("%s", tr("testing…")); }
            else if (gProbe->done) ImGui::TextColored(gProbe->ok ? pal::okGreen() : pal::badRed(), "%s", gProbe->msg.c_str());
        }
    }

    changed |= ImGui::Checkbox(tr("Connect on start"), &S.autoConnect);

    if (typo::collapsingHeader(tr("Calibration and limits"), ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::InputFloat(tr("Azimuth offset (°)"), &S.azOff, 0.5f, 1.f, "%.1f");
        changed |= ImGui::InputFloat(tr("Elevation offset (°)"), &S.elOff, 0.5f, 1.f, "%.1f");
        ImGui::TextDisabled("%s", tr("Corrected = mechanical + offset. Map bearings use corrected azimuth."));
        changed |= ImGui::Checkbox(tr("Shortest path azimuth"), &S.shortPath);
        changed |= ImGui::Checkbox(tr("Limit azimuth travel"), &S.limitAz);
        ImGui::BeginDisabled(!S.limitAz);
        changed |= ImGui::InputFloat(tr("Az min"), &S.azMin, 1.f, 5.f, "%.0f");
        ImGui::SameLine();
        changed |= ImGui::InputFloat(tr("Az max"), &S.azMax, 1.f, 5.f, "%.0f");
        ImGui::EndDisabled();
        changed |= ImGui::Checkbox(tr("Limit elevation travel"), &S.limitEl);
        ImGui::BeginDisabled(!S.limitEl);
        changed |= ImGui::InputFloat(tr("El min"), &S.elMin, 1.f, 5.f, "%.0f");
        ImGui::SameLine();
        changed |= ImGui::InputFloat(tr("El max"), &S.elMax, 1.f, 5.f, "%.0f");
        ImGui::EndDisabled();
        changed |= ImGui::InputFloat(tr("Park azimuth"), &S.parkAz, 1.f, 5.f, "%.0f");
        ImGui::SameLine();
        changed |= ImGui::InputFloat(tr("Park elevation"), &S.parkEl, 1.f, 5.f, "%.0f");

        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Calibrate with a geostationary satellite"));
        ImGui::PushTextWrapPos(440 * gUi);
        ImGui::TextDisabled("%s", tr("Peak the dish on a DVB-S signal from the satellite (signal meter or OnAir lock). Leave the rotator as it is — even if it reads 0° when you are not on north. OnAir compares the true sky direction to the mechanical reading and stores the offset."));
        ImGui::PopTextWrapPos();
        if (!receiverLocationSet()) {
            ImGui::TextColored(pal::warnAmber(), "%s", tr("Set the receiver position in ADS-B or View > Map first."));
        } else {
            double lat = 0, lon = 0;
            receiverLocation(lat, lon);
            const std::vector<GeoVis> vis = visibleGeoSlots(lat, lon, receiverAltitudeM() / 1000.0);
            if (vis.empty()) {
                ImGui::TextDisabled("%s", tr("No catalogued GEO slots above 5° elevation from here."));
            } else {
                S.geoPick = std::max(0, std::min(S.geoPick, (int)vis.size() - 1));
                if (ImGui::BeginCombo(tr("Satellite"), vis[(size_t)S.geoPick].slot < kGeoSlotCount ? kGeoSlots[vis[(size_t)S.geoPick].slot].name : "?")) {
                    for (int i = 0; i < (int)vis.size(); i++) {
                        const GeoVis& g = vis[(size_t)i];
                        char label[96];
                        snprintf(label, sizeof label, "%s  (az %.0f°, el %.0f°)", kGeoSlots[g.slot].name, g.az, g.el);
                        if (ImGui::Selectable(label, i == S.geoPick)) S.geoPick = i;
                    }
                    ImGui::EndCombo();
                }
                const GeoVis& g = vis[(size_t)S.geoPick];
                dect2::Look sky;
                dect2::Observer o;
                o.latDeg = lat;
                o.lonDeg = lon;
                o.altKm = receiverAltitudeM() / 1000.0;
                dect2::geoLookAngles(o, kGeoSlots[g.slot].lonEast, sky);
                char skyLine[80];
                snprintf(skyLine, sizeof skyLine, "True direction from your location: az %.1f°, el %.1f°", sky.azDeg, sky.elDeg);
                ImGui::TextDisabled("%s", skyLine);
                ImGui::BeginDisabled(!canAz);
                if (ImGui::Button(tr("Calibrate azimuth from peaked satellite"))) {
                    S.azOff = (float)dect2::rotatorAzOffsetForTrueBearing(sky.azDeg, st.azDeg);
                    char b[128];
                    snprintf(b, sizeof b, "Az offset %.1f° (mech %.1f° now reads as true %.1f°)", S.azOff, st.azDeg, sky.azDeg);
                    S.calMsg = b;
                    saveUiPrefs();
                }
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!canEl);
                if (ImGui::Button(tr("Calibrate elevation from peaked satellite"))) {
                    S.elOff = (float)dect2::rotatorElOffsetForTrueElevation(sky.elDeg, st.elDeg);
                    char b[128];
                    snprintf(b, sizeof b, "El offset %.1f° (mech %.1f° → true %.1f°)", S.elOff, st.elDeg, sky.elDeg);
                    S.calMsg = b;
                    saveUiPrefs();
                }
                ImGui::EndDisabled();
                if (ImGui::Button(tr("Calibrate az + el from peaked satellite")) && canAz && canEl) {
                    S.azOff = (float)dect2::rotatorAzOffsetForTrueBearing(sky.azDeg, st.azDeg);
                    S.elOff = (float)dect2::rotatorElOffsetForTrueElevation(sky.elDeg, st.elDeg);
                    char b[160];
                    snprintf(b, sizeof b, "Offsets az %.1f°, el %.1f° (mech %.1f°/%.1f° → %.1f°/%.1f°)", S.azOff, S.elOff, st.azDeg, st.elDeg, sky.azDeg, sky.elDeg);
                    S.calMsg = b;
                    saveUiPrefs();
                }
                ImGui::BeginDisabled(!canAz || !a.engine.running() || S.peak.on);
                if (ImGui::Button(tr("Auto-find azimuth offset (signal peak)"))) {
                    if (!rot.connected()) applyConnect();
                    const double mel = st.elValid ? st.elDeg : (double)S.cmdEl;
                    startPeakAzCal(sky.azDeg, sky.elDeg, st.azDeg, mel);
                }
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("%s", tr("Sweeps ±24° on the rotator and picks the strongest DVB-S/S2 lock (Ku or L-band) or spectrum carrier, then sets the offset to the selected satellite. Tune near the signal first."));
                if (S.linkUi != 0) {
                    if (ImGui::Button(tr("Go to computed aim (rough)"))) {
                        if (!rot.connected()) applyConnect();
                        rot.gotoAzEl((float)sky.azDeg, (float)sky.elDeg);
                    }
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Uses current offsets. After calibration this should match the satellite; before calibration, peak on signal first, then calibrate."));
                }
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s", tr("Manual aim"));
        ImGui::PushTextWrapPos(440 * gUi);
        ImGui::TextDisabled("%s", tr("Or point at a landmark / north: enter the true bearing and apply."));
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::InputFloat(tr("True azimuth now (°)"), &S.calTrueAz, 1.f, 5.f, "%.1f");
        S.calTrueAz = std::fmod(S.calTrueAz, 360.f);
        if (S.calTrueAz < 0) S.calTrueAz += 360.f;
        ImGui::SameLine();
        if (ImGui::Button(tr("North = 0°"))) S.calTrueAz = 0.f;
        ImGui::SetNextItemWidth(120 * gUi);
        ImGui::InputFloat(tr("True elevation now (°)"), &S.calTrueEl, 1.f, 2.f, "%.1f");
        S.calTrueEl = std::max(0.f, std::min(90.f, S.calTrueEl));
        ImGui::BeginDisabled(!canAz);
        if (ImGui::Button(tr("Set azimuth offset from aim"))) {
            S.azOff = (float)dect2::rotatorAzOffsetForTrueBearing(S.calTrueAz, st.azDeg);
            char b[96];
            snprintf(b, sizeof b, "Azimuth offset set to %.1f° (mechanical %.1f° → true %.1f°)", S.azOff, st.azDeg, S.calTrueAz);
            S.calMsg = b;
            saveUiPrefs();
        }
        ImGui::EndDisabled();
        if (!canAz) ImGui::SameLine(), ImGui::TextDisabled("%s", tr("(connect and read position)"));
        ImGui::BeginDisabled(!canEl);
        if (ImGui::Button(tr("Set elevation offset from aim"))) {
            S.elOff = (float)dect2::rotatorElOffsetForTrueElevation(S.calTrueEl, st.elDeg);
            char b[96];
            snprintf(b, sizeof b, "Elevation offset set to %.1f° (mechanical %.1f° → true %.1f°)", S.elOff, st.elDeg, S.calTrueEl);
            S.calMsg = b;
            saveUiPrefs();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Use a known tilt (e.g. 0° horizontal) with the antenna at that angle."));
        if (!S.calMsg.empty()) {
            ImGui::PushTextWrapPos(440 * gUi);
            ImGui::TextColored(pal::okGreen(), "%s", S.calMsg.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    if (typo::collapsingHeader(tr("Satellite passes"), ImGuiTreeNodeFlags_DefaultOpen)) {
        changed |= ImGui::Checkbox(tr("Point the mount at the tracked satellite"), &S.passDrive);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Turn on Track in View > Satellite passes. The mount waits at the rise point before AOS, follows the satellite through the pass (with axis speeds on mounts that take them) and parks or stops at LOS."));
        ImGui::BeginDisabled(!S.passDrive);
        changed |= ImGui::Checkbox(tr("Park after the pass (otherwise stop)"), &S.passPark);
        ImGui::EndDisabled();
        if (S.passDrive && S.linkUi == 0) ImGui::TextColored(pal::warnAmber(), "%s", tr("Choose a connection above first."));
        if (S.passDrive && S.linkUi != 0) {
            ImGui::TextDisabled(rot.supportsRates() ? "%s" : "%s", rot.supportsRates() ? tr("This mount takes axis speeds: smooth tracking.") : tr("Goto mount: a new position about once a second."));
        }
    }

    if (changed) saveUiPrefs();

    ImGui::Spacing();
    if (S.linkUi == 0) {
        if (st.connected) rot.disconnect();
        ImGui::TextDisabled("%s", tr("Rotator is off."));
    } else {
        const bool on = st.connected;
        bool want = on;
        if (ImGui::Checkbox(tr("Connected"), &want)) {
            if (want) applyConnect();
            else rot.disconnect();
        }
        if (!st.lastError.empty()) ImGui::TextColored(pal::badRed(), "%s", st.lastError.c_str());
        changed |= ImGui::Checkbox(tr("Show mechanical position"), &S.showMechanical);
        if (changed) saveUiPrefs();

        ImGui::PushFont(a.mono, 0);
        if (st.azValid || st.elValid) {
            if (S.showMechanical)
                ImGui::Text("%s  mech az %.1f° el %.1f°", st.moving ? "moving" : "idle", st.azValid ? st.azDeg : 0.0, st.elValid ? st.elDeg : 0.0);
            ImGui::Text("%s  az %.1f°  el %.1f°%s", st.moving ? "moving" : "idle",
                        st.azValid ? st.azCorrectedDeg : 0.0, st.elValid ? st.elCorrectedDeg : 0.0, st.hasTarget ? "  (target)" : "");
        } else ImGui::TextDisabled("%s", tr("position unknown (not connected or no reply yet)"));
        ImGui::PopFont();

        if (!S.cmdAz && st.azValid) S.cmdAz = (float)st.azCorrectedDeg;
        if (!S.cmdEl && st.elValid) S.cmdEl = (float)st.elCorrectedDeg;
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::SliderFloat(tr("Azimuth (corrected)"), &S.cmdAz, 0.f, 360.f, "%.1f°");
        ImGui::SetNextItemWidth(200 * gUi);
        ImGui::SliderFloat(tr("Elevation (corrected)"), &S.cmdEl, S.elMin, S.elMax, "%.1f°");
        if (ImGui::Button(tr("Go"))) {
            if (!on) applyConnect();
            rot.gotoAzEl(S.cmdAz, S.cmdEl);
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Park"))) {
            if (!on) applyConnect();
            rot.park();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Stop"))) rot.stop();
        ImGui::SameLine();
        if (ImGui::Button(tr("Read position"))) rot.requestPoll();

        if (receiverLocationSet()) {
            ImGui::Spacing();
            ImGui::TextDisabled("%s", tr("Point at a bearing from the receiver (ADS-B / Map position):"));
            static float mapBrg = 0.f;
            ImGui::SetNextItemWidth(200 * gUi);
            ImGui::SliderFloat(tr("Bearing"), &mapBrg, 0.f, 360.f, "%.0f°");
            if (ImGui::Button(tr("Point rotator"))) {
                if (!on) applyConnect();
                rot.gotoAzEl(mapBrg, S.cmdEl);
            }
        } else ImGui::TextDisabled("%s", tr("Set the receiver position in ADS-B or View > Map to use bearings."));
    }
    ImGui::End();
}
