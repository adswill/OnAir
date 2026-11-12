// Tools > Propagation, Advanced mode: every input of the model (transmitter, receiver chain and SDR, path, ionosphere, atmosphere, terrain),
// the link budget term by term with an explanation on each, sweeps (signal vs distance, range vs power and frequency, the HF MUF over the
// day), CSV export and a second scenario to compare. See prop_ui.cpp for the window, the worker and the map.
#include "prop_internal.h"
#include "../gfx/plot.h"
#include "dect2/common/u8file.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace propui {

namespace {

const char* kPol = "vertical\0horizontal\0circular (right)\0circular (left)\0";

void section(const char* title, bool open, void (*body)(State&, pr::Scenario&)) {
    if (ImGui::CollapsingHeader(title, open ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::PushID(title);
        body(st(), cur());
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void txSection(State& S, pr::Scenario& s) {
    placeRowUi();
    S.labelCol = 150 * gUi;
    const auto& ps = pr::presets();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Preset"); ImGui::SameLine(S.labelCol);
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##advpreset", ps[s.preset].name)) {
        for (int i = 0; i < (int)ps.size(); i++)
            if (ImGui::Selectable(ps[i].name, i == s.preset)) pr::applyPreset(s, i);
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", ps[s.preset].note);
    numField("Power", &s.power, nullptr, "Power into the antenna cable.", 80 * gUi);
    ImGui::SameLine();
    int u = (int)s.powerUnit;
    ImGui::SetNextItemWidth(60 * gUi);
    if (ImGui::Combo("##advpu", &u, "W\0dBW\0dBm\0kW\0")) s.powerUnit = (pr::PowerUnit)u;
    numField("Antenna gain", &s.txGainDbi, "dBi", "Gain toward the horizon over an isotropic antenna: dipole 2.15, 5/8 vertical about 3, small yagi 7 to 10. "
             "Ignored when a pattern file is loaded below.");
    numField("Feeder loss", &s.txFeederDb, "dB", "Cable and connectors between the transmitter and the antenna (e.g. 20 m of RG-58 at 145 MHz: about 4 dB).");
    numField("Height above ground", &s.txHeightM, "m", "Antenna height above the ground under it (with terrain: above the terrain at the transmitter).");
    int pol = (int)s.txPol;
    enumCombo("Polarisation", &pol, kPol, "Vertical for most mobile and FM, horizontal for TV and HF dipoles. A mismatch with the receive antenna costs 3 to 20 dB.");
    s.txPol = (pr::Pol)pol;
    const int i = S.advanced && S.compare ? S.editing : 0;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Pattern file"); ImGui::SameLine(S.labelCol);
    if (ImGui::SmallButton("Load...")) {
        const std::string f = openFileDialog();
        if (!f.empty()) { s.txPatternPath = f; loadPatternFor(i); }
    }
    ImGui::SetItemTooltip("%s", "An antenna pattern (CSV lines \"azimuth_deg,gain_dbi\", optional \"el,elevation_deg,relative_db\"), e.g. exported by the antenna designer. "
                                "It replaces the gain above, by bearing from the boresight.");
    if (!s.txPatternPath.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##pat")) { s.txPatternPath.clear(); loadPatternFor(i); }
        if (S.pattern[i]) {
            ImGui::SameLine(); ImGui::TextDisabled("%s", S.pattern[i]->name.c_str());
            numField("Boresight azimuth", &s.txAzDeg, "deg", "Where the pattern's 0 degrees points (0 north, 90 east).");
        } else ImGui::TextColored(pal::warnAmber(), "%s", S.patternMsg[i].c_str());
    }
    const double eirp = pr::powerDbw(s.power, s.powerUnit) + s.txGainDbi - s.txFeederDb;
    ImGui::TextDisabled("EIRP %.1f dBW (%.1f dBm, %.3g W); ERP %.3g W", eirp, eirp + 30, std::pow(10.0, eirp / 10), std::pow(10.0, (eirp - 2.15) / 10));
}

void signalSection(State& S, pr::Scenario& s) {
    S.labelCol = 150 * gUi;
    numField("Frequency", &s.fMHz, "MHz", "Below 30 MHz the HF model (ground wave + skywave), from 30 MHz the VHF/UHF model.");
    // 4.0: "will mode X decode here?" from OnAir's measured thresholds
    const pr::ModeSnr* cm = s.decodeMode.empty() ? nullptr : pr::findModeThreshold(s.decodeMode);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Will it decode?"); ImGui::SameLine(S.labelCol);
    ImGui::SetNextItemWidth(210 * gUi);
    if (ImGui::BeginCombo("##advdmode", cm ? cm->name : "(type the SNR below)")) {
        if (ImGui::Selectable("(type the SNR below)", !cm)) s.decodeMode.clear();
        for (const pr::ModeSnr& m : pr::modeThresholds())
            if (ImGui::Selectable(m.name, cm == &m)) s.decodeMode = m.id;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", "Pick an OnAir mode: the SNR it needs (measured by OnAir's own sensitivity sweep, docs/SENSITIVITY.md) and its bandwidth "
                                "replace the two fields below, so the map and the margin answer 'will this mode decode here?'.");
    if (cm) {
        enumCombo("Channel", &s.decodeCondition, "clean (white noise)\0real radio (offsets, echo, phase noise)\0mobile fading (TU6)\0",
                  "Which measured threshold to use: white noise only, the 'combined' set of real-radio impairments, or Rayleigh fading "
                  "(a car or a handheld on the move). A mode without a figure for that case falls back to the one before it.");
        ImGui::TextDisabled("%s needs %.0f dB SNR in %.4g kHz (measured on generated signals).", cm->name, pr::modeRequiredSnrDb(*cm, s.decodeCondition), cm->bwHz / 1e3);
    } else {
        numField("Bandwidth", &s.bandwidthHz, "Hz", "The receiver's noise bandwidth: 2400 SSB, 12500 narrow FM, 200000 broadcast FM, 125000 LoRa, 7.6e6 DVB-T.", 100 * gUi, "%.6g");
        numField("Required SNR", &s.requiredSnrDb, "dB", "SNR in that bandwidth at which the mode just works: narrow FM 12, SSB 10, DAB 10, DVB-T 64QAM about 20, FT8 -21, LoRa SF12 -20.");
    }
}

void statsSection(State& S, pr::Scenario& s) {
    S.labelCol = 150 * gUi;
    if (s.fMHz < 30) ImGui::TextDisabled("Used from 30 MHz (ITU-R P.1812-8, the Auto path model).");
    numField("Time", &s.timePct, "%", "ITU-R P.1812: the path loss is not exceeded for this % of the time (1 to 50). 50 = the median, normal conditions. "
             "1 to 10 = the strong signals of super-refraction and ducting (DX, and the interference view).", 70 * gUi);
    numField("Locations", &s.locPct, "%", "... and at this % of locations around each point (1 to 99). 50 = the median; coverage planning uses 90 or 95 "
             "(a safe answer: the signal is at least this strong at nine places in ten).", 70 * gUi);
    numField("Location spread", &s.sigmaLocDb, "dB", "Standard deviation of the location variability (P.1812 Table 6: 5.5 dB for broadcasting; "
             "8 to 10 dB for a handheld among buildings).", 70 * gUi);
    struct Cl { const char* name; double dn, n0; };
    static const Cl cl[] = {{"temperate maritime", 40, 315}, {"temperate continental", 45, 320}, {"Mediterranean", 50, 335}, {"hot desert / Gulf coast", 60, 360},
                            {"humid tropics", 70, 380}};
    int sel = -1;
    for (int i = 0; i < 5; i++) if (std::fabs(s.deltaN - cl[i].dn) < 1e-9 && std::fabs(s.n0 - cl[i].n0) < 1e-9) sel = i;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Climate"); ImGui::SameLine(S.labelCol);
    ImGui::SetNextItemWidth(190 * gUi);
    if (ImGui::BeginCombo("##advclim", sel >= 0 ? cl[sel].name : "custom")) {
        for (int i = 0; i < 5; i++) if (ImGui::Selectable(cl[i].name, i == sel)) { s.deltaN = cl[i].dn; s.n0 = cl[i].n0; }
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", "Typical refractivity for the region (rounded readings of the ITU-R P.453 maps). Over warm seas such as the Gulf the lapse rate "
                                "is larger: the radio horizon is farther and ducting is common in summer.");
    numField("Refractivity lapse", &s.deltaN, "N/km", "dN, the average refractivity drop over the lowest 1 km (P.453 map DN50). It sets the median effective "
             "earth radius k = 157 / (157 - dN): 40 gives 1.34 (about 4/3).", 70 * gUi);
    ImGui::SameLine(); ImGui::TextDisabled("k = %.2f", 157.0 / (157.0 - std::clamp(s.deltaN, 0.0, 150.0)));
    numField("Sea-level N0", &s.n0, "N", "Sea-level surface refractivity (P.453 map N050), used by the troposcatter term: 300 dry to 400 humid.", 70 * gUi);
}

void rxSection(State& S, pr::Scenario& s) {
    S.labelCol = 150 * gUi;
    numField("Antenna gain", &s.rxGainDbi, "dBi", "Receive antenna gain toward the transmitter (a rubber duck is about -5 to 0, a discone 0 to 2, a yagi 7 to 15).");
    numField("Height above ground", &s.rxHeightM, "m", "A handheld is about 1.5 m, a car 1.5 to 2 m, a roof aerial 10 m.");
    numField("Feeder loss", &s.rxFeederDb, "dB", "Cable loss between the antenna and the radio (or the preamplifier and the radio). It adds to the noise figure.");
    int pol = (int)s.rxPol;
    enumCombo("Polarisation", &pol, kPol, "The receive antenna's polarisation.");
    s.rxPol = (pr::Pol)pol;
    const auto& rs = pr::radios();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Radio"); ImGui::SameLine(S.labelCol);
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##advradio", rs[s.radio].name)) {
        for (int i = 0; i < (int)rs.size(); i++)
            if (ImGui::Selectable(rs[i].name, i == s.radio)) s.radio = i;
        ImGui::EndCombo();
    }
    if (*rs[s.radio].note) ImGui::SetItemTooltip("%s", rs[s.radio].note);
    if (s.radio == 0) numField("Noise figure", &s.rxNfDb, "dB", "The receiver's own noise figure (an SDR 4 to 8, a good preamplifier 0.5 to 1).");
    else {
        ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Gain setting"); ImGui::SameLine(S.labelCol);
        ImGui::SetNextItemWidth(140 * gUi);
        float g = (float)s.radioGainPct;
        if (ImGui::SliderFloat("##advgain", &g, 0, 100, "%.0f %%")) s.radioGainPct = g;
        ImGui::SetItemTooltip("%s", "The radio's gain, as a share of its range. Low gain costs noise figure (roughly +16 dB at 0 %); too much overloads on strong signals.");
        ImGui::SameLine();
        ImGui::TextDisabled("NF %.1f dB", pr::radioNfDb(s, s.fMHz));
    }
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Preamplifier"); ImGui::SameLine(S.labelCol);
    ImGui::Checkbox("at the antenna##advpre", &s.preamp);
    ImGui::SetItemTooltip("%s", "A low-noise amplifier at the antenna, before the feeder: it hides the feeder loss and the radio's noise figure (Friis).");
    if (s.preamp) {
        numField("  gain", &s.preampGainDb, "dB", "Preamplifier gain (typically 15 to 25 dB).", 70 * gUi);
        numField("  noise figure", &s.preampNfDb, "dB", "Preamplifier noise figure (typically 0.5 to 1.5 dB).", 70 * gUi);
    }
    const double nf = pr::cascadeNfDb(s.rxFeederDb, pr::radioNfDb(s, s.fMHz), s.preamp, s.preampGainDb, s.preampNfDb);
    const double floorR = pr::ktbDbm(std::max(s.bandwidthHz, 1.0)) + nf;
    ImGui::TextDisabled("Chain noise figure %.1f dB. Radio alone: noise %.1f dBm, sensitivity %.1f dBm (%.3g uV) for %.0f dB SNR.", nf, floorR,
                        floorR + s.requiredSnrDb, pr::dbmToUv50(floorR + s.requiredSnrDb), s.requiredSnrDb);
    helpMark("Sensitivity = kTB + noise figure + required SNR, before the outside noise. The link budget adds the man-made, galactic and atmospheric noise at the antenna.");
}

void pathSection(State& S, pr::Scenario& s) {
    S.labelCol = 150 * gUi;
    int pm = (int)s.pathModel, hm = (int)s.hfModel, gr = (int)s.ground, cl = (int)s.rxClutter, en = (int)s.env;
    if (s.fMHz >= 30)
        enumCombo("Path model", &pm, "auto: ITU-R P.1812-8 (recommended)\0free space only\0smooth earth only (P.526)\0two-ray exact\0the 3.9 model\0",
                  "Auto: ITU-R P.1812-8 over the terrain profile (flat sea level when no terrain is loaded), with the time and location "
                  "percentages under 'Statistics and climate': line of sight, diffraction, troposcatter and ducting. Free space: the upper "
                  "bound. Smooth earth: P.526 alone. Two-ray exact: with the reflection's peaks and nulls. The 3.9 model: for comparison.");
    else
        enumCombo("HF modes", &hm, "ground wave + skywave\0ground wave only\0skywave only\0", "Which HF propagation modes are added up.");
    enumCombo("Ground", &gr, "sea\0wet ground\0average land\0dry ground / city\0",
              "Ground constants (sea 5 S/m, wet 10 mS/m, average 5 mS/m, dry 1 mS/m): they decide the HF ground wave and, a little, VHF diffraction.");
    enumCombo("Around the receiver", &cl, "open\0suburban\0urban\0dense urban\0forest\0",
              "ITU-R P.2108 clutter class: the loss of a receive antenna below the roofs or trees around it (none when it is above them: 10 to 20 m).");
    enumCombo("Noise there", &en, "quiet rural\0rural\0residential\0city\0",
              "ITU-R P.372 man-made noise. It dominates HF and low VHF: a city is about 20 dB noisier than quiet countryside.");
    s.pathModel = (pr::PathModel)pm; s.hfModel = (pr::HfModel)hm; s.ground = (pr::Ground)gr; s.rxClutter = (pr::Clutter)cl; s.env = (pr::Env)en;
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Atmospheric noise"); ImGui::SameLine(S.labelCol);
    ImGui::Checkbox("lightning (HF)##advatm", &s.atmosphericNoise);
    ImGui::SetItemTooltip("%s", "Noise from thunderstorms, carried by the ionosphere: strong at night on the low HF bands, in summer and in the tropics (rough P.372 fit).");
}

void ionoSection(State& S, pr::Scenario& s) {
    S.labelCol = 150 * gUi;
    if (s.fMHz >= 30) ImGui::TextDisabled("Used below 30 MHz (and for the atmospheric noise).");
    numField("Solar flux (SFI)", &s.sfi, "sfu", "The 10.7 cm solar flux index, 65 (quiet sun) to 300 (very active). Type today's value from any space-weather source: nothing is downloaded.");
    numField("Sunspot number", &s.r12, "R12", "The 12-month smoothed sunspot number the ITU-R P.1239 ionospheric maps are indexed by (0 to 160; "
             "higher counts as 160 for foF2). -1: worked out from the SFI above (ITU-R P.371).", 70 * gUi);
    if (s.r12 < 0) { ImGui::SameLine(); ImGui::TextDisabled("= %.0f from the SFI", pr::hf::r12FromSfi(s.sfi)); }
    numField("K index", &s.kIndex, "", "Planetary K index 0 to 9. Used only by the 3.9 HF model (the P.533 maps are monthly medians, not storm-time).", 60 * gUi);
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("HF skywave model"); ImGui::SameLine(S.labelCol);
    ImGui::Checkbox("use the 3.9 estimate##advhfleg", &s.hfLegacy);
    ImGui::SetItemTooltip("%s", "Off (default): ITU-R P.533-14 with the CCIR ionospheric maps of P.1239. On: the simple one-layer estimate of 3.9, for comparison.");
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Date and time"); ImGui::SameLine(S.labelCol);
    ImGui::Checkbox("now (UTC)##advnow", &s.useNow);
    if (s.useNow) { ImGui::SameLine(); ImGui::TextDisabled("%04d-%02d-%02d %02d:%02d", s.year, s.month, s.day, s.hour, s.minute); }
    else {
        ImGui::SetCursorPosX(S.labelCol);
        ImGui::SetNextItemWidth(56 * gUi); ImGui::InputInt("##advy", &s.year, 0, 0); ImGui::SameLine();
        ImGui::SetNextItemWidth(34 * gUi); ImGui::InputInt("##advmo", &s.month, 0, 0); ImGui::SameLine();
        ImGui::SetNextItemWidth(34 * gUi); ImGui::InputInt("##advd", &s.day, 0, 0); ImGui::SameLine();
        ImGui::SetNextItemWidth(34 * gUi); ImGui::InputInt("##advh", &s.hour, 0, 0); ImGui::SameLine();
        ImGui::SetNextItemWidth(34 * gUi); ImGui::InputInt("##advmi", &s.minute, 0, 0);
        ImGui::SameLine(); ImGui::TextDisabled("y m d h min");
        s.year = std::clamp(s.year, 1950, 2100);
    }
    const double c = pr::solarCosZenith(s.txLat, s.txLon, s.year, s.month, s.day, s.hour + s.minute / 60.0);
    const bool north = s.txLat >= 0;
    const int m = s.month;
    const char* season = (m >= 5 && m <= 8) == north ? "summer" : (m == 11 || m == 12 || m <= 2) == north ? "winter" : "equinox season";
    ImGui::TextDisabled("At the transmitter: %s, sun %.0f deg %s the horizon; %s.", c > 0.1 ? "day" : c < -0.1 ? "night" : "dawn or dusk",
                        std::fabs(std::asin(std::clamp(c, -1.0, 1.0)) * 57.29578), c >= 0 ? "above" : "below", season);
}

void atmoSection(State& S, pr::Scenario& s) {
    S.labelCol = 150 * gUi;
    numField("Rain rate", &s.rainMmH, "mm/h", "Rain along the path (P.838): it matters above about 5 GHz. 2.5 light, 12.5 moderate, 25 heavy, 50 very heavy, 100 tropical.", 70 * gUi);
    ImGui::SameLine();
    static const double rr[] = {0, 2.5, 12.5, 25, 50, 100};
    int sel = -1;
    for (int i = 0; i < 6; i++) if (std::fabs(s.rainMmH - rr[i]) < 1e-9) sel = i;
    ImGui::SetNextItemWidth(100 * gUi);
    if (ImGui::Combo("##advrain", &sel, "none\0light\0moderate\0heavy\0very heavy\0tropical\0") && sel >= 0) s.rainMmH = rr[sel];
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Gases"); ImGui::SameLine(S.labelCol);
    ImGui::Checkbox("oxygen and water vapour##advgas", &s.gas);
    ImGui::SetItemTooltip("%s", "ITU-R P.676-13 line-by-line absorption of oxygen and water vapour at sea level (15 C, 7.5 g/m3): 0.02 dB/km at 12 GHz, "
                                "0.2 dB/km at 22 GHz, 15 dB/km at 60 GHz.");
}

void startTileDownload(State& S, const pr::Scenario& s) {
    if (S.dlBusy) return;
    if (S.dlTh.joinable()) S.dlTh.join();
    double la = NAN, lo = NAN; std::string lab;
    receiverPoint(la, lo, lab);
    const auto tiles = pr::demTilesFor(s.txLat, s.txLon, S.dlRadiusKm, la, lo, 9);
    S.dlBusy = true;
    { std::lock_guard<std::mutex> lk(S.dlMu); S.dlMsg = tiles.empty() ? "no tiles here (SRTM covers 60 S to 60 N)" : "downloading..."; }
    S.dlTh = std::thread([tiles] {
        State& T = st();
        namespace fs = std::filesystem;
        const std::string dir = plat::dataDir() + "/terrain";
        std::error_code ec;
        fs::create_directories(fs::u8path(dir), ec);
        int ok = 0, have = 0, bad = 0;
        std::vector<std::string> files;
        for (const auto& t : tiles) {
            const std::string f = dir + "/" + pr::demTileName(t.first, t.second) + ".hgt.gz";
            if (fs::exists(fs::u8path(f), ec) && fs::file_size(fs::u8path(f), ec) > 1000) { files.push_back(f); have++; continue; }
            { std::lock_guard<std::mutex> lk(T.dlMu); T.dlMsg = "downloading " + pr::demTileName(t.first, t.second) + "..."; }
            if (plat::fetchUrl(pr::demTileUrl(t.first, t.second), f, "OnAir")) { files.push_back(f); ok++; }
            else { fs::remove(fs::u8path(f), ec); bad++; }
        }
        char b[200];
        std::snprintf(b, sizeof b, "%d tile(s) downloaded, %d already here%s", ok, have,
                      bad ? "; some did not download (offline, too slow, or open sea): try again, or add files yourself" : "");
        std::lock_guard<std::mutex> lk(T.dlMu);
        T.dlMsg = b;
        T.dlDone = files;
        T.dlBusy = false;
    });
}

void terrainSection(State& S, pr::Scenario& s) {
    {   // tiles finished by the download thread: add them
        std::vector<std::string> done;
        { std::lock_guard<std::mutex> lk(S.dlMu); done.swap(S.dlDone); }
        bool add = false;
        for (const auto& f : done)
            if (std::find(S.terrainFiles.begin(), S.terrainFiles.end(), f) == S.terrainFiles.end()) { S.terrainFiles.push_back(f); add = true; }
        if (add) reloadTerrain();
    }
    ImGui::TextWrapped("Without terrain every path is a flat sea-level profile, so hills are ignored. Download elevation tiles (opt-in) or add files "
                       "you have: SRTM .hgt tiles (keep their names, like N54E025.hgt), downloaded .hgt.gz tiles, or an uncompressed GeoTIFF in "
                       "WGS 84 latitude / longitude.");
    S.labelCol = 150 * gUi;
    numField("Download around", &S.dlRadiusKm, "km", "Tiles of 1 x 1 degree covering this radius around the transmitter, and the path to the chosen point (at most 9).", 70 * gUi);
    S.dlRadiusKm = std::clamp(S.dlRadiusKm, 1.0, 300.0);
    ImGui::BeginDisabled(S.dlBusy);
    if (ImGui::SmallButton("Download elevation tiles")) startTileDownload(S, s);
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("%s", "Downloads the public 'Terrain Tiles' (SRTM-based, 1 arc-second, open data on Amazon Web Services; about 3 to 15 MB a tile) "
                                "into OnAir's data folder (terrain/), kept for next time. Only when you press this button; nothing is sent but the tile names.");
    { std::lock_guard<std::mutex> lk(S.dlMu); if (!S.dlMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.dlMsg.c_str()); } }
    ImGui::TextDisabled("Elevation data: Terrain Tiles (Mapzen), NASA SRTM and contributors.");
    if (ImGui::SmallButton("Add elevation file...")) {
        const std::string f = openFileDialog();
        if (!f.empty() && std::find(S.terrainFiles.begin(), S.terrainFiles.end(), f) == S.terrainFiles.end()) {
            S.terrainFiles.push_back(f);
            reloadTerrain();
            if (!S.terrainMsg.empty()) S.terrainFiles.pop_back();   // keep only files that load
        }
    }
    if (!S.terrainFiles.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove all##terr")) { S.terrainFiles.clear(); reloadTerrain(); }
    }
    ImGui::TextDisabled("%s", S.terrain ? S.terrain->describe().c_str() : "no terrain: smooth earth");
    if (!S.terrainMsg.empty()) ImGui::TextColored(pal::warnAmber(), "%s", S.terrainMsg.c_str());
    if (S.terrain) ImGui::TextDisabled("Paths that start or end outside the tiles use a flat sea-level profile. The sweeps always do; the Profile tab uses the tiles.");
}

void viewSection(State& S, pr::Scenario&) {
    S.labelCol = 150 * gUi;
    enumCombo("Map resolution", &S.resolution, "coarse (40)\0medium (80)\0fine (140)\0", "Grid cells per side. Coarse is the cheapest, for a phone or a Raspberry Pi.", 120 * gUi);
    numField("Budget distance", &S.budgetKm, "km", "Where the link budget is computed when no point is chosen on the map (north of the transmitter).", 70 * gUi);
    numField("HF chart distance", &S.hfChartKm, "km", "Path length of the HF time-of-day chart when no point is chosen on the map.", 70 * gUi);
    S.budgetKm = std::clamp(S.budgetKm, 0.05, 20000.0);
    S.hfChartKm = std::clamp(S.hfChartKm, 50.0, 20000.0);
}

// ------------------------------------------------------------------ results
bool budgetPoint(const Result& r, int i, pr::Point& q, std::string& where) {
    double la, lo; std::string lab;
    if (receiverPoint(la, lo, lab)) { q = pr::evaluateAt(r.p[i], la, lo); where = lab; return true; }
    q = pr::evaluate(r.p[i], st().budgetKm, 0);
    where = pr::kmText(st().budgetKm) + " north of the transmitter";
    return false;
}

void probeLine(const Result& r, int i) {
    pr::Point q; std::string where;
    budgetPoint(r, i, q, where);
    const pr::Params& p = r.p[i];
    if (r.compare) { ImGui::TextColored(i ? ImVec4(0.45f, 0.85f, 1, 1) : ImVec4(1, 1, 1, 1), "%s", i ? "B" : "A"); ImGui::SameLine(); }
    const ImVec4 c = q.marginDb >= 10 ? pal::okGreen() : q.marginDb >= 0 ? pal::warnAmber() : pal::badRed();
    ImGui::TextColored(c, "%s: SNR %.1f dB, margin %+.1f dB", pr::ratingName(pr::rating(q.marginDb)), q.snrDb, q.marginDb);
    ImGui::SameLine();
    ImGui::TextDisabled("%s, %.1f km at %.0f deg, %s, loss %.1f dB, %.1f dBm (%.1f dBuV/m), noise %.1f dBm", where.c_str(), q.distKm, q.bearingDeg,
                        pr::pointModelName(q.model), q.pathLossDb, q.rxDbm, q.fieldDbuvm, q.noiseDbm);
    if (p.fMHz < 30) {
        if (q.hops > 0) ImGui::TextDisabled("    skywave %d hop(s) at %.0f deg, MUF %.1f MHz, absorption %.1f dB%s", q.hops, q.elevDeg, q.muf, q.absorptionDb,
                                            q.aboveMufDb > 0.5 ? ", above the MUF (scatter)" : "");
        else ImGui::TextDisabled("    no skywave: %.4g MHz is above the MUF (%.1f MHz) of this path now", p.fMHz, q.muf);
    }
}

void rangeLine(const Result& r, int i) {
    const pr::Range& g = r.range[i];
    if (r.compare) { ImGui::TextColored(i ? ImVec4(0.45f, 0.85f, 1, 1) : ImVec4(1, 1, 1, 1), "%s", i ? "B" : "A"); ImGui::SameLine(); }
    const pr::Verdict v = pr::verdict(r.p[i], g, nullptr, nullptr);
    ImGui::TextWrapped("%s", v.headline.c_str());
    if (r.p[i].terrain && r.bestKm[i] > g.maxKm * 1.2) { ImGui::SameLine(); ImGui::TextDisabled("(median of 8 directions; best %s)", pr::kmText(r.bestKm[i]).c_str()); }
    else if (r.p[i].fMHz >= 30) { ImGui::SameLine(); ImGui::TextDisabled("(radio horizon %s)", pr::kmText(pr::radioHorizonKm(r.p[i].txHeightM, r.p[i].rxHeightM)).c_str()); }
}

void budgetTable(const Result& r) {
    pr::Point qa, qb; std::string wa, wb;
    budgetPoint(r, 0, qa, wa);
    const auto a = pr::budget(r.p[0], qa);
    std::vector<pr::BudgetRow> b;
    if (r.compare) { budgetPoint(r, 1, qb, wb); b = pr::budget(r.p[1], qb); }
    ImGui::TextDisabled("At %s. Hover a term for what it means.", wa.c_str());
    const int cols = r.compare ? 5 : 3;
    if (ImGui::BeginTable("##propbudget", cols, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                          ImVec2(0, std::max(160 * gUi, ImGui::GetContentRegionAvail().y - 34 * gUi)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Term", ImGuiTableColumnFlags_WidthStretch, 3.0f);
        ImGui::TableSetupColumn(r.compare ? "A" : "Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        if (r.compare) { ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_WidthStretch, 1.0f); ImGui::TableSetupColumn("B - A", ImGuiTableColumnFlags_WidthStretch, 1.0f); }
        ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableHeadersRow();
        for (const pr::BudgetRow& row : a) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (row.total) ImGui::TextColored(pal::heading(), "%s", row.label.c_str()); else ImGui::TextUnformatted(row.label.c_str());
            if (ImGui::IsItemHovered()) { ImGui::BeginTooltip(); ImGui::PushTextWrapPos(380 * gUi); ImGui::TextUnformatted(row.help.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip(); }
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", row.value);
            if (r.compare) {
                const pr::BudgetRow* m = nullptr;
                for (const auto& x : b) if (x.label == row.label) { m = &x; break; }
                ImGui::TableNextColumn();
                if (m) ImGui::Text("%.2f", m->value);
                ImGui::TableNextColumn();
                if (m) {
                    const double d = m->value - row.value;
                    if (std::fabs(d) < 0.005) ImGui::TextDisabled("0"); else ImGui::Text("%+.2f", d);
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", row.unit.c_str());
        }
        ImGui::EndTable();
    }
}

void exportCsv(const Result& r) {
    char nm[64];
    const std::time_t now = std::time(nullptr);
    std::strftime(nm, sizeof nm, "propagation_%Y%m%d_%H%M%S.csv", std::localtime(&now));
    const std::string path = saveFileDialog(nm);
    if (path.empty()) return;
    pr::Point qa, qb; std::string wa, wb;
    budgetPoint(r, 0, qa, wa);
    const auto a = pr::budget(r.p[0], qa);
    std::vector<pr::BudgetRow> b;
    if (r.compare) { budgetPoint(r, 1, qb, wb); b = pr::budget(r.p[1], qb); }
    std::string s = "# OnAir propagation: link budget at " + wa + "\n" + pr::budgetCsv(a, r.compare ? &b : nullptr);
    if (r.sweeps) {
        for (int i = 0; i < (r.compare ? 2 : 1); i++) {
            s += std::string("\n# sweeps") + (r.compare ? (i ? " B" : " A") : "") + "\n";
            s += pr::seriesCsv({r.sigVsDist[i], r.rangeVsPower[i], r.rangeVsFreq[i]});
            if (r.p[i].fMHz < 30 && !r.day[i].hour.empty()) s += "# HF over the day\n" + pr::hfDayCsv(r.day[i]);
        }
    }
    FILE* f = dect2::u8fopen(path, "wb");
    if (!f) { st().csvMsg = "could not write " + path; return; }
    std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    st().csvMsg = "saved " + path;
}

void seriesPlot(const char* id, const Result& r, const pr::Series* sa, const pr::Series* sb, double hline, const char* hlabel) {
    if (!sa || sa->x.empty()) return;
    if (plt::BeginPlot(id, ImVec2(-1, 170 * gUi), plt::Flags_NoTitle)) {
        plt::SetupAxes(sa->xLabel.c_str(), sa->yLabel.c_str(), plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        if (sa->logX) plt::SetupAxisScale(plt::X1, plt::Scale_Log10);
        plt::Spec s; s.LineColor = ImVec4(1, 1, 1, 1); s.LineWeight = 2;
        plt::PlotLine(r.compare ? "A" : sa->name.c_str(), sa->x.data(), sa->y.data(), (int)sa->x.size(), s);
        if (sb && !sb->x.empty()) {
            plt::Spec t; t.LineColor = ImVec4(0.45f, 0.85f, 1, 1); t.LineWeight = 2;
            plt::PlotLine("B", sb->x.data(), sb->y.data(), (int)sb->x.size(), t);
        }
        if (hlabel) {
            plt::Spec h; h.LineColor = pal::warnAmber(); h.Flags = plt::InfLines_Horizontal;
            plt::PlotInfLines(hlabel, &hline, 1, h);
        }
        plt::EndPlot();
    }
}

void sweepsTab(const Result& r) {
    if (!r.sweeps) { ImGui::TextDisabled("computing..."); return; }
    const bool cmp = r.compare;
    ImGui::TextDisabled("Toward %s (bearing %.0f deg); smooth earth. White: A%s.", st().havePoint || receiverLocationSet() ? "the receiver point" : "north",
                        r.sweepBearing, cmp ? ", blue: B" : "");
    ImGui::TextUnformatted("Signal against distance");
    seriesPlot("##sw1", r, &r.sigVsDist[0], cmp ? &r.sigVsDist[1] : nullptr, r.p[0].requiredSnrDb, "needed");
    if (!r.lossByModel.empty()) {
        ImGui::TextUnformatted(r.p[0].fMHz >= 30 ? "Path loss against distance, model by model (scenario A)" : "Path loss against distance (scenario A)");
        if (plt::BeginPlot("##swloss", ImVec2(-1, 200 * gUi), plt::Flags_NoTitle)) {
            plt::SetupAxes("distance (km)", "path loss (dB)", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
            plt::SetupAxisScale(plt::X1, plt::Scale_Log10);
            static const ImVec4 col[5] = {ImVec4(0.6f, 0.6f, 0.6f, 1), ImVec4(0.45f, 0.85f, 1, 1), ImVec4(1, 1, 1, 1), ImVec4(1, 0.55f, 0.3f, 1), ImVec4(0.5f, 1, 0.5f, 1)};
            for (size_t k = 0; k < r.lossByModel.size(); k++) {
                const pr::Series& s = r.lossByModel[k];
                plt::Spec sp; sp.LineColor = col[k % 5]; sp.LineWeight = k == 2 ? 2.5f : 1.5f;
                plt::PlotLine(s.name.c_str(), s.x.data(), s.y.data(), (int)s.x.size(), sp);
            }
            plt::EndPlot();
        }
        ImGui::TextDisabled("Higher is a weaker signal. The orange (90 %% of locations) curve is the safe planning value; green shows the ducting nights.");
    }
    ImGui::TextUnformatted("Range against transmitter power");
    seriesPlot("##sw2", r, &r.rangeVsPower[0], cmp ? &r.rangeVsPower[1] : nullptr, 0, nullptr);
    ImGui::TextUnformatted("Range against frequency (same antennas and power)");
    seriesPlot("##sw3", r, &r.rangeVsFreq[0], cmp ? &r.rangeVsFreq[1] : nullptr, 0, nullptr);
    if (r.p[0].fMHz < 30 && !r.day[0].hour.empty()) {
        const pr::HfDay& d = r.day[0];
        ImGui::Text("HF over the day (UTC), %s path: median MUF, FOT (the MUF on 90 %% of days) and LUF (dots: the lowest frequency that still meets the SNR)", pr::kmText(r.sweepKm).c_str());
        if (plt::BeginPlot("##swday", ImVec2(-1, 190 * gUi), plt::Flags_NoTitle)) {
            plt::SetupAxes("UTC hour", "MHz", 0, plt::AxisFlags_AutoFit);
            plt::SetupAxisLimits(plt::X1, 0, 24, plt::Cond_Always);
            plt::Spec m; m.LineColor = ImVec4(1, 1, 1, 1); m.LineWeight = 2;
            plt::PlotLine("MUF", d.hour.data(), d.muf.data(), (int)d.hour.size(), m);
            plt::Spec f; f.LineColor = pal::okGreen();
            plt::PlotLine("FOT", d.hour.data(), d.fot.data(), (int)d.hour.size(), f);
            std::vector<double> lh, lv;
            for (size_t i = 0; i < d.hour.size(); i++) if (d.luf[i] > 0) { lh.push_back(d.hour[i]); lv.push_back(d.luf[i]); }
            plt::Spec l; l.Marker = plt::Marker_Circle; l.MarkerFillColor = pal::badRed(); l.MarkerLineColor = pal::badRed(); l.MarkerSize = 3;
            if (!lh.empty()) plt::PlotScatter("LUF", lh.data(), lv.data(), (int)lh.size(), l);
            const double fo = r.p[0].fMHz;
            plt::Spec h; h.LineColor = pal::warnAmber(); h.Flags = plt::InfLines_Horizontal;
            plt::PlotInfLines("your frequency", &fo, 1, h);
            plt::EndPlot();
        }
        ImGui::TextDisabled("Usable when your frequency is between the LUF dots and the MUF line.");
    }
}

void profileTab(const Result& r) {
    const pr::ProfileView& v = r.profile;
    if (v.dKm.size() < 2) { ImGui::TextDisabled("computing..."); return; }
    std::string lab; double la, lo;
    const bool pt = receiverPoint(la, lo, lab);
    ImGui::TextWrapped("Toward %s: %.1f km. Ground (brown) bent by the earth's curvature for k = %.2f; the line of sight between the antenna tops "
                       "(white) and the first Fresnel zone (blue). Worst clearance: %.0f %% of the Fresnel radius at %.1f km (60 %% or more: no "
                       "diffraction loss; below 0: the path is blocked).",
                       pt ? lab.c_str() : "north (no point chosen)", v.dKm.back(), v.kFactor, v.worstClearancePct, v.worstKm);
    if (!v.terrain) ImGui::TextColored(pal::warnAmber(), "%s", "No terrain under this path: a flat sea-level profile. Download tiles in Advanced > Terrain.");
    const size_t n = v.dKm.size();
    std::vector<double> top(n), bot(n), f60(n);
    double gmin = 1e9;
    for (size_t i = 0; i < n; i++) { top[i] = v.losM[i] + v.fresnelM[i]; bot[i] = v.losM[i] - v.fresnelM[i]; f60[i] = v.losM[i] - 0.6 * v.fresnelM[i]; gmin = std::min(gmin, v.groundM[i]); }
    if (plt::BeginPlot("##profile", ImVec2(-1, std::max(220 * gUi, ImGui::GetContentRegionAvail().y - 10 * gUi)), plt::Flags_NoTitle)) {
        plt::SetupAxes("distance from the transmitter (km)", "height above sea level (m)", plt::AxisFlags_AutoFit, plt::AxisFlags_AutoFit);
        plt::Spec g; g.LineColor = ImVec4(0.65f, 0.45f, 0.25f, 1); g.FillColor = ImVec4(0.45f, 0.32f, 0.18f, 0.6f);
        plt::PlotShaded("ground", v.dKm.data(), v.groundM.data(), (int)n, gmin - 20, g);
        plt::PlotLine("ground", v.dKm.data(), v.groundM.data(), (int)n, g);
        plt::Spec l; l.LineColor = ImVec4(1, 1, 1, 1); l.LineWeight = 2;
        plt::PlotLine("line of sight", v.dKm.data(), v.losM.data(), (int)n, l);
        plt::Spec f; f.LineColor = ImVec4(0.45f, 0.75f, 1, 1);
        plt::PlotLine("first Fresnel zone", v.dKm.data(), top.data(), (int)n, f);
        plt::PlotLine("first Fresnel zone", v.dKm.data(), bot.data(), (int)n, f);
        plt::Spec s6; s6.LineColor = pal::warnAmber();
        plt::PlotLine("60 % of the zone", v.dKm.data(), f60.data(), (int)n, s6);
        plt::EndPlot();
    }
}

void hfChartTab(const Result& r) {
    if (!r.haveChart || r.hfChart.freqMHz.empty()) { ImGui::TextDisabled("computing..."); return; }
    const pr::hf::Chart& c = r.hfChart;
    const pr::Params& p = r.p[0];
    std::string lab; double la, lo;
    const bool pt = receiverPoint(la, lo, lab);
    const double km = pr::greatCircleKm(p.txLat, p.txLon, r.tgtLat, r.tgtLon);
    ImGui::TextWrapped("ITU-R P.533-14 reliability toward %s (%s), month %d, sunspot number %.0f: the share of days the SNR reaches %.0f dB in %.0f Hz, "
                       "by UTC hour and band. The number is the median SNR (dB). Your frequency is marked with >.",
                       pt ? lab.c_str() : "north", pr::kmText(km).c_str(), p.month, p.r12 >= 0 ? p.r12 : pr::hf::r12FromSfi(p.sfi), p.requiredSnrDb, p.bandwidthHz);
    const size_t nf = c.freqMHz.size();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float lw = 80 * gUi, ch = 20 * gUi;
    const float cw = std::max(14 * gUi, (ImGui::GetContentRegionAvail().x - lw) / 24.0f);
    ImVec2 o = ImGui::GetCursorScreenPos();
    for (int h = 0; h < 24; h += 3) {
        char b[8]; std::snprintf(b, sizeof b, "%02d", h);
        dl->AddText(ImVec2(o.x + lw + h * cw + 2, o.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), b);
    }
    o.y += ch;
    for (size_t k = 0; k < nf; k++) {
        const size_t i = nf - 1 - k;   // highest band on top
        const float y = o.y + k * ch;
        char b[32];
        const bool mine = std::fabs(c.freqMHz[i] - p.fMHz) < 0.05;
        std::snprintf(b, sizeof b, "%s%.4g MHz", mine ? "> " : "", c.freqMHz[i]);
        dl->AddText(ImVec2(o.x, y + 2), ImGui::GetColorU32(mine ? ImGuiCol_Text : ImGuiCol_TextDisabled), b);
        for (int h = 0; h < 24; h++) {
            const float rel = c.rel[h * nf + i], snr = c.snr[h * nf + i];
            const ImVec4 col = rel < 0.02f ? ImVec4(0.18f, 0.18f, 0.2f, 1) : ImVec4(0.85f * (1 - rel) + 0.1f, 0.25f + 0.6f * rel, 0.2f, 1);
            const ImVec2 a(o.x + lw + h * cw, y), z(a.x + cw - 1, y + ch - 1);
            dl->AddRectFilled(a, z, ImGui::GetColorU32(col));
            if (cw > 22 * gUi && snr > -90) { std::snprintf(b, sizeof b, "%.0f", snr); dl->AddText(ImVec2(a.x + 2, a.y + 2), IM_COL32(255, 255, 255, 200), b); }
            if (ImGui::IsMouseHoveringRect(a, z)) ImGui::SetTooltip("%02d UTC, %.4g MHz: reliability %.0f %%, median SNR %.1f dB, MUF %.1f MHz", h, c.freqMHz[i], 100 * rel, snr, c.muf[h]);
        }
    }
    ImGui::Dummy(ImVec2(lw + 24 * cw, (nf + 1) * ch + 4));
    std::string mufs = "Median MUF by hour (MHz):";
    for (int h = 0; h < 24; h += 2) { char b[16]; std::snprintf(b, sizeof b, " %02d:%.0f", h, c.muf[h]); mufs += b; }
    ImGui::TextDisabled("%s", mufs.c_str());
    ImGui::TextDisabled("Green: open on most days; red: rarely; grey: closed. Monthly medians from the CCIR maps: a given day differs, sporadic E is not included.");
}

void satTab() {
    State& S = st();
    SatUi& u = S.sat;
    const pr::Scenario& s = S.sc[0];
    S.labelCol = 170 * gUi;
    ImGui::TextWrapped("An earth-space link from the transmitter position (your station, %.3f, %.3f): geometry, ITU-R P.618-14 rain and scintillation, "
                       "P.676-13 slant-path gases, the sky noise, and the C/N against what the mode needs.", s.txLat, s.txLon);
    enumCombo("Satellite", &u.kind, "geostationary\0low orbit (one pass)\0", "A GEO satellite at an orbit longitude, or a low-orbit satellite at an altitude and the elevation you want to look at.");
    if (u.kind == 0) numField("Orbit longitude", &u.satLon, "deg E", "e.g. Astra 19.2, Eutelsat Hot Bird 13, Es'hail-2 / QO-100 25.9, Arabsat 26, Nilesat 7 W = -7.", 80 * gUi);
    else {
        numField("Altitude", &u.leoAltKm, "km", "ISS 420, NOAA / Meteor 820, Iridium 780, Starlink 550, GPS 20200.", 80 * gUi);
        numField("Elevation", &u.leoElevDeg, "deg", "The elevation of the moment you want the budget for (0 horizon .. 90 overhead).", 80 * gUi);
    }
    numField("Frequency", &u.fGHz, "GHz", "Downlink (or uplink) frequency: 0.137 APT, 1.7 HRPT / GOES, 2.4 QO-100 up, 10.5 QO-100 down, 11.7 Ku TV, 20 Ka.", 80 * gUi);
    numField("EIRP of the sender", &u.eirpDbw, "dBW", "The satellite's EIRP toward you for a downlink (Ku TV 45 to 55, QO-100 NB beacon about 30), or your own for an uplink.", 80 * gUi);
    numField("Dish diameter", &u.dishM, "m", "Receive antenna diameter (also used for scintillation).", 80 * gUi);
    ImGui::SameLine();
    if (ImGui::SmallButton("gain from dish")) u.rxGainDbi = 10 * std::log10(std::max(u.eff, 0.05) * std::pow(3.14159265 * u.dishM * u.fGHz / 0.299792458, 2));
    numField("Antenna gain", &u.rxGainDbi, "dBi", "Receive antenna gain (a 0.8 m dish at 11.7 GHz with 60 % efficiency: 37.6 dBi; a turnstile for 137 MHz: about 3).", 80 * gUi);
    numField("LNB / LNA noise figure", &u.nfDb, "dB", "The first amplifier's noise figure (a Ku LNB 0.5 to 1, an SDR alone 4 to 8).", 80 * gUi);
    numField("Feeder loss", &u.feederDb, "dB", "Loss before the first amplifier (0 for an LNB at the dish).", 80 * gUi);
    numField("Rain rate R0.01", &u.r001, "mm/h", "The rain rate exceeded 0.01 % of a year (ITU-R P.837 map or local data): Gulf 10 to 25, Northern Europe 20 to 30, "
             "Mediterranean 30 to 50, tropics 60 to 120.", 80 * gUi);
    numField("Availability", &u.availPct, "%", "The share of the year the link must work: the rain and scintillation margins are for the rest (99.9 = 8.8 hours a year down).", 80 * gUi);
    enumCombo("Polarisation", &u.pol, "horizontal\0vertical\0circular\0", "Rain attenuates horizontal polarisation a little more (P.838).");
    const auto& ms = pr::modeThresholds();
    ImGui::AlignTextToFramePadding(); ImGui::TextDisabled("Mode"); ImGui::SameLine(S.labelCol);
    ImGui::SetNextItemWidth(200 * gUi);
    if (ImGui::BeginCombo("##satmode", u.mode >= 0 && u.mode < (int)ms.size() ? ms[u.mode].name : "(type the bandwidth and SNR)")) {
        if (ImGui::Selectable("(type the bandwidth and SNR)", u.mode < 0)) u.mode = -1;
        for (int i = 0; i < (int)ms.size(); i++) if (ImGui::Selectable(ms[i].name, i == u.mode)) u.mode = i;
        ImGui::EndCombo();
    }
    if (u.mode >= 0 && u.mode < (int)ms.size()) { u.bwHz = ms[u.mode].bwHz; u.reqSnrDb = pr::modeRequiredSnrDb(ms[u.mode], 1); }
    else {
        numField("Bandwidth", &u.bwHz, "Hz", "Noise bandwidth of the signal.", 100 * gUi, "%.6g");
        numField("Required C/N", &u.reqSnrDb, "dB", "What the mode needs (DVB-S2 QPSK 1/2 about 1, 8PSK 3/4 about 7.5).", 80 * gUi);
    }
    S.labelCol = 0;
    // compute
    pr::sat::Link l;
    l.fGHz = std::max(u.fGHz, 0.01); l.eirpDbw = u.eirpDbw; l.rxGainDbi = u.rxGainDbi; l.rxNoiseFigureDb = u.nfDb; l.rxFeederDb = u.feederDb;
    l.antennaM = u.dishM; l.efficiency = u.eff; l.stLat = s.txLat; l.stHkm = 0; l.rainR001 = std::max(u.r001, 0.0);
    l.availabilityPct = std::clamp(u.availPct, 50.0, 99.999); l.tauDeg = u.pol == 0 ? 0 : u.pol == 1 ? 90 : 45;
    l.bandwidthHz = std::max(u.bwHz, 1.0); l.requiredSnrDb = u.reqSnrDb;
    double rr = 0;
    if (u.kind == 0) {
        const pr::sat::Look k = pr::sat::look(s.txLat, s.txLon, 0, 0, u.satLon, 35786);
        l.elevDeg = k.elevDeg; l.rangeKm = k.rangeKm;
        ImGui::Separator();
        if (k.elevDeg <= 0) { ImGui::TextColored(pal::badRed(), "The satellite is below your horizon (elevation %.1f deg).", k.elevDeg); return; }
        ImGui::Text("Look angles: elevation %.1f deg, azimuth %.1f deg (true), range %.0f km.", k.elevDeg, k.azDeg, k.rangeKm);
    } else {
        l.elevDeg = std::clamp(u.leoElevDeg, 0.0, 90.0); l.rangeKm = pr::sat::slantRangeKm(u.leoAltKm, l.elevDeg);
        rr = pr::sat::rangeRateKmS(u.leoAltKm, l.elevDeg);
        ImGui::Separator();
        ImGui::Text("Range %.0f km at %.0f deg; orbital speed %.2f km/s; Doppler now up to %.2f kHz, at the horizon %.2f kHz (an overhead pass).",
                    l.rangeKm, l.elevDeg, pr::sat::orbitalSpeedKmS(u.leoAltKm), pr::sat::dopplerHz(l.fGHz * 1000, rr) / 1e3,
                    pr::sat::dopplerHz(l.fGHz * 1000, pr::sat::rangeRateKmS(u.leoAltKm, 0)) / 1e3);
    }
    const pr::sat::Budget b = pr::sat::budget(l);
    ImGui::Text("Free space %.1f dB, gases %.2f dB, rain %.2f dB and scintillation %.2f dB (exceeded %.3g %% of the year), total %.1f dB.",
                b.fsplDb, b.gasDb, b.rainDb, b.scintDb, 100 - l.availabilityPct, b.totalLossDb);
    ImGui::Text("Sky %.0f K, system %.0f K, G/T %.1f dB/K. Received %.1f dBW, C/N0 %.1f dBHz.", b.skyK, b.tsysK, b.gtDbK, b.rxDbw, b.cn0DbHz);
    const ImVec4 c = b.marginDb >= 3 ? pal::okGreen() : b.marginDb >= 0 ? pal::warnAmber() : pal::badRed();
    ImGui::TextColored(c, "C/N %.1f dB in %.4g kHz (clear sky %.1f dB): margin %+.1f dB at %.3g %% availability.", b.cnDb, l.bandwidthHz / 1e3,
                       b.clearSkyCnDb, b.marginDb, l.availabilityPct);
    ImGui::TextDisabled("Rain height from the latitude (a fit of the P.839 map); no cloud attenuation (P.840 needs its maps). LEO Doppler for a pass through the zenith.");
}

void modelsTab(const Result& r) {
    ImGui::PushTextWrapPos(0);
    ImGui::TextColored(pal::heading(), "Now: %s", pr::modelName(r.p[0]));
    ImGui::TextWrapped("%s", pr::modelLimits(r.p[0]).c_str());
    ImGui::Separator();
    ImGui::TextColored(pal::heading(), "30 MHz and up");
    ImGui::BulletText("Auto = ITU-R P.1812-8 (09/2025), checked against all 57 published ITU validation results: free space with multipath and focusing "
                      "corrections, delta-Bullington diffraction over the terrain profile for the median effective earth radius (k from the "
                      "refractivity lapse rate, P.453) and for the super-refractive k = 3, troposcatter, ducting and layer reflection, blended for "
                      "the time %% you set, then the location variability for the locations %%.");
    ImGui::BulletText("Free space (P.525), exact two-ray ground reflection, P.526 smooth earth and the 3.9 combination stay selectable for comparison; "
                      "the Sweeps tab draws them side by side.");
    ImGui::BulletText("Gases: ITU-R P.676-13 line by line (44 oxygen and 35 water vapour lines). Rain: ITU-R P.838-3 coefficients with the P.530 "
                      "path reduction. Receiver clutter: ITU-R P.2108. Terrain: downloaded SRTM tiles or your own files; without them a flat profile.");
    ImGui::BulletText("Not modelled: buildings in detail, building entry loss, foliage depth, sporadic E, aircraft scatter.");
    ImGui::TextColored(pal::heading(), "Below 30 MHz");
    ImGui::BulletText("Sky wave: ITU-R P.533-14 with the ITU-R P.1239-4 ionosphere: the CCIR numerical maps of foF2 and M(3000)F2 for the month, hour "
                      "and sunspot number (with the 1960 magnetic field for the modified dip), foE, E and F2 modes with E-layer screening, mirror "
                      "heights, absorption at every 90 km penetration point, auroral loss, the long-path method beyond 7000 km, and the reliability "
                      "from the decile spread of MUF, signal and noise (P.1239, P.842, P.372).");
    ImGui::BulletText("Ground wave: Norton's surface wave (Terman's form) over the chosen ground with the earth's curvature from P.526.");
    ImGui::BulletText("Not modelled: sporadic E, storm-time ionosphere (monthly medians only), antenna patterns by take-off angle unless a pattern file is loaded.");
    ImGui::TextColored(pal::heading(), "Satellites");
    ImGui::BulletText("Look angles from a WGS 84 station, slant range, LEO Doppler; ITU-R P.618-14 rain and scintillation for the availability, "
                      "P.676-13 slant-path gases through the P.835 reference atmosphere, sky noise from the attenuation.");
    ImGui::TextColored(pal::heading(), "Noise and the receiver");
    ImGui::BulletText("ITU-R P.372-18: man-made noise by environment, galactic noise above foF2, atmospheric noise (a fit of the P.372 maps). "
                      "The receive chain by Friis. 'Will it decode?' uses each mode's measured threshold from docs/SENSITIVITY.md.");
    ImGui::TextDisabled("All formulas are written from the public ITU-R texts; docs/PROPAGATION-4.0.md lists the versions and the validation.");
    ImGui::PopTextWrapPos();
}

} // namespace

void advancedInputs(App&) {
    State& S = st();
    if (S.compare) {
        for (int i = 0; i < 2; i++) {
            if (i) ImGui::SameLine(0, 0);
            const bool on = S.editing == i;
            ImGui::PushStyleColor(ImGuiCol_Button, on ? (i ? ImVec4(0.2f, 0.5f, 0.7f, 1) : pal::accent(0.55f)) : ImVec4(0.16f, 0.17f, 0.18f, 1));
            if (ImGui::Button(i ? "Edit B" : "Edit A", ImVec2(70 * gUi, 0))) S.editing = i;
            ImGui::PopStyleColor();
        }
        ImGui::SameLine(0, 8 * gUi);
        if (ImGui::SmallButton(S.editing ? "Copy A here" : "Copy B here")) {
            const int o = 1 - S.editing;
            S.sc[S.editing] = S.sc[o];
            S.pattern[S.editing] = S.pattern[o];
            S.patternMsg[S.editing] = S.patternMsg[o];
        }
        ImGui::SameLine(0, 8 * gUi);
        ImGui::TextDisabled("map:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50 * gUi);
        ImGui::Combo("##advshow", &S.show, "A\0B\0");
    } else S.editing = 0;
    section("Transmitter", true, txSection);
    section("Signal", true, signalSection);
    section("Receiver and radio", true, rxSection);
    section("Path and surroundings", false, pathSection);
    section("Statistics and climate (30 MHz and up)", false, statsSection);
    section("Ionosphere and time (HF)", cur().fMHz < 30, ionoSection);
    section("Rain and gases", cur().fMHz > 5000, atmoSection);
    section("Terrain (your elevation file)", false, terrainSection);
    section("Map and charts", false, viewSection);
    S.labelCol = 0;
    issuesUi(cur());
}

void advancedResults(App& a, const std::shared_ptr<const Result>& r) {
    State& S = st();
    if (r) for (int i = 0; i < (r->compare ? 2 : 1); i++) rangeLine(*r, i);
    else ImGui::TextDisabled("computing...");
    if (ImGui::BeginTabBar("##proptabs")) {
        if (ImGui::BeginTabItem("Map")) {
            const float lines = r ? (float)(r->compare ? 2 : 1) * (r->p[0].fMHz < 30 ? 2.f : 1.f) : 1.f;
            mapView(a, (lines + 0.5f) * ImGui::GetFrameHeightWithSpacing());
            if (r) for (int i = 0; i < (r->compare ? 2 : 1); i++) probeLine(*r, i);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Link budget")) {
            if (r) {
                if (ImGui::SmallButton("Export CSV")) exportCsv(*r);
                ImGui::SetItemTooltip("%s", "The link budget, the sweeps and (HF) the MUF over the day, in one CSV file.");
                if (!S.csvMsg.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", S.csvMsg.c_str()); }
                budgetTable(*r);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Sweeps")) {
            if (r) {
                ImGui::BeginChild("##propsweeps", ImVec2(0, 0));
                sweepsTab(*r);
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        if (r && r->p[0].fMHz >= 30 && ImGui::BeginTabItem("Profile")) {
            ImGui::BeginChild("##propprofile", ImVec2(0, 0));
            profileTab(*r);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (r && r->p[0].fMHz < 30 && ImGui::BeginTabItem("HF hours x bands")) {
            ImGui::BeginChild("##prophfchart", ImVec2(0, 0));
            hfChartTab(*r);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Satellite")) {
            ImGui::BeginChild("##propsat", ImVec2(0, 0));
            satTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Models")) {
            if (r) {
                ImGui::BeginChild("##propmodels", ImVec2(0, 0));
                modelsTab(*r);
                ImGui::EndChild();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

} // namespace propui
