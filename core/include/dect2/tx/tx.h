// Transmit: which modes OnAir can send, how it makes the signal, and the session that feeds a radio's TxSink (source.h).
//
// THE LIST (txModes() in tx.cpp is the single source of truth). Deny-list, not allow-list: every engine mode that has a generator is offered,
// except pointless / jammer / impersonation cases below. The first eight (DVB-T2/T, ATSC, DAB, ISDB-T, DVB-S/S2/S2X, DTMB, ADS-B) and a few
// extra digital modes (FM, DMR, AIS, APRS, CW, pagers) have an engine round-trip in tests/tx/test_tx.cpp. Other offered rows reuse the mode's
// *_gen.h / *_tx.h chain; their roundTrip field says so when that loop-back was not run.
//
//   id      mode               generator
//   dvbt2   DVB-T2             T2Generator with a real PLP and the test transport stream
//   dvbt    DVB-T              dvbt::Generator + the demo programme
//   atsc    ATSC 1.0 (8-VSB)   atsc::Generator + the demo programme
//   dab     DAB / DAB+         makeDabSynth
//   isdbt   ISDB-T             makeModeSynth(6)
//   dvbs    DVB-S/S2/S2X       makeDvbsSynth (dvbs_tx.h)
//   dtmb    DTMB               makeDtmbSynth (dtmb_tx.h)
//   adsb    ADS-B / Mode S     makeAdsbSynth (simulated aircraft: test content)
//   pager   POCSAG / FLEX      makePagerSynth (FM 25 kHz; every listed speed)
//   plus analog TV, FM (live mic / file, stereo+RDS), airband AM, DMR/P25/NXDN/D-STAR/M17/dPMR/TETRA, AIS/ACARS/VDL2/HFDL, APRS/packet, HF digital, CW, PSK, JS8, …
// Voice: FM/NFM/AM/SSB take the microphone or a WAV/MP3; M17 and FreeDV encode with Codec 2 when the library is loaded; AMBE/IMBE modes
// (DMR, P25, NXDN, dPMR, D-STAR) cannot encode — the panel says so and still offers a 1 kHz tone / silence / WAV, plus the digital IDs.
// Every offered mode has a payload UI (callsign / text / IDs / file / mic as it fits). Empty fields fall back to the demo signal.
// Imagery / scene generators that cannot carry a typed string (DAB, ISDB-T, DVB-S, satellite pictures, …) send analog NFM of the mic/file
// when that box is checked; otherwise they keep the demo.
// Carrier tune (every mode): an unmodulated carrier at DC for setting drive / SWR; independent of the payload, not saved as transmitting.
//   (see txModes() for the complete rows)
//
// NOT OFFERED: only a mode with no generator (txRefusalReason says "this mode has no transmit generator"). OnAir refuses no mode, frequency or power.
//
// No frequency and no power limit is applied here: see TxTune in source.h.
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/source/source.h"
#include "dect2/audio/audioin.h"
#include "dect2/audio/videoin.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/tx/tx_iqstream.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dect2 {

struct TxModeSpec {
    const char* id;            // "dvbt2"
    const char* name;          // "DVB-T2"
    int stdMode;               // the engine standard code (App's engineStd): 0 DVB-T2 / DVB-T, 3 ATSC, 4 DAB, 6 ISDB-T, 8 DVB-S/S2, 9 DTMB, 13 ADS-B
    int variant;               // DVB: 1 DVB-T2, 2 DVB-T (the engine's stdMode choice); 0 otherwise
    double nominalRateHz;      // the rate the generator is run at when the radio allows it (OFDM standards: the channel's own rate)
    bool fixedRate;            // the generator makes samples at the standard's own rate (resampled to the radio's); false: it can make any rate from minRateHz
    double minRateHz;          // lowest rate its generator works at
    const char* generator;     // what makes the signal
    const char* options;       // what the user can set
    const char* roundTrip;     // the test that decodes it
};

const std::vector<TxModeSpec>& txModes();
const TxModeSpec* txModeById(const std::string& id);
// The mode of the app's current selection (engine standard code and, for DVB, 1 T2 / 2 T / 0 = T2); nullptr: no generator for this mode
const TxModeSpec* txModeFor(int stdMode, int dvbVariant = 1);
// Same list as the receiver: engine family (App::family) plus a protocol sub-id (ft8, pocsag, navtex, …). nullptr: no generator.
const TxModeSpec* txModeForList(int family, const char* subId, int dvbVariant = 1);
// The reason a mode is not on the list: it has no generator ("" for a mode that is)
std::string txRefusalReason(int stdMode);
// Engine standard from the mode-list family number (same mapping as App::engineStd without DVB auto).
int txEngineStd(int family);

// What the transmit panel draws for this row. Every txModes() id has a kind; there is no generic catch-all.
enum class TxPayloadKind {
    TvFile, AnalogTv, BroadcastAudio,
    Ftx, Rtty, Keyboard, Cw, Psk, Js8, Hamdrm, Sstv, Freedv,
    Pager, Packet, Ais, AdsB, VoiceFm, VoiceAir, VoiceDigital, Qo100,
    Marine, AcarsFamily, Sonde, Same, Ale, VorIls, LoraRaw, Dect,
    CubeSat, WeatherScene, SatScene, Flarm, Zigbee, Dji, HfBeacon, Vdes, Uplink, Mesh, Mac, Kaal, Kait, Kavb, Kapp, Kaft, Kaws
};
TxPayloadKind txPayloadKind(const std::string& id);

// Open a video / MPEG-TS path for a TV generator. On success `outTs` is a looping 188-byte packet source (raw .ts, or remuxed via libavformat).
// Empty path: false, note says demo. A file that is not TS and cannot be remuxed: false with an error in note (the caller keeps the demo).
bool txOpenMpegTs(const std::string& path, std::function<void(uint8_t*)>& outTs, std::string& note);

// Power spectrum of n complex samples (Hann, FFT size nfft, DC at bin 0). For a carrier at IF centre, bin 0 is the peak.
void txSpectrumDb(const cf32* x, size_t n, int nfft, std::vector<float>& db);

// Soft limiter used on every block before it reaches the radio: samples with a magnitude up to 0.9 pass untouched (bit-identical), above that the
// magnitude is rounded smoothly towards 1.0 (never above), the phase is kept; NaN / Inf become 0. Returns the largest input magnitude seen.
// `limited` (optional) gets the number of samples that were compressed.
float txSoftLimit(cf32* x, size_t n, size_t* limited = nullptr);
// Peak, RMS (both of the magnitude, full scale 1.0) of a block
struct TxLevel { float peak = 0, rms = 0; };
TxLevel txMeasure(const cf32* x, size_t n);
// Occupied bandwidth (Hz) holding 99% of the power of a spectrum from txSpectrumDb (DC at bin 0, bins span rateHz). 0 when empty.
double txOccupiedBandwidthHz(const std::vector<float>& db, double rateHz);
// A radio's own frequency range as information for the panel: "" when the frequency is inside it or the range is unknown, otherwise a sentence
// that names the range. It is never a block: the driver is asked for the frequency anyway.
std::string txRangeInfo(const DeviceInfo& d, double hz);
// Why a radio cannot be used to transmit ("" when it can): receive-only radios, sound cards, files
std::string txUnavailableReason(const DeviceInfo& d);
// The radio's name for the TX window: once per device (a USRP's several receive inputs are one radio), without "(native, experimental)"
std::string txRadioLabel(const DeviceInfo& d);
// Indexes into the device list: the radios the TX window offers (one per device), and the receive-only ones (one per radio) it hides behind a count
std::vector<int> txRadioChoices(const std::vector<DeviceInfo>& devs);
std::vector<int> txReceiveOnlyRadios(const std::vector<DeviceInfo>& devs);
// A sink that takes samples at the pace of `rateHz` and sends them nowhere: the "Test (dry run, no RF)" path. No radio is claimed or opened.
std::unique_ptr<TxSink> makeNullTxSink();

// A generator at a given sample rate: endless, continuous, clean (no noise, offsets or echoes).
// How far above the dial the radio is tuned for a mode (0: none). The receivers of most modes listen tuneOffsetHz above the dial, away from the
// radio's DC spike, and the mode's generator puts its signal tuneOffsetHz below the radio's centre; a transmitter must tune the same way, or the
// signal goes out that far below the frequency asked for (APRS 50 kHz down, #351). The modes with their own transmit chain place theirs themselves.
double txModeTuneOffsetHz(const TxModeSpec& m);

class TxGenerator {
public:
    virtual ~TxGenerator() = default;
    virtual double sampleRate() const = 0;
    virtual void generate(cf32* out, size_t n) = 0;
    // A generator of a finite transmission (a file, a message) says when everything has been generated; the session then ends by itself, like a one-pass
    // transmission. The default is endless.
    virtual bool finished() const { return false; }
};

// What to send. Strings are the operator's own content (callsign, text, a file). They are never packed into SynthConfig::modeOpt.
// An empty string (and address 0, lat/lon 0) means "use the mode's built-in demo / test signal for that field".
struct TxRequest {
    std::string modeId;
    SynthConfig synth;
    double bandwidthMhz = 8;       // DVB-T2 / DVB-T channel width
    TxTune tune;                   // frequency, gain, ...; sampleRate: 0 = automatic, positive = operator-requested TX Fs
    bool loop = true;              // true: until stopped; false: one pass of onceSeconds, then it stops by itself
    double onceSeconds = 10;
    std::string callsign;          // FT8, APRS, D-STAR, M17, …
    std::string grid;              // Maidenhead (FT8 / WSPR)
    std::string text;              // RTTY / CW / PSK / pager / APRS comment / free-text
    std::string payload;           // a message prepared as it goes on the air (the Flipper chat screen: the encrypted octets); empty: built from the fields
    std::string dest;              // APRS destination, D-STAR UR, M17 dst, FT8 report target
    std::string path;              // APRS digipeater path, e.g. WIDE1-1,WIDE2-1
    std::string file;              // .ts / .mp2 for TV, .wav for FM/AM
    std::string name;              // AIS vessel name, FM RDS PS, analog TV ident, first DVB/DAB service
    std::string provider;          // DVB SDT provider / DAB ensemble
    std::string network;           // DVB network name
    std::vector<std::string> services;  // DVB/DAB service names (1..4). Empty: built-in names.
    int serviceId = 1;             // first DVB service_id
    int kind = 0;                  // FT8: 0 CQ, 1 report, 2 free text; analog: 0 FM 1 AM 2 USB 3 LSB
    int baudIdx = 0;               // RTTY baud table; pager speed (PagerSpeed)
    int shiftIdx = 0;              // RTTY shift table
    int variant = 0;               // PSK mode, Olivia tones, USB/LSB (0 USB 1 LSB)
    int wpm = 20;                  // CW
    int dbm = 37;                  // WSPR power
    int function = 3;              // POCSAG function bits
    uint32_t address = 0;          // pager capcode, AIS MMSI, DMR/P25 radio ID
    double toneHz = 1000;          // analog test tone
    double lat = 0, lon = 0;       // AIS / APRS position
    bool reverse = false;          // RTTY: mark is the upper tone
    bool carrierTune = false;      // unmodulated carrier at DC (IF centre); payload generator is bypassed. Not persisted.
    int audioSrc = 2;              // 0 microphone, 1 file (WAV/MP3), 2 1 kHz tone, 3 silence. Mic is opened only when transmitting.
    std::string audioDevice;       // empty = default input
    bool audioMute = false;
    float micGain = 1.f;
    double deviationHz = 0;        // 0 = mode default (75 kHz WFM, 5 kHz NFM)
    double preemphUs = 50;         // 50 Europe, 75 US
    int rdsPi = 0x4A21;
    int slot = 1;                  // DMR timeslot 1 or 2
    int colorCode = 1;
    int talkgroup = 0;
    bool analogVoice = false;      // digital-voice modes: send analog NFM of the audio instead of the vocoder / test pattern
    // Filled by TxSession (or a test). 48 kHz interleaved stereo. Empty: the generator makes a tone / silence itself.
    std::function<int(float* stereo, int frames)> pullAudio;
    int videoSrc = 0;              // analog TV: 0 test card, 1 video file, 2 camera / capture card
    std::string videoDevice;       // camera unique id (listVideoInputs); empty = default
    std::function<bool(int w, int h, float* y, float* u, float* v)> pullVideo;   // analog YUV; empty = test card
    std::vector<DemoTsService> mux; // DVB/ATSC/ISDB/DVB-S programmes (own video+audio each). Empty: services[] names.
    std::string fileNote;          // after start / makeTxGenerator: how the video/TS file was used (demo, remux, error)
    // The advanced parameters of the panel (app/panels/tx_params.cpp has the one table): numbers by key, e.g. "am.depth". A generator reads the keys it knows with
    // o(key, default); a key it does not know is ignored. Keys: out.* (every mode), audio.* / am.* / ssb.* / fm.* / vox.* (analog voice), bfm.* (FM broadcast).
    std::map<std::string, double> opt;
    double o(const char* key, double def) const { const auto it = opt.find(key); return it == opt.end() ? def : it->second; }
};

// Whether this mode's vocoder can encode live speech with a library OnAir already loads (Codec 2 for M17 / FreeDV).
bool txCanEncodeVoice(const TxModeSpec& m);
// Plain words for the panel: encode path, or why live encode is off (AMBE/IMBE decode-only, library missing). Empty when not a voice mode.
std::string txVocoderNote(const TxModeSpec& m);
bool txModeNeedsAudio(const TxModeSpec& m, const TxRequest& r);
// A mode whose generator ends by itself (a message or a file sent a set number of times: KAPP, KAFT, KAWS). Into an IQ file it is written as fast as it is made.
bool txModeFinite(const TxModeSpec& m);

// FT8 / FT4 / WSPR / JT* message built from callsign, grid, dest, text and kind. Empty when those fields are empty (the demo stations are used).
std::string txFtxMessage(const TxRequest& r);

// The output stage of every mode (keys out.trimDb, out.rampMs, out.swapIq of r.opt) on n samples that start at sample number firstSample of the transmission
constexpr double kTxDefaultRampMs = 2;   // the key-up / key-down fade when out.rampMs is not set
void txOutputStage(const TxRequest& r, cf32* x, size_t n, uint64_t firstSample, double rateHz);

// The radio's rate limits for the choice of a rate (0 = unknown / no limit)
struct TxRateLimits { double minHz = 0, maxHz = 0; };

// The generator of mode at rateHz (a fixed-rate mode makes its own rate and is resampled to rateHz when that differs). nullptr and err when the mode's generator cannot work
// at that rate or the options are invalid. The synthetic-signal impairments of r.synth are turned off. Empty payload fields fall back to the mode's demo signal.
std::unique_ptr<TxGenerator> makeTxGenerator(const TxModeSpec& mode, const TxRequest& r, double rateHz, std::string& err);

// The sample rate to ask the radio for: explicit tune.sampleRate, or the mode's own kept within the radio's limits.
// Explicit requests are not clamped; TxSession rejects invalid or unsupported rates before opening the sink.
double txRequestRate(const TxModeSpec& mode, const TxRequest& r, const TxRateLimits& lim);

// The transmitter's radio. It is independent of the receivers: a radio a receiver (Engine) has open is refused here, in plain words, unless it is
// genuinely full duplex (DeviceInfo::txFullDuplex: then nothing is claimed and both may use it); and a radio the transmitter holds is refused to the
// receivers by the same claim (device_claims.h). A synthetic source or a file has no claim. One transmitter at a time: claiming a second radio releases the first.
bool txClaimRadio(const DeviceInfo& d, std::string& err);
void txReleaseRadio();

// One transmission: opens the sink, runs a generator thread and a sender thread, and stops. Nothing here starts by itself or restarts.
class TxSession {
public:
    TxSession() = default;
    ~TxSession() { stop(); }
    TxSession(const TxSession&) = delete;
    TxSession& operator=(const TxSession&) = delete;

    // Opens the radio and starts sending. False with err in plain words (the driver's own text for a refusal) and nothing sent.
    bool start(std::unique_ptr<TxSink> sink, const TxRequest& r, const TxRateLimits& lim, std::string& err);
    void stop();                       // stops sending and closes the radio; no sample goes out after it returns. Safe to call twice.
    bool running() const { return running_; }   // sending now (false once a one-pass transmission has ended or the radio failed)
    // Unmodulated carrier at DC, independent of the payload. While on, generate() is CW of amplitude 1.0. Not persisted. Stop still applies.
    void setCarrierTune(bool on);
    bool carrierTune() const { return carrierTune_.load(); }
    struct Stats { uint64_t samples = 0; uint64_t underruns = 0; double seconds = 0; double rateHz = 0; bool failed = false; bool finished = false; bool carrierTune = false; float audioPeak = 0;
                   float peak = 0, rms = 0; double limitedPct = 0; size_t queued = 0, queueMax = 0; std::string error; std::string fileNote; };
    // Change frequency / gain / bandwidth while sending (a radio that cannot says so in err and goes on as it was). Applied between two writes by the sender thread.
    bool retune(const TxTune& t, std::string& err);
    bool dryRun() const { return dry_; }
    int iqClients() const { return iq_ ? iq_->clients() : -1; }   // TCP IQ stream: connected clients, -1 when the stream is off or could not open
    Stats stats() const;
    std::vector<std::string> log() const;          // what was sent and what happened, oldest first, each with the time
    void setLogCallback(std::function<void(const std::string&)> f) { std::lock_guard<std::mutex> lk(mu_); cb_ = std::move(f); }
    // A copy of the last ~0.5 s of IQ actually handed to the radio (generator output, including carrier tune). Empty when idle.
    void copyTap(std::vector<cf32>& out, double& rateHz) const;

private:
    void closeSink();
    void generatorLoop();
    void senderLoop();
    void note(const std::string& s);
    void fail(const std::string& why);
    void tapWrite(const cf32* x, size_t n);

    std::unique_ptr<TxSink> sink_;
    std::unique_ptr<TxIqStream> iq_;   // the IQ on a TCP socket (tx_iqstream.h), when the setting is on
    std::unique_ptr<TxGenerator> gen_;
    std::unique_ptr<AudioIn> audio_;
    std::unique_ptr<VideoIn> video_;
    TxRequest req_;
    std::atomic<bool> carrierTune_{false};
    double rate_ = 0;
    std::atomic<bool> running_{false}, stop_{false}, genDone_{false}, failed_{false};
    std::atomic<uint64_t> sent_{0}, under_{0}, limited_{0}, madeTotal_{0};
    std::atomic<float> peak_{0}, rms_{0}, audioPeak_{0};
    std::atomic<bool> senderDone_{false}, sinkClosed_{false};
    bool dry_ = false;
    std::atomic<bool> rfOn_{false};   // this session told the map guard (net/map_guard.h) that it is on air
    std::mutex ctl_;                       // start / stop / retune are serialised
    std::mutex rtMu_;                      // the pending retune
    std::condition_variable rtCv_;
    bool rtPending_ = false, rtDone_ = false, rtOk_ = false;
    TxTune rtTune_;
    std::string rtErr_;
    std::vector<std::vector<cf32>> pool_;  // recycled sample blocks (guarded by mu_)
    mutable std::mutex mu_;
    std::condition_variable cvIn_, cvOut_;
    std::deque<std::vector<cf32>> q_;
    size_t maxQ_ = 8;
    std::thread gen_t_, send_t_;
    std::vector<std::string> log_;
    std::string error_;
    bool finished_ = false;
    std::function<void(const std::string&)> cb_;
    bool started_ = false;
    std::vector<cf32> tap_;
    size_t tapPos_ = 0, tapHave_ = 0;
};

} // namespace dect2
