// SSB conversations: find single-sideband voice (USB / LSB) on the captured span and organise it into conversations the user can just listen to.
//
//   IQ (the wide capture) -> Scanner: Welch spectrum -> narrow occupied bands (1.5 - 3.8 kHz) -> sideband decision (spectral tilt + band convention)
//                            -> per candidate: mix to the suppressed carrier, decimate to ~8 kHz -> VoiceAnalyzer (syllabic envelope, pitch / periodicity,
//                               tone and carrier absence, voice activity with hang time) -> Detection
//   Detection -> ConversationBook: same frequency (+-tolerance, followed while it drifts) + alternating overs with gaps < the gap setting = one
//                conversation / net; strength, trend, speakers, list, filters, export, timeline, auto-listen choice.
//   SweepPlanner: for radios too narrow for the band, the steps and the dwell of a scan that uses the same detector.
//
// Nothing here touches the radio, the screen or the network; the glue (app/ssbconv) retunes the Multi-VFO channelizer and shows the list.
// Strength is an ESTIMATE in S-units from the signal over the noise floor of the capture (S9 = 48 dB over it; not calibrated to microvolts).
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2::ssbconv {

using cf = std::complex<float>;

enum class Side { Unknown = 0, Usb, Lsb };
const char* sideName(Side s);   // "USB", "LSB", "?"

// ---------------------------------------------------------------- band plan labels
struct BandInfo {
    std::string band;       // "40 m", "HF utility", ""
    std::string service;    // "Amateur", "Maritime SSB", "Aviation HF", "CB (27 MHz)", "Utility", "" (nothing known)
    Side expected = Side::Unknown;   // the sideband used there by convention
};
BandInfo bandInfo(double hz);                 // from the built-in table (the amateur bands, maritime, aviation, CB, 60 m channels)
Side conventionSide(double hz);               // LSB below 10 MHz, USB above; 60 m, maritime, aviation and utility USB
double estimateS(double snrDb);               // S-units 0..9 (+ fractions); above S9 it stays 9 (see overDb)
double overS9Db(double snrDb);                // dB over S9 (0 below)
std::string sText(double snrDb);              // "S5", "S9+12"

// ---------------------------------------------------------------- voice analysis
struct VoiceFeatures {
    float depthDb = 0;        // p90 - p10 of the 10 ms frame energies (speech 15..40 dB, a data signal or noise < 6)
    float modRatio = 0;       // share of the envelope's modulation power at 2.5 - 8 Hz (syllables)
    float voiced = 0;         // share of the active frames with a pitch-like periodicity (not a flat tone)
    float toneDb = 0;         // strongest spectral line over the median (a carrier or an FSK tone is > 25 dB)
    float tiltDb = 0;         // audio spectrum 300-1100 Hz over 1900-2700 Hz (speech: +8 .. +25 on the correct sideband)
    float bandFrac = 0;       // share of the energy at 250 - 2900 Hz
    float pitchHz = 0;        // median pitch of the voiced frames (0 none)
    float activeFrac = 0;     // share of the frames above the noise floor in the history
    float score = 0;          // 0..1, speech-likeness
};

struct WindowVoice {
    bool active = false;      // voice in this window (VAD with hang time)
    float level = -200;       // dB, mean power of the active frames (or of the window)
    float activeShare = 0;    // frames above the floor + margin in this window
};

class VoiceAnalyzer {
public:
    VoiceAnalyzer();
    void reset();
    // One window of complex baseband audio (the suppressed carrier at 0, the signal at 0..+3 kHz). noiseDb: the noise power in the same units, dB.
    // raw (optional): the same window before the sideband filter (the suppressed carrier at 0, an adjacent station on the other side of it still in):
    // the carrier / tone check and the mirrored-sideband check (an AM or DSB signal has the same audio on both sides of the carrier) look at it,
    // while x (the filtered window, only the wanted sideband) is what the frames, pitch and band share are measured on, so a neighbour 3.3 - 5 kHz away does not spoil them.
    WindowVoice process(const cf* x, size_t n, double rateHz, double noiseDb, const cf* raw = nullptr, size_t nRaw = 0);
    VoiceFeatures features() const { return f_; }
    float tiltNow() const { return tiltNow_; }       // this window only
    float pitchNow() const { return pitchNow_; }
    // The history is a few seconds of 10 ms frames; features() needs about 2 s of it to mean anything.
    size_t frames() const { return fr_.size(); }

private:
    struct Fr { float db; bool active; float pitch; };
    std::deque<Fr> fr_;
    VoiceFeatures f_;
    float tiltNow_ = 0, pitchNow_ = 0;
    double hang_ = 0;   // frames of hang time left
    double mNeg_ = 0, mPos_ = 0;   // energy at -800..-250 Hz and +250..+800 Hz of the raw window (forgetting): a DSB / AM signal has both
    double sLo_ = 0, sHi_ = 0;   // energy at 300-1100 Hz and 1900-2700 Hz over the active windows (forgetting), for the tilt
    void recompute();
};

// ---------------------------------------------------------------- the scanner
struct Detection {
    uint32_t trackId = 0;
    double time = 0;              // the window's end, seconds (the clock the caller feeds)
    double dialHz = 0;            // the suppressed carrier (what the radio is tuned to for this station)
    double lowHz = 0, highHz = 0; // occupied edges (RF)
    double bwHz = 0;
    Side side = Side::Unknown;
    float sideConf = 0;           // 0..1
    bool confirmed = false;       // enough windows to be sure it is voice
    bool voiceNow = false;        // voice activity in this window (VAD with hang time)
    float speech = 0;             // 0..1
    float snrDb = 0, levelDb = 0;
    float pitchHz = 0;
    float tiltDb = 0;
    double sinceT = 0;            // when this activity began (the first window of the track, or after a quiet spell)
    std::vector<float> audio;     // this window's demodulated audio (mono, audioRate) when the track is confirmed; for recording / transcription
    double audioRate = 0;
};

struct ScannerOptions {
    double windowSec = 0.5;       // analysis window (the spectrum and the voice frames are per window)
    double minSnrDb = 3.5;        // a band must stand this far out of the noise floor in the spectrum averaged over 250 Hz (about 6 sigma)
    double minBwHz = 1500, maxBwHz = 3800;   // occupied bandwidth of SSB voice (an AM broadcast, a data carrier or a tone is outside)
    double freqTolHz = 100;       // the same station / conversation this far apart (it follows slow drift)
    double confirmScore = 0.50;   // speech score a track needs ...
    int confirmWindows = 6;       // ... in this many windows (of its first 12) to be confirmed
    double dropSec = 180;         // a track that was not seen for this long is forgotten
    double edgeGuardHz = 1000;    // ignore candidates this close to the edge of the capture (the radio's filter roll-off)
    double dcGuardHz = 150;       // the DC spike: bins this close to 0 Hz take the level of their neighbours
    int maxFft = 1 << 18;         // spectrum size cap
    int maxFftBlocks = 12;        // spectra averaged per window (the blocks are spread over the window)
    size_t maxTracks = 64;
    bool useConvention = true;    // the band convention counts as a weak vote for the sideband
    // Optional second opinion on a candidate: the samples of the candidate mixed to baseband (the signal around 0, rate in Hz); true = it is a digital mode
    // (the app wires sigid::identify() here). Called at most once every 10 s per track until it is confirmed, never for a confirmed voice track.
    std::function<bool(const cf* x, size_t n, double rateHz, double centreOffsetHz)> digitalVeto;
};

class Scanner {
public:
    explicit Scanner(const ScannerOptions& o = ScannerOptions());
    ~Scanner();
    void setOptions(const ScannerOptions& o);
    const ScannerOptions& options() const;
    // Samples of the capture (centre centreHz, sample rate rateHz). tEnd: the clock (seconds) at the end of this block. Returns the detections of every
    // window that completed in this call (usually 0 or 1). A change of rate or centre starts a new window.
    std::vector<Detection> feed(const cf* x, size_t n, double rateHz, double centreHz, double tEnd);
    void reset();
    // Occupied bands of the last window (for drawing on the spectrum): (lowHz, highHz) RF, with the confirmed flag
    struct Band { double lowHz, highHz; bool confirmed; Side side; };
    std::vector<Band> lastBands() const;
    double noiseFloorDb() const;
    size_t trackCount() const;
    double busy() const;   // share of real time the last window took to analyse (needs the caller's timing; 0 until a window completed)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------- conversations
enum class Kind { Single = 0, Qso, RagChew, Net };
const char* kindName(Kind k);   // "single call", "QSO", "rag chew", "net"

struct Over { double t0 = 0, t1 = 0; int speaker = 0; };

struct Conversation {
    uint32_t id = 0;
    double dialHz = 0;            // follows the drift
    double firstDialHz = 0;
    Side side = Side::Unknown;
    std::string band, service;
    double startT = 0, lastVoiceT = 0, lastSeenT = 0;
    bool active = false;          // voice in the last activeSec
    bool ended = false;
    int overs = 0;
    double talkSec = 0;           // time with voice
    std::vector<Over> overList;   // the last 400 overs
    int speakers = 0;
    Kind kind = Kind::Single;
    double snrDb = 0, snrMaxDb = 0, snrTrendDbPerMin = 0;
    float bwHz = 0;
    float speech = 0;
    bool favorite = false, muted = false, recording = false;
    bool announced = false;       // the NEW event went out (after the first over: a blip of a weak signal is not a conversation)
    std::vector<std::string> clues;                   // callsigns heard (transcription hook)
    std::deque<std::pair<double, float>> snrHist;     // (time, snr) one point per ~5 s, last 10 minutes
    std::vector<std::pair<double, double>> segments;  // voice intervals (merged when closer than 3 s), for the timeline
    std::vector<uint32_t> trackIds;
    int audioKnownSpeakers = 0;
    std::string label() const;                        // "7.143 MHz LSB"
};

struct BookOptions {
    double conversationGapSec = 30;   // silence longer than this ends a conversation
    double overGapSec = 2.0;          // silence shorter than this is a pause inside an over
    double activeSec = 4;             // "ACTIVE now": voice in the last this many seconds
    double freqTolHz = 100;           // the same conversation; its frequency follows the signal
    double minOverSec = 0.8;          // shorter voice bursts do not count as an over
    double hideShortSec = 0;          // used by filters only
    size_t maxConversations = 500;
    double keepEndedSec = 24 * 3600;
};

enum class EventType { New, Ended };
struct BookEvent {
    EventType type = EventType::New;
    double time = 0;
    uint32_t id = 0;
    Conversation snapshot;
};

struct Filter {
    std::string band;                 // "" all; "40 m"
    double minSnrDb = -1e9;
    bool activeOnly = false;
    double hideShortSec = 0;          // hide conversations with less talk time than this
    bool hideEnded = false;
    bool favoritesOnly = false;
    bool showMuted = false;
    Side side = Side::Unknown;        // Unknown: both
    std::string service;              // "" all
};

enum class SortBy { Activity, Strength, Start, Frequency };

class ConversationBook {
public:
    explicit ConversationBook(const BookOptions& o = BookOptions());
    void setOptions(const BookOptions& o);
    const BookOptions& options() const { return o_; }
    // The detections of one scanner call; `now` is the clock of the audio (the same seconds). Returns the events (new / ended conversations).
    std::vector<BookEvent> update(double now, const std::vector<Detection>& dets);
    // Without new detections (the radio is elsewhere, or paused): ages conversations. Call about once a second.
    std::vector<BookEvent> tick(double now);

    std::vector<Conversation> list(double now, const Filter& f = Filter(), SortBy s = SortBy::Activity) const;
    const Conversation* find(uint32_t id) const;
    uint32_t convForTrack(uint32_t trackId) const;   // the conversation (not ended) a scanner track belongs to, 0 none
    size_t size() const { return conv_.size(); }
    void clear();

    // user choices
    void setFavorite(uint32_t id, bool on);
    void setMuted(uint32_t id, bool on);      // mutes this frequency: further activity within the tolerance is ignored and listed only with showMuted
    void setRecording(uint32_t id, bool on);
    bool isIgnoredHz(double hz) const;
    void ignoreHz(double hz, bool on);
    bool isFavoriteHz(double hz) const;
    void favoriteHz(double hz, bool on);                  // the same by frequency (restoring the saved stars)
    const std::vector<double>& favorites() const { return fav_; }
    const std::vector<double>& ignored() const { return ignore_; }
    void addClue(uint32_t id, const std::string& text);   // text from the transcription (hook): callsigns in it are added

    // the best conversation to listen to: active now first, favourites, strength; `exclude` are skipped (the user pressed Skip)
    uint32_t bestActive(double now, const std::vector<uint32_t>& exclude = {}) const;

    // export
    std::string toCsv(double now) const;
    std::string toJson(double now) const;
    static std::string fmtTime(double t);   // "2026-10-31 14:03:07" UTC

    struct Bar { double t0, t1; uint32_t id; Side side; bool favorite; };
    struct TimelineRow { double dialHz; std::string label; std::vector<Bar> bars; };
    // Rows per frequency (conversations within the tolerance share a row), bars for the last `hours`
    std::vector<TimelineRow> timeline(double now, double hours) const;

private:
    BookOptions o_;
    std::vector<Conversation> conv_;
    uint32_t nextId_ = 1;
    struct Speaker { float pitch = 0; double off = 0; };
    std::map<uint32_t, std::vector<Speaker>> spk_;
    struct Acc { double t0 = 0, off = 0; std::vector<float> pitches; std::vector<double> offs; bool counted = false; int speaker = 0; };
    std::map<uint32_t, Acc> acc_;                        // per conversation: the over in progress
    std::map<uint32_t, double> lastVoice_, splitT_;
    std::vector<double> ignore_, fav_;
    std::vector<std::pair<uint32_t, bool>> pendingRec_;
    void classify(Conversation& c) const;
    void closeOver(Conversation& c, double t1);
    void countOver(Conversation& c, Acc& a, double tEnd);
};

// Callsigns in transcribed text ("W1ABC", "whiskey one alpha bravo charlie", "DL1XYZ"); upper case keys, in order, without duplicates.
std::vector<std::string> findAmateurCallsigns(const std::string& text);

// ---------------------------------------------------------------- auto-listen
class AutoListen {
public:
    bool enabled = false;
    bool hold = false;            // keep the current conversation while it lasts
    double switchDelaySec = 2.5;  // wait this long after a conversation goes quiet before switching (and fade out over fadeSec)
    double fadeSec = 0.6;
    // Returns the conversation to listen to now (0: none). Call about every 0.25 s.
    uint32_t update(const ConversationBook& b, double now);
    void skip(uint32_t current);  // the user pressed Skip: not this one again until it ends
    void select(uint32_t id, double now);   // the user clicked Listen: that one, auto-listen holds it
    double gain(double now) const;          // 0..1 fade for the volume while switching
    uint32_t current() const { return cur_; }
private:
    uint32_t cur_ = 0;
    std::vector<uint32_t> skipped_;
    double quietSince_ = -1, switchT_ = -1e9;
};

// ---------------------------------------------------------------- scan of a narrow radio
struct SweepRange { double loHz, hiHz; std::string name; };
std::vector<SweepRange> defaultSweepRanges();   // the HF amateur bands and the maritime / aviation / utility blocks
class SweepPlanner {
public:
    SweepPlanner(std::vector<SweepRange> ranges, double spanHz, double dwellSec = 6, double maxDwellSec = 40);
    double centreHz() const;                      // where the radio should be now
    // Call about once a second: activeSignals = confirmed voice signals in the current step. Returns true when the radio must retune to centreHz().
    bool update(double now, int activeSignals);
    size_t steps() const { return centres_.size(); }
    size_t index() const { return idx_; }
    const std::string& rangeName() const;
private:
    std::vector<double> centres_;
    std::vector<size_t> rangeOf_;
    std::vector<SweepRange> ranges_;
    double dwell_, maxDwell_, stepStart_ = -1;
    size_t idx_ = 0;
};

// ---------------------------------------------------------------- audio clips
// 16-bit mono WAV of a conversation's audio plus a JSON metadata file next to it. Off until the user switches it on.
class ClipRecorder {
public:
    explicit ClipRecorder(std::string folder) : dir_(std::move(folder)) {}
    void append(const Conversation& c, const float* pcm, size_t n, double rateHz, double now);
    // The conversation ended / recording was switched off: writes the metadata, closes the file. Returns the WAV path ("" when nothing was recorded).
    std::string finish(const Conversation& c, double now);
    std::string pathFor(uint32_t id) const;
    size_t openClips() const { return clips_.size(); }
private:
    struct Clip { std::string path; size_t samples = 0; double rate = 8000; double t0 = 0; void* fp = nullptr; };
    std::string dir_;
    std::map<uint32_t, Clip> clips_;
};

// ---------------------------------------------------------------- test / demo signals (a speech-like SSB generator; deterministic)
namespace synth {
struct Station {
    double offsetHz = 0;          // dial (suppressed carrier) from the capture centre
    Side side = Side::Usb;
    double f0 = 120;              // pitch, Hz
    double gain = 1;              // amplitude (noise sigma is 1 per component / sqrt(2) at the options' noise level)
    double driftHzPerSec = 0;
    double fadeDepthDb = 0;       // slow fading depth
    double fadeRateHz = 0.3;
    double talkSec = 8, silenceSec = 6, startSec = 0;   // the station talks talkSec, is quiet silenceSec ... repeating from startSec
    double endSec = 1e9;          // and is off the air from here on
    int seed = 1;
    // the shape of what is sent: voice (default), or a decoy
    enum Kind { Voice, Ale8Fsk, MultiTone, Rtty, Psk31, Ft8, Am, Noise, Tone } kind = Voice;
    double bwHz = 2400;           // for Noise / MultiTone / Am
};
// Builds `seconds` of IQ at rateHz centred on 0 with the stations and complex white noise of the given power (linear, per sample).
std::vector<cf> make(double rateHz, double seconds, const std::vector<Station>& st, double noisePower, int seed = 7);
// The scenario of the panel's Demo and of the screenshots: a 20 m capture (96 kHz, centre 14.200 MHz, `seconds` long) with a QSO of two stations that
// alternate on one frequency, an LSB conversation that drifts, a net of four voices that ends early, a single call, a weak station and decoys.
struct Demo { double rateHz = 96000, centreHz = 14.2e6, seconds = 100, noisePower = 0.05; std::vector<Station> stations; };
Demo demo();
// Only the voice audio of one station (real, 8 kHz): the same generator the SSB uses; for the unit tests of the voice analyzer
std::vector<float> voiceAudio(double seconds, double f0, int seed, double talkSec = 1e9, double silenceSec = 0);
}  // namespace synth

}  // namespace dect2::ssbconv
