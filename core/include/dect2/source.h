// IQ sample sources: HackRF, recorded file, built-in synthetic DVB-T2-like signal.
#pragma once
#include "ring.h"
#include "t2gen.h"
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
    double pace = 1.0;     // 1 = real time; below 1 the signal runs in slow motion (tests on slow machines: the receiver gets 1/pace times the time)
    bool gainModel = false; // scale the level with the LNA/VGA/amp gains (for testing AGC): 62 dB total = the nominal level
    int mode = 0;           // 0: the DVB / ATSC / DAB settings above; 8 and up: the test signal of that engine standard (see mode_synth.h)
    int modeOpt[8] = {};    // options of that test signal; every mode says what they mean in its <mode>_gen.h
    double modeVal[4] = {};
};

struct TuneSettings {
    double bandwidthMhz = 8;
    SynthConfig synth;
    double centerHz = 522e6;
    double sampleRate = 18285714.2857; // 2x native rate for an 8 MHz channel
    double basebandFilterHz = 0;       // 0 = auto
    double gainDb = 30;                // overall gain of generic (SoapySDR) radios; the three HackRF stages below are not used there
    int lnaDb = 16;                    // 0..40 step 8
    int vgaDb = 20;                    // 0..62 step 2
    bool ampOn = false;
};

struct DeviceInfo {
    enum Kind { Synthetic, File, HackRF, Soapy, Native } kind = Synthetic;   // Native: a radio driven by its own library (experimental), see source_native.cpp
    std::string name;   // display name
    std::string serial; // HackRF serial / file path
    std::string board;  // "HackRF One", "HackRF Pro", "airspy", "sdrplay", ...
    // generic radios (SoapySDR)
    std::string soapyArgs;     // device arguments, e.g. "driver=airspy,serial=..."
    std::string nativeArgs;    // what the radio's own library wants to open it (index, serial, URI, ...)
    double maxRateHz = 0;      // fastest complex sample rate the radio offers (0 = unknown)
    double minRateHz = 0;
    double gainMinDb = 0, gainMaxDb = 0;
    bool isRadio() const { return kind == HackRF || kind == Soapy || kind == Native; }
    bool isGeneric() const { return kind == Soapy || kind == Native; }   // one overall gain, a sample-rate range reported by the radio
};

class IqSource {
public:
    virtual ~IqSource() = default;
    virtual bool start(const TuneSettings& s, IqRing& ring, std::string& err) = 0;
    virtual void stop() = 0;
    virtual bool retune(const TuneSettings& s, std::string& err) = 0; // live changes
    virtual double sampleRate() const = 0;
    virtual bool realtimeHardware() const { return false; }
};

std::vector<DeviceInfo> listHackrfDevices(std::string& err);
// Every other radio that SoapySDR knows (Airspy, SDRplay, RTL-SDR, PlutoSDR, LimeSDR, BladeRF, USRP, ...). Empty when built without SoapySDR.
std::vector<DeviceInfo> listSoapyDevices(std::string& err);
bool soapySupported();
// Radios driven by their own libraries when those are installed: RTL-SDR, Airspy, BladeRF, LimeSDR, PlutoSDR, USRP (experimental).
std::vector<DeviceInfo> listNativeDevices(std::string& err);
std::unique_ptr<IqSource> makeNativeSource(const DeviceInfo& d);
// Every radio found: HackRF first, then the native ones, then the SoapySDR ones (a radio found natively is not listed again through SoapySDR).
std::vector<DeviceInfo> listRadios(std::string& err);
std::unique_ptr<IqSource> makeSource(const DeviceInfo& d);

// File format helpers (sidecar-free for now): .cs8 interleaved signed 8-bit IQ, .cu8 unsigned, .cf32 float.
enum class FileFormat { CS8, CU8, CF32 };
FileFormat guessFormat(const std::string& path);
// Sample rate in Hz from a file name such as "capture_10Msps.cs8" or "x_2.5msps.cu8"; 0 if the name does not say.
double guessSampleRate(const std::string& path);
std::unique_ptr<IqSource> makeFileSource(const std::string& path, FileFormat fmt, double sampleRate, bool loop);

} // namespace dect2
