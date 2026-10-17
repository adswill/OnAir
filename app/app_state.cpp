// application state: preferences, device list, bandwidth, ingesting spectrum and receiver telemetry, channel list
#include "app.h"
#include <cstring>
#include "dect2/usb_diag.h"
#include "dect2/rate_choice.h"
#include "dect2/hdr_rx.h"
#include "dect2/sonde_rx.h"

// the name of the built-in test signal in the source list: it follows the mode
static std::string synthLabel(const App& a) {
    if (const SubMode* sm = curSub(a)) return std::string("Synthetic test signal (") + sm->name + ")";
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

// the protocol entries (dect2::subModes()): index + 1 in App::sub
const SubMode* curSub(const App& a) {
    int n = 0;
    const SubMode* t = subModes(n);
    return a.sub >= 1 && a.sub <= n && t[a.sub - 1].stdMode == a.family + 2 ? &t[a.sub - 1] : nullptr;
}
int subIndexOf(const char* id) {
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; id && i < n; i++) if (!strcmp(t[i].id, id)) return i + 1;
    return 0;
}
static int firstSubOf(int f) {   // HF digital, mesh, pagers and marine are only listed as their protocols: 0 for the other families
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; i < n; i++) if (t[i].stdMode == f + 2) return i + 1;
    return 0;
}
static int slotOf(int f, int sub) { return sub >= 1 && sub <= kNumSubModes ? kNumFamilies + sub - 1 : f; }
int modeSlot(const App& a) { return slotOf(a.family, curSub(a) ? a.sub : 0); }
static std::string slotKey(int slot) {   // the suffix of the per-mode settings: the family number, or "." and the protocol's id
    if (slot < kNumFamilies) return std::to_string(slot);
    int n = 0;
    const SubMode* t = subModes(n);
    return slot - kNumFamilies < n ? std::string(".") + t[slot - kNumFamilies].id : std::to_string(slot);
}

void setFamily(App& a, int f) { setMode(a, f, firstSubOf(f)); }

void setMode(App& a, int f, int sub) {
    {   // a protocol entry of another family, or none for a family that has them: the family's first
        int n = 0;
        const SubMode* t = subModes(n);
        if (!(sub >= 1 && sub <= n && t[sub - 1].stdMode == f + 2)) sub = firstSubOf(f);
    }
    const int from = modeSlot(a), to = slotOf(f, sub);
    if (a.family >= 6 && from < kNumSlots && to != from) a.famFreq[from] = a.freqMhz;   // each of the newer modes comes back on the frequency it was left on
    if (a.family >= 0 && from < kNumSlots && to != from) {   // the gains that suit FM are not the ones for a TV channel: remember them per mode
        a.famGain[from] = {a.tune.lnaDb, a.tune.vgaDb, a.tune.ampOn, true};
        const App::FamGain& g = a.famGain[to];
        if (g.known) { a.tune.lnaDb = g.lna; a.tune.vgaDb = g.vga; a.tune.ampOn = g.amp; }
        else if (f == 5) { a.tune.lnaDb = 24; a.tune.vgaDb = 20; a.tune.ampOn = false; }   // FM stations are strong
        else if (a.family == 5 || f >= 6) { a.tune.lnaDb = 32; a.tune.vgaDb = 20; a.tune.ampOn = true; }
    }
    a.sub = sub;
    setFamilyFlags(a, f);
    if (f >= 6) {
        const ModeTuning* mt = modeTuning(f + 2);
        const SubMode* sm = curSub(a);
        const double def = sm ? sm->defMhz : mt ? mt->defMhz : a.freqMhz;
        a.freqMhz = mt && a.famFreq[to] >= mt->minMhz && a.famFreq[to] <= mt->maxMhz ? a.famFreq[to] : def;
    }
    if (f == 0 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 522.0;       // a TV channel, not a leftover FM or DAB frequency
    if (f == 1 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 473.0;
    if (f == 3 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 522.0;
    if (f == 2 && !(a.freqMhz >= 174 && a.freqMhz <= 240)) a.freqMhz = 218.640;
    if (f == 4 && !(a.freqMhz >= 170 && a.freqMhz <= 770)) a.freqMhz = 473.143;   // the centre of UHF channel 13/14 of the 6 MHz raster
    if (f == 5 && !(a.freqMhz >= 87.5 && a.freqMhz <= 108)) a.freqMhz = 100.0;     // FM band, default to 100 MHz
    nameSynth(a);
}

// what to tell the engine: 0 auto, 1 DVB-T2, 2 DVB-T, 3 ATSC, 4 DAB, 5 ATSC 3.0, 6 ISDB-T, 7 FM, 8 DVB-S/S2, 9 DTMB, 10 analog TV, 11 DMR, 12 DRM, 13 ADS-B, 14 GNSS, 15 radiosonde, 16 AIS, 17 marine, 18 ACARS, 19 Inmarsat-C, 20 Inmarsat Aero, 21 Iridium, 22 mesh, 23 HD Radio, 24 CDR, 25 pagers, 26 APRS / packet, 27 HF digital, 28 airband, 29 VDL Mode 2, 30 ISM sensors, 32 VOR / ILS, 33 M17, 35 D-STAR, 36 CW, 37 HFDL, 38 PSK31, 39 JS8Call, 44 spectrum analyser, 40 NXDN, 42 DVB-C, 47 dPMR, 46 Orbcomm, 45 Mode S uplink, 48 TETRA, 51 HF ALE, 49 MAC, 50 radio astronomy, 52 P25, 53 Bluetooth LE, 54 ionospheric sounders, 55 QO-100, 56 Wi-Fi networks, 57 general receiver, 63 CubeSat telemetry, 69 HF beacons (Engine::start maps these to activeStandard())
int engineStd(const App& a) { return a.family >= 6 ? a.family + 2 : a.family == 1 ? 3 : a.family == 2 ? 4 : a.family == 3 ? 5 : a.family == 4 ? 6 : a.family == 5 ? 7 : a.stdMode; }

void refreshDevices(App& a) {
    // the radio chosen before stays chosen when it is found again (a radio unplugged or added moves the others in the list)
    const bool hadRadio = a.devIdx >= 2 && a.devIdx < (int)a.devices.size();
    const std::string keep = hadRadio ? a.devices[a.devIdx].name : "";
    // A radio that is open (the receiver or a channel scan has it) may be missing from a new list: some drivers cannot look at a radio in
    // use (WinUSB on Windows, the SDRplay service), and the list would then drop the radio being received and move the choice to another
    // entry. It stays in the list as it was.
    const bool inUse = hadRadio && (a.devices[a.devIdx].isRadio() || a.devices[a.devIdx].isAudio()) && (a.engine.running() || a.scanner.progress().running);
    const DeviceInfo keepDev = inUse ? a.devices[a.devIdx] : DeviceInfo();
    a.devices.clear();
    DeviceInfo s; s.kind = DeviceInfo::Synthetic; s.name = synthLabel(a); a.devices.push_back(s);
    DeviceInfo f; f.kind = DeviceInfo::File; f.name = "IQ recording file…"; a.devices.push_back(f);
    for (auto& d : listAudioDevices()) a.devices.push_back(d);
    std::string err;
    setNetworkRadios(a.netRadios);   // the ones the user added by address come after the ones found
    setHamlibRigs(a.hamlibRigs);
    // HackRF, the radios with a native driver (RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP) and the rest through SoapySDR
    for (auto& d : listRadios(err)) a.devices.push_back(d);
    for (App* o : receiversAll()) {   // a radio another receiver has open may be missing from the list (drivers cannot look at a radio in use): it stays, as "in use by RX n"
        if (o == &a || !o->engine.running() || o->devIdx < 2 || o->devIdx >= (int)o->devices.size() ||
            !(o->devices[o->devIdx].isRadio() || o->devices[o->devIdx].isAudio())) continue;
        const DeviceInfo& h = o->devices[o->devIdx];
        bool have = false;
        for (const auto& d : a.devices) have |= deviceClaimKey(d) == deviceClaimKey(h);
        if (!have) a.devices.push_back(h);
    }
    a.hackrfErr = err;
    a.hackrfErrHidden = false;
    a.usbHints = std::vector<std::string>();
    if (std::none_of(a.devices.begin(), a.devices.end(), [](const DeviceInfo& d) { return d.isRadio(); }))
        a.usbHints = usbRadioHints();   // sound cards do not mean a USB radio was found
    for (auto& h : sdrplayApiHints()) a.usbHints.push_back(h);
    if (hadRadio) {
        int idx = -1;
        for (int i = 2; i < (int)a.devices.size(); i++) if (a.devices[i].name == keep) { idx = i; break; }
        if (idx < 0 && inUse) { a.devices.push_back(keepDev); idx = (int)a.devices.size() - 1; }
        if (idx < 0) {   // it is gone: another radio must not inherit its antenna power
            a.tune.biasTee = false;
        }
        a.devIdx = idx >= 0 ? idx : std::min(a.devIdx, (int)a.devices.size() - 1);
    }
    size_t nHack = 0, nAudio = 0;
    for (const auto& d : a.devices) { if (d.kind == DeviceInfo::HackRF) nHack++; if (d.isAudio()) nAudio++; }
    const std::string line = "device scan: " + std::to_string(nHack) + " HackRF, " + std::to_string(nAudio) + " sound card(s), " +
        std::to_string(a.devices.size() - 2 - nHack - nAudio) + " other radio(s)" + (soapySupported() ? "" : " (built without SoapySDR)");
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
    const bool have = a.devIdx >= 0 && a.devIdx < (int)a.devices.size();
    const std::string key = have ? radioKeyOf(a.devices[a.devIdx]) : std::string();
    // the rate limits as the settings make them (an rtl_tcp server set to another radio than an RTL-SDR): after a rescan and a change too
    struct Caps { App& a; bool have; ~Caps() { if (have) applyRadioOptionCaps(a.devices[a.devIdx], a.tune); } } caps{a, have};
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
    if (const SubMode* sm = curSub(a)) return sm->id;   // each protocol entry has its own rate
    if (const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr) {
        // GNSS: a rate chosen per tune (L1 keeps the plain key)
        if (mt->stdMode == 14 && *gnssTuneKey(a.freqMhz)) return std::string(mt->id) + "-" + gnssTuneKey(a.freqMhz);
        return mt->id;
    }
    static const char* kIds[] = {"dvb", "atsc", "dab", "atsc3", "isdbt", "fm"};
    return a.family >= 0 && a.family < 6 ? kIds[a.family] : "mode" + std::to_string(a.family);
}
static std::string ratePrefKey(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return "";
    const std::string rk = radioKeyOf(a.devices[a.devIdx]);
    return rk.empty() ? "" : "rate." + rateModeId(a) + "." + rk;
}
std::string rateContext(const App& a) { return ratePrefKey(a) + "|" + std::to_string(a.devIdx) + "|" + std::to_string(a.family) + "." + std::to_string(a.sub) + "|" + std::to_string(a.bwIdx) + "|" + std::to_string(a.dtmbBwMhz); }
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
// antenna power, saved per radio (prefs "bias.<radioKeyOf>"): kept in every mode and after a restart, never given to another radio
std::string biasContext(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size() || !a.devices[a.devIdx].hasBiasTee) return "";
    const std::string rk = radioKeyOf(a.devices[a.devIdx]);
    return rk.empty() ? "" : "bias." + rk;   // per radio, in every mode: the antenna and its amplifier do not change with the mode
}
void loadBiasTee(App& a) {
    const std::string k = biasContext(a);
    a.tune.biasTee = !k.empty() && plat::prefs().getI(k.c_str(), 0) != 0;
}
void saveBiasTee(const App& a) {
    const std::string k = biasContext(a);
    if (k.empty()) return;
    plat::prefs().setI(k.c_str(), a.tune.biasTee ? 1 : 0);
    plat::prefs().flush();
}
double modeMinSampleRate(const App& a) {
    if (engineStd(a) == 23) return hdrMinSampleRate(a.freqMhz);   // HD Radio: AM needs far less than FM
    return minSampleRateFor(engineStd(a), a.tune.bandwidthMhz > 0 ? a.tune.bandwidthMhz : kBw[a.bwIdx].mhz);
}

void applyBandwidth(App& a) {
    syncRadioSettings(a);   // the selected radio's own settings go with every start
    if (const std::string bc = biasContext(a); bc != a.biasCtx) { a.biasCtx = bc; loadBiasTee(a); }   // antenna power saved for this mode and radio
    // HackRF Pro: the tuned centre is only exact at <= 10 Msps and at 20 Msps, so use 10 Msps (8 for narrow channels)
    a.tune.sampleRate = kBw[a.bwIdx].mhz >= 7 ? 10e6 : 8e6;
    {   // a radio that cannot reach that rate runs as fast as it can (the source picks the nearest rate it offers)
        const DeviceInfo& dv = a.devices[a.devIdx];
        // the DVB-T/T2 channel is natively 64/7 Msps per 8 MHz: a PlutoSDR can run exactly that, which saves the receiver its resampler (a sixth
        // of its work); on its USB cable it goes above the 4 Msps the cable carries (linkRateFor), with a note on screen (linkNote)
        const bool plutoDvb = dv.kind == DeviceInfo::Native && dv.board == "pluto" && a.family == 0 && kBw[a.bwIdx].mhz >= 5;
        if (plutoDvb) a.tune.sampleRate = dvbNativeRate(kBw[a.bwIdx].mhz);
        if (dv.isGeneric()) a.tune.sampleRate = linkRateFor(dv, a.tune, a.tune.sampleRate, minSampleRateFor(engineStd(a), kBw[a.bwIdx].mhz));
        // "30 dB" is nearly deaf on an SDRplay (0..103), full gain on an Airspy (0..21), attenuation on an HF+: a radio that the gain was not
        // set for starts at 60 % of its own range (settings from before this rule keep their gain)
        if (dv.isGeneric() && dv.gainMaxDb > dv.gainMinDb) {
            const std::string key = dv.board + ":" + (dv.serial.empty() ? dv.name : dv.serial);
            if (a.gainDev.empty()) a.gainDev = key;
            else if (a.gainDev != key) {
                const double frac = dv.board == "sdrplay" ? 0.4 : 0.6;   // Soapy SDRplay: a mid slider still meant max LNA before the RF/IF split
                a.tune.gainDb = std::round(dv.gainMinDb + frac * (dv.gainMaxDb - dv.gainMinDb));
                a.gainDev = key;
                if (dv.board == "sdrplay") a.tune.radio.erase("rfgr");
            }
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
        } else if (mt->stdMode == 10 && a.atvFm && !a.atvNbtv) {   // analog FPV video: a link about 17 MHz wide, the radio's filter at 18 MHz
            a.tune.bandwidthMhz = 18; a.tune.sampleRate = 20e6; a.tune.basebandFilterHz = 18e6;
        } else if (mt->stdMode == 10 && a.atvWide && !a.atvNbtv) {   // system E: a 14 MHz channel
            a.tune.bandwidthMhz = 14; a.tune.sampleRate = 20e6; a.tune.basebandFilterHz = 16e6;
        } else if (mt->stdMode == 22 && meshSampleRate(a) > mt->sampleRate) {   // Mesh: 2.4 GHz LoRa (812.5 / 1625 kHz) needs more
            a.tune.sampleRate = meshSampleRate(a); a.tune.basebandFilterHz = 0.875 * a.tune.sampleRate;
        } else if (mt->stdMode == 14) {   // GNSS: the rate of the tune (L1 / E1, BeiDou B1I, ...)
            a.tune.sampleRate = gnssAutoRate(a.freqMhz);
            if (a.tune.sampleRate > 2 * mt->basebandHz) a.tune.basebandFilterHz = 0.8 * a.tune.sampleRate;
        } else if (mt->stdMode == 23 && hdrIsAmBand(a.freqMhz)) {   // HD Radio AM: a 30 kHz station; 1 Msps suits the HF radios (an HF+ gives 912 ksps)
            a.tune.sampleRate = hdrSampleRate(a.freqMhz); a.tune.basebandFilterHz = 0;
        } else if (mt->stdMode == 15 && dect2::sondeIsLbandMhz(a.freqMhz)) {   // LMS6-1680: wide FSK (~170 kHz)
            a.tune.bandwidthMhz = 0.4; a.tune.sampleRate = 2e6; a.tune.basebandFilterHz = 0.4e6;
        }
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric()) a.tune.sampleRate = linkRateFor(dv, a.tune, a.tune.sampleRate, mt->stdMode == 23 ? hdrMinSampleRate(a.freqMhz) : mt->minSampleRate);
        if (const SubMode* sm = curSub(a)) { applySubModeSynth(a.tune.synth, *sm); applySubMode(a.engine, sm); }   // one protocol: only its decoder, only its test signal
    } else if (a.dabMode) {   // a DAB ensemble is 1.536 MHz wide: 2.048 Msps is the natural rate (RTL-SDR dongles do it too)
        a.tune.bandwidthMhz = 1.7; a.tune.sampleRate = 2.048e6; a.tune.basebandFilterHz = 1.75e6;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric()) a.tune.sampleRate = linkRateFor(dv, a.tune, a.tune.sampleRate, minSampleRateFor(4, 1.7));
    } else if (a.fmMode) {   // one 200 kHz station; 4 Msps keeps the neighbours that fold in from the sides well down
        a.tune.bandwidthMhz = 0.25; a.tune.sampleRate = 4e6; a.tune.basebandFilterHz = 2.5e6;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric()) a.tune.sampleRate = linkRateFor(dv, a.tune, a.tune.sampleRate, minSampleRateFor(7, 0.25));
    } else if (a.isdbtMode) {   // ISDB-T: 6, 7 or 8 MHz (the Maldives) or automatic, which needs the radio open for the widest; the test signal follows a set width
        const int w = a.isdbtBwMhz;
        a.tune.bandwidthMhz = w ? w : 8; a.tune.sampleRate = w == 6 ? 8e6 : 10e6;
        a.tune.isdbtBwMhz = w; a.tune.synth.isdbtBwMhz = w ? w : 6;
        if (a.devices[a.devIdx].isGeneric()) a.tune.sampleRate = linkRateFor(a.devices[a.devIdx], a.tune, a.tune.sampleRate, minSampleRateFor(6, a.tune.bandwidthMhz));
    } else if (a.atscMode) { a.tune.bandwidthMhz = 6; a.tune.sampleRate = 8e6; if (a.devices[a.devIdx].isGeneric()) a.tune.sampleRate = linkRateFor(a.devices[a.devIdx], a.tune, a.tune.sampleRate, minSampleRateFor(3, 6)); }   // an ATSC channel is always 6 MHz wide
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
    a.netRadios = netRadiosFromString(d.getS("netRadios", ""));
    a.hamlibRigs = hamlibRigsFromString(d.getS("hamlibRigs", ""));
    a.hamlibAddModel = std::max(1, (int)d.getI("hamlibAddModel", a.hamlibAddModel));
    a.hamlibAddBaud = std::max(0, (int)d.getI("hamlibAddBaud", 0));
    snprintf(a.hamlibAddPath, sizeof a.hamlibAddPath, "%s", d.getS("hamlibAddPath", "").c_str());
    if (d.has("freqMhz")) a.freqMhz = d.getD("freqMhz", a.freqMhz);
    if (d.has("lna")) a.tune.lnaDb = (int)d.getI("lna", a.tune.lnaDb);
    if (d.has("vga")) a.tune.vgaDb = (int)d.getI("vga", a.tune.vgaDb);
    if (d.has("gain")) a.tune.gainDb = d.getD("gain", a.tune.gainDb);
    a.gainDev = d.getS("gainDev", "");
    a.tune.ampOn = d.getB("amp", false);
    a.recFormat = d.getI("recFormat", 0) == 1 ? 1 : 0;
    a.recSigmf = d.getB("recSigmf", true);
    a.tvBufMb = (int)d.getI("tvBufMb", 256); a.engine.player().setBufferMB(a.tvBufMb); a.tvBufMb = a.engine.player().bufferMB();
    a.tvRecMin = std::max(0, (int)d.getI("tvRecMin", 0));
    a.tvRecAtOn = d.getB("tvRecAtOn", false);
    a.tvRecAtH = std::max(0, std::min(23, (int)d.getI("tvRecAtH", 0)));
    a.tvRecAtM = std::max(0, std::min(59, (int)d.getI("tvRecAtM", 0)));
    a.engine.setRecordSigmf(a.recSigmf);
    if (d.has("family")) {
        const int f = std::max(0, std::min(kNumFamilies - 1, (int)d.getI("family", 0)));
        // the protocol entry; settings from before the split (HF digital, Mesh, Pagers, Marine as one entry) open the family's first
        a.sub = subIndexOf(d.getS("subMode", "").c_str());
        a.family = f;
        if (!curSub(a)) a.sub = firstSubOf(f);
        setFamilyFlags(a, f);
        if (a.fmMode && !(a.freqMhz > 0)) a.freqMhz = 100.0;   // a saved FM frequency outside the broadcast band is kept
    }
    for (int f = 0; f < kNumSlots; f++) {   // as many as savePrefs writes
        const std::string k = slotKey(f);
        if (!d.has(("gLna" + k).c_str())) continue;
        a.famGain[f] = {(int)d.getI(("gLna" + k).c_str(), 32), (int)d.getI(("gVga" + k).c_str(), 20), d.getB(("gAmp" + k).c_str(), true), true};
    }
    {   // a protocol entry never used yet starts with the gains its family had as one entry
        int n = 0;
        const SubMode* t = subModes(n);
        for (int i = 0; i < n && i < kNumSubModes; i++) if (!a.famGain[kNumFamilies + i].known && t[i].stdMode >= 8) a.famGain[kNumFamilies + i] = a.famGain[t[i].stdMode - 2];
    }
    if (const int sl = modeSlot(a); sl >= 0 && sl < kNumSlots && a.famGain[sl].known) { a.tune.lnaDb = a.famGain[sl].lna; a.tune.vgaDb = a.famGain[sl].vga; a.tune.ampOn = a.famGain[sl].amp; }
    for (int f = 6; f < kNumSlots; f++) a.famFreq[f] = d.getD(("fFreq" + slotKey(f)).c_str(), 0.0);
    if (const SubMode* sm = curSub(a)) {   // the protocol entry's own frequency
        const ModeTuning* mt = modeTuning(sm->stdMode);
        const double fr = a.famFreq[modeSlot(a)];
        if (mt && !(a.freqMhz >= mt->minMhz && a.freqMhz <= mt->maxMhz && fr > 0)) a.freqMhz = fr >= mt->minMhz && fr <= mt->maxMhz ? fr : sm->defMhz;
    }
    if (d.has("uiTextScale")) a.uiTextScale = std::max(0.8f, std::min(2.f, (float)d.getI("uiTextScale", 100) / 100.f));
    a.splitSide = (float)d.getI("splitSide", 0); a.splitRight = (float)d.getI("splitRight", 0); a.splitBottom = (float)d.getI("splitBottom", 0);
    a.splitDock = (float)d.getI("splitDock", 0); a.splitBot2 = (float)d.getI("splitBot2", 0);
    if (d.has("newUi")) a.newUi = d.getB("newUi", true);
    // the spectrum and the waterfall (plots.cpp): every choice is kept
    if (d.has("wfMap")) a.wf.map = std::max(-1, std::min((int)Waterfall::NumMaps - 1, (int)d.getI("wfMap", -1)));
    a.wf.autoTrack = d.getB("wfAuto", false);
    if (d.has("wfMin") && d.has("wfMax")) { a.wf.minDb = (float)d.getD("wfMin", a.wf.minDb); a.wf.maxDb = (float)d.getD("wfMax", a.wf.maxDb); }
    a.wf.setRate((float)d.getD("wfRate", 0.0));
    if (d.has("specMin") && d.has("specMax")) { a.yMin = (float)d.getD("specMin", a.yMin); a.yMax = (float)d.getD("specMax", a.yMax); }
    if (!(a.yMax - a.yMin >= 10)) { a.yMin = -110; a.yMax = -30; }
    if (d.has("specZoom")) a.specZoom = std::max(0.f, std::min(1.f, (float)d.getD("specZoom", a.specZoom)));
    if (!(a.wf.maxDb - a.wf.minDb >= 3)) { a.wf.minDb = -100; a.wf.maxDb = -32; }
    a.peakHold = d.getB("specHold", a.peakHold);
    a.holdDecay = std::max(0.f, std::min(60.f, (float)d.getD("holdDecay", a.holdDecay)));
    a.specAvgSec = std::max(0.f, std::min(5.f, (float)d.getD("specAvg", a.specAvgSec)));
    a.specFill = d.getB("specFill", a.specFill); a.specGrid = d.getB("specGrid", a.specGrid); a.specPeaks = d.getB("specPeaks", a.specPeaks);
    a.specReadout = d.getB("specReadout", a.specReadout); a.specDbm = d.getB("specDbm", a.specDbm);
    if (d.has("tuningMode")) a.tuningMode = std::max(0, std::min(1, (int)d.getI("tuningMode", 0)));
    a.modeGroupsOpen = d.getS("modeGroupsOpen", "");
    if (d.has("uiVariant")) a.uiVariant = std::max(0, std::min(7, (int)d.getI("uiVariant", 0)));
    if (d.has("dtmbBw")) a.dtmbBwMhz = d.getI("dtmbBw", 8) == 6 ? 6 : 8;
    if (d.has("isdbtBw")) { const int w = (int)d.getI("isdbtBw", 6); a.isdbtBwMhz = w == 0 || w == 7 || w == 8 ? w : 6; }
    if (d.has("atvFm")) a.atvFm = d.getI("atvFm", 0) != 0;
    if (d.has("atvNbtv")) a.atvNbtv = std::max(0, std::min(2, (int)d.getI("atvNbtv", 0)));
    if (d.has("atvNbtvLines")) a.atvNbtvLines = std::max(24, std::min(240, (int)d.getI("atvNbtvLines", 32)));
    if (d.has("atvNbtvFps")) a.atvNbtvFps = std::max(1.f, std::min(60.f, (float)d.getI("atvNbtvFps", 125) / 10.f));
    if (d.has("wideMaskLo")) a.wideFamMask = (uint64_t)(uint32_t)d.getI("wideMaskLo", 0) | ((uint64_t)(uint32_t)d.getI("wideMaskHi", 0) << 32);
    if (d.has("mtvOn")) a.mtvOn = d.getI("mtvOn", 0) != 0;
    if (d.has("mtvMax")) a.mtvMax = std::max(0, std::min(8, (int)d.getI("mtvMax", 0)));
    if (d.has("mtvGrid")) a.mtvGrid = std::max(0, std::min(2, (int)d.getI("mtvGrid", 0)));
    if (d.has("reduceCpuHidden")) a.reduceCpuHidden = d.getI("reduceCpuHidden", 1) != 0;
    if (d.has("discordOn")) a.discordOn = d.getI("discordOn", 0) != 0;
    if (d.has("discordFreq")) a.discordFreq = d.getI("discordFreq", 1) != 0;
    if (a.rxId == 1) {
        a.mapSend = d.getI("mapSend", 0) != 0;
        a.mapConsent = d.getI("mapConsent", 0) != 0;
        a.mapDeclined = d.getI("mapDeclined", 0) != 0;
        a.mapStationId = d.getS("mapStationId", "");
        a.mapServer = d.getS("mapServer", "");
        if (a.mapSend && !a.mapConsent) a.mapSend = false;   // never on without consent
    }
    if (d.has("lightUi")) a.lightUi = d.getI("lightUi", 0) != 0;
    if (d.has("highContrast")) a.highContrast = d.getI("highContrast", 0) != 0;
    a.uiLang = d.getS("uiLang", "");
    i18n::set(a.uiLang.empty() ? plat::uiLanguage() : a.uiLang);
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
            if (f.size() >= 10) { r.pi = (int)strtol(f[6].c_str(), nullptr, 16); r.tp = f[7] == "1"; r.rt = f[8]; r.psText = f[9]; }   // since full RDS
            if (r.found) a.fmScan.results.push_back(r);
        }
    }
    if (d.has("fmSaved")) {   // the saved-stations library, separate from the band-scan list in fmStations
        for (const auto& r : fmSavedDecode(d.getS("fmSaved", ""))) a.fmSaved.push_back(r);
    }
    if (d.has("fmMono")) a.fmMono = d.getI("fmMono", 0) != 0;
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
    a.audioIqMode = std::max(0, std::min(3, (int)d.getI("audioIq", 0)));
    if (d.has("outPath")) snprintf(a.filePath, sizeof a.filePath, "%s", d.getS("outPath", "").c_str());
    if (d.has("udpHost")) snprintf(a.udpHost, sizeof a.udpHost, "%s", d.getS("udpHost", "").c_str());
    if (d.has("udpPort")) a.out.port = (int)d.getI("udpPort", a.out.port);
    a.out.rtp = d.getB("udpRtp", false);
    a.out.dropNull = d.getB("dropNull", false);
    a.popTop = d.getB("popTop", true);
    a.subsOn = d.getB("subsOn", true); a.subsPick = d.getS("subsPick", ""); a.ttxPage = std::max(100, std::min(899, (int)d.getI("ttxPage", 100)));
    a.updCheck = d.getB("updCheck", true); a.updAuto = d.getB("updAuto", true); a.updPre = d.getB("updBeta", false);
    a.updLast = d.getD("updLast", 0); a.updSkip = d.getS("updSkip", "");
    if (a.rxId == 1) loadFeedPrefs(a);   // the feeds are shared by all receivers: RX 1 owns their settings
    a.channels = d.getChannels();
}

void savePrefs(const App& a) {
    plat::Prefs& d = plat::prefs();
    if (a.rxId == 1) {
        d.setS("netRadios", netRadiosToString(a.netRadios));
        d.setS("hamlibRigs", hamlibRigsToString(a.hamlibRigs));
        d.setI("hamlibAddModel", a.hamlibAddModel);   // the picker remembers its last choice
        d.setI("hamlibAddBaud", a.hamlibAddBaud);
        d.setS("hamlibAddPath", a.hamlibAddPath);
    }
    d.setS("devName", a.devIdx >= 2 && a.devIdx < (int)a.devices.size() ? a.devices[a.devIdx].name : std::string());   // the radio of this receiver, found again at the next start
    d.setD("freqMhz", a.freqMhz);
    d.setI("lna", a.tune.lnaDb);
    d.setI("vga", a.tune.vgaDb);
    d.setD("gain", a.tune.gainDb);
    d.setS("gainDev", a.gainDev);
    d.setB("amp", a.tune.ampOn);
    d.setI("family", a.family);
    { const SubMode* sm = curSub(a); d.setS("subMode", sm ? sm->id : ""); }
    d.setI("fmDeemph", a.fmDeemph);
    d.setI("fmMono", a.fmMono ? 1 : 0);
    d.setI("fmChanKhz", a.fmChanKhz);
    if (a.rxId == 1) d.setB("newUi", a.newUi);
    d.setI("wfMap", a.wf.map); d.setB("wfAuto", a.wf.autoTrack); d.setD("wfMin", a.wf.minDb); d.setD("wfMax", a.wf.maxDb); d.setD("wfRate", a.wf.rowsPerSec);
    d.setD("specMin", a.yMin); d.setD("specMax", a.yMax); d.setD("specZoom", a.specZoom);
    d.setB("specHold", a.peakHold); d.setD("holdDecay", a.holdDecay); d.setD("specAvg", a.specAvgSec);
    d.setB("specFill", a.specFill); d.setB("specGrid", a.specGrid); d.setB("specPeaks", a.specPeaks); d.setB("specReadout", a.specReadout); d.setB("specDbm", a.specDbm);
    d.setI("tuningMode", a.tuningMode);
    const int curSlot = modeSlot(a);
    for (int f = 0; f < kNumSlots; f++) {   // gains per mode (the current mode from the live settings)
        const bool cur = f == curSlot;
        if (!cur && !a.famGain[f].known) continue;
        const std::string k = slotKey(f);
        d.setI(("gLna" + k).c_str(), cur ? a.tune.lnaDb : a.famGain[f].lna);
        d.setI(("gVga" + k).c_str(), cur ? a.tune.vgaDb : a.famGain[f].vga);
        d.setB(("gAmp" + k).c_str(), cur ? a.tune.ampOn : a.famGain[f].amp);
    }
    for (int f = 6; f < kNumSlots; f++) if (a.famFreq[f] > 0 || (f == curSlot && a.family >= 6)) d.setD(("fFreq" + slotKey(f)).c_str(), f == curSlot ? a.freqMhz : a.famFreq[f]);
    if (a.rxId > 1) {   // a further receiver keeps only its own radio, mode, frequency and gains: everything else is shared and written by RX 1
        d.setI("standard", a.stdMode); d.setI("bw", a.bwIdx);
        d.flush();
        return;
    }
    d.setI("uiTheme", a.uiTheme);
    d.setS("uiLang", a.uiLang);
    d.setI("lightUi", a.lightUi ? 1 : 0);
    d.setI("highContrast", a.highContrast ? 1 : 0);
    d.setI("reduceCpuHidden", a.reduceCpuHidden ? 1 : 0);
    d.setI("discordOn", a.discordOn ? 1 : 0);
    d.setI("discordFreq", a.discordFreq ? 1 : 0);
    d.setI("mapSend", a.mapSend ? 1 : 0);
    d.setI("mapConsent", a.mapConsent ? 1 : 0);
    d.setI("mapDeclined", a.mapDeclined ? 1 : 0);
    d.setS("mapStationId", a.mapStationId);
    d.setS("mapServer", a.mapServer);
    d.setI("dtmbBw", a.dtmbBwMhz);
    d.setI("isdbtBw", a.isdbtBwMhz);
    d.setI("atvFm", a.atvFm ? 1 : 0);
    d.setI("wideMaskLo", (int)(uint32_t)(a.wideFamMask & 0xffffffffu)); d.setI("wideMaskHi", (int)(uint32_t)(a.wideFamMask >> 32));
    d.setI("mtvOn", a.mtvOn ? 1 : 0); d.setI("mtvMax", a.mtvMax); d.setI("mtvGrid", a.mtvGrid);
    d.setI("atvNbtv", a.atvNbtv);
    d.setI("atvNbtvLines", a.atvNbtvLines);
    d.setI("atvNbtvFps", (int)std::lround(a.atvNbtvFps * 10));
    d.setI("uiVariant", a.uiVariant);
    d.setI("uiTextScale", (int)std::lround(a.uiTextScale * 100));
    d.setI("splitSide", (int)std::lround(a.splitSide)); d.setI("splitRight", (int)std::lround(a.splitRight)); d.setI("splitBottom", (int)std::lround(a.splitBottom));
    d.setI("splitDock", (int)std::lround(a.splitDock)); d.setI("splitBot2", (int)std::lround(a.splitBot2));
    d.setS("modeGroupsOpen", a.modeGroupsOpen);
    {   // the stations the FM scan found: mhz|name|type|snr|stereo|rds|pi (hex)|tp|radio text|scrolling name, one per line
        std::string st;
        for (const auto& r : a.fmScan.results) {
            if (!r.found) continue;
            auto clean = [](std::string s) { for (auto& c : s) if (c == '|' || c == '\n') c = ' '; return s; };
            char b[64]; snprintf(b, sizeof b, "%.1f|", r.mhz);
            st += b + clean(r.name) + "|" + clean(r.pty) + "|" + std::to_string((int)r.snr) + "|" + (r.stereo ? "1" : "0") + "|" + (r.rds ? "1" : "0");
            char pi[8]; snprintf(pi, sizeof pi, "%04X", r.pi & 0xFFFF);
            st += std::string("|") + pi + "|" + (r.tp ? "1" : "0") + "|" + clean(r.rt) + "|" + clean(r.psText) + "\n";
        }
        if (!a.fmScan.running) d.setS("fmStations", st);   // not a half-finished scan
    }
    d.setS("fmSaved", fmSavedEncode(a.fmSaved));   // the saved-stations library, not the scan results
    d.setChannels(a.channels);
    d.setI("audioIq", a.audioIqMode);
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
    d.setB("subsOn", a.subsOn); d.setS("subsPick", a.subsPick); d.setI("ttxPage", a.ttxPage);
    d.setB("updCheck", a.updCheck); d.setB("updAuto", a.updAuto); d.setB("updBeta", a.updPre);
    d.setD("updLast", a.updLast); d.setS("updSkip", a.updSkip);
    saveFeedPrefs(a);
    d.flush();
}

void ingestSpectrum(App& a) {
    if (!a.engine.latestSpectrum(a.spec, a.lastSeq)) return;   // straight into the kept frame: no copy that allocates, 30 times a second
    const SpectrumFrame& f = a.spec;
    a.lastSeq = f.seq;
    const double now = glfwGetTime();
    const float dt = a.specLastT > 0 ? (float)std::min(0.5, std::max(0.001, now - a.specLastT)) : 1.f / 30;
    a.specLastT = now;
    size_t n = f.dbfs.size();
    if (a.smooth.size() != n || a.peak.size() != n) { a.smooth = f.dbfs; a.peak = f.dbfs; }   // both lines always have the frame size (a retune clears the peak line)
    const float alpha = a.specAvgSec <= 0.001f ? 1.f : 1.f - std::exp(-dt / a.specAvgSec);   // a time constant, so the look does not depend on the frame rate
    const float fall = a.holdDecay * dt;
    float* sm = a.smooth.data(); float* pk = a.peak.data(); const float* in = f.dbfs.data();
    for (size_t i = 0; i < n; i++) {
        sm[i] += alpha * (in[i] - sm[i]);
        pk[i] = a.holdDecay > 0 ? std::max(pk[i] - fall, in[i]) : std::max(pk[i], in[i]);
    }
    if (a.wf.offer(f.dbfs, now)) a.wf.stamp(now);
    if (a.wf.autoTrack && !a.wf.paused && now - a.wf.trackT > 0.25) { a.wf.trackT = now; a.wf.fitRange(a.smooth, false); }
}

// The level offset to dBm of the selected radio (what the antenna feeds in for a full-scale sample: found with a signal generator), kept per radio by name
static std::string gDbmKey; static float gDbmVal = 0;
float dbmOffset(const App& a) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return 0;
    const std::string k = "dbmOff_" + a.devices[a.devIdx].name;
    if (k != gDbmKey) { gDbmKey = k; gDbmVal = (float)plat::prefs().getD(k.c_str(), 0.0); }
    return gDbmVal;
}
void setDbmOffset(const App& a, float db) {
    if (a.devIdx < 0 || a.devIdx >= (int)a.devices.size()) return;
    gDbmKey = "dbmOff_" + a.devices[a.devIdx].name; gDbmVal = db;
    plat::prefs().setD(gDbmKey.c_str(), db);
    plat::prefs().flush();
}

void ingestRx(App& a) {
    double now = glfwGetTime();
    if (a.engine.running() && now - a.tsT > 0.25) {
        a.ts = a.engine.tsSnapshot(); a.bb = a.engine.bbStats(); a.tsT = now;
        if (a.engine.multiChannelActive()) a.mtvSvc = a.engine.multiServices(); else a.mtvSvc.clear();
    }
    RxTelemetry t;
    if (!a.engine.latestRx(t, a.rxSeq)) return;
    a.rxSeq = t.seq;
    if (t.p1Count > 0) a.rxSeen = true;
    if (t.dataFrames < a.rx.dataFrames) { a.mpd.reset(); a.quality.reset(); a.cst.mod = -1; } // receiver was restarted or retuned
    a.rx = std::move(t);
    if (a.rx.standard == 12) uplinkNoteAdsb(a.rx.adsb);   // ADS-B callsigns for the Mode S uplink tables
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
    const bool file = !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() &&
                      (a.devices[a.devIdx].kind == DeviceInfo::File || a.devices[a.devIdx].isAudio() || a.devices[a.devIdx].kind == DeviceInfo::Hamlib);
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

