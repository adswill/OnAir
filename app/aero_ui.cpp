// Inmarsat Aero screens: the ACARS messages with a detail pane, the aircraft map, the aircraft, the log-ons, the P channels and the signal units.
// What the mode does and does not decode is said on the Channels view and in the Receiver tab.
#include "adsb_map.h"
#include "app.h"
#include "dect2/aero_tel.h"
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
    int sat = 0;                         // 0 = not chosen, else 1 + index into kSats
    double hintSince = -1;               // when the tuner started waiting for a first message with no satellite chosen
    int viewMode = 0;                    // 0 messages, 1 map, 2 aircraft, 3 log-ons, 4 channels, 5 signal units
    char fLabel[8] = "";                 // filter: label starts with this
    char fReg[16] = "";                  // filter: registration, flight or AES address contains this
    bool badToo = false;                 // also show messages whose block check failed
    double selectedTime = -1;
    adsbmap::View map;
    bool mapFollow = true;               // the map fits the aircraft until the user moves it
    uint32_t selAes = 0;                 // the aircraft chosen on the map (AES address, or 0 with selReg)
    std::string selReg;
};
State S;

// Satellites that carry classic Aero P channels. The frequency is the middle of the busiest group of 600 and 1200 bit/s P channels;
// the receiver searches about 2 MHz around it, so the 10500 bit/s channels near 1546 MHz are usually inside too.
// Source: the channel tables of github.com/alphafox02/inmarsat-sniffer (satellites.c), taken from Inmarsat's L-band frequency list.
struct Sat { const char* name; double mhz; const char* tip; };
const Sat kSats[] = {
    {"Alphasat 25E (Europe, Africa)", 1545.12,
     "P channels 1545.115 to 1545.130 MHz (600 and 1200 bit/s, Fucino).\nOther group: 10500 bit/s at 1546.0125 and 1546.0275 MHz."},
    {"Inmarsat-4 F3 98W (Americas)", 1545.10,
     "P channels 1545.020 to 1545.205 MHz (600 and 1200 bit/s, Laurentides and Paumalu); the busiest group.\nOther group: 10500 bit/s at 1546.005 to 1546.0775 MHz."},
    {"Inmarsat-3 F5 54W (Atlantic, South America)", 1545.03,
     "P channels 1545.025 to 1545.040 MHz (600 bit/s, Burum).\nOther group: 10500 bit/s at 1546.055 and 1546.070 MHz."},
    {"Inmarsat-6 F1 83.5E (Indian Ocean, Asia-Pacific)", 1545.19,
     "P channels 1545.160 to 1545.225 MHz (600 bit/s, Perth).\nOther group: 10500 bit/s at 1546.0425 to 1546.1225 MHz."},
    {"Inmarsat-4 F1 143.5E (Pacific, Asia)", 1545.10,
     "No published channel list found for this satellite: this tunes to the middle of the Aero P channel band, and the receiver\nsearches about 1 MHz either side for carriers by itself (community reports put its channels near 1545 MHz)."},
};
constexpr int kNumSats = (int)(sizeof kSats / sizeof kSats[0]);

void loadState() {
    if (S.loaded) return;
    S.loaded = true;
    const long v = plat::prefs().getI("aeroSat", 0);
    S.sat = v >= 1 && v <= kNumSats ? (int)v : 0;
    S.map.zoom = (int)plat::prefs().getI("aeroZoom", 5);
    S.map.online = plat::prefs().getB("adsbMap", true);     // the same switch as the ADS-B map
    if (const char* e = getenv("DECT2_AERO_VIEW")) if (!strcmp(e, "map")) S.viewMode = 1;   // dev: open on the map (screenshots)
}

bool live(const App& a) { return a.engine.running() && a.rx.standard == 19; }

const char* kNotDecoded = "Decoded: the P channels (ground to aircraft) at 600, 1200 and 10500 bit/s, rate found on its own.\n"
                          "Not decoded: C channel voice (not supported by design). R and T channels (aircraft to ground) are not started.";

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

std::string oneLine(const std::string& s) {
    std::string o;
    for (char c : s) o += (c == '\n' || c == '\r' || (unsigned char)c < 32) ? ' ' : c;
    return o;
}

std::string aes(uint32_t id) { char b[12]; snprintf(b, sizeof b, "%06X", id); return b; }

const char* stateName(int s) { return s >= 3 ? "data" : s == 2 ? "frames in sync" : s == 1 ? "carrier locked" : "carrier seen"; }
ImVec4 stateColour(int s) { return s >= 3 ? pal::okGreen() : s == 2 ? pal::warnAmber() : pal::grey(); }

void notDecodedNote() {
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("P channels only: C channel voice is not supported, R and T channels are not started."); ImGui::PopTextWrapPos(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kNotDecoded);
}

// ---------------------------------------------------------------- views

bool passes(const dect2::AeroMessage& m) {
    if (!m.crcOk && !S.badToo) return false;
    if (S.fLabel[0] && lower(m.label).rfind(lower(S.fLabel), 0) != 0) return false;
    if (S.fReg[0] && !contains(m.registration, S.fReg) && !contains(m.flight, S.fReg) && !contains(aes(m.aesId), S.fReg)) return false;
    return true;
}

void filterRow() {
    ImGui::TextDisabled("Label"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(46 * gUi);
    ImGui::InputText("##aol", S.fLabel, sizeof S.fLabel);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Show only blocks whose label starts with this, for example H1 or A6.");
    flowNext(12 * gUi);
    ImGui::TextDisabled("Registration / flight / address"); ImGui::SameLine(0, 5 * gUi);
    ImGui::SetNextItemWidth(110 * gUi);
    ImGui::InputText("##aor", S.fReg, sizeof S.fReg);
    flowNext(12 * gUi);
    ImGui::Checkbox("with failed check", &S.badToo);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Also list messages whose ACARS block check or parity failed.");
    flowEnd();   // the filters wrap in a narrow tab
}

void detailPane(const App& a, const dect2::AeroMessage* m) {
    if (!m) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Click a message to read all of it."); ImGui::PopTextWrapPos(); } return; }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(110 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    kv("time", "%s UTC  %d bit/s channel", clock(m->wallTime).c_str(), m->bitRate);
    kv("aircraft", "AES %s  GES %02X  %s", aes(m->aesId).c_str(), m->gesId < 0 ? 0 : m->gesId, m->registration.empty() ? "-" : m->registration.c_str());
    kv("direction", "%s, mode %s, block id %s%s", m->uplink ? "uplink" : "downlink", m->mode.empty() ? "-" : m->mode.c_str(), m->blockId.empty() ? "-" : m->blockId.c_str(), m->crcOk ? "" : ", block check FAILED");
    kv("label", "%s  %s", m->label.c_str(), m->labelText.empty() ? "(not in the table)" : m->labelText.c_str());
    if (!m->flight.empty() || !m->msgNo.empty()) kv("flight", "%s  message %s", m->flight.c_str(), m->msgNo.c_str());
    if (m->hasPos) kv("position", "%.4f %.4f", m->lat, m->lon);
    if (!m->decoded.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Decoded");
        ImGui::PushFont(a.mono, 0);
        ImGui::TextUnformatted(m->decoded.c_str());
        ImGui::PopFont();
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Text");
    ImGui::BeginChild("##aotext", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::PushFont(a.mono, 0);
    if (m->text.empty()) ImGui::TextDisabled("(no text)"); else ImGui::TextWrapped("%s", m->text.c_str());
    ImGui::PopFont();
    ImGui::EndChild();
}

void messageView(App& a) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    const bool on = live(a);
    filterRow();
    const float H = ImGui::GetContentRegionAvail().y;
    const float listH = std::max(120.f * gUi, H * 0.58f);
    if (ImGui::BeginTable("##aomsg", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, listH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("AES"); ImGui::TableSetupColumn("Reg"); ImGui::TableSetupColumn("Flight");
        ImGui::TableSetupColumn("Label"); ImGui::TableSetupColumn("Meaning"); ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int shown = 0;
        if (on) {
            for (size_t i = t.messages.size(); i-- > 0;) {          // newest on top
                const auto& m = t.messages[i];
                if (!passes(m)) continue;
                shown++;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                char id[24]; snprintf(id, sizeof id, "##m%zu", i);
                if (ImGui::Selectable((clock(m.wallTime) + id).c_str(), S.selectedTime == m.time, ImGuiSelectableFlags_SpanAllColumns)) S.selectedTime = m.time;
                ImGui::TableNextColumn(); ImGui::TextUnformatted(aes(m.aesId).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.registration.c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(m.flight.c_str());
                ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, m.crcOk ? pal::accent() : pal::badRed()); ImGui::TextUnformatted(m.label.c_str()); ImGui::PopStyleColor();
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(m.labelText, ImGui::GetContentRegionAvail().x).c_str());   // whole in the pane below
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ellipsize(oneLine(m.text), ImGui::GetContentRegionAvail().x).c_str());   // a preview: the whole text is shown when it is selected
            }
        }
        ImGui::EndTable();
        if (!on) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "start the receiver to see messages"); ImGui::PopTextWrapPos(); }
        else if (!shown) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(t.messages.empty() ? "no message yet (aircraft messages are rare: a busy channel gives a few a minute)" : "no message matches the filter"); ImGui::PopTextWrapPos(); }
    }
    ImGui::Spacing();
    ImGui::BeginChild("##aodet", ImVec2(0, 0));
    const dect2::AeroMessage* sel = nullptr;
    if (on) for (const auto& m : t.messages) if (m.time == S.selectedTime) sel = &m;
    detailPane(a, sel);
    ImGui::EndChild();
}

// ---------------------------------------------------------------- the aircraft map

const char* kMapNote = "Positions are the aircraft's own reports (ADS-C, text position messages): every few minutes over the satellite, not live like ADS-B.";
const char* kMapTip = "On the L-band P channels the ground stations talk to the aircraft; position reports are aircraft-to-ground (R and T channels, not decoded),\n"
                      "so a real P channel gives few or none. The test signal sends some so the map can be tried.";

std::string nameOf(const dect2::AeroAircraft& c) {
    if (!c.registration.empty() && !c.flight.empty()) return c.registration + " " + c.flight;
    if (!c.registration.empty()) return c.registration;
    if (!c.flight.empty()) return c.flight;
    return aes(c.aesId);
}

bool isSel(const dect2::AeroAircraft& c) {
    if (S.selAes) return c.aesId == S.selAes;
    return !S.selReg.empty() && c.registration == S.selReg;
}

bool sameAircraft(const dect2::AeroMessage& m, const dect2::AeroAircraft& c) {
    if (c.aesId) return m.aesId == c.aesId;
    return !c.registration.empty() && m.registration == c.registration;
}

// ADS-C reports blue, text reports amber; older than half an hour greyed
ImU32 planeColour(const dect2::AeroAircraft& c, double now, float alpha = 1.f) {
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
    const dect2::AeroTelemetry& t = a.rx.aero;
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
    const dect2::AeroAircraft* hit = nullptr; float best = 18 * gUi;
    ImVec2 click; const bool clicked = adsbmap::clickedAt(click);
    int withPos = 0;
    std::vector<ImVec4> placed;              // label boxes already drawn (x0, y0, x1, y1), so labels do not cover each other
    if (on) {
        for (const auto& c : t.aircraft) {   // tracks and predicted routes first, so the planes sit on top
            if (!c.hasPos) continue;
            withPos++;
            const ImU32 col = planeColour(c, t.timeSec, 0.5f);
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
            if (c.hasTrack) drawPlane(dl, q, (float)c.trackDeg, (sel ? 12.f : 10.f) * gUi, planeColour(c, t.timeSec), sel);
            else {
                dl->AddCircleFilled(q, 5 * gUi, planeColour(c, t.timeSec));
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
                dl->AddRectFilled(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), IM_COL32(10, 12, 14, 210), 2.f);
                dl->AddRect(lp, ImVec2(lp.x + ts.x + 8 * gUi, lp.y + ts.y + 4 * gUi), sel ? IM_COL32(255, 255, 255, 200) : IM_COL32(60, 64, 68, 255), 2.f);
                dl->AddText(ImVec2(lp.x + 4 * gUi, lp.y + 2 * gUi), IM_COL32(235, 238, 240, 255), s.c_str());
            }
            const float d = std::hypot(q.x - click.x, q.y - click.y);
            if (clicked && d < best) { best = d; hit = &c; }
        }
    }
    if (clicked) { S.selAes = hit ? hit->aesId : 0; S.selReg = hit ? hit->registration : ""; }
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + 8 * gUi));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.06f, 0.07f, 0.08f, 0.85f));
    if (ImGui::Button("+", ImVec2(26 * gUi, 0))) { S.map.zoom = std::min(12, S.map.zoom + 1); S.mapFollow = false; }
    ImGui::SameLine(0, 2 * gUi);
    if (ImGui::Button("-", ImVec2(26 * gUi, 0))) { S.map.zoom = std::max(2, S.map.zoom - 1); S.mapFollow = false; }
    ImGui::SameLine(0, 6 * gUi);
    if (ImGui::Button("fit")) S.mapFollow = true;
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 10 * gUi);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.8f, 0.82f, 0.84f, 1));
    if (ImGui::Checkbox("online map", &S.map.online)) { plat::prefs().setB("adsbMap", S.map.online); savePrefs(a); }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fetch map tiles from the OpenStreetMap tile server (tile.openstreetmap.org) and keep them in the cache folder.\nOnly tile numbers are sent, never the position itself. Switch off to work offline.");
    plat::prefs().setI("aeroZoom", S.map.zoom);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8 * gUi, p0.y + size.y - ImGui::GetTextLineHeight() - 6 * gUi));
    if (!on) adsbmap::legend(size.x, "start the receiver");
    else if (!withPos) adsbmap::legend(size.x, "no position report yet");
    else adsbmap::legend(size.x, "%d of %zu aircraft placed", withPos, t.aircraft.size());
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + size.y));
}

// the chosen aircraft: its last position and its latest messages; with none chosen, the aircraft that have a position
void mapSide(App& a) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    const bool on = live(a);
    const dect2::AeroAircraft* c = nullptr;
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
            const std::string row = nameOf(x) + "   " + ago(t.timeSec, x.posTime) + " ago" + id;
            if (ImGui::Selectable(row.c_str(), false)) { S.selAes = x.aesId; S.selReg = x.registration; }
        }
        if (!n) { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("(no aircraft has sent a position yet)"); ImGui::PopTextWrapPos(); }
        return;
    }
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(84 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    ImGui::PushStyleColor(ImGuiCol_Text, pal::heading());
    ImGui::TextUnformatted(nameOf(*c).c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton("clear")) { S.selAes = 0; S.selReg.clear(); }
    kv("AES", "%s", c->aesId ? aes(c->aesId).c_str() : "-");
    if (c->hasPos) {
        kv("position", "%.4f %.4f", c->lat, c->lon);
        if (c->hasAlt) kv("altitude", "%d ft", c->altFt); else kv("altitude", "%s", "-");
        if (c->hasTrack) kv("track", "%.0f deg%s", c->trackDeg, c->posSource == 1 && c->hasSpeed ? "" : " (from the last two positions)");
        if (c->hasSpeed) kv("speed", "%.0f kt over the ground", c->speedKt);
        kv("source", "%s", c->posKind.c_str());
        if (c->reportSecPastHour >= 0) { const int s = (int)c->reportSecPastHour; kv("report", "at xx:%02d:%02d (minutes past the hour)", s / 60, s % 60); }
        else if (c->reportSecOfDay >= 0) { const int s = c->reportSecOfDay; kv("report", "at %02d:%02d:%02d UTC", s / 3600, s / 60 % 60, s % 60); }
        kv("received", "%s UTC, %s ago", clock(c->posWall).c_str(), ago(t.timeSec, c->posTime).c_str());
        kv("reports", "%llu, track of %zu points", (unsigned long long)c->positions, c->track.size());
    } else {
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("no position from this aircraft yet"); ImGui::PopTextWrapPos(); }
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Latest messages");
    ImGui::BeginChild("##aomapmsg", ImVec2(0, 0), ImGuiChildFlags_Borders);
    int shown = 0;
    for (size_t i = t.messages.size(); i-- > 0 && shown < 10;) {
        const auto& m = t.messages[i];
        if (!sameAircraft(m, *c)) continue;
        shown++;
        ImGui::PushStyleColor(ImGuiCol_Text, m.crcOk ? pal::accent() : pal::badRed());
        ImGui::Text("%s  %s %s", clock(m.wallTime).c_str(), m.label.c_str(), m.uplink ? "up" : "down");
        ImGui::PopStyleColor();
        ImGui::PushFont(a.mono, 0);
        std::string body = m.decoded.empty() ? oneLine(m.text) : m.decoded.substr(0, m.decoded.find('\n'));
        if (m.hasPos && !m.decoded.empty()) {
            const size_t a1 = m.decoded.find('\n');
            if (a1 != std::string::npos) body = m.decoded.substr(a1 + 3, m.decoded.find('\n', a1 + 1) - a1 - 3);
        }
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
    ImGui::BeginChild("##aomapside", ImVec2(sideW, size.y));
    mapSide(a);
    ImGui::EndChild();
}

void aircraftView(const App& a) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    const bool on = live(a);
    if (!ImGui::BeginTable("##aoair", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("AES address"); ImGui::TableSetupColumn("Registration"); ImGui::TableSetupColumn("Flight"); ImGui::TableSetupColumn("Messages");
    ImGui::TableSetupColumn("Last label"); ImGui::TableSetupColumn("Log-on"); ImGui::TableSetupColumn("Last heard");
    ImGui::TableSetupColumn("Position"); ImGui::TableSetupColumn("Alt ft");
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto& c : t.aircraft) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.aesId ? aes(c.aesId).c_str() : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.registration.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.flight.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.messages);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.lastLabel.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(c.loggedOn ? "logged on" : "-");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(ago(t.timeSec, c.lastHeard).c_str());
            ImGui::TableNextColumn(); if (c.hasPos) { ImGui::PushTextWrapPos(0); ImGui::Text("%.3f %.3f (%s ago)", c.lat, c.lon, ago(t.timeSec, c.posTime).c_str()); ImGui::PopTextWrapPos(); } else ImGui::TextUnformatted("-");
            ImGui::TableNextColumn(); if (c.hasPos && c.hasAlt) ImGui::Text("%d", c.altFt); else ImGui::TextUnformatted("-");
        }
    }
    ImGui::EndTable();
}

void logonView(const App& a) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    const bool on = live(a);
    if (!ImGui::BeginTable("##aolog", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Time UTC"); ImGui::TableSetupColumn("Event"); ImGui::TableSetupColumn("AES address"); ImGui::TableSetupColumn("GES"); ImGui::TableSetupColumn("Channel");
    ImGui::TableHeadersRow();
    if (on) {
        for (size_t i = t.logons.size(); i-- > 0;) {
            const auto& l = t.logons[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(clock(l.wallTime).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(l.logon ? "log-on" : "log-off");
            ImGui::TableNextColumn(); ImGui::TextUnformatted(aes(l.aesId).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%02X", l.gesId < 0 ? 0 : l.gesId);
            ImGui::TableNextColumn(); ImGui::Text("%d", l.channel + 1);
        }
    }
    ImGui::EndTable();
}

void channelView(const App& a) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    const bool on = live(a);
    notDecodedNote();
    ImGui::Spacing();
    if (!on || t.channels.empty()) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", on ? "no carrier seen yet: the band search needs a few seconds" : a.engine.running() ? "starting" : "start the receiver to see the channels"); ImGui::PopTextWrapPos(); } return; }
    if (!ImGui::BeginTable("##aoch", 10, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("MHz"); ImGui::TableSetupColumn("State"); ImGui::TableSetupColumn("bit/s");
    ImGui::TableSetupColumn("Level dB"); ImGui::TableSetupColumn("Eb/N0 dB"); ImGui::TableSetupColumn("Frames"); ImGui::TableSetupColumn("UW miss");
    ImGui::TableSetupColumn("SUs ok / bad"); ImGui::TableSetupColumn("Raw BER");
    ImGui::TableHeadersRow();
    int n = 0;
    for (const auto& c : t.channels) {
        n++;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%d", n);
        ImGui::TableNextColumn(); ImGui::Text("%.4f", a.freqMhz + c.offsetHz / 1e6);
        ImGui::TableNextColumn(); ImGui::PushStyleColor(ImGuiCol_Text, stateColour(c.state)); ImGui::TextUnformatted(stateName(c.state)); ImGui::PopStyleColor();
        ImGui::TableNextColumn(); if (c.bitRate) ImGui::Text("%d", c.bitRate); else ImGui::TextDisabled("trying");
        ImGui::TableNextColumn(); ImGui::Text("%.0f", c.levelDb);
        ImGui::TableNextColumn(); if (c.state >= 1) ImGui::Text("%.1f", c.ebn0Db); else ImGui::TextUnformatted("-");
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.frames);
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.uwMisses);
        ImGui::TableNextColumn(); ImGui::Text("%llu / %llu", (unsigned long long)c.susOk, (unsigned long long)c.susBad);
        ImGui::TableNextColumn(); if (c.frames) ImGui::Text("%.3f", c.channelBer); else ImGui::TextUnformatted("-");
    }
    ImGui::EndTable();
}

void suView(const App& a) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    const bool on = live(a);
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Signal units seen, by type (SUs of 12 bytes; the ones with a failed check are counted as \"Bad CRC\")."); ImGui::PopTextWrapPos(); }
    if (!ImGui::BeginTable("##aosu", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_BordersInnerV, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Type"); ImGui::TableSetupColumn("Count"); ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    if (on) {
        for (const auto& s : t.suTypes) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("0x%02X", s.type);
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)s.count);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name.c_str());
        }
    }
    ImGui::EndTable();
}

// ---------------------------------------------------------------- hooks

void tab(App& a) {
    loadState();
    subNav("aerov", S.viewMode, {"Messages", "Aircraft map", "Aircraft", "Log-ons", "Channels", "Signal units"});
    if (S.viewMode == 0) messageView(a);
    else if (S.viewMode == 1) mapView(a);
    else if (S.viewMode == 2) aircraftView(a);
    else if (S.viewMode == 3) logonView(a);
    else if (S.viewMode == 4) channelView(a);
    else suView(a);
}

void list(App& a) {
    if (!live(a)) { { ImGui::PushTextWrapPos(0); ImGui::TextDisabled(a.engine.running() ? "starting" : "start the receiver to see aircraft"); ImGui::PopTextWrapPos(); } return; }
    const dect2::AeroTelemetry& t = a.rx.aero;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%zu aircraft, %zu messages", t.aircraft.size(), t.messages.size()); ImGui::PopTextWrapPos(); }
    if (!ImGui::BeginTable("##aol_s", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("AES"); ImGui::TableSetupColumn("Reg"); ImGui::TableSetupColumn("Msgs");
    ImGui::TableHeadersRow();
    for (const auto& c : t.aircraft) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.aesId ? aes(c.aesId).c_str() : "-");
        ImGui::TableNextColumn(); ImGui::TextUnformatted(c.registration.c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)c.messages);
    }
    ImGui::EndTable();
}

void receiver(App& a) {
    if (!live(a)) { ImGui::TextDisabled("%s", a.engine.running() ? "starting" : "stopped"); return; }
    const dect2::AeroTelemetry& t = a.rx.aero;
    auto kv = [&](const char* k, const char* fmt, auto... v) { ImGui::TextDisabled("%s", k); kvColumn(130 * gUi); ImGui::PushFont(a.mono, 0); ImGui::PushTextWrapPos(0); ImGui::Text(fmt, v...); ImGui::PopTextWrapPos(); ImGui::PopFont(); };
    int sync = 0;
    for (const auto& c : t.channels) sync += c.state >= 2 ? 1 : 0;
    kv("tuned to", "%.4f MHz, %zu carriers seen, %d in sync", a.freqMhz, t.channels.size(), sync);
    kv("frames", "%llu decoded", (unsigned long long)t.frames);
    kv("signal units", "%llu good, %llu failed the check", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad);
    kv("messages", "%llu ACARS, %llu log-on events, %llu with a position", (unsigned long long)t.messagesTotal, (unsigned long long)t.logonsTotal,
       (unsigned long long)t.positionsTotal);
    kv("strongest", "Eb/N0 %.1f dB, %+.0f Hz from the tuned frequency", t.snrDb, t.cfoHz);
    kv("signal time", "%.1f s", t.timeSec);
    ImGui::Spacing();
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Decoded: P channels at 600, 1200 and 10500 bit/s (rate found on its own)."); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Not supported: C channel voice."); ImGui::PopTextWrapPos(); }
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Not started: R and T channels (aircraft to ground)."); ImGui::PopTextWrapPos(); }
}

void status(App& a) {
    const bool run = a.engine.running(), on = live(a);
    const dect2::AeroTelemetry& t = a.rx.aero;
    const SignalStats& st = a.spec.stats;
    const AdcStatus adc = classifyAdc(st.rmsDbfs, st.peak, st.clipFraction);
    StatusPanel panel;   // a tinted panel behind the status lines (they wrap in a narrow window)
    int sync = 0;
    for (const auto& c : t.channels) sync += c.state >= 2 ? 1 : 0;
    lamp("IQ", run ? (adc == AdcStatus::Overload ? 3 : (adc == AdcStatus::Good ? 1 : 2)) : 0, (int)Ic::Wave); flowNext(12 * gUi);
    lamp("Carrier", !on ? 0 : t.channels.empty() ? 0 : 1); flowNext(12 * gUi);
    lamp("Frames", !on ? 0 : t.state == 2 ? 1 : sync > 0 ? 2 : 0); flowNext(12 * gUi);
    lamp("Messages", !on ? 0 : t.messagesTotal > 0 ? 1 : t.blocksOk > 0 ? 2 : 0); flowNext(10 * gUi);
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
    snprintf(b, sizeof b, "%d", sync); ro("Channels", b);
    snprintf(b, sizeof b, "%llu / %llu", (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad); ro("SUs good / bad", b);
    snprintf(b, sizeof b, "%zu", t.aircraft.size()); ro("Aircraft", b);
    ro("Scope", "P channels");
}

void summary(const App& a, std::string& l1, std::string& l2) {
    l1 = "Inmarsat Aero";
    if (live(a)) l2 = dect2::aeroSummary(a.rx.aero);
}

void decoder(App& a, bool&) {
    (void)a;
    ImGui::TextDisabled("P CHANNELS ONLY");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", kNotDecoded);
}

void synth(App& a, bool& changed) {
    SynthConfig& sc = a.tune.synth;
    { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("simulated P channels"); ImGui::PopTextWrapPos(); }
    int mask = sc.modeOpt[0] > 0 ? sc.modeOpt[0] : 6;
    bool m600 = mask & 1, m1200 = mask & 2, m10500 = mask & 4;
    bool ch = false;
    flowNext(); ch |= ImGui::Checkbox("600", &m600);
    flowNext(); ch |= ImGui::Checkbox("1200", &m1200);
    flowNext(); ch |= ImGui::Checkbox("10500", &m10500);
    if (ch) {
        int nm = (m600 ? 1 : 0) | (m1200 ? 2 : 0) | (m10500 ? 4 : 0);
        if (!nm) nm = 6;
        sc.modeOpt[0] = nm; changed = true;
    }
    flowNext(); ImGui::TextDisabled("Eb/N0"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float eb = sc.modeVal[0] != 0 ? (float)sc.modeVal[0] : 12.f;
    if (ImGui::SliderFloat("##aoeb", &eb, 2.f, 30.f, "%.0f dB")) { sc.modeVal[0] = eb; changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Signal to noise per channel bit of each simulated channel.");
    flowNext(); ImGui::TextDisabled("drift"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float dr = (float)sc.modeVal[1];
    if (ImGui::SliderFloat("##aodr", &dr, -5.f, 5.f, "%.1f Hz/s")) { sc.modeVal[1] = dr; changed = true; }
    flowNext(); ImGui::TextDisabled("CFO"); ImGui::SameLine(); ImGui::SetNextItemWidth(90 * gUi);
    float cfo = (float)(sc.cfoHz / 1e3);
    if (ImGui::SliderFloat("##aocfo", &cfo, -3, 3, "%.2f kHz")) { sc.cfoHz = cfo * 1e3; changed = true; }
}

void tuner(App& a, bool& retune) {
    loadState();
    const dect2::AeroTelemetry& t = a.rx.aero;
    ImGui::TextDisabled("Satellite"); ImGui::SameLine(0, 6 * gUi);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo("##aosat", S.sat ? kSats[S.sat - 1].name : "Not chosen (tune by hand)")) {
        if (ImGui::Selectable("Not chosen (tune by hand)", S.sat == 0)) { S.sat = 0; plat::prefs().setI("aeroSat", 0); savePrefs(a); }
        for (int i = 0; i < kNumSats; i++) {
            if (ImGui::Selectable(kSats[i].name, S.sat == i + 1)) {
                S.sat = i + 1; plat::prefs().setI("aeroSat", i + 1); savePrefs(a);
                a.freqMhz = kSats[i].mhz; retune = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Tunes to %.3f MHz.\n%s", kSats[i].mhz, kSats[i].tip);
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Optional. Choosing a satellite tunes to its P channels; you can still tune by hand.");
    // the hint only when nothing has come in for a while and no satellite is chosen
    const bool idle = !live(a) || t.messagesTotal > 0 || t.blocksOk > 0;
    if (S.sat != 0 || idle) S.hintSince = -1;
    else if (S.hintSince < 0) S.hintSince = ImGui::GetTime();
    if (S.hintSince >= 0 && ImGui::GetTime() - S.hintSince > 20.0)
        { ImGui::PushTextWrapPos(0); ImGui::TextDisabled("Tip: pick your satellite above so the receiver starts on its channels."); ImGui::PopTextWrapPos(); }
}

void meters(const App& a, std::vector<ModeMeter>& out) {
    const dect2::AeroTelemetry& t = a.rx.aero;
    int sync = 0;
    for (const auto& c : t.channels) sync += c.state >= 2 ? 1 : 0;
    out.push_back({"CHANNELS", "%.0f", (double)sync, 0, 6, sync > 0 ? 1 : 0});
    out.push_back({"Eb/N0  dB", "%.0f", t.snrDb, 0, 30, t.snrDb >= 10 ? 1 : t.snrDb >= 6 ? 2 : 3});
    out.push_back({"AIRCRAFT", "%.0f", (double)t.aircraft.size(), 0, 30, t.aircraft.empty() ? 0 : 1});
    const double tot = (double)(t.blocksOk + t.blocksBad);
    const double good = tot > 0 ? 100.0 * (double)t.blocksOk / tot : 0.0;
    out.push_back({"SUs OK  %", "%.0f", good, 0, 100, tot == 0 ? 0 : good >= 90 ? 1 : good >= 60 ? 2 : 3});
}

} // namespace

extern const ModeUi kAeroUi;
const ModeUi kAeroUi = {
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
    .meters = meters,
};
