// Automatic gain, part 1: the radio models (gain chains with their step tables) and the stage planner. See docs/AUTOGAIN.md.
// The noise figures and IIP3 values are typical figures from the parts' data sheets and published measurements, good enough to rank two
// splits of the same overall gain; they are not calibrated per unit.
#include "dect2/source/autogain.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace dect2::ag {

const char* stageKindName(StageKind k) {
    switch (k) {
    case StageKind::Lna: return "LNA";
    case StageKind::Mixer: return "mixer";
    case StageKind::Vga: return "VGA";
    case StageKind::If: return "IF";
    case StageKind::Amp: return "amp";
    case StageKind::Atten: return "attenuator";
    default: return "gain";
    }
}
const char* policyName(Policy p) {
    switch (p) {
    case Policy::Sensitivity: return "sensitivity";
    case Policy::Linearity: return "linearity";
    default: return "balanced";
    }
}

// ------------------------------------------------------------------------------------------------------------------------------ GainStage
double GainStage::snap(double db) const {
    if (steps.empty()) return db;
    double best = steps.front();
    for (double v : steps) if (std::fabs(v - db) < std::fabs(best - db) - 1e-9) best = v;
    return best;
}
double GainStage::snapDown(double db) const {
    if (steps.empty()) return db;
    double best = steps.front();
    for (double v : steps) if (v <= db + 1e-6) best = v;
    return best;
}
int GainStage::indexOf(double db) const {
    int bi = 0;
    for (int i = 0; i < (int)steps.size(); i++) if (std::fabs(steps[(size_t)i] - db) < std::fabs(steps[(size_t)bi] - db) - 1e-9) bi = i;
    return bi;
}

int RadioProfile::stageIndex(const std::string& sid) const {
    for (int i = 0; i < (int)stages.size(); i++) if (stages[(size_t)i].id == sid) return i;
    return -1;
}
int RadioProfile::settableCount(bool incUser) const {
    int n = 0;
    for (const auto& s : stages) if (s.settable && (incUser || !s.userOnly)) n++;
    return n;
}
bool RadioProfile::calibrated() const {
    for (const auto& s : stages) if (s.settable && !s.actualDb.empty() && s.actualDb.size() == s.steps.size()) return true;
    return false;
}
// the real gain of an overall setting: linear interpolation in the measured table (the table is by step, ascending)
static double realOf(const GainStage& s, double nominal) {
    if (s.actualDb.empty() || s.actualDb.size() != s.steps.size() || s.steps.size() < 2) return nominal;
    if (nominal <= s.steps.front()) return s.actualDb.front() + (nominal - s.steps.front());
    if (nominal >= s.steps.back()) return s.actualDb.back() + (nominal - s.steps.back());
    size_t i = 1;
    while (i + 1 < s.steps.size() && s.steps[i] < nominal) i++;
    const double span = s.steps[i] - s.steps[i - 1];
    const double f = span > 1e-9 ? (nominal - s.steps[i - 1]) / span : 0;
    return s.actualDb[i - 1] + f * (s.actualDb[i] - s.actualDb[i - 1]);
}
double RadioProfile::realDelta(double a, double b) const {
    if (settableCount() != 1) return b - a;
    for (const auto& s : stages) if (s.settable) return realOf(s, b) - realOf(s, a);
    return b - a;
}
double RadioProfile::minTotal(bool ampOn) const {
    double t = 0;
    for (const auto& s : stages) {
        if (!s.settable) continue;
        if (s.userOnly) { if (ampOn) t += s.maxDb(); continue; }
        t += s.minDb();
    }
    return t;
}
double RadioProfile::maxTotal(bool ampOn) const {
    double t = 0;
    for (const auto& s : stages) {
        if (!s.settable) continue;
        if (s.userOnly) { if (ampOn) t += s.maxDb(); continue; }
        t += s.maxDb();
    }
    return t;
}

// ------------------------------------------------------------------------------------------------------------------------------ tables
namespace {

std::vector<double> range(double lo, double hi, double step) {
    std::vector<double> v;
    if (!(step > 0)) step = 1;
    for (double x = lo; x <= hi + 1e-6; x += step) v.push_back(std::round(x * 10) / 10);
    if (v.empty() || v.back() < hi - 1e-6) v.push_back(hi);
    return v;
}
std::vector<double> tenths(std::initializer_list<int> t) {
    std::vector<double> v;
    for (int x : t) v.push_back(x / 10.0);
    return v;
}
const std::vector<double>& r820tTable() {   // librtlsdr's R820T / R828D gain list (tenths of dB)
    static const std::vector<double> t = tenths({0, 9, 14, 27, 37, 77, 87, 125, 144, 157, 166, 197, 207, 229, 254, 280, 297, 328, 338, 364, 372, 386, 402, 421, 434, 439, 445, 480, 496});
    return t;
}
const std::vector<double>& e4000Table() {
    static const std::vector<double> t = tenths({-10, 15, 40, 65, 90, 115, 140, 165, 190, 215, 240, 290, 340, 420});
    return t;
}

GainStage stage(const char* id, const char* label, StageKind k, std::vector<double> steps, double nf, double nfLow, double iip3, bool front, const char* note) {
    GainStage s;
    s.id = id; s.label = label; s.kind = k; s.steps = std::move(steps); s.nfDb = nf; s.nfLowGainDb = nfLow; s.iip3Dbm = iip3; s.front = front; s.note = note;
    return s;
}
GainStage info(GainStage s) { s.settable = false; return s; }
GainStage amp(double db, double nf, double iip3, const char* note) {
    GainStage s = stage("amp", "Amp", StageKind::Amp, {0, db}, nf, 0, iip3, true, note);
    s.userOnly = true;
    return s;
}

RadioProfile rtlProfile(const char* id, const char* name, const std::vector<double>& table, const char* notes, bool bias, bool remote) {
    RadioProfile r;
    r.id = id; r.name = name; r.adcBits = 8; r.hasBias = bias; r.remote = remote; r.driverSplits = true;
    r.settleSec = remote ? 0.7 : 0.3;
    r.stages.push_back(stage("overall", "Tuner gain", StageKind::Overall, table, 3.5, 6, -10, true,
                             "The tuner's gain list: the driver sets the tuner's LNA and mixer together (the IF VGA stays fixed). Low steps raise the noise figure a lot."));
    r.stages.push_back(info(stage("tlna", "Tuner LNA", StageKind::Lna, range(0, 30, 2), 3.5, 8, -12, true, "Inside the tuner, set by the driver with the overall gain.")));
    r.stages.push_back(info(stage("tmix", "Tuner mixer", StageKind::Mixer, range(0, 15, 1), 10, 0, 0, true, "Inside the tuner, set by the driver with the overall gain.")));
    r.stages.push_back(info(stage("tvga", "Tuner IF VGA", StageKind::Vga, range(0, 26, 1), 15, 0, 10, false, "Fixed by the driver in manual gain mode.")));
    r.notes = notes;
    return r;
}

std::vector<RadioProfile> buildProfiles() {
    std::vector<RadioProfile> v;
    {   // HackRF One / Pro
        RadioProfile r;
        r.id = "hackrf"; r.name = "HackRF One / Pro"; r.adcBits = 8; r.hasAmp = true; r.hasBias = true; r.settleSec = 0.25;
        r.stages.push_back(amp(14, 6.5, -5, "The +14 dB RF amplifier at the antenna: lowers the noise figure on weak signals but overloads (and can be damaged) on strong ones. Only you switch it."));
        r.stages.push_back(stage("lna", "LNA", StageKind::Lna, range(0, 40, 8), 8, 14, -18, true,
                                 "The IF LNA (MAX2837), 8 dB steps, before the baseband filter: sets the noise figure. Drop it first when a strong signal is near."));
        r.stages.push_back(stage("vga", "VGA", StageKind::Vga, range(0, 62, 2), 22, 0, 5, false,
                                 "The baseband VGA, 2 dB steps, after the channel filter: does not see neighbours outside the filter; fine trim of the ADC level."));
        r.ampNote = "Switching the amp on helps only weak signals with nothing strong in the band; it is the first stage a strong FM or GSM transmitter overloads.";
        r.notes = "8-bit ADC: about 48 dB of dynamic range. Prefer LNA high + VGA low for weak signals, LNA low + VGA high near strong ones.";
        v.push_back(r);
    }
    v.push_back(rtlProfile("rtl-r820t", "RTL-SDR (R820T / R820T2)", r820tTable(), "8-bit ADC. The R820T's 29 gain steps from 0 to 49.6 dB.", true, false));
    v.push_back(rtlProfile("rtl-blog-v3", "RTL-SDR Blog V3", r820tTable(), "R820T2 tuner, 8-bit ADC, bias-tee, direct sampling below 24 MHz (no tuner gain there).", true, false));
    v.push_back(rtlProfile("rtl-r828d", "RTL-SDR (R828D)", r820tTable(), "R828D tuner (three inputs), same gain list as the R820T.", true, false));
    v.push_back(rtlProfile("rtl-blog-v4", "RTL-SDR Blog V4", r820tTable(),
                           "R828D tuner with a built-in HF upconverter and filter bank (FM and AM notch): the strong-signal handling is better than the V3, the gain list the same.", true, false));
    v.push_back(rtlProfile("rtl-blog-v4l", "RTL-SDR Blog V4 (Lite / V4L)", r820tTable(), "R828D tuner as the V4; treated as a V4.", true, false));
    v.push_back(rtlProfile("rtl-e4000", "RTL-SDR (E4000)", e4000Table(), "E4000 tuner: gain list from -1 to 42 dB.", false, false));
    v.push_back(rtlProfile("rtltcp", "rtl_tcp (network RTL-SDR)", r820tTable(), "An RTL-SDR on the network: every gain change is a round trip, the loop waits longer.", false, true));
    {
        RadioProfile r;
        r.id = "airspy"; r.name = "Airspy R2 / Mini"; r.adcBits = 12; r.hasBias = true; r.driverSplits = true; r.settleSec = 0.25;
        r.stages.push_back(stage("overall", "Gain step", StageKind::Overall, range(0, 21, 1), 3.5, 7, -8, true,
                                 "libairspy's 22-step gain table (linearity or sensitivity, the radio's Gain mode setting): each step sets LNA, mixer and VGA together. Steps, not dB."));
        r.stages.push_back(info(stage("alna", "LNA", StageKind::Lna, range(0, 15, 1), 3.5, 8, -10, true, "R820T2 LNA, 0-15.")));
        r.stages.push_back(info(stage("amix", "Mixer", StageKind::Mixer, range(0, 15, 1), 9, 0, 0, true, "R820T2 mixer, 0-15.")));
        r.stages.push_back(info(stage("avga", "VGA", StageKind::Vga, range(0, 15, 1), 15, 0, 10, false, "IF VGA after the filter, 0-15.")));
        r.notes = "12-bit ADC. The Gain mode setting picks the table: Linearity (the default) for strong signals, Sensitivity for weak ones.";
        v.push_back(r);
    }
    {
        RadioProfile r;
        r.id = "airspyhf"; r.name = "Airspy HF+"; r.adcBits = 16; r.hasAmp = true; r.driverSplits = true; r.settleSec = 0.25;
        r.stages.push_back(stage("overall", "Gain", StageKind::Overall, range(0, 48, 6), 6, 6, 10, true,
                                 "Attenuator 0-48 dB in 6 dB steps as one gain; the top 6 dB switch the preamp on (only with your permission)."));
        r.ampNote = "The preamp is part of the top 6 dB of the slider; the auto modes stay at 42 dB unless you allow the amp.";
        r.notes = "Very high dynamic range: gain is rarely the limit; keep the preamp off near strong broadcast stations.";
        v.push_back(r);
    }
    {
        RadioProfile r;
        r.id = "sdrplay"; r.name = "SDRplay RSP (1A / 1B / 2 / dx / duo)"; r.adcBits = 14; r.hasBias = true; r.driverSplits = true; r.settleSec = 0.3;
        r.stages.push_back(stage("overall", "Gain", StageKind::Overall, range(0, 100, 1), 4, 10, -10, true,
                                 "One gain for OnAir: the driver turns it into an LNA state (RFGR) and an IF gain reduction (IFGR 20-59 dB). Reductions: more reduction = less gain."));
        GainStage rf = stage("rfgr", "RF gain reduction (LNA state)", StageKind::Lna, range(0, 9, 1), 4, 12, -10, true,
                             "The LNA state: 0 = the most gain. Raise the state (less gain) first on strong signals.");
        rf.reduction = true; rf.settable = false;
        r.stages.push_back(rf);
        GainStage ifg = stage("ifgr", "IF gain reduction", StageKind::If, range(20, 59, 1), 15, 0, 10, false, "IFGR 20-59 dB after the IF filter.");
        ifg.reduction = true; ifg.settable = false;
        r.stages.push_back(ifg);
        r.notes = "14-bit ADC (12 bits above 6 Msps). The SDRplay API counts gains as reductions; OnAir shows them as gains.";
        v.push_back(r);
    }
    auto overallRadio = [&](const char* id, const char* name, int bits, double lo, double hi, double step, double settle, bool bias, const char* stageNote,
                            std::vector<GainStage> infos, const char* notes) {
        RadioProfile r;
        r.id = id; r.name = name; r.adcBits = bits; r.hasBias = bias; r.driverSplits = true; r.settleSec = settle;
        r.stages.push_back(stage("overall", "Gain", StageKind::Overall, range(lo, hi, step), 4, 8, -10, true, stageNote));
        for (auto& s : infos) r.stages.push_back(info(s));
        r.notes = notes;
        v.push_back(r);
    };
    overallRadio("pluto", "ADALM-Pluto (AD9363)", 12, 0, 73, 1, 0.3, false, "AD9363 manual gain 0-73 dB: its gain table sets LNA, mixer, TIA and LPF together.",
                 {stage("plna", "LNA + mixer", StageKind::Lna, range(0, 30, 3), 3, 6, -14, true, "Front gain, from the chip's table."),
                  stage("plpf", "TIA + LPF", StageKind::If, range(0, 43, 1), 12, 0, 5, false, "Baseband gain after the filter.")},
                 "12-bit ADC. The chip's own table already puts the front gain first.");
    overallRadio("lime", "LimeSDR (LMS7002M)", 12, 0, 73, 1, 0.3, false, "LMS7002M overall gain: LNA 0-30, TIA 0-12, PGA -12..19 dB.",
                 {stage("llna", "LNA", StageKind::Lna, range(0, 30, 1), 2.5, 8, -10, true, "LNA 0-30 dB."),
                  stage("ltia", "TIA", StageKind::Mixer, {0, 9, 12}, 8, 0, 0, true, "TIA 0, 9 or 12 dB."),
                  stage("lpga", "PGA", StageKind::Vga, range(-12, 19, 1), 15, 0, 10, false, "PGA -12..19 dB after the filter.")},
                 "12-bit ADC.");
    overallRadio("usrp", "USRP B200 / B210 (AD9364)", 12, 0, 76, 1, 0.3, false, "AD9364 gain 0-76 dB through UHD.", {}, "12-bit ADC.");
    overallRadio("bladerf", "bladeRF 2.0 micro", 12, -15, 60, 1, 0.3, true, "AD9361 gain -15..60 dB (bladeRF 1: LNA + VGA1 + VGA2 as one).",
                 {stage("blna", "LNA", StageKind::Lna, {0, 3, 6}, 3, 4, -10, true, "bladeRF 1 LNA 0 / 3 / 6 dB.")}, "12-bit ADC.");
    overallRadio("fobos", "RigExpert Fobos SDR", 14, 0, 95, 1, 0.3, false, "LNA (0 / 16 / 33 dB) + VGA (0-62 dB, 2 dB steps) as one gain; the driver fills the LNA first.",
                 {stage("flna", "LNA", StageKind::Lna, {0, 16, 33}, 4, 8, -10, true, "0, 16 or 33 dB."),
                  stage("fvga", "VGA", StageKind::Vga, range(0, 62, 2), 15, 0, 10, false, "0-62 dB.")},
                 "14-bit ADC.");
    overallRadio("rx888", "RX888 / RX888 MkII", 16, 0, 90, 1, 0.3, true, "HF: attenuator 0-31.5 dB + AD8370 VGA; VHF/UHF: R828D tuner gain, both as one gain.",
                 {stage("ratt", "HF attenuator", StageKind::Atten, range(-31.5, 0, 0.5), 6, 0, 30, true, "Before the ADC on HF."),
                  stage("rvga", "VGA (AD8370)", StageKind::Vga, range(-11, 34, 1), 7, 0, 20, true, "HF IF gain.")},
                 "16-bit ADC at up to 64 Msps: the whole HF band at once wants headroom, not gain.");
    overallRadio("spyserver", "SpyServer (network)", 8, 0, 16, 1, 0.8, false, "The server's gain index (Airspy or RTL-SDR behind it).", {},
                 "A radio on the network: every change is a round trip; the server may refuse gain changes.");
    overallRadio("kiwisdr", "KiwiSDR (network)", 14, 0, 120, 1, 1.0, false, "The Kiwi's manual gain (its own AGC when 'Gain control' is automatic).", {}, "Shared by many listeners.");
    overallRadio("generic", "Other radio (one overall gain)", 12, 0, 60, 1, 0.3, false, "The driver's overall gain; the driver spreads it over its stages.", {}, "");
    for (auto& r : v) if (r.remote || r.id == "spyserver" || r.id == "kiwisdr") r.remote = true;
    return v;
}

bool has(const std::string& s, const char* what) {
    std::string a = s, b = what;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

} // namespace

const std::vector<RadioProfile>& radioProfiles() {
    static const std::vector<RadioProfile> v = buildProfiles();
    return v;
}
const RadioProfile* radioProfileById(const std::string& id) {
    for (const auto& r : radioProfiles()) if (r.id == id) return &r;
    return nullptr;
}

RadioProfile genericProfile(const std::string& name, double lo, double hi, double step) {
    RadioProfile r = *radioProfileById("generic");
    r.name = name.empty() ? r.name : name;
    if (!(hi > lo)) hi = lo + 1;
    r.stages[0].steps = range(lo, hi, step > 0 ? step : 1);
    return r;
}

RadioProfile radioProfileFor(const DeviceInfo& d) {
    auto pick = [&](const char* id) { return *radioProfileById(id); };
    auto withRange = [&](RadioProfile r) {   // the driver's reported range wins over the table's (a different model or firmware)
        if (r.stages.empty() || r.stages[0].id != "overall" || !(d.gainMaxDb > d.gainMinDb)) return r;
        GainStage& s = r.stages[0];
        if (std::fabs(s.minDb() - d.gainMinDb) > 0.6 || std::fabs(s.maxDb() - d.gainMaxDb) > 0.6) {
            const double step = s.steps.size() > 1 ? std::max(0.5, (s.maxDb() - s.minDb()) / (double)(s.steps.size() - 1)) : 1;
            s.steps = range(d.gainMinDb, d.gainMaxDb, std::min(step, 1.0));
        }
        return r;
    };
    if (d.kind == DeviceInfo::HackRF) return pick("hackrf");
    const std::string& b = d.board;
    if (b == "rtlsdr") {
        if (has(d.name, "e4000")) return pick("rtl-e4000");
        if (has(d.name, "v4l") || has(d.name, "v4 lite")) return pick("rtl-blog-v4l");
        if (has(d.name, "v4")) return pick("rtl-blog-v4");
        if (has(d.name, "v3") || has(d.name, "blog")) return pick("rtl-blog-v3");
        if (has(d.name, "r828d")) return pick("rtl-r828d");
        return pick("rtl-r820t");
    }
    if (b == "rtltcp") return pick("rtltcp");
    if (b == "airspy") return withRange(pick("airspy"));
    if (b == "airspyhf") return pick("airspyhf");
    if (b == "sdrplay" || b == "nrspst" || has(d.name, "sdrplay") || has(d.name, "rsp")) {
        RadioProfile r = withRange(pick("sdrplay"));
        for (const auto& rs : d.settings)   // a driver that lists the LNA state as a setting (SoapySDRPlay): OnAir sets it itself
            if (rs.key == "rfgr" && rs.values.size() > 2) {
                GainStage& rf = r.stages[(size_t)r.stageIndex("rfgr")];
                int states = 0;
                for (const auto& x : rs.values) if (x != "auto") states++;
                rf.steps = range(0, std::max(1, states - 1) * 6.0, 6);   // about 6 dB a state; gain = (top state - state) * 6
                rf.settable = true; rf.settingKey = "rfgr"; rf.settingIsIndex = true;
            }
        return r;
    }
    if (b == "pluto" || has(d.name, "pluto")) return withRange(pick("pluto"));
    if (b == "lime" || has(d.name, "lime")) return withRange(pick("lime"));
    if (b == "usrp" || has(d.name, "usrp")) return withRange(pick("usrp"));
    if (b == "bladerf" || has(d.name, "bladerf")) return withRange(pick("bladerf"));
    if (b == "fobos") return withRange(pick("fobos"));
    if (b == "rx888") return withRange(pick("rx888"));
    if (b == "spyserver") return withRange(pick("spyserver"));
    if (b == "kiwisdr") return withRange(pick("kiwisdr"));
    RadioProfile r = genericProfile(d.name, d.gainMinDb, d.gainMaxDb, 1);
    r.remote = d.isNetwork();
    if (r.remote) r.settleSec = 0.7;
    return r;
}

// ------------------------------------------------------------------------------------------------------------------------------ planner
namespace {
double dbToLin(double db) { return std::pow(10.0, db / 10.0); }
}

void cascade(const RadioProfile& r, StagePlan& p) {
    // Friis over the stages in order; an unset stage at its value in the plan. A stage's noise figure worsens as it is turned down
    // (nfLowGainDb at its lowest step, falling linearly to nfDb at its top); the ADC adds a stage of 30 dB NF at the end.
    double f = 1, g = 1;
    double iip3inv = 0, gFront = 1;
    for (size_t i = 0; i < r.stages.size() && i < p.db.size(); i++) {
        const GainStage& s = r.stages[i];
        if (!s.settable && r.driverSplits) continue;   // already inside the overall stage
        const double gdb = p.db[i];
        if (s.kind == StageKind::Amp && gdb <= 0) continue;   // bypassed amp: no noise, no gain
        const double span = s.maxDb() - s.minDb();
        const double frac = span > 0 ? (s.maxDb() - gdb) / span : 0;
        const double nf = s.nfDb + s.nfLowGainDb * std::max(0.0, std::min(1.0, frac));
        f += (dbToLin(nf) - 1) / g;
        if (s.front) { iip3inv += gFront / dbToLin(s.iip3Dbm); gFront *= dbToLin(gdb); }
        g *= dbToLin(gdb);
    }
    iip3inv += gFront / dbToLin(r.postIip3Dbm);
    f += (dbToLin(30) - 1) / std::max(g, 1e-3);
    p.nfDb = 10 * std::log10(f);
    p.iip3Dbm = iip3inv > 0 ? -10 * std::log10(iip3inv) : 30;
}

StagePlan planStages(const RadioProfile& r, double totalDb, Policy pol, uint32_t mask, const std::vector<double>& fixed, bool ampOn, double frontCap) {
    StagePlan p;
    p.db.assign(r.stages.size(), 0.0);
    p.ampOn = ampOn;
    std::vector<int> movable;
    double fixedSum = 0;
    for (size_t i = 0; i < r.stages.size(); i++) {
        const GainStage& s = r.stages[i];
        if (!s.settable) { p.db[i] = i < fixed.size() ? fixed[i] : s.minDb(); continue; }
        if (s.userOnly) { p.db[i] = ampOn ? s.maxDb() : 0; fixedSum += p.db[i]; continue; }
        if (!(mask & (1u << i))) { p.db[i] = s.snap(i < fixed.size() ? fixed[i] : s.minDb()); fixedSum += p.db[i]; continue; }
        movable.push_back((int)i);
    }
    double want = totalDb - fixedSum;
    if (movable.empty()) { p.totalDb = fixedSum; cascade(r, p); return p; }
    // everything at its lowest step, then the rest by the policy's order
    double base = 0;
    for (int i : movable) { p.db[(size_t)i] = r.stages[(size_t)i].minDb(); base += p.db[(size_t)i]; }
    double rest = want - base;
    std::vector<int> order = movable;   // signal order: front first
    if (pol == Policy::Linearity) std::reverse(order.begin(), order.end());
    double frontUsed = 0;
    for (int i : movable) if (r.stages[(size_t)i].front) frontUsed += p.db[(size_t)i];
    auto give = [&](int i, double budget) {
        const GainStage& s = r.stages[(size_t)i];
        double lim = s.maxDb();
        if (s.front) lim = std::min(lim, p.db[(size_t)i] + std::max(0.0, frontCap - frontUsed));
        const double target = std::min(lim, p.db[(size_t)i] + std::max(0.0, budget));
        const double v = std::max(s.minDb(), s.snapDown(target));
        const double add = v - p.db[(size_t)i];
        if (s.front) frontUsed += add;
        p.db[(size_t)i] = v;
        return add;
    };
    if (pol == Policy::Balanced && order.size() >= 2) {
        // the front stages to about 60 % of their range first, then the back stages, then whatever is left
        for (int i : order) {
            const GainStage& s = r.stages[(size_t)i];
            if (!s.front) continue;
            rest -= give(i, std::min(rest, (s.maxDb() - s.minDb()) * 0.6));
        }
    }
    for (int i : order) { if (rest <= 1e-6) break; rest -= give(i, rest); }
    // the remainder (smaller than every stage's step): the stage with the finest steps rounds to the nearest
    if (std::fabs(rest) > 1e-6) {
        int fine = -1; double fineStep = 1e9;
        for (int i : order) {
            const GainStage& s = r.stages[(size_t)i];
            const double st = s.steps.size() > 1 ? (s.maxDb() - s.minDb()) / (double)(s.steps.size() - 1) : 1e9;
            if (st < fineStep) { fineStep = st; fine = i; }
        }
        if (fine >= 0) {
            const GainStage& s = r.stages[(size_t)fine];
            double v = s.snap(p.db[(size_t)fine] + rest);
            if (s.front && frontUsed - p.db[(size_t)fine] + v > frontCap + 1e-6) v = s.snapDown(p.db[(size_t)fine] + rest);
            p.db[(size_t)fine] = v;
            // the greedy split can leave a total unreachable that another combination reaches (a full VGA with the LNA a step short): move
            // one coarse stage a step and let the fine stage make up the difference, when that comes closer to the wanted total
            for (int it = 0; it < 3; it++) {
                double got = 0;
                for (int i : movable) got += p.db[(size_t)i];
                const double err = want - got;
                if (std::fabs(err) <= fineStep * 0.5 + 1e-6) break;
                double bestErr = std::fabs(err);
                int bi = -1; double bv = 0, bf = 0;
                for (int c : order) {
                    if (c == fine) continue;
                    const GainStage& cs = r.stages[(size_t)c];
                    const int k = cs.indexOf(p.db[(size_t)c]);
                    for (int dk : {1, -1}) {
                        const int nk = k + (err > 0 ? dk : -dk);
                        if (nk < 0 || nk >= (int)cs.steps.size()) continue;
                        const double nv = cs.steps[(size_t)nk];
                        if (cs.front && frontUsed - p.db[(size_t)c] + nv > frontCap + 1e-6) continue;
                        const double nf = s.snap(p.db[(size_t)fine] - (nv - p.db[(size_t)c]) + err);
                        const double e2 = std::fabs(err - (nv - p.db[(size_t)c]) - (nf - p.db[(size_t)fine]));
                        if (e2 < bestErr - 1e-6) { bestErr = e2; bi = c; bv = nv; bf = nf; }
                    }
                }
                if (bi < 0) break;
                if (r.stages[(size_t)bi].front) frontUsed += bv - p.db[(size_t)bi];
                p.db[(size_t)bi] = bv; p.db[(size_t)fine] = bf;
            }
        }
    }
    p.totalDb = 0;
    for (size_t i = 0; i < r.stages.size(); i++) if (r.stages[i].settable) p.totalDb += p.db[i];
    cascade(r, p);
    return p;
}

std::vector<double> totalSteps(const RadioProfile& r, Policy pol, uint32_t mask, bool ampOn) {
    std::vector<double> out;
    const double lo = r.minTotal(ampOn), hi = r.maxTotal(ampOn);
    double fineStep = 1e9;
    for (size_t i = 0; i < r.stages.size(); i++) {
        const GainStage& s = r.stages[i];
        if (!s.settable || s.userOnly || !(mask & (1u << i)) || s.steps.size() < 2) continue;
        for (size_t k = 1; k < s.steps.size(); k++) fineStep = std::min(fineStep, s.steps[k] - s.steps[k - 1]);
    }
    if (fineStep > 1e8) fineStep = 1;
    fineStep = std::max(0.1, fineStep);
    std::vector<double> fixed;
    for (double t = lo; t <= hi + 1e-6; t += fineStep * 0.5) {
        const double got = planStages(r, t, pol, mask, fixed, ampOn).totalDb;
        if (out.empty() || got > out.back() + 0.05) out.push_back(got);
    }
    return out;
}

std::string stageSettingValue(const GainStage& s, double gainDb) {
    char b[32];
    if (s.settingIsIndex) {
        const int idx = s.indexOf(gainDb);
        const int fromTop = s.reduction ? (int)s.steps.size() - 1 - idx : idx;
        snprintf(b, sizeof b, "%d", fromTop);
    } else if (s.reduction) snprintf(b, sizeof b, "%g", s.maxDb() - gainDb);
    else snprintf(b, sizeof b, "%g", gainDb);
    return b;
}

} // namespace dect2::ag
