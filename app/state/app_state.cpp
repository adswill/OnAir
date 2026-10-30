// application state: preferences, device list, bandwidth, ingesting spectrum and receiver telemetry, channel list
#include "rx_width.h"
#include "../rooms/rooms.h"
#include "../app.h"
#include "../clips/clips_ui.h"
#include "../gfx/file_vfo.h"
#include <cstring>
#include "dect2/source/usb_diag.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/hdr/hdr_rx.h"
#include "dect2/sonde/sonde_rx.h"

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
void setFamilyFlags(App& a, int f) {
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
int firstSubOf(int f) {   // HF digital, mesh, pagers and marine are only listed as their protocols: 0 for the other families
    for (const auto& m : allModes()) if (m.family == f && !m.subId) return 0;   // a family with a plain entry of its own (the general receiver next to AM broadcast) keeps it
    int n = 0;
    const SubMode* t = subModes(n);
    for (int i = 0; i < n; i++) if (t[i].stdMode == f + 2) return i + 1;
    return 0;
}
static int slotOf(int f, int sub) { return sub >= 1 && sub <= kNumSubModes ? kNumFamilies + sub - 1 : f; }
int modeSlot(const App& a) { return slotOf(a.family, curSub(a) ? a.sub : 0); }
std::string slotKey(int slot) {   // the suffix of the per-mode settings: the family number, or "." and the protocol's id
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
        else if (a.family == 5 || f >= 6) { a.tune.lnaDb = 32; a.tune.vgaDb = 34; a.tune.ampOn = false; }   // the amp is off unless the user turns it on (it can be damaged by strong signals): its 14 dB are in the VGA
        else a.tune.ampOn = false;   // a mode without remembered gains (the TV modes) never inherits another mode's amp
    }
    // #338: between two satellite modes the frequency stays when it is on the LNB's output
    double carryMhz = 0;
    if (a.family >= 6 && f >= 6 && a.family != f) {
        const ModeTuning* ot = modeTuning(a.family + 2);
        const ModeTuning* nt = modeTuning(f + 2);
        // Only the modes that listen to a satellite dish's LNB output (1000 to 2100 MHz): DVB-S/S2 (8), MAC (49), QO-100 (55), the TV sat beacon decoder
        // (146) and ISDB-S (156). The tuning ranges cannot tell them apart any more (every mode accepts any frequency), and a frequency such as 1927 MHz
        // used to follow the user into ADS-B, airband or FM.
        const auto lnb = [](const ModeTuning* t) { return t->stdMode == 8 || t->stdMode == 49 || t->stdMode == 55 || t->stdMode == 146 || t->stdMode == 156; };
        if (ot && nt && lnb(ot) && lnb(nt) && a.freqMhz >= 1000 && a.freqMhz <= 2100) carryMhz = a.freqMhz;
    }
    a.sub = sub;
    setFamilyFlags(a, f);
    if (f >= 6) {
        const ModeTuning* mt = modeTuning(f + 2);
        const SubMode* sm = curSub(a);
        const double def = sm ? sm->defMhz : mt ? mt->defMhz : a.freqMhz;
        a.freqMhz = mt && a.famFreq[to] >= mt->minMhz && a.famFreq[to] <= mt->maxMhz ? a.famFreq[to] : def;
        if (carryMhz > 0) a.freqMhz = carryMhz;
    }
    if (f == 0 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 522.0;       // a TV channel, not a leftover FM or DAB frequency
    if (f == 1 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 473.0;
    if (f == 3 && !(a.freqMhz >= 170 && a.freqMhz <= 870)) a.freqMhz = 522.0;
    if (f == 2 && !(a.freqMhz >= 174 && a.freqMhz <= 240)) a.freqMhz = 218.640;
    if (f == 4 && !(a.freqMhz >= 170 && a.freqMhz <= 770)) a.freqMhz = 473.143;   // the centre of UHF channel 13/14 of the 6 MHz raster
    if (f == 5) {   // FM band (with the plan's extended FM below 87.5 MHz), default to 100 MHz
        double lo = 87.5, hi = 108;
        fmBandRange(a, lo, hi);
        if (!(a.freqMhz >= lo && a.freqMhz <= hi)) a.freqMhz = 100.0;
    }
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
    syncNetRadios(a);   // the list is shared by all receivers: what another one added or removed
    setNetworkRadios(a.netRadios);   // the ones the user added by address come after the ones found
    setHamlibRigs(a.hamlibRigs);
    setSynthGroups(a.synthGroups);   // the saved radio groups: listed after the radios they are made of
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
    for (auto& h : limeLibraryHints()) a.usbHints.push_back(h);
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
    const DeviceInfo& dv = a.devices[a.devIdx];
    const std::string rk = dv.kind == DeviceInfo::Synthetic ? std::string("synthetic") : radioKeyOf(dv);   // (3.4: the test signal keeps a chosen rate too)
    return rk.empty() ? "" : rxRateKey(rateModeId(a), rk);
}
std::string rateContext(const App& a) { return ratePrefKey(a) + "|" + std::to_string(a.devIdx) + "|" + std::to_string(a.family) + "." + std::to_string(a.sub) + "|" + std::to_string(a.bwIdx) + "|" + std::to_string(a.dtmbBwMhz) + "|" + std::to_string(rxWidthMhz(a)); }
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
        } else if (mt->stdMode == 22 && meshRtlRate(a) > 0) {   // Mesh on an RTL-SDR: 1.024 Msps, no lost samples (#386)
            a.tune.sampleRate = meshRtlRate(a); a.tune.basebandFilterHz = 0.45 * a.tune.sampleRate;
        } else if (mt->stdMode == 22 && meshSampleRate(a) > mt->sampleRate) {   // Mesh: 2.4 GHz LoRa (812.5 / 1625 kHz) needs more
            a.tune.sampleRate = meshSampleRate(a); a.tune.basebandFilterHz = 0.875 * a.tune.sampleRate;
        } else if (mt->stdMode == 61) {   // Meteor-M LRPT: a 2 Msps capture keeps the 150 kHz channel inside the window while the tracker moves the centre
            a.tune.sampleRate = 2e6; a.tune.basebandFilterHz = 0;
        } else if (mt->stdMode == 14) {   // GNSS: the rate of the tune (L1 / E1, BeiDou B1I, ...)
            a.tune.sampleRate = gnssAutoRate(a.freqMhz);
            if (a.tune.sampleRate > 2 * mt->basebandHz) a.tune.basebandFilterHz = 0.8 * a.tune.sampleRate;
        } else if (mt->stdMode == 23 && hdrIsAmBand(a.freqMhz)) {   // HD Radio AM: a 30 kHz station; 1 Msps suits the HF radios (an HF+ gives 912 ksps)
            a.tune.sampleRate = hdrSampleRate(a.freqMhz); a.tune.basebandFilterHz = 0;
        } else if (mt->stdMode == 72) {   // Wi-Fi HaLow: the rate of the receive width (4 Msps Auto 1 - 4 MHz, 20 Msps Auto all, else the width's own); the marker shows the channel width
            a.tune.sampleRate = halowWantedRateHz(); a.tune.bandwidthMhz = halowWantedWidthMhz(); a.tune.basebandFilterHz = 0;
        } else if (mt->stdMode == 16 && a.freqMhz > 156.9 && a.freqMhz < 161.9) {   // AIS between the pairs: one capture for AIS 1 + 2 and 75 + 76 (message 27)
            a.tune.bandwidthMhz = 5.4; a.tune.sampleRate = 8e6; a.tune.basebandFilterHz = 7e6;
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
    // 3.4: the custom bandwidth of any mode (Advanced > Custom); the mode's own value stays the default. It replaces the bandwidth, the rate that
    // carries it and the radio's analogue filter, whatever the branches above chose.
    a.autoBwMhz = a.tune.bandwidthMhz;
    if (const double w = rxWidthMhz(a); w > 0) {
        const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
        const RxWidthPlan rw = rxWidthPlan(w, a.tune.bandwidthMhz, a.tune.sampleRate, a.tune.basebandFilterHz, mt ? mt->minSampleRate : 0);
        a.tune.bandwidthMhz = rw.bandwidthMhz; a.tune.sampleRate = rw.sampleRate; a.tune.basebandFilterHz = rw.basebandHz;
        const DeviceInfo& dv = a.devices[a.devIdx];
        if (dv.isGeneric()) a.tune.sampleRate = linkRateFor(dv, a.tune, a.tune.sampleRate, a.tune.sampleRate);   // a link that is too slow: some samples lost, the width kept
    }
    // the rate chosen for this mode on this radio replaces the mode's own (Auto: nothing changes); one outside this radio's limits or below
    // what the mode needs now (a wider DVB channel) is not used, the control shows Auto
    a.autoRateHz = a.tune.sampleRate;
    a.chosenRateHz = 0;
    a.rateCtx = rateContext(a);
    if (const double want = savedSampleRate(a); want > 0 && (a.devices[a.devIdx].isRadio() || a.devices[a.devIdx].kind == DeviceInfo::Synthetic)) {   // (3.4: any saved rate is used; the driver decides)
        const RateCheck c = checkManualRate(rateLimitsOf(a.devices[a.devIdx]), modeMinSampleRate(a), want);
        if (c.ok) { a.tune.sampleRate = want; a.chosenRateHz = want; }
    }
    // the rate was lowered to the radio's top: a filter meant for the faster rate lets everything up to its edge fold into the band (the
    // 8-9 MHz filters of DTMB, analog TV, DVB-S and Iridium on a PlutoSDR's 4 Msps over USB); a rate the user raised: the mode's narrow filter
    // would hide what the wider capture is for (P25 traffic channels away from the control channel), so the radio opens its own filter. A
    // width the user typed (Advanced > Custom bandwidth) is the user's filter and is kept as planned: the user decides, never the heuristic (3.4).
    if (rxWidthMhz(a) > 0) { if (a.tune.basebandFilterHz > a.tune.sampleRate) a.tune.basebandFilterHz = 0; }
    else a.tune.basebandFilterHz = basebandFilterFor(a.tune.basebandFilterHz, a.autoRateHz, a.tune.sampleRate);
    a.tune.rateChosen = a.chosenRateHz > 0 || rxWidthMhz(a) > 0;   // a rate or width the user chose is not held down by a driver's own cap (3.4)
}

std::string openFileDialog() { return plat::openFileDialog(); }

std::string saveFileDialog(const char* name) { return plat::saveFileDialog(name); }

// the spectrum settings the engine keeps: the FFT size (also the saved one at start) and the noise blanker
static void syncSpectrumEngine(App& a) {
    if (a.engine.spectrumFft() != a.specFft) a.engine.setSpectrumFft(a.specFft);
    static bool on = false; static float thr = 0, wid = 0; static const Engine* who = nullptr;
    if (who != &a.engine || on != a.nbOn || thr != a.nbThr || wid != a.nbWidthUs) {
        who = &a.engine; on = a.nbOn; thr = a.nbThr; wid = a.nbWidthUs;
        a.engine.setNoiseBlanker(a.nbOn, a.nbThr, a.nbWidthUs);
    }
}

double specBinsPerPixel();   // plots.cpp: FFT bins per pixel column of the spectrum as last drawn

// Fast waterfall rows (Waterfall::fast(), the CW preset): the engine makes a row every 1/rowsPerSec seconds from its own short transforms and queues
// them; a frame of the screen takes whatever has come (several rows) and writes them all, so the rate does not depend on the frame rate.
static void fastRowsIngest(App& a) {
    const double want = a.wf.fast() ? a.wf.rowsPerSec : 0;
    if (a.wf.fastSent != want) { a.engine.setSpectrumFastRows(want); a.wf.fastSent = want; }
    if (want <= 0) return;
    static std::vector<float> rows;   // (one waterfall per App; the buffer ping-pongs with the engine's queue: no allocation in the steady state)
    size_t bins = 0; double dt = 0; uint64_t dropped = 0;   // dt: the engine's exact row time (the scale uses 1 / rowsPerSec, the same)
    const size_t n = a.engine.takeFastRows(rows, bins, dt, dropped);
    if (!n || bins < 64) return;
    const double fsHz = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
    const double now = glfwGetTime();
    a.wf.fastBins = bins;
    a.wf.setCols(specaxis::historyCols(a.wf.detail, bins, Waterfall::kMaxCols));
    a.wf.follow(radioCenterMhz(a), fsHz / 1e6);
    const float off = a.specPerHz ? spipe::perHzOffset(fsHz, bins) : 0.f;
    bool any = false;
    for (size_t r = 0; r < n; r++) {
        float* row = rows.data() + r * bins;
        if (off != 0.f) for (size_t i = 0; i < bins; i++) row[i] -= off;
        any = a.wf.offerFast(row, bins) || any;
    }
    if (any) { a.wf.pushT = now; wfExportRow(a, now); }
    static const bool perf = getenv("DECT2_FPS") != nullptr;   // DECT2_FPS=1: rows a second taken, the biggest batch, rows the engine dropped
    if (perf) {
        static double t0 = now; static size_t rowsN = 0, maxBatch = 0; static uint64_t drop = 0;
        rowsN += n; maxBatch = std::max(maxBatch, n); drop += dropped;
        if (now - t0 > 2.0) { const float* last = rows.data() + (n - 1) * bins; size_t pk = 0; for (size_t i = 1; i < bins; i++) if (last[i] > last[pk]) pk = i;
            fprintf(stderr, "fast rows: %.0f a second (wanted %.0f), %zu bins, biggest batch %zu, dropped %llu, newest row peak %.1f dB at bin %zu\n", rowsN / (now - t0), a.wf.rowsPerSec, bins, maxBatch, (unsigned long long)drop, last[pk], pk); t0 = now; rowsN = maxBatch = 0; drop = 0; }
    }
    (void)dt;
}

void ingestSpectrum(App& a) {
    syncSpectrumEngine(a);
    Waterfall::themeMode() = pal::wfMode();
    static const bool legacy = getenv("DECT2_SPEC_LEGACY") != nullptr;   // the pre-3.0 pipeline, for CPU comparisons (core spectrum.cpp too)
    if (legacy) { a.wf.detail = 0; a.wf.rowTau = 0; }
    if (a.specDemo && !a.engine.running()) {   // --specdemo: the synthetic test spectrum (spec_look.h), 30 frames a second, no radio needed
        static double nextT = 0;
        const double t = glfwGetTime();
        if (t < nextT) return;
        nextT = t + 1.0 / 30;
        const double fsMhz = a.tune.sampleRate > 0 ? a.tune.sampleRate / 1e6 : 10.0;
        slook::demoSpectrum(a.spec.dbfs, 8192, radioCenterMhz(a), fsMhz, t, (uint32_t)(a.lastSeq + 1));
        a.spec.seq = a.lastSeq + 1;
    } else
    if (!a.engine.latestSpectrum(a.spec, a.lastSeq)) { fastRowsIngest(a); return; }   // straight into the kept frame: no copy that allocates, 30 times a second
    const SpectrumFrame& f = a.spec;
    const uint64_t prevSeq = a.lastSeq;
    a.lastSeq = f.seq;
    const double now = glfwGetTime();
    const float dt = a.specLastT > 0 ? (float)std::min(0.5, std::max(0.001, now - a.specLastT)) : 1.f / 30;
    a.specLastT = now;
    size_t n = f.dbfs.size();
    // the frame as the plots show it: dB per Hz (the floor does not move when the FFT size changes) or the engine's dB per bin
    {
        const double fsHz = a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate;
        const float off = a.specPerHz ? spipe::perHzOffset(fsHz, n) : 0.f;
        a.specView.resize(n);
        const float* src = f.dbfs.data(); float* dst = a.specView.data();
        for (size_t i = 0; i < n; i++) dst[i] = src[i] - off;
    }
    if (a.smooth.size() != n || a.peak.size() != n) { a.smooth = a.specView; a.peak = a.specView; a.minPk.clear(); a.smoothPow.clear(); }   // the lines always have the frame size (a retune clears the hold lines)
    if (a.minHold && a.minPk.size() != n) a.minPk = a.specView;
    const float fall = a.holdDecay * dt;
    float* pk = a.peak.data(); const float* in = a.specView.data();
    // the trace: averaged in POWER with a time constant (the look does not depend on the frame rate) that grows with the bins each pixel
    // column covers, so a big FFT on a narrow plot keeps a smooth noise floor instead of a comb (shared with tools/spec_render)
    spipe::powerEma(a.smoothPow, in, a.smooth.data(), n, spipe::alphaFor(dt, spipe::traceTau(a.specAvgSec, specBinsPerPixel())));
    // the choice of peak rule is made once, outside the loops, so each is a plain loop the compiler vectorises (the branch inside kept it scalar)
    if (a.holdDecay > 0) for (size_t i = 0; i < n; i++) pk[i] = std::max(pk[i] - fall, in[i]);
    else for (size_t i = 0; i < n; i++) pk[i] = std::max(pk[i], in[i]);
    if (a.minHold && a.minPk.size() == n) {   // the min-hold line: the lowest level, rising back at the hold decay
        float* mn = a.minPk.data();
        if (a.holdDecay > 0) for (size_t i = 0; i < n; i++) mn[i] = std::min(mn[i] + fall, in[i]);
        else for (size_t i = 0; i < n; i++) mn[i] = std::min(mn[i], in[i]);
    }
    {   // the history has the width the detail setting and the FFT give (#243), and follows the radio: a retune moves the rows along (#270)
        const double fsMhz = (a.engine.sampleRate() > 0 ? a.engine.sampleRate() : a.tune.sampleRate) / 1e6;
        a.wf.setCols(specaxis::historyCols(a.wf.detail, a.wf.fast() && a.wf.fastBins ? a.wf.fastBins : n, Waterfall::kMaxCols));
        a.wf.follow(radioCenterMhz(a), fsMhz);   // also while paused: the frozen picture must stay under its frequencies
    }
    // frames the screen did not take (it drew slower than they came): the row is repeated for them, so the picture keeps its true speed (#246)
    const int missed = a.wf.rowsPerSec <= 0 ? specaxis::missedRows(prevSeq, f.seq) : 0;
    if (a.wf.fast()) fastRowsIngest(a);   // the rows come from the engine's own short transforms, in batches
    else if (a.wf.offer(a.specView, now, missed)) {
        const int k = a.wf.slow() ? 1 : 1 + missed;   // one time stamp per row, spread over the gap
        const double t0 = a.wf.pushT > 0 && a.wf.pushT < now ? a.wf.pushT : now;
        for (int i = 1; i <= k; i++) a.wf.stamp(t0 + (now - t0) * i / k);
        wfExportRow(a, now);
    }
    roomsFeedRow(a.specView, a.wf.minDb, a.wf.maxDb);   // Live rooms (a flag check when not live)
    clipsui::feed(f.dbfs);   // radio clips: the retroactive history
    if (a.wf.autoTrack && !a.wf.paused && now - a.wf.trackT > 0.25) {
        const float tdt = a.wf.trackT > 0 ? (float)std::min(2.0, now - a.wf.trackT) : 0.25f;
        a.wf.trackT = now; a.wf.fitRange(a.smooth, false, tdt);
    }
    // the spectrum's own dB range on the floor of the view (specAutoDb): the look puts the floor specBelow dB over the bottom, the span stays
    static double specT = 0;
    if (a.specAutoDb && now - specT > 0.25 && n >= 64) {
        const float tdt = specT > 0 ? (float)std::min(2.0, now - specT) : 0.25f;
        specT = now;
        size_t i0 = (size_t)(std::max(0.0, std::min(1.0, a.wf.viewU0)) * n), i1 = (size_t)(std::max(0.0, std::min(1.0, a.wf.viewU1)) * n);
        if (i1 < i0 + 32) { i0 = 0; i1 = n; }
        static std::vector<float> scratch;
        const float fl = a.specFloor.update(spipe::noiseFloor(a.smooth.data() + i0, i1 - i0, scratch), tdt);
        if (std::isfinite(fl)) {
            const slook::LookSet& L = slook::lookSet(a.specLook);
            const float span = std::max(30.f, a.yMax - a.yMin), lo = std::round((fl - L.specBelow) * 2.f) / 2.f;   // half-dB steps: the labels do not shimmer
            if (std::fabs(lo - a.yMin) >= 0.5f) { a.yMin = lo; a.yMax = lo + span; }
        }
    }
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
    if (!a.engine.running()) a.liveMuxValid = false;
    else {   // another transponder (frequency, symbol rate, mode) tuned: the services of the old one go (the demux itself drops them when the stream id changes)
        MuxId cur; cur.freqHz = a.tune.centerHz; cur.family = a.family; cur.symRateHz = dvbsSymbolRateMsym() * 1e6;
        if (a.liveMuxValid && isDifferentMux(a.liveMux, cur)) { a.engine.clearServices(); a.ts = TsSnapshot(); a.mtvSvc.clear(); }
        a.liveMux = cur; a.liveMuxValid = true;
    }
    if (a.engine.running() && now - a.tsT > 0.25) {
        a.ts = a.engine.tsSnapshot(); tvRecOverrideTs(a); a.bb = a.engine.bbStats(); a.tsT = now;
        tvHdIngest(a);
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

bool fileSourceSelected(const App& a) {
    return !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() && a.devices[a.devIdx].kind == DeviceInfo::File;
}
bool fileVfoActive(const App& a) { return fileSourceSelected(a) && a.engine.running() && a.engine.fileCentreHz() > 0; }
double fileCentreMhz(const App& a) {
    if (fileVfoActive(a)) return a.engine.fileCentreHz() / 1e6;
    return a.file.centreHz > 0 ? a.file.centreHz / 1e6 : a.freqMhz;
}

// A click, a drag, a typed or wheel-stepped frequency on a recording: the spectrum and waterfall stay on the recording's centre and rate (the
// file cannot retune anything); only the demodulator's offset moves, clamped so the passband stays inside the recorded span.
void fileSetVfo(App& a, double absMhz) {
    if (!fileVfoActive(a)) { a.freqMhz = absMhz; return; }
    const double bw = std::max(0.0, channelWidth(a).mhz) * 1e6;
    const filevfo::Plan p = filevfo::plan(absMhz, fileCentreMhz(a), a.engine.sampleRate(), bw);
    a.freqMhz = p.vfoMhz;
    a.tune.centerHz = p.vfoMhz * 1e6;
    a.engine.setFileVfoHz(a.tune.centerHz);
    a.fileEdgeCode = p.tooWide ? 3 : p.clamped ? 2 : p.nearEdge ? 1 : 0;
    savePrefs(a);
}

// The File info dialog: the true centre frequency of a recording whose header lacks it. The tune box keeps its offset from the centre.
void fileSetCentre(App& a, double centreMhz) {
    if (!(centreMhz > 0)) return;
    const double off = fileVfoActive(a) ? a.engine.decodeShiftHz() / 1e6 : a.freqMhz - fileCentreMhz(a);
    a.file.centreHz = centreMhz * 1e6;
    a.freqMhz = centreMhz + off;
    a.tune.centerHz = a.freqMhz * 1e6;
    if (fileVfoActive(a)) a.engine.setFileCentreHz(a.file.centreHz);
    a.peak.clear(); a.minPk.clear();
    savePrefs(a);
}

// Before a recording starts: its centre is kept (the first start takes the frequency typed as it); a frequency box value inside the span is the
// VFO offset, one far outside it (a mode's own default) leaves the VFO on the centre
void fileStartPrep(App& a) {
    if (!fileSourceSelected(a)) return;
    if (a.file.centreHz <= 0) a.file.centreHz = a.freqMhz * 1e6;
    const double rate = a.file.sampleRate;
    if (rate > 0 && std::fabs(a.freqMhz * 1e6 - a.file.centreHz) > filevfo::usableHalfSpanHz(rate)) a.freqMhz = a.file.centreHz / 1e6;
    a.tune.centerHz = a.freqMhz * 1e6;
}

double radioCenterMhz(const App& a) {
    if (fileVfoActive(a)) return a.engine.fileCentreHz() / 1e6;   // a recording: the axis never moves
    const ModeTuning* mt = a.family >= 6 ? modeTuning(a.family + 2) : nullptr;
    const bool file = !a.devices.empty() && a.devIdx >= 0 && a.devIdx < (int)a.devices.size() &&
                      (a.devices[a.devIdx].kind == DeviceInfo::File || a.devices[a.devIdx].isAudio() || a.devices[a.devIdx].kind == DeviceInfo::Hamlib);
    const double shift = a.engine.running() ? a.engine.decodeShiftHz() / 1e6 : 0.0;   // sticky tuning: the tuned frequency sits this far from the radio centre
    return a.freqMhz - shift + (mt && !file ? mt->tuneOffsetHz / 1e6 : 0.0) + audioViewOffsetHz(a) / 1e6;
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

// The name of an unnamed channel in the current mode (engine standard numbers: 0 DVB-T2, 1 DVB-T, 2 ATSC, 4 ATSC 3.0, 5 ISDB-T, 7 DVB-S/S2, 8 DTMB):
// a DTMB channel used to be listed as "DVB-T2 mux" (issue: Chinese users read it as a wrong standard).
const char* muxName(const App& a) {
    switch (a.engine.activeStandard()) {
        case 1: return "DVB-T mux";
        case 2: return "ATSC mux";
        case 4: return "ATSC 3.0 mux";
        case 5: return "ISDB-T mux";
        case 7: return "DVB-S/S2 transponder";
        case 8: return "DTMB";
        default: return "DVB-T2 mux";
    }
}

std::string channelLabel(const SavedChannel& c, const char* unnamed) {
    char b[200];
    snprintf(b, sizeof b, "%.3f MHz  %s%s", c.freqMhz, c.name.empty() ? unnamed : c.name.c_str(), c.nServices > 1 ? (" +" + std::to_string(c.nServices - 1)).c_str() : "");
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
        a.engine.log("channel: " + channelLabel(c, muxName(a)));
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

