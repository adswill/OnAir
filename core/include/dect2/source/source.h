// IQ sample sources: HackRF, recorded file, built-in synthetic DVB-T2-like signal.
#pragma once
#include <cstdio>
#include "dect2/common/ring.h"
#include "dect2/t2/t2gen.h"
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// Synthetic test-signal configuration (never transmitted; samples are generated in memory only).
struct SynthConfig {
    TxParams tx;           // FFT size, guard interval, ...
    double snrDb = 30;
    double cfoHz = 0;
    double sroPpm = 0;
    double echoDb = 0;     // 0 = off; otherwise echo attenuation
    int echoDelay = 300;   // samples
    bool atsc = false;     // generate ATSC 8-VSB (6 MHz channel) instead
    bool dab = false;      // generate a DAB (mode I) ensemble instead
    bool demoTv = false;   // DVB-T / ATSC: carry a looping test-card programme (picture and beep) instead of the small test stream
    bool dvbt = false;     // generate DVB-T (with a small test transport stream) instead of DVB-T2
    int dvbtMode = 1, dvbtGuard = 2, dvbtMod = 2, dvbtRate = 1; // 8K, GI 1/8, 64-QAM, 2/3
    int isdbtBwMhz = 6;    // ISDB-T test signal: channel width 6, 7 or 8 MHz
    double pace = 1.0;     // 1 = real time; below 1 the signal runs in slow motion (tests on slow machines: the receiver gets 1/pace times the time)
    bool gainModel = false; // scale the level with the LNA/VGA/amp gains (for testing AGC): 62 dB total = the nominal level
    int mode = 0;           // 0: the DVB / ATSC / DAB settings above; 8 and up: the test signal of that engine standard (see mode_synth.h)
    int modeOpt[8] = {};    // options of that test signal; every mode says what they mean in its <mode>_gen.h
    double modeVal[4] = {};
};

struct TuneSettings {
    double bandwidthMhz = 8;
    int isdbtBwMhz = 6;                // ISDB-T channel width: 6 (Japan, the Americas), 7, 8 MHz (the Maldives); 0 = automatic (6, 7 and 8 tried)
    SynthConfig synth;
    double centerHz = 522e6;
    double sampleRate = 18285714.2857; // 2x native rate for an 8 MHz channel
    double basebandFilterHz = 0;       // 0 = auto
    double gainDb = 30;                // overall gain of generic (SoapySDR) radios; the three HackRF stages below are not used there
    int lnaDb = 16;                    // 0..40 step 8
    int vgaDb = 20;                    // 0..62 step 2
    bool ampOn = false;
    bool biasTee = false;              // DC power up the antenna input (active antennas, LNAs); only radios with DeviceInfo::hasBiasTee act on it
    // The radio's own settings the user changed (DeviceInfo::settings): key -> value. A key that is not here means the setting's default,
    // which is what the driver did before the setting existed.
    std::map<std::string, std::string> radio;
};

// A hardware setting of a radio beyond its gain and antenna power: frequency correction, direct sampling, notch filters, gain modes, ...
// The radio's entry lists the ones it has (DeviceInfo::settings); the app shows them, keeps the user's choices per radio and hands them to
// the driver in TuneSettings::radio, which applies them on start and on every retune.
struct RadioSetting {
    enum Type { Bool, Choice, Number } type = Bool;
    std::string key;                       // the key in TuneSettings::radio
    std::string label, help;               // what the app shows, and its tooltip
    std::vector<std::string> values;       // Choice: the values the driver understands ...
    std::vector<std::string> names;        // ... and what the app shows for each
    double minV = 0, maxV = 0, step = 0;   // Number
    std::string unit;                      // Number: "ppm", "dB", ...
    std::string def;                       // the default: "1"/"0" for Bool, one of values for Choice, a number
    bool restart = false;                  // takes effect when the radio is opened (the app restarts a running receiver)
};

// The values in TuneSettings::radio as the drivers read them (def when the user did not set the key)
inline std::string radioOption(const TuneSettings& s, const std::string& key, const std::string& def = std::string()) {
    const auto it = s.radio.find(key);
    return it == s.radio.end() ? def : it->second;
}
inline bool radioFlag(const TuneSettings& s, const std::string& key, bool def = false) {
    const auto it = s.radio.find(key);
    return it == s.radio.end() ? def : (it->second == "1" || it->second == "true");
}
inline double radioNumber(const TuneSettings& s, const std::string& key, double def = 0) {
    const auto it = s.radio.find(key);
    if (it == s.radio.end() || it->second.empty()) return def;
    char* end = nullptr;
    const double v = strtod(it->second.c_str(), &end);
    return end && end != it->second.c_str() ? v : def;
}
// The frequency correction every radio offers (key "ppm"): the radio's clock error in parts per million, positive when its clock runs fast
// (a signal then shows up below its real frequency)
// "Decimate in OnAir" (#233, radio setting "decim"): the radio runs this many times faster than the mode asks and OnAir brings the
// samples down to the mode's rate itself (for radios that are poor at low rates). 1 = off; 2, 4, 8 or 16.
inline int radioDecimation(const TuneSettings& s) {
    const int d = (int)radioNumber(s, "decim", 1);
    return d == 2 || d == 4 || d == 8 || d == 16 ? d : 1;
}
inline double radioPpm(const TuneSettings& s) { const double p = radioNumber(s, "ppm", 0); return p > -1000 && p < 1000 ? p : 0; }
// The frequency to ask a radio for so that it really receives hz, for radios whose library has no correction of its own: a clock that runs
// fast by ppm tunes every frequency that much too high
inline double ppmCorrectedHz(double hz, double ppm) { return ppm == 0 ? hz : hz / (1.0 + ppm * 1e-6); }

// OnAir never refuses a frequency: it asks the driver as requested. Outside what a radio is specified for (DeviceInfo::minFreqHz..maxFreqHz, 0 = not
// known) it adds this plain-words note to the start/retune message (a warning, not an error); whatever the driver itself refuses stays an error.
inline std::string freqSpecNote(const char* radio, double hz, double lo, double hi) {
    if (lo <= 0 && hi <= 0) return "";
    if ((lo <= 0 || hz >= lo) && (hi <= 0 || hz <= hi)) return "";
    char b[300];
    snprintf(b, sizeof b, "this %s is specified for %.6g to %.6g MHz; the driver may not reach %.6g MHz or the signal may be weak or absent", radio, lo / 1e6, hi / 1e6, hz / 1e6);
    return b;
}
inline void addNote(std::string& err, const std::string& note) {
    if (note.empty()) return;
    err += err.empty() ? note : "; " + note;
}

// The descriptor of that setting (step: the finest the radio's library takes, e.g. whole ppm for the RTL-SDR)
RadioSetting ppmSetting(double step = 0.1);
// Descriptors for the drivers' listings
RadioSetting boolSetting(const std::string& key, const std::string& label, const std::string& help, bool def = false, bool restart = false);
RadioSetting choiceSetting(const std::string& key, const std::string& label, const std::string& help, std::vector<std::string> values,
                           std::vector<std::string> names, const std::string& def, bool restart = false);

// How a sound-card source turns captured stereo audio into complex IQ (Audio source, source_audio.cpp).
enum class AudioIqMode { StereoIq = 0, Usb = 1, Lsb = 2, MonoReal = 3 };

struct AudioHilbert {
    static constexpr int N = 63;   // odd FIR, delay (N-1)/2
    float x[N] = {};
    int p = 0;
};

void audioStereoToIq(const float* stereo, int n, AudioIqMode mode, cf32* out, AudioHilbert& h);

struct DeviceInfo {
    enum Kind { Synthetic, File, HackRF, Soapy, Native, Audio, Hamlib, Group } kind = Synthetic;   // Native: a radio driven by its own library (experimental), see source_native.cpp. Audio: a sound card (WebSDR / VAC / speaker tap), source_audio.cpp. Hamlib: CAT rig + optional sound card for IQ (source_hamlib.cpp). Group: 2-5 radios stitched into one wide one (synth_group.h).
    std::string name;   // display name
    std::string serial; // HackRF serial / file path
    std::string board;  // "HackRF One", "HackRF Pro", "airspy", "sdrplay", ...
    // generic radios (SoapySDR)
    std::string soapyArgs;     // device arguments, e.g. "driver=airspy,serial=..."
    std::string nativeArgs;    // what the radio's own library wants to open it (index, serial, URI, ...)
    double maxRateHz = 0;      // fastest complex sample rate the radio offers (0 = unknown)
    double minRateHz = 0;
    // The rate its link carries without losing samples when that is below maxRateHz (a PlutoSDR on its USB 2 cable: about 4 Msps of 16-bit
    // samples), 0 = whatever the radio offers. linkRateFor() (rate_choice.h) keeps a mode within it when the mode fits, else goes above it.
    double steadyRateHz = 0;
    // The rates the radio offers when its driver lists them: each pair a range (lo == hi: one rate), between them rates it cannot run at (the
    // RTL-SDR's 0.3 to 0.9 Msps). Empty = any rate between minRateHz and maxRateHz (or not known). The driver takes the smallest one at or
    // above what is asked for (rate_choice.h).
    std::vector<std::pair<double, double>> rateRanges;
    double gainMinDb = 0, gainMaxDb = 0;
    double minFreqHz = 0, maxFreqHz = 0;   // the range the radio is SPECIFIED for (0 = unknown): information and a note only, OnAir asks the driver for any frequency
    bool hasBiasTee = false;               // the radio and its library can power the antenna input (TuneSettings::biasTee)
    // Transmit (TxSink below). canTransmit: OnAir has a transmit path for this radio (HackRF, PlutoSDR, SoapySDR radios that have a TX channel);
    // every other radio is receive-only here. The ranges are the radio's own TX scale (0 = not known until the radio has been opened); OnAir
    // adds no limit of its own to the frequency or the level. txFullDuplex: the radio can receive while it transmits, through the same open
    // handle (native PlutoSDR; incompatible shared clock/filter settings are refused by the driver).
    bool canTransmit = false;
    bool txFullDuplex = false;
    int txChannels = 1;                    // transmit outputs (LimeSDR-USB, bladeRF 2.0, USRP: more than one). 0: not known until the radio has been opened
    double txMinRateHz = 0, txMaxRateHz = 0;
    double txGainMinDb = 0, txGainMaxDb = 0;
    std::string txName;                    // the name the TX window lists the radio under when one device has several receive entries (a USRP's inputs); "" = name
    std::string txGainNote;               // what the TX gain number means on this radio ("HackRF: 0-47 dB TX VGA, +14 dB with the amplifier")
    std::vector<RadioSetting> settings;    // the radio's own settings the app offers (TuneSettings::radio); empty for files and the test signal
    bool isRadio() const { return kind == HackRF || kind == Soapy || kind == Native || kind == Hamlib || kind == Group; }
    bool isGeneric() const { return kind == Soapy || kind == Native || kind == Group; }   // one overall gain, a sample-rate range reported by the radio
    bool isAudio() const { return kind == Audio; }
    bool network = false;                  // reached over a network link, not USB (set where the name does not say "(network)": a Pluto found by mDNS)
    bool isNetwork() const { return network || name.find(" (network") != std::string::npos; }   // rtl_tcp, SpyServer, KiwiSDR, a Pluto by IP, ...
};

class IqSource {
public:
    virtual ~IqSource() = default;
    virtual bool start(const TuneSettings& s, IqRing& ring, std::string& err) = 0;
    virtual void stop() = 0;
    virtual bool retune(const TuneSettings& s, std::string& err) = 0; // live changes
    virtual double sampleRate() const = 0;
    virtual bool realtimeHardware() const { return false; }
    virtual std::string statusNote() const { return {}; }   // stream trouble for the status line (overflow counts ...); empty while clean
};

// ---- transmit ---------------------------------------------------------------------------------------------------------------------------
// What a transmission asks of the radio. No limit is applied by OnAir: the frequency goes to the driver as given (a hertz-sized value, a
// terahertz one) plus offsetHz, the gain is the radio's own TX gain in dB clamped only to the scale the radio itself reports.
struct TxTune {
    double centerHz = 0;         // the frequency the operator means; sinks do not add offsetHz into this
    double offsetHz = 0;         // added only to the frequency sent to the radio (txRadioHz). 0: no change. May be negative
    double sampleRate = 0;       // complex samples per second the radio is asked for; what it really runs is TxSink::sampleRate()
    double gainDb = 0;           // the radio's TX gain scale (DeviceInfo::txGainMinDb .. txGainMaxDb)
    double bandwidthHz = 0;      // analogue filter, 0 = automatic
    bool ampOn = false;          // HackRF: the +14 dB amplifier
    int channel = 0;             // which transmit output (0 = TX1). A radio with one output refuses any other
    std::map<std::string, std::string> radio;   // the radio's own settings (TuneSettings::radio), e.g. "ppm"
};

// The hertz value a transmit sink writes at its set-frequency call. PPM correction, where a radio has it, is applied to this result.
inline double txRadioHz(const TxTune& t) { return t.centerHz + t.offsetHz; }

#ifndef ONAIR_WEB_SESSION   // the transmit path does not exist for OnAir Web code (server/web): see dect2/tx/tx.h
// A radio that transmits. open() starts the radio's transmit path (a driver that refuses a frequency or a rate says why in err, in plain
// words); write() hands it complex float samples (full scale = +-1.0; beyond that is clipped) and returns when the radio has taken them, so a
// real radio paces the caller; close() stops the transmitter: no sample goes out after it returns. Used from one thread at a time.
class TxSink {
public:
    virtual ~TxSink() = default;
    virtual bool open(const TxTune& t, std::string& err) = 0;
    virtual bool write(const cf32* x, size_t n, std::string& err) = 0;   // false: the radio is gone / refused the data; err says why
    virtual bool retune(const TxTune&, std::string& err) { err = "this radio cannot change its transmit settings while sending"; return false; }
    virtual void close() = 0;
    virtual double sampleRate() const = 0;     // the rate the radio runs at after open()
    virtual bool realtime() const { return true; }   // true: write() is paced by the hardware, so an empty queue is an underrun (zeros are sent); false: recording sinks (the queue waits)
    virtual bool fullDuplex() const { return false; }
    virtual std::string name() const { return "radio"; }
};
// The transmit path of a radio, nullptr when it has none (DeviceInfo::canTransmit false) or the build lacks it
std::unique_ptr<TxSink> makeTxSink(const DeviceInfo& d);
std::unique_ptr<TxSink> makeUsbHackrfTxSink(const std::string& serial);   // hackrf_usb.cpp
void hackrfTxPack(const cf32* x, size_t n, int8_t* out);                  // the HackRF's DAC format: float (1.0 = full scale) to signed 8-bit I/Q pairs
std::unique_ptr<TxSink> makeNativeTxSink(const DeviceInfo& d);           // source_native.cpp (PlutoSDR)
#endif

// Radios the user hid from the list (by display name; saved in the settings). Synthetic/file entries are never hidden.
void setHiddenRadios(const std::vector<std::string>& names);
std::vector<std::string> hiddenRadios();
// Whether listing the radios may search the network by itself (#215): the SoapySDR modules that look for servers or boards on the LAN
// (SoapyRemote, SoapyPlutoSDR's mDNS and pluto.local, SoapyUHD, ...), UHD's network broadcast and librfnm's network search. Off by default:
// then only radios on USB (or PCIe) are looked for, and a network radio is listed when the user adds its address (Add network radio).
void setNetworkDiscovery(bool on);
bool networkDiscovery();
// The SoapySDR enumerate arguments for one module while network discovery is off: "" = skip the module (it searches the network), else the
// arguments that keep it to local devices ("driver=uhd,type=b200": USB USRPs only)
std::string soapyLocalEnumerateArgs(const std::string& module);
std::vector<DeviceInfo> listHackrfDevices(std::string& err);
// Every other radio that SoapySDR knows (Airspy, SDRplay, RTL-SDR, PlutoSDR, LimeSDR, BladeRF, USRP, ...). Empty when built without SoapySDR.
std::vector<DeviceInfo> listSoapyDevices(std::string& err);
bool soapySupported();
// Radios driven by their own libraries when those are installed: RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP, SDRplay (experimental).
std::vector<DeviceInfo> listNativeDevices(std::string& err);
std::unique_ptr<IqSource> makeNativeSource(const DeviceInfo& d);
// Every radio found: HackRF first, then the native ones, then the SoapySDR ones (a radio found natively is not listed again through SoapySDR).
std::vector<DeviceInfo> listRadios(std::string& err);
std::unique_ptr<IqSource> makeSource(const DeviceInfo& d);

// File format helpers (sidecar-free for now): .cs8 interleaved signed 8-bit IQ, .cu8 unsigned, .cf32 float.
enum class FileFormat { CS8, CU8, CF32, CI16, CU16 };   // CI16 / CU16 come from SigMF and WAV files only
FileFormat guessFormat(const std::string& path);
// Sample rate in Hz from a file name such as "capture_10Msps.cs8" or "x_2.5msps.cu8"; 0 if the name does not say.
double guessSampleRate(const std::string& path);
// offsetBytes: where the samples start (SigMF / WAV header), endBytes: where they end (0 = end of file), bigEndian: multi-byte samples.
std::unique_ptr<IqSource> makeFileSource(const std::string& path, FileFormat fmt, double sampleRate, bool loop,
                                         uint64_t offsetBytes = 0, uint64_t endBytes = 0, bool bigEndian = false);

// Sound-card / VAC / WebSDR speaker tap. listed as "Sound card: <name>". serial is the AudioIn device id (empty = default).
// Tests: serial "tone:1500" or "silence" uses AudioIn Tone/Silence and never opens a microphone.
// IQ mapping is TuneSettings::radio["audio_iq"] = "0".."3" (AudioIqMode). The capture rate is reported; the frequency is the dial only.
std::vector<DeviceInfo> listAudioDevices();
std::unique_ptr<IqSource> makeAudioSource(const DeviceInfo& d);

} // namespace dect2
