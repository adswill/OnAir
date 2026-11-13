// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Scenario, presets, radios, input checks, verdict, link budget, sweeps and CSV, see core/include/dect2/prop/prop_scenario.h.
#include "dect2/prop/prop_scenario.h"
#include "dect2/prop/prop_terrain.h"
#include "dect2/prop/itu_atmos.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace dect2::prop {

namespace {
double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    return b;
}
}

const std::vector<Preset>& presets() {
    static const std::vector<Preset> v = {
        {"FM broadcast", "A regional FM station: 10 kW from a 150 m mast, heard on a car or kitchen radio.", 98.0, 200e3, 15, 10000, 2.15, 150, 0, 2, Pol::Vertical, 87.5, 108},
        {"DAB+ radio", "A DAB+ multiplex: 5 kW from a 150 m mast; below about 10 dB the sound drops out.", 225.648, 1.536e6, 10, 5000, 2.15, 150, 0, 2, Pol::Vertical, 174, 240},
        {"DVB-T/T2 TV", "A TV mast (about 20 kW ERP from 200 m) into a roof aerial at 10 m.", 586.0, 7.61e6, 20, 1000, 13, 200, 10, 10, Pol::Horizontal, 174, 862},
        {"Airband (aircraft)", "An aircraft at 10 km altitude calling on AM; you listen with a scanner.", 125.0, 6000, 10, 20, 0, 10000, 0, 5, Pol::Vertical, 108, 137},
        {"Ham 2 m FM", "A 25 W 2 m FM station with a vertical at 10 m.", 145.5, 12500, 12, 25, 2.15, 10, 2.15, 5, Pol::Vertical, 144, 148},
        {"Ham 70 cm FM", "A 25 W 70 cm FM station with a vertical at 10 m.", 433.5, 12500, 12, 25, 2.15, 10, 2.15, 5, Pol::Vertical, 430, 440},
        {"Ham 80 m SSB", "100 W SSB into a dipole at 10 m (night band, short skip).", 3.65, 2400, 10, 100, 2.15, 10, 2.15, 10, Pol::Horizontal, 3.5, 4.0},
        {"Ham 40 m SSB", "100 W SSB into a dipole at 10 m.", 7.1, 2400, 10, 100, 2.15, 10, 2.15, 10, Pol::Horizontal, 7.0, 7.3},
        {"Ham 20 m SSB", "100 W SSB into a dipole at 10 m (the daytime DX band).", 14.2, 2400, 10, 100, 2.15, 10, 2.15, 10, Pol::Horizontal, 14.0, 14.35},
        {"Ham 10 m SSB", "100 W SSB into a dipole at 10 m (open when the sun is active).", 28.5, 2400, 10, 100, 2.15, 10, 2.15, 10, Pol::Horizontal, 28.0, 29.7},
        {"Ham 20 m FT8", "50 W FT8: decodes down to about -21 dB in 2.5 kHz.", 14.074, 2500, -21, 50, 2.15, 10, 2.15, 10, Pol::Horizontal, 14.0, 14.35},
        {"CB radio", "A 4 W CB in a car with a whip antenna.", 27.185, 8000, 10, 4, 0, 2, 0, 2, Pol::Vertical, 26.965, 27.405},
        {"PMR446", "A 0.5 W licence-free handheld held at head height.", 446.1, 12500, 12, 0.5, 0, 1.5, 0, 1.5, Pol::Vertical, 446.0, 446.2},
        {"LoRa / Meshtastic", "A 25 mW LoRa node, SF12 at 125 kHz (works down to about -20 dB).", 868.1, 125e3, -20, 0.025, 2.15, 2, 2.15, 5, Pol::Vertical, 863, 870},
        {"ADS-B (1090 MHz)", "An airliner transponder (250 W) at 10 km altitude into your ADS-B antenna.", 1090.0, 2e6, 10, 250, 0, 10000, 3, 10, Pol::Vertical, 1089, 1091},
        {"Custom", "Your own numbers.", 0, 0, 0, 0, 0, 0, 0, 0, Pol::Vertical, 0, 0},
    };
    return v;
}

void applyPreset(Scenario& s, int index) {
    const auto& v = presets();
    if (index < 0 || index >= (int)v.size()) return;
    s.preset = index;
    const Preset& p = v[index];
    if (p.fMHz <= 0) return;   // Custom: keep the numbers
    s.fMHz = p.fMHz; s.bandwidthHz = p.bandwidthHz; s.requiredSnrDb = p.requiredSnrDb;
    s.power = p.powerW; s.powerUnit = PowerUnit::W;
    s.txGainDbi = p.txGainDbi; s.txHeightM = p.txHeightM; s.rxGainDbi = p.rxGainDbi; s.rxHeightM = p.rxHeightM;
    s.txPol = s.rxPol = p.pol;
    s.txFeederDb = 0;
}

const std::vector<Radio>& radios() {
    // typical published or measured figures; a real unit can differ by a few dB
    static const std::vector<Radio> v = {
        {"Enter the noise figure", 6, 6, 0, 1e6, "Type your receiver's noise figure."},
        {"RTL-SDR Blog V3 (R820T2)", 4.5, 25, 0.5, 1766, "HF only by direct sampling, which is deaf (about 25 dB noise figure)."},
        {"RTL-SDR Blog V4 (R828D)", 4.0, 12, 0.5, 1766, "HF through its built-in upconverter."},
        {"Airspy R2 / Mini", 3.5, 3.5, 24, 1800, "No HF without an upconverter."},
        {"Airspy HF+ Discovery", 3.5, 7, 0.0005, 260, "HF and VHF up to 260 MHz."},
        {"SDRplay RSP1A / RSP1B", 4.5, 15, 0.001, 2000, ""},
        {"SDRplay RSPdx / RSPduo", 4.0, 11, 0.001, 2000, ""},
        {"HackRF One", 9, 12, 1, 6000, "The noise figure assumes its front amplifier is on; off adds about 4 dB."},
        {"HackRF Pro", 7, 10, 0.1, 6000, ""},
        {"ADALM-Pluto", 4.5, 4.5, 325, 3800, "325 MHz to 3.8 GHz (wider with the known firmware change)."},
        {"LimeSDR / LimeSDR Mini", 5, 8, 10, 3500, ""},
        {"USRP B200 / B210", 6, 6, 70, 6000, ""},
        {"bladeRF 2.0 micro", 6, 6, 47, 6000, ""},
        {"KiwiSDR", 14, 14, 0.01, 30, "HF only."},
        {"Ham transceiver / scanner", 5, 12, 0.1, 1300, "A typical receiver with its preamplifier off."},
    };
    return v;
}

double radioNfDb(const Scenario& s, double fMHz) {
    const auto& v = radios();
    if (s.radio <= 0 || s.radio >= (int)v.size()) return std::max(0.0, s.rxNfDb);
    const Radio& r = v[s.radio];
    const double base = fMHz < 30 ? r.nfHfDb : r.nfDb;
    const double g = clampd(s.radioGainPct / 100.0, 0, 1);
    return base + 16.0 * (1 - g) * (1 - g);
}

double powerDbw(double power, PowerUnit u) {
    switch (u) {
        case PowerUnit::W: return 10 * std::log10(std::max(power, 1e-12));
        case PowerUnit::kW: return 30 + 10 * std::log10(std::max(power, 1e-12));
        case PowerUnit::dBW: return power;
        case PowerUnit::dBm: return power - 30;
    }
    return 0;
}

Params toParams(const Scenario& s, const Terrain* terrain, const AntennaPattern* pattern) {
    Params p;
    p.txLat = s.txLat; p.txLon = s.txLon;
    p.txPowerDbw = powerDbw(s.power, s.powerUnit);
    p.txGainDbi = s.txGainDbi; p.txFeederDb = std::max(0.0, s.txFeederDb); p.txAzDeg = s.txAzDeg;
    p.txPattern = pattern && pattern->valid() ? pattern : nullptr;
    p.fMHz = clampd(s.fMHz, 0.01, 100000.0);
    p.bandwidthHz = std::max(s.bandwidthHz, 1.0);
    p.requiredSnrDb = s.requiredSnrDb;
    p.rxGainDbi = s.rxGainDbi; p.rxFeederDb = std::max(0.0, s.rxFeederDb);
    p.rxNfDb = radioNfDb(s, p.fMHz);
    p.preamp = s.preamp; p.preampGainDb = s.preampGainDb; p.preampNfDb = s.preampNfDb;
    p.txPol = s.txPol; p.rxPol = s.rxPol;
    p.env = s.env; p.atmosphericNoise = s.atmosphericNoise;
    p.txHeightM = std::max(0.0, s.txHeightM); p.rxHeightM = std::max(0.0, s.rxHeightM);
    p.ground = s.ground; p.rxClutter = s.rxClutter;
    p.pathModel = s.pathModel; p.hfModel = s.hfModel;
    p.sfi = s.sfi; p.kIndex = clampd(s.kIndex, 0, 9);
    p.year = s.year; p.month = s.month; p.day = s.day; p.utcHour = s.hour + s.minute / 60.0;
    p.rainMmH = std::max(0.0, s.rainMmH); p.gas = s.gas;
    p.terrain = terrain && !terrain->empty() ? terrain : nullptr;
    p.timePct = clampd(s.timePct, 1, 50); p.locPct = clampd(s.locPct, 1, 99); p.sigmaLocDb = std::max(0.0, s.sigmaLocDb);
    p.deltaN = clampd(s.deltaN, 0, 150); p.n0 = clampd(s.n0, 250, 450);
    p.hfLegacy = s.hfLegacy; p.r12 = s.r12;
    if (!s.decodeMode.empty()) {   // "will mode X decode here?": the measured threshold and its bandwidth replace the typed values
        if (const ModeSnr* m = findModeThreshold(s.decodeMode)) { p.bandwidthHz = m->bwHz; p.requiredSnrDb = modeRequiredSnrDb(*m, s.decodeCondition); }
    }
    return p;
}

std::vector<Issue> validate(const Scenario& s) {
    std::vector<Issue> v;
    auto add = [&](int l, std::string t) { v.push_back({l, std::move(t)}); };
    if (!(s.fMHz >= 0.1 && s.fMHz <= 100000)) add(2, "Frequency: enter 0.1 MHz to 100 GHz (100000 MHz).");
    else if (s.fMHz < 1.6) add(1, "Below 1.6 MHz (long and medium wave) only the ground wave is reliable; the night skywave estimate is rough.");
    else if (s.fMHz >= 30 && s.fMHz < 60) add(0, "30 to 60 MHz: sporadic E and F2 openings can carry it much farther than shown; they are not predicted.");
    if (s.fMHz > 6000 && s.rainMmH <= 0) add(0, "Above 6 GHz rain matters: set a rain rate (Advanced, Atmosphere) for a worst case.");
    if (!(s.bandwidthHz >= 1 && s.bandwidthHz <= 100e6)) add(2, "Bandwidth: enter 1 Hz to 100 MHz (the receiver's filter, e.g. 12500 for narrow FM).");
    const double pw = powerDbw(s.power, s.powerUnit);
    if ((s.powerUnit == PowerUnit::W || s.powerUnit == PowerUnit::kW) && !(s.power > 0)) add(2, "Power: enter more than 0 W.");
    else if (!std::isfinite(pw)) add(2, "Power: not a number.");
    else if (pw > 63) add(1, "Power above 2 MW: is this the ERP rather than the transmitter power? Tick nothing, just lower the gain.");
    if (!(s.txHeightM >= 0) || !(s.rxHeightM >= 0)) add(2, "Antenna heights cannot be below the ground (0 m or more).");
    else if (s.txHeightM > 40000 || s.rxHeightM > 40000) add(1, "Heights above 40 km: for satellites use a satellite link budget; this tool assumes the ground.");
    if (s.fMHz >= 30 && s.txHeightM < 1) add(1, "A transmitter antenna below 1 m is treated as 1 m.");
    if (!(s.requiredSnrDb >= -40 && s.requiredSnrDb <= 60)) add(1, "Required SNR is usually -25 dB (FT8, LoRa) to 30 dB (high-order QAM).");
    if (s.radio == 0 && !(s.rxNfDb >= 0 && s.rxNfDb <= 40)) add(2, "Noise figure: enter 0 to 40 dB (about 1 for a good preamp, 4 to 8 for an SDR).");
    if (!(s.txFeederDb >= 0) || !(s.rxFeederDb >= 0)) add(2, "Feeder loss is a loss: enter 0 dB or more.");
    else if (s.txFeederDb > 30 || s.rxFeederDb > 30) add(1, "Feeder loss above 30 dB: check the cable length and type.");
    if (std::fabs(s.txGainDbi) > 50 || std::fabs(s.rxGainDbi) > 50) add(1, "Antenna gain above 50 dBi is unusual (dish antennas reach 30 to 45 dBi).");
    if (s.preamp && !(s.preampGainDb >= 0 && s.preampGainDb <= 50)) add(2, "Preamplifier gain: 0 to 50 dB.");
    if (s.preamp && !(s.preampNfDb >= 0 && s.preampNfDb <= 20)) add(2, "Preamplifier noise figure: 0 to 20 dB.");
    if (s.fMHz < 30) {
        if (!(s.sfi >= 60 && s.sfi <= 400)) add(1, "Solar flux (SFI) is normally 65 (quiet sun) to 300 (very active); look up today's value.");
        if (!(s.kIndex >= 0 && s.kIndex <= 9)) add(2, "K index: 0 to 9.");
    }
    if (!(s.rainMmH >= 0)) add(2, "Rain rate: 0 mm/h or more.");
    else if (s.rainMmH > 200) add(1, "More than 200 mm/h of rain is a cloudburst; 25 mm/h is heavy rain, 100 tropical.");
    if (!s.useNow && (s.month < 1 || s.month > 12 || s.day < 1 || s.day > 31 || s.hour < 0 || s.hour > 23 || s.minute < 0 || s.minute > 59))
        add(2, "Date and time: month 1-12, day 1-31, hour 0-23, minute 0-59 (UTC).");
    if (s.radio > 0 && s.radio < (int)radios().size()) {
        const Radio& r = radios()[s.radio];
        if (s.fMHz < r.fMinMHz || s.fMHz > r.fMaxMHz) add(1, fmt("The %s does not tune %.4g MHz (%.4g to %.4g MHz).", r.name, s.fMHz, r.fMinMHz, r.fMaxMHz));
        if (s.radioGainPct < 30) add(0, "A low gain setting raises the radio's noise figure; turn the gain up until strong signals overload.");
    }
    if (s.preset >= 0 && s.preset < (int)presets().size()) {
        const Preset& p = presets()[s.preset];
        if (p.fMaxMHz > 0 && (s.fMHz < p.fMinMHz - 1e-9 || s.fMHz > p.fMaxMHz + 1e-9))
            add(0, fmt("%.4g MHz is outside the usual %s band (%.4g to %.4g MHz).", s.fMHz, p.name, p.fMinMHz, p.fMaxMHz));
    }
    return v;
}

bool hasErrors(const std::vector<Issue>& v) {
    for (const Issue& i : v) if (i.level >= 2) return true;
    return false;
}

int rating(double marginDb) { return marginDb >= 10 ? 2 : marginDb >= 0 ? 1 : 0; }
const char* ratingName(int r) { return r >= 2 ? "good" : r == 1 ? "fair" : "poor"; }

std::string kmText(double km) {
    if (km < 1) return fmt("%.0f m", std::max(km, 0.0) * 1000.0);
    if (km < 10) return fmt("%.1f km", km);
    if (km < 100) return fmt("%.0f km", km);
    if (km < 1000) return fmt("%.0f km", std::round(km / 5) * 5);
    return fmt("%.0f km", std::round(km / 50) * 50);
}

Verdict verdict(const Params& p, const Range& r, const Point* q, const char* rxLabel) {
    Verdict v;
    const bool hf = p.fMHz < 30;
    if (!r.any) v.headline = "With these settings it will not be heard anywhere. Try more power, a better antenna or a mode that needs less SNR.";
    else if (r.spans.size() >= 2) {
        std::string s = "Heard up to about " + kmText(r.spans[0].second);
        for (size_t i = 1; i < r.spans.size() && i < 3; i++) s += ", then again from " + kmText(r.spans[i].first) + " to " + kmText(r.spans[i].second);
        s += hf ? " (the gap is the skip zone)." : ".";
        v.headline = s;
    } else if (r.spans[0].first > 0.1)
        v.headline = "Not heard close in (skip zone); heard from about " + kmText(r.spans[0].first) + " to " + kmText(r.spans[0].second) + ".";
    else
        v.headline = "You should hear it up to about " + kmText(r.maxKm) + ".";
    if (!q) return v;
    v.rating = rating(q->marginDb);
    const char* who = rxLabel && *rxLabel ? rxLabel : "the receiver";
    if (q->marginDb >= 0) v.atRx = fmt("At %s (%s away): %s, %.0f dB to spare.", who, kmText(q->distKm).c_str(), ratingName(v.rating), q->marginDb);
    else v.atRx = fmt("At %s (%s away): poor, %.0f dB short of what is needed.", who, kmText(q->distKm).c_str(), -q->marginDb);
    auto add = [&](std::string s) { if (v.reasons.size() < 4) v.reasons.push_back(std::move(s)); };
    if (!hf) {
        switch (q->model) {
            case Model::Terrain: add(fmt("Hills between you cost about %.0f dB.", q->excessDb)); break;
            case Model::Diffraction:
                if (q->distKm > q->horizonKm) add(fmt("It is beyond the radio horizon (%s for these antenna heights): the signal bends over the earth and fades fast.", kmText(q->horizonKm).c_str()));
                else add(fmt("Near the radio horizon (%s): the earth's bulge starts to get in the way.", kmText(q->horizonKm).c_str()));
                break;
            case Model::TwoRay: add(fmt("Line of sight, but the ground reflection weakens it with distance (%.0f dB beyond free space). Higher antennas help most.", q->excessDb)); break;
            case Model::Troposcatter:
                add(fmt("Beyond the radio horizon (%s): it arrives by troposcatter, weak and fading (%.0f dB beyond free space).", kmText(q->horizonKm).c_str(), q->excessDb));
                break;
            case Model::Ducting:
                add(fmt("Only when the air forms a duct (super-refraction, %.0f %% of the time here or less): normally it is not heard.", q->beta0));
                break;
            default:
                if (q->distKm > q->horizonKm) add(fmt("Beyond the radio horizon (%s), but still in reach.", kmText(q->horizonKm).c_str()));
                else add("Clear line of sight.");
                break;
        }
        if (q->flatEarth) add("No terrain is loaded, so hills are ignored: download elevation tiles (Advanced > Terrain) for a realistic answer.");
        if (q->clutterDb >= 3) add(fmt("Buildings or trees around the receiver cost about %.0f dB.", q->clutterDb));
        if (q->rainDb >= 3) add(fmt("Rain costs %.0f dB.", q->rainDb));
        if (q->polDb > 0) add(fmt("The antennas have different polarisation (%.0f dB lost).", q->polDb));
    } else {
        if (q->hops == 0 && q->model != Model::HfSky) {
            if (p.fMHz > q->muf && q->distKm > 30) add(fmt("No skywave now: %.4g MHz is above the MUF (about %.1f MHz) of this path. Try a lower band or another time of day.", p.fMHz, q->muf));
            else add("Only the ground wave reaches here.");
        } else if (q->model == Model::HfSky) {
            add(fmt("By skywave, %d hop%s off the %s layer (MUF about %.1f MHz).", q->hops, q->hops == 1 ? "" : "s", q->hfLayer == 'E' ? "E" : "F2", q->muf));
            if (q->hfReliability >= 0)
                add(fmt("Works on about %.0f %% of the days this month at this hour (ITU-R P.533 reliability; band open on %.0f %% of days).",
                        100 * q->hfReliability, 100 * q->hfMufProb));
            if (q->aboveMufDb > 1) add(fmt("%.4g MHz is above the MUF: only scattered signal gets through (%.0f dB).", p.fMHz, q->aboveMufDb));
            if (q->absorptionDb >= 15) add(fmt("Daytime D-layer absorption costs %.0f dB; it eases after sunset or on a higher band.", q->absorptionDb));
        } else if (q->hops > 0) add("Ground wave and skywave both arrive; the ground wave is stronger here.");
        for (size_t i = 1; i < r.spans.size(); i++)
            if (q->distKm > r.spans[i - 1].second && q->distKm < r.spans[i].first) { add("You are in the skip zone: too far for the ground wave, too close for the skywave."); break; }
    }
    if (q->faDb > q->sysNfDb + 3) add(fmt("Local noise is %.0f dB above the receiver's own at this frequency: a quieter place or antenna helps more than a better radio.", q->faDb - q->sysNfDb));
    else if (q->marginDb < 10) add("The receiver's own noise sets the limit: a better antenna, a preamplifier or less feeder loss would help.");
    return v;
}

std::vector<BudgetRow> budget(const Params& p, const Point& q) {
    std::vector<BudgetRow> v;
    const double ptx = p.txPowerDbw + 30;
    v.push_back({"Transmitter power", ptx, "dBm", fmt("%.3g W into the feeder. dBm = 10 log10(P / 1 mW).", std::pow(10.0, p.txPowerDbw / 10))});
    v.push_back({"TX feeder loss", -p.txFeederDb, "dB", "Cable and connector loss between the transmitter and its antenna."});
    v.push_back({"TX antenna gain toward the receiver", q.txGainDbi, "dBi",
                 p.txPattern ? "From the antenna pattern file, at this bearing (and the skywave take-off angle on HF)." : "Gain over an isotropic antenna; a half-wave dipole is 2.15 dBi."});
    v.push_back({"EIRP", ptx - p.txFeederDb + q.txGainDbi, "dBm", "Effective isotropic radiated power = power - feeder loss + antenna gain (ERP = EIRP - 2.15 dB).", true});
    v.push_back({"Free-space loss", -q.fsDb, "dB", "ITU-R P.525: 32.45 + 20 log10 f(MHz) + 20 log10 d(km). Spreading alone, no ground."});
    std::string ex;
    switch (q.model) {
        case Model::TwoRay: ex = "Ground reflection (two-ray / plane earth): beyond the breakpoint 4 pi h1 h2 / lambda the field falls as 1/d^2."; break;
        case Model::Diffraction: ex = "ITU-R P.526 smooth-earth diffraction: the earth's bulge (4/3 radius) blocks the path; it blends in before the horizon."; break;
        case Model::Terrain: ex = "Terrain from your elevation file: delta-Bullington knife-edge diffraction (P.526 / P.1812)."; break;
        case Model::HfGround: ex = "HF ground wave: Norton surface-wave attenuation over the chosen ground, with the earth's curvature beyond it."; break;
        case Model::HfSky:
            ex = p.hfLegacy ? "HF skywave (3.9 model): free space over the slant path, D-layer absorption, above-MUF loss, 2 dB per extra ground reflection, 8.7 dB excess."
                            : "ITU-R P.533-14: the power sum of up to 3 E and 6 F2 modes, each free space over its virtual slant range + absorption + above-MUF "
                              "loss + ground reflections + auroral loss + 8.72 dB; beyond 7000 km the composite long-path formula.";
            break;
        case Model::Troposcatter: ex = "Troposcatter (ITU-R P.1812 4.4 / P.452): scattering by the turbulent troposphere beyond the horizon."; break;
        case Model::Ducting: ex = "Ducting / layer reflection (ITU-R P.1812 4.5): anomalous refraction traps the signal for a small % of the time."; break;
        default: ex = "Line of sight with clearance: nothing beyond free space."; break;
    }
    if (p.fMHz >= 30 && p.pathModel == PathModel::Auto)
        ex += fmt(" ITU-R P.1812-8 for %.0f %% of the time and %.0f %% of locations (k = %.2f; diffraction %.1f dB, troposcatter %.0f dB, ducting %.0f dB basic loss)%s.",
                  p.timePct, p.locPct, q.kFactor, q.diffDb, q.tropoDb, q.ductDb, q.flatEarth ? ", flat sea-level profile: no terrain" : "");
    v.push_back({std::string("Excess loss (") + pointModelName(q.model) + ")", -q.excessDb, "dB", ex});
    if (p.fMHz < 30) {
        v.push_back({"  of which D-layer absorption", -q.absorptionDb, "dB", "CCIR / ITU-R P.533 form: 677.2 (1 + 0.0037 R) cos(0.881 chi)^1.3 sec(i) / ((f + fH)^1.98 + 10.2) per hop; strongest at noon on the low bands."});
        v.push_back({"Skywave MUF", q.muf, "MHz", p.hfLegacy ? "Maximum usable frequency of the path: foF2 times the secant of the angle at the layer."
                                                             : "Median operational MUF (P.533 3.7 / P.1240): the basic MUF of the lowest-order mode from the CCIR maps, times Rop."});
        if (q.hfReliability >= 0) {
            v.push_back({"MUF on 90 % / 10 % of days", q.hfOwf, "MHz", fmt("The MUF exceeded on 90 %% of the days (the optimum working frequency) is %.1f MHz; on 10 %% of days it reaches %.1f MHz (P.1239 decile factors).", q.hfOwf, q.hfHpf)});
            v.push_back({"Circuit reliability", 100 * q.hfReliability, "%", "P.533 section 10 / P.842: the share of days this month at this hour on which the SNR reaches the required value, from the day-to-day and within-the-hour spread of signal and noise."});
        }
        v.push_back({"Skywave hops", (double)q.hops, "", "Number of F2 hops (0: no skywave). Each hop is at most about 3200 km (3 degree take-off angle)."});
    }
    v.push_back({"Receiver clutter", -q.clutterDb, "dB", "ITU-R P.2108 terminal clutter loss for a receive antenna below the buildings or trees around it."});
    if (p.fMHz >= 1000) {
        v.push_back({"Rain", -q.rainDb, "dB", "ITU-R P.838-3 specific attenuation k R^alpha (full regression) over an effective rain-cell length, for the rain rate you typed."});
        v.push_back({"Gases", -q.gasDb, "dB", "Oxygen and water vapour, ITU-R P.676-13 line by line at sea level (15 C, 7.5 g/m3): small below 10 GHz, peaks at 22 and 60 GHz."});
    }
    v.push_back({"Polarisation mismatch", -q.polDb, "dB", "Crossed linear antennas lose about 20 dB, linear to circular 3 dB. On HF the ionosphere mixes polarisation (in the excess loss)."});
    v.push_back({"Total path loss", -q.pathLossDb, "dB", "The sum of the losses above (basic transmission loss).", true});
    v.push_back({"RX antenna gain", p.rxGainDbi, "dBi", "Gain of the receive antenna toward the transmitter."});
    v.push_back({"Received power (antenna)", q.rxDbm, "dBm", fmt("EIRP - path loss + RX gain, at the antenna terminals: %.3g uV into 50 ohms.", dbmToUv50(q.rxDbm)), true});
    v.push_back({"Field strength", q.fieldDbuvm, "dBuV/m", "E = Pr - Gr + 20 log10 f(MHz) + 77.2 (the usual broadcast planning unit)."});
    v.push_back({"External noise Fa", q.faDb, "dB", "Man-made (by environment), galactic and atmospheric noise in dB above kTB (ITU-R P.372, simplified); 0 dB = 290 K."});
    v.push_back({"Receive chain noise figure", q.sysNfDb, "dB",
                 p.preamp ? "Friis: preamplifier, then feeder loss, then the radio, referred to the antenna." : "Friis: feeder loss plus the radio's noise figure (at its gain setting), referred to the antenna."});
    v.push_back({"Noise floor", q.noiseDbm, "dBm", fmt("kTB (-174 dBm/Hz + 10 log10 %.0f Hz) x (fa + f - 1): the external and the receiver noise together.", p.bandwidthHz)});
    v.push_back({"Sensitivity", q.noiseDbm + p.requiredSnrDb, "dBm", "The weakest signal that works: noise floor + required SNR."});
    v.push_back({"SNR", q.snrDb, "dB", "Received power - noise floor.", true});
    v.push_back({"Required SNR", p.requiredSnrDb, "dB", "What the mode needs in this bandwidth (e.g. 12 dB narrow FM, -21 dB FT8, -20 dB LoRa SF12)."});
    v.push_back({"Margin", q.marginDb, "dB", "SNR - required SNR. Keep 10 dB or more for fading and the things not modelled.", true});
    return v;
}

Series sweepSignalVsDistance(const Params& p, double maxKm, int n, double bearing) {
    Series s; s.name = "SNR"; s.xLabel = "distance (km)"; s.yLabel = "SNR (dB)"; s.logX = true;
    n = std::max(n, 2);
    const double d0 = 0.1, d1 = std::max(maxKm, 1.0);
    for (int i = 0; i < n; i++) {
        const double d = d0 * std::pow(d1 / d0, (double)i / (n - 1));
        s.x.push_back(d);
        s.y.push_back(std::max(evaluate(p, d, bearing).snrDb, -200.0));
    }
    return s;
}

Series sweepRangeVsPower(const Params& p, int n, double bearing) {
    Series s; s.name = "range"; s.xLabel = "transmitter power (W)"; s.yLabel = "range (km)"; s.logX = true;
    n = std::max(n, 2);
    for (int i = 0; i < n; i++) {
        Params q = p;
        q.txPowerDbw = p.txPowerDbw - 20 + 40.0 * i / (n - 1);
        const Range r = findRange(q, bearing);
        s.x.push_back(std::pow(10.0, q.txPowerDbw / 10));
        s.y.push_back(r.any ? r.maxKm : 0);
    }
    return s;
}

Series sweepRangeVsFrequency(const Params& p, double f0, double f1, int n, double bearing) {
    Series s; s.name = "range"; s.xLabel = "frequency (MHz)"; s.yLabel = "range (km)"; s.logX = true;
    n = std::max(n, 2);
    f0 = std::max(f0, 0.1); f1 = std::max(f1, f0 * 1.01);
    for (int i = 0; i < n; i++) {
        Params q = p;
        q.fMHz = f0 * std::pow(f1 / f0, (double)i / (n - 1));
        const Range r = findRange(q, bearing);
        s.x.push_back(q.fMHz);
        s.y.push_back(r.any ? r.maxKm : 0);
    }
    return s;
}

HfDay hfDay(const Params& p, double distKm, double bearing) {
    HfDay d;
    for (int i = 0; i <= 48; i++) {
        Params q = p;
        q.utcHour = i * 0.5;
        q.fMHz = 10;   // any HF frequency: the MUF does not depend on it
        const Point a = evaluate(q, distKm, bearing);
        d.hour.push_back(q.utcHour);
        d.muf.push_back(a.muf);
        d.fot.push_back(a.hfReliability >= 0 ? a.hfOwf : 0.85 * a.muf);   // P.533: the MUF exceeded on 90 % of days
        // LUF: the lowest frequency whose skywave alone meets the required SNR (scan 1.5 MHz .. the MUF)
        double luf = 0;
        Params s = q;
        s.hfModel = HfModel::SkyOnly;
        const double top = std::min(a.muf, 29.9);
        for (int k = 0; k < 40 && top > 1.5; k++) {
            s.fMHz = 1.5 * std::pow(top / 1.5, k / 39.0);
            if (evaluate(s, distKm, bearing).marginDb >= 0) { luf = s.fMHz; break; }
        }
        d.luf.push_back(luf);
    }
    return d;
}

const std::vector<AntennaPreset>& antennaPresets() {
    static const std::vector<AntennaPreset> v = {
        {"isotropic (reference)", 0.0, "0 dBi by definition."},
        {"rubber duck (handheld)", -3.0, "A short helical whip: -5 to 0 dBi, worse held close to the body."},
        {"quarter-wave whip / ground plane", 1.5, "On a car roof or with radials: about 1 to 2 dBi toward the horizon."},
        {"half-wave dipole", 2.15, "The classic reference: 2.15 dBi."},
        {"5/8-wave vertical", 3.2, "About 3 dBi toward the horizon on a good ground plane."},
        {"discone (wideband)", 0.0, "0 to 2 dBi over a decade of frequency."},
        {"collinear (2 x 5/8)", 5.5, "A base-station vertical, 5 to 6 dBi."},
        {"3-element yagi", 7.5, "About 7 to 8 dBi."},
        {"7-element yagi", 11.0, "About 10 to 12 dBi."},
        {"quad / log-periodic", 6.5, "A log-periodic is about 6 to 7 dBi over its range."},
        {"HF inverted V / dipole at 10 m", 6.0, "Over ground at a sensible height the low-angle gain is about 5 to 7 dBi."},
        {"HF vertical (quarter wave, radials)", 0.0, "About 0 dBi at low angles over average ground, more over sea."},
        {"loop on a window (receive)", -15.0, "A small receiving loop: low gain, but often a lower noise pickup."},
    };
    return v;
}

const std::vector<CablePreset>& cablePresets() {
    static const std::vector<CablePreset> v = {
        {"RG-174", 2.71, 0.0144},    // 100 MHz 29 dB, 1000 MHz 100 dB per 100 m
        {"RG-58", 1.536, 0.00643},   // 16 / 55
        {"RG-8X (mini-8)", 1.052, 0.0098},   // 11.5 / 43
        {"H155", 1.067, 0.00526},    // 11.2 / 39
        {"RG-213", 0.643, 0.0057},   // 7 / 26
        {"Aircell 7", 0.643, 0.00167},   // 6.6 / 22
        {"LMR-400 / Ecoflex 10", 0.38, 0.00135},   // about 3.9 / 13.4
        {"RG-6 (75 ohm TV / satellite)", 0.55, 0.0044},   // 6 / 22
        {"1/2 inch hardline", 0.2, 0.0007},   // 2.1 / 7
    };
    return v;
}

double cableLossDb(const CablePreset& c, double fMHz, double lengthM) {
    const double f = std::max(fMHz, 0.0);
    return std::max(lengthM, 0.0) / 100.0 * (c.a * std::sqrt(f) + c.b * f);
}

std::vector<Series> sweepLossByModel(const Params& p, double maxKm, int n, double bearing) {
    std::vector<Series> out;
    struct V { const char* name; int model; double t, l; };
    std::vector<V> vs;
    if (p.fMHz >= 30) {
        vs = {{"free space (P.525)", (int)PathModel::FreeSpace, 50, 50}, {"two-ray ground reflection", (int)PathModel::TwoRayExact, 50, 50},
              {"P.1812 median (50 % time, 50 % locations)", (int)PathModel::Auto, 50, 50},
              {"P.1812 90 % of locations", (int)PathModel::Auto, 50, 90}, {"P.1812 1 % of the time (enhanced)", (int)PathModel::Auto, 1, 50}};
    } else {
        vs = {{"current HF model", (int)p.pathModel, p.timePct, p.locPct}};
    }
    n = std::max(n, 2);
    const double d0 = 0.1, d1 = std::max(maxKm, 0.2);
    for (const V& v : vs) {
        Series s; s.name = v.name; s.xLabel = "distance (km)"; s.yLabel = "path loss (dB)"; s.logX = true;
        Params q = p; q.pathModel = (PathModel)v.model; q.timePct = v.t; q.locPct = v.l;
        for (int i = 0; i < n; i++) {
            const double d = d0 * std::pow(d1 / d0, (double)i / (n - 1));
            s.x.push_back(d); s.y.push_back(evaluate(q, d, bearing).pathLossDb);
        }
        out.push_back(std::move(s));
    }
    return out;
}

ProfileView profileView(const Params& p, double rxLat, double rxLon, int n) {
    ProfileView v;
    n = std::max(n, 3);
    std::vector<double> d, h;
    if (p.terrain && !p.terrain->empty()) {
        const Profile pr = pathProfile(*p.terrain, p.txLat, p.txLon, rxLat, rxLon, n);
        if (pr.complete && pr.dKm.size() >= 3) { d = pr.dKm; h = pr.hM; v.terrain = true; }
    }
    if (!v.terrain) {
        const double D = greatCircleKm(p.txLat, p.txLon, rxLat, rxLon);
        for (int i = 0; i < n; i++) { d.push_back(D * i / (n - 1)); h.push_back(0); }
    }
    const double D = std::max(d.back(), 1e-3);
    v.kFactor = itu::k50FromDeltaN(p.deltaN);
    const double hts = h.front() + p.txHeightM, hrs = h.back() + p.rxHeightM, fG = std::max(p.fMHz / 1000.0, 1e-4);
    for (size_t i = 0; i < d.size(); i++) {
        const double x = d[i];
        v.dKm.push_back(x);
        v.groundM.push_back(h[i] + x * (D - x) / (2 * v.kFactor * 6371.0) * 1000.0);   // the bulge of an earth of radius k R
        const double los = hts + (hrs - hts) * x / D;
        const double r1 = 17.32 * std::sqrt(x * (D - x) / (fG * D));                    // first Fresnel zone radius, m
        v.losM.push_back(los); v.fresnelM.push_back(r1);
        if (i > 0 && i + 1 < d.size() && r1 > 0) {
            const double c = (los - v.groundM.back()) / r1 * 100;
            if (c < v.worstClearancePct) { v.worstClearancePct = c; v.worstKm = x; }
        }
    }
    return v;
}

hf::Path hfPathFor(const Params& p, double rxLat, double rxLon) {
    hf::Path hp;
    hp.txLat = p.txLat; hp.txLon = p.txLon; hp.rxLat = rxLat; hp.rxLon = rxLon;
    hp.year = p.year; hp.month = p.month; hp.day = p.day; hp.utcHour = p.utcHour;
    hp.r12 = p.r12 >= 0 ? p.r12 : hf::r12FromSfi(p.sfi);
    hp.sfi12 = p.sfi;
    hp.fMHz = p.fMHz;
    hp.txPowerDbw = p.txPowerDbw - p.txFeederDb;
    hp.txGainDbi = p.txGainDbi; hp.rxGainDbi = p.rxGainDbi;
    hp.bandwidthHz = p.bandwidthHz; hp.requiredSnrDb = p.requiredSnrDb;
    hp.noiseEnv = (int)p.env; hp.atmosphericNoise = p.atmosphericNoise;
    const double nf = cascadeNfDb(p.rxFeederDb, p.rxNfDb, p.preamp, p.preampGainDb, p.preampNfDb);
    hp.extraNoiseDb = 10 * std::log10(std::max(std::pow(10.0, nf / 10) - 1, 1e-9));
    return hp;
}

hf::Chart hfChartFor(const Params& p, double rxLat, double rxLon, const std::vector<double>& f) { return hf::chart(hfPathFor(p, rxLat, rxLon), f); }

std::string budgetCsv(const std::vector<BudgetRow>& a, const std::vector<BudgetRow>* b) {
    std::string s = b ? "term,A,B,unit\n" : "term,value,unit\n";
    for (const BudgetRow& r : a) {
        s += "\"" + r.label + "\"," + fmt("%.2f", r.value);
        if (b) {
            const BudgetRow* m = nullptr;
            for (const BudgetRow& x : *b) if (x.label == r.label) { m = &x; break; }
            s += m ? fmt(",%.2f", m->value) : std::string(",");
        }
        s += "," + r.unit + "\n";
    }
    return s;
}

std::string seriesCsv(const std::vector<Series>& v) {
    std::string s;
    for (const Series& x : v) {
        s += "\"" + x.xLabel + "\",\"" + x.yLabel + " (" + x.name + ")\"\n";
        for (size_t i = 0; i < x.x.size() && i < x.y.size(); i++) s += fmt("%.6g,%.3f\n", x.x[i], x.y[i]);
        s += "\n";
    }
    return s;
}

std::string hfDayCsv(const HfDay& d) {
    std::string s = "utc_hour,muf_mhz,fot_mhz,luf_mhz\n";
    for (size_t i = 0; i < d.hour.size(); i++) s += fmt("%.1f,%.2f,%.2f,%.2f\n", d.hour[i], d.muf[i], d.fot[i], d.luf[i]);
    return s;
}

} // namespace dect2::prop
