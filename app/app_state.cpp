// application state: preferences, device list, bandwidth, ingesting spectrum and receiver telemetry, channel list
#include "app.h"
#include "dect2/usb_diag.h"
#include "dect2/rate_choice.h"

// the name of the built-in test signal in the source list: it follows the mode
static std::string synthLabel(const App& a) {
    switch (a.family) {
    case 0: return "Synthetic test signal (DVB-T2 8K, 8 MHz)";
    case 1: return "Synthetic test signal (ATSC 8-VSB)";
    case 2: return "Synthetic test signal (DAB+, 4 services)";
    case 3: return "Synthetic test signal (ATSC 3.0, test card)";
    case 4: return "Synthetic test signal (ISDB-T)";
    case 5: return "Synthetic test signal (FM stereo, RDS)";
    case 13: return "Synthetic test signal (Radiosonde)";
    case 14: return "Synthetic test signal (AIS)";
    case 15: return "Synthetic test signal (Marine)";
    case 16: return "Synthetic test signal (ACARS)";
    case 17: return "Synthetic test signal (Inmarsat-C)";
    case 18: return "Synthetic test signal (Inmarsat Aero)";
    case 19: return "Synthetic test signal (Iridium)";
    case 20: return "Synthetic test signal (Mesh (LoRa))";
    default: { const ModeTuning* mt = modeTuning(a.family + 2); return std::string("Synthetic test signal (") + (mt ? mt->name : "?") + ")"; }
    }
}
static void nameSynth(App& a) { if (!a.devices.empty() && a.devices[0].kind == DeviceInfo::Synthetic) a.devices[0].name = synthLabel(a); }

// The scan tab starts on the channel raster of the TV mode (its first preset): an ATSC or ISDB-T scan on the 8 MHz DVB centres
// misses every channel, and the 6 MHz modes would be measured with the 8 MHz width
static void scanRaster(App& a, int f) {
    static int raster = 0;   // what scanCfg holds now: 0 DVB (the ScanConfig defaults), 1 the 6 MHz US raster, 2 ISDB-T, 3 DTMB
    const int r = f == 0 ? 0 : f == 1 || f == 3 ? 1 : f == 4 ? 2 : f == 7 ? 3 : -1;
    if (r < 0 || r == raster) return;
    raster = r; a.scanPreset = 0;
    ScanConfig& c = a.scanCfg;
    if (r == 0 || r == 3) { c.startMHz = 474; c.stopMHz = 858; c.stepMHz = 8; c.bwMhz = 8; }
    if (r == 1) { c.startMHz = 473; c.stopMHz = 605; c.stepMHz = 6; c.bwMhz = 6; }
    if (r == 2) { c.startMHz = 473.143; c.stopMHz = 767.143; c.stepMHz = 6; c.bwMhz = 6; }
}

// the per-family flags and a frequency that suits the mode (the settings and setFamily both use it)
static void setFamilyFlags(App& a, int f) {
    a.family = f; a.atscMode = f == 1 || f == 3 || f == 4; a.atsc3Mode = f == 3; a.isdbtMode = f == 4; a.dabMode = f == 2; a.fmMode = f == 5;
    if (f >= 6) {   // a mode added after FM: its tuning table says what is a sensible frequency
        const ModeTuning* mt = modeTuning(f + 2);
        if (mt && !(a.freqMhz >= mt->minMhz && a.freqMhz <= mt->maxMhz)) a.freqMhz = mt->defMhz;
    }
    scanRaster(a, f);
    nameSynth(a);
}

void setFamily(App& a, int f) {
    if (a.family >= 6 && a.family < kNumFamilies && f != a.family) a.famFreq[a.family] = a.freqMhz;   // each of the newer modes comes back on the frequency it was left on
    if (a.family >= 0 && a.family < kNumFamilies && f != a.family) {   // the gains that suit FM are not the ones for a TV channel: remember them per mode
        a.famGain[a.family] = {a.tune.lnaDb, a.tune.vgaDb, a.tune.ampOn, true};
        a.famBias[a.family] = a.tune.biasTee;
        a.tune.biasTee = a.famBias[f];
        const App::FamGain& g = a.famGain[f];
        if (g.known) { a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp; }
        else if (f == 5) { a.tune.lnaDb = 24; a.tune.vgaDb = 20; a.tune.ampOn = false; }   // FM stations are strong
        else if (a.family == 5 || f >= 6) { a.tune.lnaDb = 32; a.tune.vgaDb = 20; a.tune.ampOn = true; }
    }
    setFamilyFlags(a, f);
    if (f >= 6) {
        const ModeTuning* mt = modeTuning(f + 2);
        a.freqMhz = mt && a.famFreq[f] >= mt->minMhz && a.famFreq[f] <= mt->maxMhz ? a.famFreq[f] : mt ? mt->defMhz : a.freqMhz;
    }
    if (f == 0 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 522.0;       // a TV channel, not a leftover FM or DAB frequency
    if (f == 1 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 473.0;
    if (f == 3 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 522.0;
    if (f == 2 && !(a.freqMhz >= 174 && a.freqMhz <= 240)) a.freqMhz = 218.640;
    if (f == 4 && !(a.freqMhz >= 170 && a.freqMhz <= 770)) a.freqMhz = 473.143;   // the centre of UHF channel 13/14 of the 6 MHz raster
    if (f == 5 && !(a.freqMhz >= 87.5 && a.freqMhz <= 108)) a.freqMhz = 100.0;     // FM band, default to 100 MHz
}

// what to tell the engine: 0 auto, 1 DVB-T2, 2 DVB-T, 3 ATSC, 4 DAB, 5 ATSC 3.0, 6 ISDB-T, 7 FM, 8 DVB-S/S2, 9 DTMB, 10 analog TV, 11 DMR, 12 DRM, 13 ADS-B, 14 GNSS, 15 radiosonde, 16 AIS, 17 marine, 18 ACARS, 19 Inmarsat-C, 20 Inmarsat Aero, 21 Iridium, 22 mesh, 23 HD Radio, 24 CDR, 25 pagers, 26 APRS / packet, 27 HF digital (Engine::start maps these to activeStandard())
int engineStd(const App& a) { return a.family >= 6 ? a.family + 2 : a.family == 1 ? 3 : a.family == 2 ? 4 : a.family == 3 ? 5 : a.family == 4 ? 6 : a.family == 5 ? 7 : a.stdMode; }

void refreshDevices(App& a) {
    // the radio chosen before stays chosen when it is found again (a radio unplugged or added moves the others in the list)
    const bool hadRadio = a.devIdx >= 2 && a.devIdx < (int)a.devices.size();
    const std::string keep = hadRadio ? a.devices[a.devIdx].name : "";
    a.devices.clear();
    DeviceInfo s; s.kind = DeviceInfo::Synthetic; s.name = synthLabel(a); a.devices.push_back(s);
    DeviceInfo f; f.kind = DeviceInfo::File; f.name = "IQ recording file…"; a.devices.push_back(f);
    std::string err;
    // HackRF, the radios with a native driver (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP) and the rest through SoapySDR
    for (auto& d : listRadios(err)) a.devices.push_back(d);
    a.hackrfErr = err;
    a.hackrfErrHidden = false;
    a.usbHints = a.devices.size() == 2 ? usbRadioHints() : std::vector<std::string>();   // reads sysfs: only here, not every frame
    if (hadRadio) {
        int idx = -1;
        for (int i = 2; i < (int)a.devices.size(); i++) if (a.devices[i].name == keep) { idx = i; break; }
        if (idx < 0) {   // it is gone: another radio must not inherit its antenna power
            a.tune.biasTee = false;
            for (bool& b : a.famBias) b = false;
        }
        a.devIdx = idx >= 0 ? idx : std::min(a.devIdx, (int)a.devices.size() - 1);
    }
    size_t nHack = 0;
    for (const auto& d : a.devices) if (d.kind == DeviceInfo::HackRF) nHack++;
    const std::string line = "device scan: " + std::to_string(nHack) + " HackRF, " + std::to_string(a.devices.size() - 2 - nHack) + " other radio(s)" + (soapySupported() ? "" : " (built without SoapySDR)");
    a.engine.log(line);
    // also in the log file (onair.log on Windows), with every radio and any error: what a user can send when a radio does not show up
    fprintf(stderr, "%s\n", line.c_str());
    for (size_t i = 2; i < a.devices.size(); i++) fprintf(stderr, "  %s\n", a.devices[i].name.c_str());
    if (!err.empty()) fprintf(stderr, "  error: %s\n", err.c_str());
    fflush(stderr);
}

// ---- the radio's own settings, saved per radio: prefs key "radio.<board>.<serial or name>", value "key=value;key=value" with ';', '=', '%'
// (and what the settings file cannot hold: '|', line breaks) written as %XX
std::string radioKeyOf(const DeviceInfo& d) {
    if (!d.isRadio()) return "";
    std::string k = "radio." + d.board + "." + (d.serial.empty() ? d.name : d.serial);
    for (auto& c : k) if (c == '=' || c == '|' || c == '\n' || c == '\r' || c == ' ') c = '_';
    return k;
}
static std::string radioEscape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        if (c == ';' || c == '=' || c == '%' || c == '|' || c < 32) { char b[4]; snprintf(b, sizeof b, "%%%02X", c); r += b; }
        else r += (char)c;
    }
    return r;
}
static std::string radioUnescape(const std::string& s) {
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) { r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else r += s[i];
    }
    return r;
}
void syncRadioSettings(App& a) {
    const std::string key = a.devIdx >= 0 && a.devIdx < (int)a.devices.size() ? radioKeyOf(a.devices[a.devIdx]) : std::string();
    if (key == a.radioKey) return;
    a.radioKey = key;
    a.tune.radio.clear();   // another radio never inherits these (a correction, a notch filter, a gain mode of the one before)
    if (key.empty()) return;
    const std::string v = plat::prefs().getS(key.c_str(), "");
    size_t pos = 0;
    while (pos < v.size()) {
        size_t e = v.find(';', pos);
        if (e == std::string::npos) e = v.size();
        const std::string item = v.substr(pos, e - pos);
        pos = e + 1;
        const size_t eq = item.find('=');
        if (eq != std::string::npos && eq > 0) a.tune.radio[radioUnescape(item.substr(0, eq))] = radioUnescape(item.substr(eq + 1));
    }
}
void saveRadioSettings(const App& a) {
    if (a.radioKey.empty()) return;
    std::string v;
    for (const auto& kv : a.tune.radio) v += (v.empty() ? "" : ";") + radioEscape(kv.first) + "=" + radioEscape(kv.second);
    plat::prefs().setS(a.radioKey.c_str(), v);
    plat::prefs().flush();
}

// ---- the sample rate chosen per mode and per radio
std::string rateModeId(const App& a) {
    if (const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr) return mt->id;
    static const char* kIds[] = {"dvb", "atsc", "dab", "atsc3", "isdbt", "fm"};
    return a.family >= 0 && a.family < 6 ? kIds[a.family] : "mode" + std::to_string(a.family);
}
static std::string ratePrefKey(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const std::string rk = radioKeyOf(a.devices[a.devIdx]);
    return rk.empty() ? "" : "rate." + rateModeId(a) + "." + rk;
}
std::string rateContext(const App& a) { return ratePrefKey(a) + "|" + std::to_string(a.devIdx) + "|" + std::to_string(a.family) + "|" + std::to_string(a.bwIdx) + "|" + std::to_string(a.dtmbBwMhz); }
double savedSampleRate(const App& a) {
    const std::string k = ratePrefKey(a);
    return k.empty() ? 0 : plat::prefs().getD(k.c_str(), 0);
}
void saveSampleRate(App& a, double hz) {
    const std::string k = ratePrefKey(a);
    if (k.empty()) return;
    plat::prefs().setD(k.c_str(), hz > 0 ? hz : 0);
    plat::prefs().flush();
}
double modeMinSampleRate(const App& a) { return minSampleRateFor(engineStd(a), a.tune.bandwidthMhz > 0 ? a.tune.bandwidthMhz : kBw[a.bwIdx].mhz); }

void applyBandwidth(App& a) {
    syncRadioSettings(a);   // the selected radio's own settings go with every start
    // HackRF Pro: the tuned centre is only exact at <= 10 Msps and at 20 Msps, so use 10 Msps (8 for narrow channels)
    a.tune.sampleRate = kBw[a.bwIdx].mhz >= 7 ? 10e6 : 8e6;
    {   // a radio that cannot reach that rate runs as fast as it can (the source picks the nearest rate it offers)
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric() && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
        // the DVB-T/T2 8 MHz channel is natively 64/7 Msps: a PlutoSDR can run exactly that, which saves the receiver its resampler (a sixth of its work)
        if (dv.kind == DeviceInfo::Native && dv.board == "pluto" && a.family == 0 && kBw[a.bwIdx].mhz == 8) a.tune.sampleRate = std::min(64e6 / 7, dv.maxRateHz > 0 ? dv.maxRateHz : 64e6 / 7);
        // "30 dB" is nearly deaf on an SDRplay (0..103), full gain on an Airspy (0..21), attenuation on an HF+: a radio that the gain was not
        // set for starts at 60 % of its own range (settings from before this rule keep their gain)
        if (dv.isGeneric() && dv.gainMaxDb > dv.gainMinDb) {
            const std::string key = dv.board + ":" + (dv.serial.empty() ? dv.name : dv.serial);
            if (a.gainDev.empty()) a.gainDev = key;
            else if (a.gainDev != key) { a.tune.gainDb = std::round(dv.gainMinDb + 0.6 * (dv.gainMaxDb - dv.gainMinDb)); a.gainDev = key; }
        }
        if (dv.isGeneric() && a.tune.gainDb > dv.gainMaxDb && dv.gainMaxDb > 0) a.tune.gainDb = dv.gainMaxDb;
        if (dv.isGeneric() && a.tune.gainDb < dv.gainMinDb && dv.gainMaxDb > dv.gainMinDb) a.tune.gainDb = dv.gainMinDb;   // a gain left by another radio
    }
    a.tune.basebandFilterHz = 0;
    a.tune.bandwidthMhz = kBw[a.bwIdx].mhz;
    a.tune.synth.atsc = a.family == 1;
    a.tune.synth.dab = a.dabMode;
    a.tune.synth.mode = engineStd(a) >= 4 ? engineStd(a) : 0;   // the test signal of DAB (4), ATSC 3.0 (5), ISDB-T (6), FM (7) and the modes after them
    if (const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr) {   // the mode says what the radio should do
        a.tune.bandwidthMhz = mt->bandwidthMhz; a.tune.sampleRate = mt->sampleRate; a.tune.basebandFilterHz = mt->basebandHz;
        if (mt->stdMode == 9) {   // DTMB: the channel width the user picked (6 MHz in Cuba); the test signal follows it
            a.tune.bandwidthMhz = a.dtmbBwMhz; a.tune.basebandFilterHz = a.dtmbBwMhz * 1e6;
            a.tune.synth.modeOpt[7] = a.dtmbBwMhz == 6 ? 1 : 0;
        }
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric() && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
    } else if (a.dabMode) {   // a DAB ensemble is 1.536 MHz wide: 2.048 Msps is the natural rate (RTL-SDR dongles do it too)
        a.tune.bandwidthMhz = 1.7; a.tune.sampleRate = 2.048e6; a.tune.basebandFilterHz = 1.75e6;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric() && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
    } else if (a.fmMode) {   // one 200 kHz station; 4 Msps keeps the neighbours that fold in from the sides well down
        a.tune.bandwidthMhz = 0.25; a.tune.sampleRate = 4e6; a.tune.basebandFilterHz = 2.5e6;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric() && dv.maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, dv.maxRateHz);
    } else if (a.atscMode) { a.tune.bandwidthMhz = 6; a.tune.sampleRate = 8e6; if (a.devices[a.devIdx].isGeneric() && a.devices[a.devIdx].maxRateHz > 0) a.tune.sampleRate = std::min(a.tune.sampleRate, a.devices[a.devIdx].maxRateHz); }   // an ATSC channel is always 6 MHz wide
    // the rate chosen for this mode on this radio replaces the mode's own (Auto: nothing changes); one outside this radio's limits or below
    // what the mode needs now (a wider DVB channel) is not used, the control shows Auto
    a.autoRateHz = a.tune.sampleRate;
    a.chosenRateHz = 0;
    a.rateCtx = rateContext(a);
    if (const double want = savedSampleRate(a); want > 0 && a.devices[a.devIdx].isRadio()) {
        const RateCheck c = checkManualRate(rateLimitsOf(a.devices[a.devIdx]), modeMinSampleRate(a), want);
        if (c.ok) { a.tune.sampleRate = want; a.chosenRateHz = want; }
    }
    // the rate was lowered to the radio's top: a filter meant for the faster rate lets everything up to its edge fold into the band (the
    // 8-9 MHz filters of DTMB, analog TV, DVB-S and Iridium on a PlutoSDR's 4 Msps over USB); the radio's own choice for its rate (0) fits
    if (a.tune.basebandFilterHz > a.tune.sampleRate) a.tune.basebandFilterHz = 0;
}

std::string openFileDialog() { return plat::openFileDialog(); }

std::string saveFileDialog(const char* name) { return plat::saveFileDialog(name); }

void loadPrefs(App& a) {
    plat::Prefs& d = plat::prefs();
    if (d.has("freqMhz")) a.freqMhz = d.getD("freqMhz", a.freqMhz);
    if (d.has("lna")) a.tune.lnaDb = (int)d.getI("lna", a.tune.lnaDb);
    if (d.has("vga")) a.tune.vgaDb = (int)d.getI("vga", a.tune.vgaDb);
    if (d.has("gain")) a.tune.gainDb = d.getD("gain", a.tune.gainDb);
    a.gainDev = d.getS("gainDev", "");
    a.tune.ampOn = d.getB("amp", false);
    a.recFormat = d.getI("recFormat", 0) == 1 ? 1 : 0;
    if (d.has("family")) { const int f = std::max(0, std::min(kNumFamilies - 1, (int)d.getI("family", 0))); setFamilyFlags(a, f); if (a.fmMode && !(a.freqMhz >= 87.5 && a.freqMhz <= 108)) a.freqMhz = 100.0; }
    for (int f = 0; f < kNumFamilies; f++) {   // as many as savePrefs writes
        if (!d.has(("gLna" + std::to_string(f)).c_str())) continue;
        a.famGain[f] = {(int)d.getI(("gLna" + std::to_string(f)).c_str(), 32), (int)d.getI(("gVga" + std::to_string(f)).c_str(), 20), d.getB(("gAmp" + std::to_string(f)).c_str(), true), true};
    }
    if (a.family >= 0 && a.family < kNumFamilies && a.famGain[a.family].known) { a.tune.lnaDb = a.famGain[a.family].lna; a.tune.vgaDb = a.famGain[a.family].vga; a.tune.ampOn = a.famGain[a.family].amp; }
    for (int f = 6; f < kNumFamilies; f++) a.famFreq[f] = d.getD(("fFreq" + std::to_string(f)).c_str(), 0.0);
    if (d.has("newUi")) a.newUi = d.getB("newUi", true);
    if (d.has("uiVariant")) a.uiVariant = std::max(0, std::min(7, (int)d.getI("uiVariant", 0)));
    if (d.has("dtmbBw")) a.dtmbBwMhz = d.getI("dtmbBw", 8) == 6 ? 6 : 8;
    if (d.has("lightUi")) a.lightUi = d.getI("lightUi", 0) != 0;
    if (d.has("uiTheme")) a.uiTheme = std::max(0, std::min(2, (int)d.getI("uiTheme", 1)));
    if (d.has("fmStations")) {
        std::string st = d.getS("fmStations", ""), line;
        size_t pos = 0;
        while (pos < st.size()) {
            size_t e = st.find('\n', pos);
            if (e == std::string::npos) e = st.size();
            line = st.substr(pos, e - pos); pos = e + 1;
            std::vector<std::string> f; size_t q = 0;
            for (;;) { size_t bar = line.find('|', q); if (bar == std::string::npos) { f.push_back(line.substr(q)); break; } f.push_back(line.substr(q, bar - q)); q = bar + 1; }
            if (f.size() < 6) continue;
            App::FmScan::Res r;
            r.mhz = atof(f[0].c_str()); r.name = f[1]; r.pty = f[2]; r.snr = (float)atof(f[3].c_str()); r.stereo = f[4] == "1"; r.rds = f[5] == "1"; r.found = r.mhz >= 87.5 && r.mhz <= 108.01;
            if (r.found) a.fmScan.results.push_back(r);
        }
    }
    if (d.has("fmDeemph")) a.fmDeemph = d.getI("fmDeemph", 50) == 75 ? 75 : 50;
    if (d.has("fmChanKhz")) { const int k = (int)d.getI("fmChanKhz", 0); a.fmChanKhz = k > 0 ? std::min(300, std::max(100, k)) : 0; }
    if (d.has("compute")) a.computeMode = std::max(0, std::min(2, (int)d.getI("compute", a.computeMode)));   // indexes the CPU / GPU / Auto names
    if (d.has("standard")) a.stdMode = std::max(0, std::min(2, (int)d.getI("standard", a.stdMode)));   // 0 auto, 1 DVB-T2, 2 DVB-T: engineStd() hands it on
    a.bwIdx = std::max(0, std::min((int)(sizeof kBw / sizeof *kBw) - 1, (int)d.getI("bw", 0)));
    if (d.has("bwAuto")) a.bwAuto = d.getB("bwAuto", a.bwAuto);
    a.engine.player().setConceal(d.getB("smoothGaps", false));
    a.engine.iqFix().dc = d.getB("dcRemove", false);
    a.engine.setAutoOffset(d.getB("offsetTune", false));
    a.engine.iqFix().iq = d.getB("iqCorrect", false);
    if (d.has("filePath")) a.file.path = d.getS("filePath", "");
    if (d.has("outPath")) snprintf(a.filePath, sizeof a.filePath, "%s", d.getS("outPath", "").c_str());
    if (d.has("udpHost")) snprintf(a.udpHost, sizeof a.udpHost, "%s", d.getS("udpHost", "").c_str());
    if (d.has("udpPort")) a.out.port = (int)d.getI("udpPort", a.out.port);
    a.out.rtp = d.getB("udpRtp", false);
    a.out.dropNull = d.getB("dropNull", false);
    a.popTop = d.getB("popTop", true);
    a.updCheck = d.getB("updCheck", true); a.updAuto = d.getB("updAuto", true); a.updPre = d.getB("updPre", true);
    a.updLast = d.getD("updLast", 0); a.updSkip = d.getS("updSkip", "");
    a.channels = d.getChannels();
}

void savePrefs(const App& a) {
    plat::Prefs& d = plat::prefs();
    d.setD("freqMhz", a.freqMhz);
    d.setI("lna", a.tune.lnaDb);
    d.setI("vga", a.tune.vgaDb);
    d.setD("gain", a.tune.gainDb);
    d.setS("gainDev", a.gainDev);
    d.setB("amp", a.tune.ampOn);
    d.setI("family", a.family);
    d.setI("fmDeemph", a.fmDeemph);
    d.setI("fmChanKhz", a.fmChanKhz);
    d.setB("newUi", a.newUi);
    for (int f = 0; f < kNumFamilies; f++) {   // gains per mode (the current mode from the live settings)
        const bool cur = f == a.family;
        if (!cur && !a.famGain[f].known) continue;
        d.setI(("gLna" + std::to_string(f)).c_str(), cur ? a.tune.lnaDb : a.famGain[f].lna);
        d.setI(("gVga" + std::to_string(f)).c_str(), cur ? a.tune.vgaDb : a.famGain[f].vga);
        d.setB(("gAmp" + std::to_string(f)).c_str(), cur ? a.tune.ampOn : a.famGain[f].amp);
    }
    for (int f = 6; f < kNumFamilies; f++) if (a.famFreq[f] > 0 || f == a.family) d.setD(("fFreq" + std::to_string(f)).c_str(), f == a.family ? a.freqMhz : a.famFreq[f]);
    d.setI("uiTheme", a.uiTheme);
    d.setI("lightUi", a.lightUi ? 1 : 0);
    d.setI("dtmbBw", a.dtmbBwMhz);
    d.setI("uiVariant", a.uiVariant);
    {   // the stations the FM scan found: mhz|name|type|snr|stereo|rds, one per line
        std::string st;
        for (const auto& r : a.fmScan.results) {
            if (!r.found) continue;
            auto clean = [](std::string s) { for (auto& c : s) if (c == '|' || c == '\n') c = ' '; return s; };
            char b[64]; snprintf(b, sizeof b, "%.1f|", r.mhz);
            st += b + clean(r.name) + "|" + clean(r.pty) + "|" + std::to_string((int)r.snr) + "|" + (r.stereo ? "1" : "0") + "|" + (r.rds ? "1" : "0") + "\n";
        }
        if (!a.fmScan.running) d.setS("fmStations", st);   // not a half-finished scan
    }
    d.setChannels(a.channels);
    d.setI("compute", a.computeMode);
    d.setI("standard", a.stdMode);
    d.setI("bw", a.bwIdx);
    d.setB("bwAuto", a.bwAuto);
    d.setB("smoothGaps", a.engine.player().conceal());
    d.setB("dcRemove", a.engine.iqFix().dc.load());
    d.setB("offsetTune", a.engine.autoOffset());
    d.setB("iqCorrect", a.engine.iqFix().iq.load());
    d.setS("filePath", a.file.path);
    d.setS("outPath", a.filePath);
    d.setS("udpHost", a.udpHost);
    d.setI("udpPort", a.out.port);
    d.setB("udpRtp", a.out.rtp);
    d.setB("dropNull", a.out.dropNull);
    d.setB("popTop", a.popTop);
    d.setB("updCheck", a.updCheck); d.setB("updAuto", a.updAuto); d.setB("updPre", a.updPre);
    d.setD("updLast", a.updLast); d.setS("updSkip", a.updSkip);
    d.flush();
}

void ingestSpectrum(App& a) {
    SpectrumFrame f;
    if (!a.engine.latestSpectrum(f, a.lastSeq)) return;
    a.lastSeq = f.seq;
    a.spec = f;
    size_t n = f.dbfs.size();
    if (a.smooth.size() != n || a.peak.size() != n) { a.smooth = f.dbfs; a.peak = f.dbfs; }   // both lines always have the frame size (a retune clears the peak line)
    for (size_t i = 0; i < n; i++) {
        a.smooth[i] += 0.35f * (f.dbfs[i] - a.smooth[i]);
        a.peak[i] = std::max(a.peak[i] - 0.15f, f.dbfs[i]);
    }
    a.wf.push(f.dbfs);
    a.wf.stamp(glfwGetTime());
}

void ingestRx(App& a) {
    double now = glfwGetTime();
    if (a.engine.running() && now - a.tsT > 0.25) { a.ts = a.engine.tsSnapshot(); a.bb = a.engine.bbStats(); a.tsT = now; }
    RxTelemetry t;
    if (!a.engine.latestRx(t, a.rxSeq)) return;
    a.rxSeq = t.seq;
    if (t.p1Count > 0) a.rxSeen = true;
    if (t.dataFrames < a.rx.dataFrames) { a.mpd.reset(); a.quality.reset(); a.cst.mod = -1; } // receiver was restarted or retuned
    a.rx = std::move(t);
    {   // statistics of the decoded constellation cells
        App::ConstStats& c = a.cst;
        const RxTelemetry& r = a.rx;
        if (r.plpConst.empty() || r.plpConstTx.size() != r.plpConst.size()) { c.seq = 0; c.mod = -1; }
        else if (r.plpConstSeq != c.seq) {
            const int nPts = 1 << (2 * (r.plpFec.mod + 1));
            if (c.mod != r.plpFec.mod || c.plp != r.plpId || (int)c.pts.size() != nPts || c.grid.empty()) {
                c.mod = r.plpFec.mod; c.plp = r.plpId;
                c.pts.assign(nPts, App::ConstStats::Pt());
                c.grid.assign(App::ConstStats::G * App::ConstStats::G, 0.f);
            }
            c.seq = r.plpConstSeq;
            for (float& v : c.grid) v *= 0.93f;
            for (auto& p : c.pts) { p.n *= 0.97; p.ei *= 0.97; p.eq *= 0.97; p.e2 *= 0.97; }
            const int G = App::ConstStats::G;
            const float lim = 1.4f;
            for (size_t i = 0; i < r.plpConst.size(); i++) {
                const cf32 o = r.plpConst[i];
                const int gx = (int)((o.real() + lim) / (2 * lim) * G), gy = (int)((lim - o.imag()) / (2 * lim) * G);
                if (gx >= 0 && gx < G && gy >= 0 && gy < G) c.grid[gy * G + gx] += 1.f;
                const unsigned l = r.plpConstTx[i];
                if (l < c.pts.size()) {
                    const cf32 e = o - qamPoint(c.mod, false, l);
                    auto& p = c.pts[l];
                    p.n += 1; p.ei += e.real(); p.eq += e.imag(); p.e2 += std::norm(e);
                }
            }
        }
    }
    a.mpd.update(a.rx);
    a.quality.update(a.rx);
    {
        const double nowT = glfwGetTime();
        if (a.engine.running() && nowT - a.histT >= 0.25) {
            App::HistSample h;
            h.t = (float)nowT;
            h.snr = a.rx.dataValid ? a.rx.dataSnrDb : NAN;
            h.mer = a.rx.plpMerDb > 0 && a.rx.plpMerDb < 90 ? (float)a.rx.plpMerDb : NAN;
            const uint64_t dOk = a.rx.blocksOk >= a.histOk ? a.rx.blocksOk - a.histOk : 0, dBad = a.rx.blocksBad >= a.histBad ? a.rx.blocksBad - a.histBad : 0;
            h.loss = (dOk + dBad) ? 100.f * (float)dBad / (float)(dOk + dBad) : NAN;
            a.histOk = a.rx.blocksOk; a.histBad = a.rx.blocksBad;
            h.cfo = a.rx.state == 2 ? (float)a.rx.cfoHz : NAN;
            h.sro = a.rx.state == 2 ? (float)a.rx.sroPpm : NAN;
            h.level = a.spec.stats.rmsDbfs; h.clip = a.spec.stats.clipFraction * 100.f;
            h.quality = a.quality.report().valid ? (float)a.quality.report().percent : NAN;
            h.aux = NAN;
            if (a.fmMode) {   // FM has no FEC blocks and no MER: the history shows how clean the station is and how well RDS comes through
                const FmTelemetry& fm = a.rx.fm;
                const bool ok = a.rx.standard == 6 && fm.carrier;
                h.snr = ok ? fm.snrDb : NAN; h.mer = NAN; h.loss = NAN; h.cfo = h.sro = NAN;
                h.quality = ok ? std::min(100.f, std::max(0.f, fm.snrDb / 45.f * 100.f)) : NAN;
                h.aux = ok && fm.rdsSync ? fm.rdsBlockOkPct : NAN;
            }
            a.hist.push_back(h);
            if (a.hist.size() > 3600) a.hist.pop_front();
            a.histT = nowT;
        }
    }
    auto push = [](std::deque<float>& d, float v) { d.push_back(v); if (d.size() > 600) d.pop_front(); };
    if (a.rx.state == 2) {
        push(a.hCfo, (float)a.rx.cfoHz);
        push(a.hSnr, a.rx.cpSnrDb);
        push(a.hTiming, a.rx.timingErr);
    }
}

double radioCenterMhz(const App& a) {
    const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
    const bool file = !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::File;
    return a.freqMhz + (mt && !file ? mt->tuneOffsetHz / 1e6 : 0.0);
}

std::vector<double> xs(const App& a) {
    size_t n = a.smooth.size();
    std::vector<double> x(n);
    double fs = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    double c = radioCenterMhz(a);
    for (size_t i = 0; i < n; i++) x[i] = c + ((double)i / n - 0.5) * fs / 1e6;
    return x;
}

void applyOutputs(App& a) {
    a.out.host = a.udpHost;
    a.out.path = a.filePath;
    a.out.serviceId = a.selService;
    a.engine.setOutputs(a.out);
    savePrefs(a);
}

std::string channelLabel(const SavedChannel& c) {
    char b[200];
    snprintf(b, sizeof b, "%.3f MHz  %s%s", c.freqMhz, c.name.empty() ? "DVB-T2 mux" : c.name.c_str(), c.nServices > 1 ? (" +" + std::to_string(c.nServices - 1)).c_str() : "");
    return b;
}

// Merge DVB-T2 muxes found by the scanner into the remembered channel list.
void harvestScan(App& a) {
    ScanProgress pr = a.scanner.progress();
    if (!pr.running && a.scanWasRunning) {   // the scan stopped the receiver: it carries on once the scan has let go of the radio
        a.scanWasRunning = false;
        if (!a.engine.running()) startReceiver(a);
    }
    const double now = glfwGetTime();
    const bool due = pr.running ? now - a.scanHarvestT > 0.5 : a.scanWas;
    if (!due) return;
    a.scanWas = pr.running;
    a.scanHarvestT = now;
    bool changed = false;
    for (auto& r : a.scanner.results()) {
        if (!r.t2) continue;
        SavedChannel sc;
        sc.freqMhz = r.freqMHz; sc.bwMhz = r.bwMhz; sc.mode = r.mode; sc.snrDb = r.snrDb; sc.nServices = (int)r.services.size();
        if (!r.services.empty()) { sc.name = r.services[0]; size_t br = sc.name.rfind(" ["); if (br != std::string::npos) sc.name.resize(br); }
        if (sc.name.empty()) sc.name = r.networkName;
        bool found = false;
        for (auto& c : a.channels)
            if (std::fabs(c.freqMhz - sc.freqMhz) < 0.01) {
                if (c.favourite) { sc.name = c.name; sc.favourite = true; }
                if (c.name != sc.name || c.nServices != sc.nServices || c.mode != sc.mode) { c = sc; changed = true; }
                found = true; break;
            }
        if (!found) { a.channels.push_back(sc); changed = true; }
    }
    if (changed) {
        std::sort(a.channels.begin(), a.channels.end(), [](const SavedChannel& x, const SavedChannel& y) { return x.freqMhz < y.freqMhz; });
        savePrefs(a);
    }
}

// Select a remembered channel: stop what is playing and tune the HackRF to it.
void tuneToChannel(App& a, const SavedChannel& c) {
    int hw = -1;   // the radio chosen in the toolbar, else the last one in the list (as the scan tab picks it)
    if (a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].isRadio()) hw = a.devIdx;
    else for (int i = 0; i < (int)a.devices.size(); i++) if (a.devices[i].isRadio()) hw = i;
    if (hw < 0) { a.engine.log("channel selector needs a radio"); return; }
    if (a.scanner.progress().running) a.scanner.stop();
    a.scanWasRunning = false;   // the channel is started here, not the receiver the scan stopped
    const double oldBw = a.tune.bandwidthMhz, oldRate = a.tune.sampleRate;
    const bool sameDev = a.devIdx == hw;
    a.devIdx = hw;   // before applyBandwidth: the rate and gain limits are those of this radio
    a.freqMhz = c.freqMhz;
    for (int k = 0; k < (int)(sizeof(kBw) / sizeof(kBw[0])); k++) if (kBw[k].mhz == c.bwMhz) a.bwIdx = k;
    a.tune.centerHz = a.freqMhz * 1e6;
    applyBandwidth(a);
    a.engine.player().select(-1);
    a.selService = -1;
    if (a.engine.running() && sameDev && oldBw == a.tune.bandwidthMhz && oldRate == a.tune.sampleRate) {
        a.engine.log("channel: " + channelLabel(c));
        a.engine.retuneReset(a.tune);
    } else {
        if (a.engine.running()) a.engine.stop();
        a.devIdx = hw;
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[hw], a.tune, a.file);
    }
    a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
    a.mpd.reset(); a.quality.reset();
    savePrefs(a);
}

// Automatic bandwidth: follow what the engine detected; a change between 8 and 10 Msps (7/8 MHz vs narrower) needs a restart
void followBandwidth(App& a) {
    a.engine.setBandwidthAuto(a.bwAuto && !a.atscMode);
    if (!a.bwAuto || a.atscMode || !a.engine.running()) return;
    const bool hw = a.devices[a.devIdx].isRadio();
    const double want = a.engine.activeBandwidth();
    if (want == a.tune.bandwidthMhz) return;
    int idx = -1;
    for (int k = 0; k < (int)(sizeof kBw / sizeof *kBw); k++) if (kBw[k].mhz == want) idx = k;
    if (idx < 0) return;
    const double oldRate = a.tune.sampleRate;
    a.bwIdx = idx;
    applyBandwidth(a);
    savePrefs(a);
    if (hw && a.tune.sampleRate != oldRate) {
        a.engine.log(a.tune.sampleRate > oldRate ? "restarting at 10 Msps" : "restarting at 8 Msps");
        a.engine.stop();
        a.engine.setComputeMode(a.computeMode); a.engine.setStandard(engineStd(a));
        a.engine.start(a.devices[a.devIdx], a.tune, a.file);
        a.smooth.clear(); a.peak.clear(); a.lastSeq = 0;
        a.mpd.reset(); a.quality.reset();
    }
}

