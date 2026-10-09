// ACARS screens: the message list with a detail pane, the aircraft table and the channel bars.
#include "adsb_map.h"
#include "app.h"
#include "dect2/acars_tel.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

struct State {
    bool loaded = false;
    int viewMode = 0;                    // 0 messages, 1 map, 2 aircraft, 3 channels
    char fLabel[8] = "";                 // filter: label starts with this
    char fReg[16] = "";                  // filter: registration or flight contains this
    bool downOnly = false;
    uint64_t selected = 0;               // serial of the message shown in the detail pane (0 = none)
    char chans[96] = "";                 // watched channels in MHz, empty = every carrier on the 25 kHz grid
    float thrDb = 8.f;
    char pushedChans[96] = "\x01";
    double pushedCenter = 0, pushedThr = -1;
    bool wasRunning = false;
    adsbmap::View map;
    bool mapFollow = true;               // the map fits the aircraft until the user moves it
    std::string selReg;                  // the aircraft chosen on the map
};
State S;

bool live(const App& a) { return a.engine.running() && a.rx.standard == 17; }

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    plat::Prefs& d = plat::prefs();
    snprintf(S.chans, sizeof S.chans, "%s", d.getS("acarsChans", "").c_str());
    S.thrDb = (float)d.getD("acarsThr", 8.0);
    S.map.zoom = (int)d.getI("acarsZoom", 6);
    if (const char* e = getenv("DECT2_ACARS_VIEW")) if (!strcmp(e, "map")) S.viewMode = 1;   // dev: open on the map (screenshots)
}

std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }

bool contains(const std::string& hay, const char* needle) {
    if (!*needle) return true;
    return lower(hay).find(lower(needle)) != std::string::npos;
}

std::string clock(int64_t t) {
    if (t <= 0) return "-";
    const time_t tt = (time_t)t;
    struct tm g;
#ifdef _WIN32
    gmtime_s(&g, &tt);
#else
    gmtime_r(&tt, &g);
#endif
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", g.tm_hour, g.tm_min, g.tm_sec);
    return b;
}

std::string ago(double now, double then) {
    if (then <= 0) return "-";
    const double s = std::max(0.0, now - then);
    char b[24];
    if (s < 90) snprintf(b, sizeof b, "%.0f s", s);
    else if (s < 5400) snprintf(b, sizeof b, "%.0f min", s / 60);
    else snprintf(b, sizeof b, "%.1f h", s / 3600);
    return b;
}

// the text on one line: line breaks and control characters become spaces
std::string oneLine(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\n' || c == '\r' || (unsigned char)c < 32) ? ' ' : c;
    return o;
}

ImVec4 levelColour(float db) { return db >= -60 ? pal::okGreen() : db >= -80 ? pal::warnAmber() : pal::badRed(); }

// pushes the settings to the receiver
void tick(App& a) {
    loadState();
    if (!a.engine.running()) { S.wasRunning = false; S.pushedCenter = 0; S.pushedThr = -1; S.pushedChans[0] = '\x01'; S.pushedChans[1] = 0; return; }
    dect2::AcarsReceiver& r = a.engine.acars();
    if (S.pushedCenter != a.freqMhz) { r.setCenterHz(a.freqMhz * 1e6); S.pushedCenter = a.freqMhz; }
    if (S.pushedThr != S.thrDb) { r.setThresholdDb(S.thrDb); S.pushedThr = S.thrDb; }
    if (std::strcmp(S.pushedChans, S.chans) != 0) {
        std::vector<double> hz;
        const char* p = S.chans;
        while (*p) {
            char* e = nullptr;
            const double mhz = std::strtod(p, &e);
            if (e == p) { p++; continue; }
            if (mhz >= 100 && mhz <= 160) hz.push_back(mhz * 1e6);
            p = e;
        }
        r.setChannels(hz);
        std::snprintf(S.pushedChans, sizeof S.pushedChans, "%s", S.chans);
    }
}

// ---------------------------------------------------------------- views

const dect2::AcarsMessage* findMsg(const dect2::AcarsTelemetry& t, uint64_t serial) {
    for (const auto& m : t.messages) if (m.serial == serial) return &m;
    return nullptr;
}

void filterRow() {
    ImGui::TextDisabled("Label"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(46 * gUi);
    ImGui::InputText("##acl", S.fLabel, sizeof S.fLabel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show only blocks whose label starts with this, for example H1 or 5Z.");
    flowNext(12 * gUi);
    ImGui::TextDisabled("Registration / flight"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(100 * gUi);
    ImGui::InputText("##acr", S.fReg, sizeof S.fReg);
    flowNext(12 * gUi);
    ImGui::Checkbox("downlink only", &S.downOnly);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Leave out the uplinks (ground to aircraft).");
    flowEnd();   // the filters wrap in a narrow tab
}

bool passes(const dect2::AcarsMessage& m) {
    if (S.downOnly && !m.downlink) return false;
    if (S.fLabel[0] && lower(m.label).rfind(lower(S.fLabel), 0) != 0) return false;
    if (S.fReg[0] && !contains(m.reg, S.fReg) && !contains(m.flightId, S.fReg)) return false;
    return true;
}

void detailPane(const App& a, const dect2::AcarsMessage* m) {
    if (!m) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Click a message to read all of it."); ImGui::PopTextWrapPos(); } return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(110 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("time", "%s UTC  %.4f MHz  %.0f dB", clock(m->wallTime).c_str(), m->freqHz / 1e6, m->levelDb);
    kv("aircraft", "%s%s%s", m->reg.empty() ? "-" : m->reg.c_str(), m->flightId.empty() ? "" : "  flight ", m->flightId.c_str());
    kv("direction", "%s, mode %c, block id %c, %s", m->downlink ? "downlink" : "uplink", m->mode ? m->mode : '-', m->blockId ? m->blockId : '-', m->finalBlock ? "last block" : "more blocks follow");
    kv("label", "%s  %s", m->label.c_str(), m->labelText.empty() ? "(not in the table)" : m->labelText.c_str());
    if (!m->msgNum.empty() || m->msgSeq) kv("message", "%s%c", m->msgNum.c_str(), m->msgSeq ? m->msgSeq : ' ');
    if (!m->sublabel.empty() || !m->mfi.empty()) kv("prefix", "%s %s", m->sublabel.c_str(), m->mfi.c_str());
    if (m->ack) kv("ack", "%s", m->ack == '!' ? "NAK" : m->ack == '^' ? "ACK" : std::string(1, m->ack).c_str());
    if (m->parityFixed) kv("repaired", "%d bit%s flipped", m->parityFixed, m->parityFixed == 1 ? "" : "s");
    if (!m->decoded.empty()) { ImGui::TextDisabled("reading"); kvColumn(110 * gUi); ImGui::TextWrapped("%s", m->decoded.c_str()); }
    if (m->hasPos) kv("position", "%.4f %.4f", m->lat, m->lon);
    if (!m->adsc.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Decoded ADS-C");
        ImGui::PushFont(a.mono, 0);
        ImGui::TextUnformatted(m->adsc.c_str());
        ImGui::PopFont();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Text");
    ImGui::BeginChild("##actext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    if (m->text.empty()) ImGui::TextDisabled("(no text)"); else ImGui::TextWrapped("%s", m->text.c_str());
    ImGui::PopFont();
    ImGui::EndChild();
}

void messageView(App& a) {
    const dect2::AcarsTelemetry& t = a.rx.acars;
    const bool on = live(a);
    filterRow();
    const float H = ImGui::GetContentRegionAvail().y;
    const float listH = std::max(120.f * gUi, H * 0.58f);
    if (ImGui::BeginTable("##acmsg", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Reg"); ImGui::TableSetupColumn("Flight");
        ImGui::TableSetupColumn("Label"); ImGui::TableSetupColumn("Meaning"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int shown = 0;
        if (on) {
            for (const auto& m : t.messages) {
                if (!passes(m)) continue;
                shown++;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[24]; snprintf(id, sizeof id, "##m%llu", (unsigned long long)m.serial);
                if (ImGui::Selectable((clock(m.wallTime) + id).c_str(), S.selected == m.serial, ImGuiSelectableFlags_SpanAllColumns)) S.selected = m.serial;
                ImGui::TableNextColumn(); ImGui::Text("%.4f", m.freqHz / 1e6);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.reg.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.downlink ? m.flightId.c_str() : "");
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, pal::accent()); ImGui::TextUnformatted(m.label.c_str()); ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(m.labelText, ImGui::GetContentRegionAvail().x).c_str());   // whole in the pane below
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(m.decoded.empty() ? oneLine(m.text) : "[" + m.decoded + "] " + oneLine(m.text), ImGui::GetContentRegionAvail().x).c_str());   // a preview: the whole text is in the pane below
            }
        }
        ImGui::EndTable();
        if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); }
        else if (!shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.messages.empty() ? "no message yet" : "no message matches the filter"); ImGui::PopTextWrapPos(); }
    }
    ImGui::Spacing();
    ImGui::BeginChild("##acdet", ImVec2(0, 0));
    detailPane(a, on ? findMsg(t, S.selected) : nullptr);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the aircraft map

const char* kMapNote = "Positions are the aircraft's own reports (ADS-C, text position messages): every few minutes per aircraft, not live like ADS-B.";
const char* kMapTip = "On VHF the aircraft transmit directly, so ADS-C (label B6, or H1 with #M1B/B6) and POS reports reach you from aircraft within\n"
                      "line of sight. An aircraft shows up after its first report and moves with each new one; the line behind it is its track.";

std::string nameOf(const dect2::AcarsAircraft& c) {
    if (!c.reg.empty() && !c.flight.empty()) return c.reg + " " + c.flight;
    return c.reg.empty() ? c.flight : c.reg;
}

bool isSel(const dect2::AcarsAircraft& c) { return !S.selReg.empty() && c.reg == S.selReg; }

// ADS-C reports blue, text reports amber; older than half an hour greyed
ImU32 planeColour(const dect2::AcarsAircraft& c, double now, float alpha = 1.f) {
    if (now - c.posTime > 1800) return IM_COL32(150, 154, 158, (int)(200 * alpha));
    return c.posSource == 2 ? IM_COL32(230, 178, 70, (int)(255 * alpha)) : IM_COL32(115, 184, 210, (int)(255 * alpha));
}

// an airliner seen from above, nose to the heading
void drawPlane(ImDrawList* dl, ImVec2 c, float headingDeg, float r, ImU32 fill, bool outline) {
    static const float pts[][2] = {{0, -1}, {0.09f, -0.75f}, {0.10f, -0.15f}, {1.0f, 0.30f}, {1.0f, 0.46f}, {0.10f, 0.22f}, {0.09f, 0.68f}, {0.40f, 0.90f}, {0.40f, 1.0f}, {0, 0.92f},
                                   {-0.40f, 1.0f}, {-0.40f, 0.90f}, {-0.09f, 0.68f}, {-0.10f, 0.22f}, {-1.0f, 0.46f}, {-1.0f, 0.30f}, {-0.10f, -0.15f}, {-0.09f, -0.75f}};
    const int n = (int)(sizeof pts / sizeof *pts);
    const float h = headingDeg * 3.14159265f / 180.f, ch = std::cos(h), sh = std::sin(h);
    ImVec2 q[18];
    for (int i = 0; i < n; i++) q[i] = ImVec2(c.x + (pts[i][0] * ch - pts[i][1] * sh) * r, c.y + (pts[i][0] * sh + pts[i][1] * ch) * r);
    dl->AddConcavePolyFilled(q, n, fill);
    dl->AddPolyline(q, n, outline ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), ImDrawFlags_Closed, outline ? 1.6f : 1.f);
}

void mapCanvas(App& a, ImVec2 size) {
    const dect2::AcarsTelemetry& t = a.rx.acars;
    const bool on = live(a);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (S.mapFollow && on) {   // fit the aircraft that have a position, until the user moves the map
        double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180; int n = 0;
        for (const auto& c : t.aircraft) if (c.hasPos) { la0 = std::min(la0, c.lat); la1 = std::max(la1, c.lat); lo0 = std::min(lo0, c.lon); lo1 = std::max(lo1, c.lon); n++; }
        if (n) {
            S.map.lat = (la0 + la1) / 2; S.map.lon = (lo0 + lo1) / 2;
            const double span = std::max(0.5, std::max(lo1 - lo0, (la1 - la0) * 1.4));
            S.map.zoom = std::max(2, std::min(10, (int)std::floor(std::log2(0.7 * size.x * 360.0 / (256.0 * span)))));
        }
    }
    const double lat0 = S.map.lat, lon0 = S.map.lon;
    adsbmap::draw(S.map, size);
    if (S.map.lat != lat0 || S.map.lon != lon0) S.mapFollow = false;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, ImVec2(p0.x + size.x, p0.y + size.y), true);
    const dect2::AcarsAircraft* hit = nullptr; float best = 18 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    int withPos = 0;
    std::vector<ImVec4> placed;              // label boxes already drawn (x0, y0, x1, y1), so labels do not cover each other
    if (on) {
        for (const auto& c : t.aircraft) {   // tracks and predicted routes first, so the planes sit on top
            if (!c.hasPos) continue;
            withPos++;
            const ImU32 col = planeColour(c, t.nowSec, 0.5f);
            for (size_t i = 1; i < c.track.size(); i++)
                dl->AddLine(adsbmap::project(c.track[i - 1].lat, c.track[i - 1].lon), adsbmap::project(c.track[i].lat, c.track[i].lon), col, 1.5f);
            if (isSel(c) && !c.route.empty()) {   // the route the aircraft says it will fly, dotted
                ImVec2 prev = adsbmap::project(c.lat, c.lon);
                for (const auto& r : c.route) {
                    const ImVec2 q = adsbmap::project(r.first, r.second);
                    const float len = std::hypot(q.x - prev.x, q.y - prev.y);
                    const int dots = std::max(1, (int)(len / (8 * gUi)));
                    for (int k = 0; k < dots; k += 2)
                        dl->AddLine(ImVec2(prev.x + (q.x - prev.x) * k / dots, prev.y + (q.y - prev.y) * k / dots),
                                    ImVec2(prev.x + (q.x - prev.x) * (k + 1) / dots, prev.y + (q.y - prev.y) * (k + 1) / dots), IM_COL32(255, 255, 255, 140), 1.2f);
                    dl->AddCircle(q, 3 * gUi, IM_COL32(255, 255, 255, 160), 10, 1.2f);
                    prev = q;
                }
            }
        }
        for (const auto& c : t.aircraft) {
            if (!c.hasPos) continue;
            const ImVec2 q = adsbmap::project(c.lat, c.lon);
            if (q.x < p0.x - 60 || q.y < p0.y - 60 || q.x > p0.x + size.x + 60 || q.y > p0.y + size.y + 60) continue;
            const bool sel = isSel(c);
            if (c.hasTrack) drawPlane(dl, q, (float)c.trackDeg, (sel ? 12.f : 10.f) * gUi, planeColour(c, t.nowSec), sel);
            else {
                dl->AddCircleFilled(q, 5 * gUi, planeColour(c, t.nowSec));
                dl->AddCircle(q, 5 * gUi, sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(0, 0, 0, 150), 16, sel ? 1.6f : 1.f);
            }
            if (S.map.zoom >= 5 || sel || withPos <= 12) {
                std::string s = nameOf(c);
                if (c.hasAlt) { char b[24]; snprintf(b, sizeof b, "  FL%03d", (c.altFt + 50) / 100); s += b; }
                const ImVec2 ts = ImGui::CalcTextSize(s.c_str());
                const float w = ts.x + 8 * gUi, h = ts.y + 4 * gUi;
                // right of the plane, else left, below or above, whichever is free first
                const ImVec2 tries[4] = {{q.x + 13 * gUi, q.y - h * 0.5f}, {q.x - 13 * gUi - w, q.y - h * 0.5f}, {q.x + 8 * gUi, q.y + 9 * gUi}, {q.x + 8 * gUi, q.y - 9 * gUi - h}};
                ImVec2 lp = tries[0];
                for (const ImVec2& tp : tries) {
                    bool free = true;
                    for (const ImVec4& r : placed) free &= tp.x + w < r.x || tp.x > r.z || tp.y + h < r.y || tp.y > r.w;
                    if (free) { lp = tp; break; }
                }
                placed.push_back(ImVec4(lp.x, lp.y, lp.x + w, lp.y + h));
                dl->AddRectFilled(lp, ImVec2(lp.x + w, lp.y + h), IM_COL32(10, 12, 14, 210), 2.f);
                dl->AddRect(lp, ImVec2(lp.x + w, lp.y + h), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
                dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), s.c_str());
            }
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = &c; }
        }
    }
    if (clicked) S.selReg = hit ? hit->reg : "";
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(adsbmap::kMaxZoom, S.map.zoom + 1); S.mapFollow = false; }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(adsbmap::kMinZoom, S.map.zoom - 1); S.mapFollow = false; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 6 * gUi);
    adsbmap::tileControls("never the position itself");
    plat::prefs().setI("acarsZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no position report yet");
    else adsbmap::legend(size.x, "%d of %zu aircraft placed", withPos, t.aircraft.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

// the chosen aircraft: its last position and its latest messages; with none chosen, the aircraft that have a position
void mapSide(App& a) {
    const dect2::AcarsTelemetry& t = a.rx.acars;
    const bool on = live(a);
    const dect2::AcarsAircraft* c = nullptr;
    if (on) for (const auto& x : t.aircraft) if (isSel(x)) c = &x;
    if (!c) {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Blue: ADS-C report. Amber: text report.\nGrey: older than 30 minutes."); ImGui::PopTextWrapPos(); }
        ImGui::Spacing();
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Click an aircraft on the map, or here:"); ImGui::PopTextWrapPos(); }
        if (!on) return;
        int n = 0;
        for (const auto& x : t.aircraft) {
            if (!x.hasPos) continue;
            n++;
            char id[16]; snprintf(id, sizeof id, "##ap%d", n);
            const std::string row = nameOf(x) + "   " + ago(t.nowSec, x.posTime) + " ago" + id;
            if (ImGui::Selectable(row.c_str(), false)) S.selReg = x.reg;
        }
        if (!n) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("(no aircraft has sent a position yet)"); ImGui::PopTextWrapPos(); }
        return;
    }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(84 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    ImGui::TextUnformatted(nameOf(*c).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) S.selReg.clear();
    if (c->icao) kv("ICAO", "%06X", c->icao);
    if (c->hasPos) {
        kv("position", "%.4f %.4f", c->lat, c->lon);
        if (c->hasAlt) kv("altitude", "%d ft", c->altFt); else kv("altitude", "%s", "-");
        if (c->hasTrack) kv("track", "%.0f deg%s", c->trackDeg, c->posSource == 1 && c->hasSpeed ? "" : " (from the last two positions)");
        if (c->hasSpeed) kv("speed", "%.0f kt over the ground", c->speedKt);
        kv("source", "%s", c->posKind.c_str());
        if (c->reportSecPastHour >= 0) { const int s = (int)c->reportSecPastHour; kv("report", "at xx:%02d:%02d (minutes past the hour)", s / 60, s % 60); }
        else if (c->reportSecOfDay >= 0) { const int s = c->reportSecOfDay; kv("report", "at %02d:%02d:%02d UTC", s / 3600, s / 60 % 60, s % 60); }
        kv("received", "%s UTC, %s ago", clock(c->posWall).c_str(), ago(t.nowSec, c->posTime).c_str());
        kv("reports", "%u, track of %zu points", c->positions, c->track.size());
    } else {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no position from this aircraft yet"); ImGui::PopTextWrapPos(); }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Latest messages");
    ImGui::BeginChild("##acmapmsg", ImVec2(0, 0), ImGuiChildFlags_Borders);
    int shown = 0;
    for (const auto& m : t.messages) {      // newest first
        if (shown >= 10) break;
        if (m.reg != c->reg) continue;
        shown++;
        ImGui::PushStyleColor(ImGuiCol_Text, pal::accent());
        ImGui::Text("%s  %s %s", clock(m.wallTime).c_str(), m.label.c_str(), m.downlink ? "down" : "up");
        ImGui::PopStyleColor();
        ImGui::PushFont(a.mono, 0);
        std::string body = m.decoded.empty() ? oneLine(m.text) : m.decoded;
        if (body.size() > 160) body = body.substr(0, 160) + "...";
        ImGui::TextWrapped("%s", body.empty() ? "(no text)" : body.c_str());
        ImGui::PopFont();
        ImGui::Spacing();
    }
    if (!shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("none in the last %zu messages", t.messages.size()); ImGui::PopTextWrapPos(); }
    ImGui::EndChild();
}

void mapView(App& a) {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", kMapNote);
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kMapTip);
    const ImVec2 av = ImGui::GetContentRegionAvail();
    const float sideW = std::max(250.f * gUi, std::min(380.f * gUi, av.x * 0.3f));
    const ImVec2 size(std::max(200.f, av.x - sideW - 8 * gUi), std::max(160.f, av.y));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    mapCanvas(a, size);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + size.x + 8 * gUi, p0.y));
    ImGui::BeginChild("##acmapside", ImVec2(sideW, size.y));
    mapSide(a);
    ImGui::EndChild();
}

void aircraftView(const App& a) {
    const dect2::AcarsTelemetry& t = a.rx.acars;
    const bool on = live(a);
    if (!ImGui::BeginTable("##acair", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Registration"); ImGui::TableSetupColumn("Flight"); ImGui::TableSetupColumn("Blocks"); ImGui::TableSetupColumn("Last label");
    ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("Level dB"); ImGui::TableSetupColumn("Last heard");
    ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Alt ft");
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto& c : t.aircraft) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.reg.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.flight.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%u", c.messages);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.lastLabel.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%.4f", c.freqHz / 1e6);
            ImGui::TableNextColumn(); if (c.levelDb > -119) { ImGui::PushStyleColor(ImGuiCol_Text, levelColour(c.levelDb)); ImGui::Text("%.0f", c.levelDb); ImGui::PopStyleColor(); } else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.nowSec, c.lastHeardSec).c_str());
            ImGui::TableNextColumn(); if (c.hasPos) { ImGui::PushTextWrapPos(0); ImGui::Text("%.3f %.3f (%s ago)", c.lat, c.lon, ago(t.nowSec, c.posTime).c_str()); ImGui::PopTextWrapPos(); } else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (c.hasPos && c.hasAlt) ImGui::Text("%d", c.altFt); else ImGui::TextUnformatted("-");
        }
    }
    ImGui::EndTable();
}

// one bar per channel: the level over a scale of -100 to -20 dB, a carrier marker, the counts as text
void channelView(const App& a) {
    const dect2::AcarsTelemetry& t = a.rx.acars;
    const bool on = live(a);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float W = ImGui::GetContentRegionAvail().x;
    const float rowH = ImGui::GetTextLineHeightWithSpacing() * 1.5f;
    if (!on || t.channels.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", on ? "no carrier seen yet" : a.engine.running() ? "starting" : "start the receiver to see the channels"); ImGui::PopTextWrapPos(); } return; }
    const float labW = 96 * gUi, txtW = std::min(300.f * gUi, W * 0.45f);
    const float barW = std::max(60.f, W - labW - txtW - 12 * gUi);
    ImGui::BeginChild("##acch", ImVec2(0, 0));
    for (const auto& c : t.channels) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(W, rowH));
        const float y0 = p.y + 2, y1 = p.y + rowH - 2;
        char b[64];
        snprintf(b, sizeof b, "%.4f", c.freqHz / 1e6);
        dl->AddText(ImVec2(p.x, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), c.active ? IM_COL32(220, 222, 225, 255) : IM_COL32(120, 124, 128, 255), b);
        const float bx = p.x + labW;
        dl->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + barW, y1), IM_COL32(8, 9, 10, 255));
        const float f = std::max(0.f, std::min(1.f, (c.levelDb + 100.f) / 80.f));
        if (c.levelDb > -119) {
            const ImVec4 col = c.active ? levelColour(c.levelDb) : pal::grey();
            dl->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + barW * f, y1), ImGui::ColorConvertFloat4ToU32(ImVec4(col.x, col.y, col.z, c.active ? 0.9f : 0.4f)));
        }
        for (int d = -100; d <= -20; d += 20) { const float x = bx + barW * (d + 100) / 80.f; dl->AddLine(ImVec2(x, y1 - 4 * gUi), ImVec2(x, y1), IM_COL32(70, 74, 78, 255)); }
        snprintf(b, sizeof b, "%.0f dB", c.levelDb);
        dl->AddText(ImVec2(bx + 4 * gUi, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(235, 237, 240, 255), c.levelDb > -119 ? b : "");
        char r[128];
        snprintf(r, sizeof r, "%s  SNR %.0f dB  %+.0f Hz  %u ok %u bad  %s", c.active ? "on " : "off", c.snrDb, c.cfoHz, c.messages, c.bad, c.lastHeardSec > 0 ? ago(t.nowSec, c.lastHeardSec).c_str() : "-");
        dl->AddText(ImVec2(bx + barW + 10 * gUi, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), c.active ? IM_COL32(190, 194, 198, 255) : IM_COL32(120, 124, 128, 255), r);
    }
    ImGui::EndChild();
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    loadState();
    subNav("acarsv", S.viewMode, {"Messages", "Aircraft map", "Aircraft", "Channels"});
    if (S.viewMode == 0) messageView(a);
    else if (S.viewMode == 1) mapView(a);
    else if (S.viewMode == 2) aircraftView(a);
    else channelView(a);
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see aircraft"); ImGui::PopTextWrapPos(); } return; }
    const dect2::AcarsTelemetry& t = a.rx.acars;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu aircraft, %zu messages", t.aircraft.size(), t.messages.size()); ImGui::PopTextWrapPos(); }
    if (!ImGui::BeginTable("##acl_s", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Reg"); ImGui::TableSetupColumn("Flight"); ImGui::TableSetupColumn("Blocks");
    ImGui::TableHeadersRow();
    for (const auto& c : t.aircraft) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.reg.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.flight.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%u", c.messages);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::AcarsTelemetry& t = a.rx.acars;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int act = 0;
    for (const auto& c : t.channels) act += c.active ? 1 : 0;
    kv("tuned to", "%.4f MHz, %zu channels seen, %d active", t.centerHz / 1e6, t.channels.size(), act);
    kv("frames", "%llu started, %llu good, %llu failed the check", (unsigned long long)t.framesStarted, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("repaired", "%llu blocks needed bit repair", (unsigned long long)t.parityFixed);
    kv("positions", "%llu blocks with a position", (unsigned long long)t.positionsTotal);
    kv("strongest", "%.0f dB SNR, %+.0f Hz off", t.snrDb, t.cfoHz);
    kv("signal time", "%.1f s", t.nowSec);
    kv("watching", "%s", S.chans[0] ? S.chans : "every carrier on the 25 kHz grid");
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::AcarsTelemetry& t = a.rx.acars;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    int act = 0;
    for (const auto& c : t.channels) act += c.active ? 1 : 0;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : act > 0 ? 1 : 0); flowNext(12 * gUi);
    lamp("Messages", !on ? 0 : t.state == 2 ? 1 : t.blocksOk > 0 ? 2 : 0); flowNext(10 * gUi);
    ImGui::TextDisabled("|"); flowNext(10 * gUi);
    auto ro = [&](const char* label, const std::string& val) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", label); ImGui::SameLine(0, 5 * gUi);
        ImGui::PushFont(a.mono, 0); ImGui::TextUnformatted(val.c_str()); ImGui::PopFont();
        flowNext(15 * gUi);
    };
    char b[64];
    if (!on) { ro("State", run ? "starting" : "stopped"); return; }
    ro("State", t.state == 2 ? "Decoding" : t.state == 1 ? "Carrier" : "Searching");
    snprintf(b, sizeof b, "%d", act); ro("Channels", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("Good / bad", b);
    snprintf(b, sizeof b, "%zu", t.aircraft.size()); ro("Aircraft", b);
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "ACARS";
    if (live(a)) l2 = dect2::acarsSummary(a.rx.acars);
}

void tuner(App& a, bool& retune) {
    loadState();
    (void)a; (void)retune;
    ImGui::TextDisabled("Channels MHz");
    ImGui::SetNextItemWidth(std::min(230 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::InputTextWithHint("##acch", "all on the 25 kHz grid", S.chans, sizeof S.chans)) plat::prefs().setS("acarsChans", S.chans);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Watch only these channels, separated by spaces: for example 131.525 131.725 131.825.\nEmpty: every carrier in the captured band on the 25 kHz grid is found and decoded.\nEurope uses 131.525, 131.725 and 131.825, North America 131.550 and 130.025.");
}

void decoder(App& a, bool&) {
    loadState();
    (void)a;
    ImGui::TextDisabled("THRESHOLD"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(std::min(90 * gUi, ImGui::GetContentRegionAvail().x));   // no wider than the side panel
    if (ImGui::SliderFloat("##acthr", &S.thrDb, 3.f, 20.f, "%.0f dB")) plat::prefs().setD("acarsThr", S.thrDb);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Carrier over noise needed to start decoding a channel. Lower finds weaker aircraft but also decodes noise.");
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated aircraft"); ImGui::PopTextWrapPos(); }
    flowNext(); ImGui::SetNextItemWidth(90 * gUi);
    int n = sc.modeOpt[0] > 0 ? sc.modeOpt[0] : 6;
    if (ImGui::SliderInt("##acn", &n, 1, 24, "%d aircraft")) { sc.modeOpt[0] = n; changed = true; }
    flowNext(); ImGui::SetNextItemWidth(110 * gUi);
    static const char* sets[] = {"Europe", "131.525 only", "N. America"};
    int cs = std::max(0, std::min(2, sc.modeOpt[3]));
    if (ImGui::Combo("##acset", &cs, sets, 3)) { sc.modeOpt[3] = cs; changed = true; }
    flowNext(); ImGui::TextDisabled("AM"); ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    int am = sc.modeOpt[2] > 0 ? sc.modeOpt[2] : 60;
    if (ImGui::SliderInt("##acam", &am, 10, 95, "%d %%")) { sc.modeOpt[2] = am; changed = true; }
    flowNext(); ImGui::TextDisabled("rate"); ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    float rate = sc.modeVal[0] > 0 ? (float)sc.modeVal[0] : 1.f;
    if (ImGui::SliderFloat("##acrate", &rate, 0.5f, 4.f, "x%.1f")) { sc.modeVal[0] = rate; changed = true; }
    flowNext(); ImGui::TextDisabled("weak ch."); ImGui::SameLine(); ImGui::SetNextItemWidth(80 * gUi);
    float att = (float)sc.modeVal[1];
    if (ImGui::SliderFloat("##acatt", &att, 0.f, 40.f, "-%.0f dB")) { sc.modeVal[1] = att; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Extra attenuation of the last channel of the set: a weak channel next to strong ones.");
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##accfo", &cfo, -3, 3, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::AcarsTelemetry& t = a.rx.acars;
    int act = 0;
    for (const auto& c : t.channels) act += c.active ? 1 : 0;
    out.push_back({"CHANNELS", "%.0f", (double)act, 0, 8, act > 0 ? 1 : 0});
    out.push_back({"SNR  dB", "%.0f", t.snrDb, 0, 40, t.snrDb >= 20 ? 1 : t.snrDb >= 10 ? 2 : 3});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraft.size(), 0, 30, t.aircraft.empty() ? 0 : 1});
    const double tot = (double)(t.blocksOk + t.blocksBad);
    const double good = tot > 0 ? 100.0 * (double)t.blocksOk / tot : 0.0;
    out.push_back({"BLOCKS OK  %", "%.0f", good, 0, 100, tot == 0 ? 0 : good >= 90 ? 1 : good >= 60 ? 2 : 3});
}

} // namespace

extern const ModeUi kAcarsUi;
const ModeUi kAcarsUi = {
    .sideTitle = "AIRCRAFT",
    .tabName = "Messages",
    .tabIcon = Ic::Doc,
    .tab = tab,
    .receiver = receiver,
    .list = list,
    .status = status,
    .summary = summary,
    .tuner = tuner,
    .decoder = decoder,
    .synth = synth,
    .tick = tick,
    .meters = meters,
};
