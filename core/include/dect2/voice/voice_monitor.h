// The voice monitor shared by the digital voice modes (DMR, P25, NXDN, dPMR, D-STAR, Yaesu System Fusion, TETRA, M17): one per mode.
// The receiver asks it, frame by frame, whether the voice of a call may be played (offer), then hands it the sound that was played
// (audio) for the recording. The monitor keeps the calls heard (active and history), plays one call at a time per mode (whatever
// carrier or timeslot it is on: a group call sent by several sites or slots of a wide capture is heard once), and applies the user's
// rules: the allow list, the mute list, the priority list, hold on one talkgroup, a squelch on the frame error rate, the mode's own
// volume and output device, and the recording of each played call to a WAV file with a JSON file of its details.
//
// Encrypted calls are never played or recorded: the receiver reports them (encrypted = true) so they are listed, nothing more.
// Thread safety: every member may be called from any thread (one mutex).
#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {
class AudioOut;
namespace sess { class SessionStore; }

namespace voice {

// What the monitor answered for a voice frame
enum class Verdict {
    Play,            // played (and recorded when recording is on)
    Busy,            // another call of this mode is being played
    LowerPriority,   // a priority call is being played
    Muted,           // on the mute list
    Filtered,        // not on the allow list (allow-only is on)
    NotHeld,         // the monitor holds another talkgroup
    Squelched,       // the frame error rate is above the squelch
    Encrypted,       // never decoded
    NoCodec,         // the voice codec plug-in is not installed
};
const char* verdictText(Verdict v);

// One voice frame's call, as the receiver knows it
struct CallInfo {
    uint32_t src = 0, dst = 0;   // radio IDs (0: not known)
    bool group = true;           // dst is a talkgroup
    int slot = 0;                // timeslot or channel on the carrier (0: none)
    double carrierHz = 0;        // the carrier (wide capture, trunking; 0: the tuned one)
    bool encrypted = false;
    bool codecMissing = false;
    bool emergency = false;
    float errorRate = -1.f;      // bit error rate of the frame 0..1 (-1: not known)
    std::string srcText, dstText;   // callsigns or names when the mode has no numeric IDs (D-STAR, M17, Yaesu)
    std::string alias;           // talker alias
    std::string access;          // the channel's access code as shown: "CC 1", "NAC 293", "RAN 5", "MCC 234 MNC 10"
};

// The text of the target ("TG 91", "1234", "CQCQCQ") and of the source
std::string targetText(const CallInfo& c);
std::string sourceText(const CallInfo& c);

// A list of IDs as the user types it: numbers, ranges "100-199", callsigns, "LY*" prefixes, separated by commas, spaces or new lines.
// "TG" before a number is ignored. A call matches when its target or its source does.
class IdList {
public:
    IdList() = default;
    explicit IdList(const std::string& text) { parse(text); }
    void parse(const std::string& text);
    bool empty() const { return ranges_.empty() && texts_.empty(); }
    bool matchTarget(const CallInfo& c) const;
    bool matchSource(const CallInfo& c) const;
    bool match(const CallInfo& c) const { return matchTarget(c) || matchSource(c); }
    bool matchId(uint32_t id) const;
    bool matchText(const std::string& s) const;
private:
    std::vector<std::pair<uint32_t, uint32_t>> ranges_;
    std::vector<std::string> texts_;   // upper case; a trailing '*' is a prefix
};

struct Settings {
    float volume = 1.f;          // the mode's volume 0..1, times the program's volume
    float squelchPct = 0.f;      // frames with a bit error rate above this many percent are not played (0: off)
    std::string device;          // the output device ("" the system default; AudioOut::devices())
    bool record = false;         // record each played call
    std::string recordDir;       // "" the default folder (setDefaultRecordDir)
    bool allowOnly = false;      // play only the calls on the allow list
    std::string allow, mute, priority, hold;   // IdList texts; hold: only this talkgroup is played
    std::string labels;          // "91=Worldwide" per line: names shown for IDs
    double hangSec = 2.0;        // a call is over after this long without a frame; the played call keeps the sound this long
};

// A call as listed
struct CallEntry {
    uint64_t id = 0;
    CallInfo info;               // the last frame's
    std::string key;             // carrier / slot / target / source
    double firstSec = 0, lastSec = 0;   // monitor clock
    int64_t wallStart = 0, wallEnd = 0; // Unix time
    Verdict verdict = Verdict::Busy;    // the last frame's
    uint64_t frames = 0, played = 0;
    float errorRate = -1.f;      // smoothed
    bool active = true;
    bool priority = false;
    std::string label;           // the label of the target (labels)
    std::string file;            // the recording (empty: none)
    uint64_t sessionId = 0;      // the call's Session in the session store (core/include/dect2/session), 0 = none
    double seconds() const { return lastSec > firstSec ? lastSec - firstSec : 0.0; }
};

class Monitor {
public:
    explicit Monitor(std::string mode);
    ~Monitor();
    const std::string& mode() const { return mode_; }

    // ---- the receiver
    Verdict offer(const CallInfo& c);
    // the sound played for a call that was given Play (interleaved stereo or mono): written to its recording when recording is on
    void audio(const CallInfo& c, const float* pcm, size_t frames, int channels, int rate);
    void endCall(const CallInfo& c);     // the receiver saw the end of the call (terminator)
    float volume() const;                // the mode's volume

    // ---- the interface
    Settings settings() const;
    void setSettings(const Settings& s);
    uint64_t settingsGen() const;        // changes with every setSettings (the receivers reopen their output when the device changed)
    std::vector<CallEntry> calls() const;     // active calls, the playing one first
    bool nowTalking(CallEntry& out) const;    // the call being played
    std::vector<CallEntry> history(const std::string& query = std::string(), size_t max = 500) const;   // newest first; query: any part of the IDs, names, label, verdict
    void holdCurrent();                  // hold the talkgroup being played (or the newest call)
    void holdTarget(const std::string& token);
    void releaseHold();
    void muteToken(const std::string& token);   // add to the mute list
    void clearHistory();
    std::string label(const CallInfo& c) const;
    static std::string targetToken(const CallInfo& c);   // what holdCurrent / muteToken put in the list for this call

    // ---- the session store (docs/MODE-EXPERIENCE.md, 2.2a). Every call is also handed to the store as a Session when it opens, now and then
    // while it runs, and when it ends: only a queue push, never SQLite or file work under this monitor's mutex. The store's key is (mode, receiver).
    void setSessionReceiver(uint16_t receiver);   // the receiver instance this monitor serves (a wide capture or a second SDR has its own monitor)
    uint16_t sessionReceiver() const;
    void setSessionStore(sess::SessionStore* store);   // nullptr: the program's store (sess::global()); tests pass their own, and reset it before it is destroyed

    // ---- tests: the clocks (seconds) and the end of idle calls
    void setClock(std::function<double()> monotonic, std::function<int64_t()> wall = nullptr);
    void housekeep();

    std::string saveSettings() const;            // key=value lines
    void loadSettings(const std::string& text);

private:
    struct Rec;
    struct Live { CallEntry e; std::unique_ptr<Rec> rec; double sessSec = -1e9; Verdict sessVerdict = Verdict::Busy; bool sessEnc = false, sessEmerg = false; };
    std::string mode_;
    mutable std::mutex mu_;
    Settings set_;
    IdList allow_, mute_, prio_, hold_;
    std::map<uint32_t, std::string> labelNum_;
    std::map<std::string, std::string> labelText_;
    uint64_t gen_ = 1, nextId_ = 1;
    std::map<std::string, Live> live_;
    std::deque<CallEntry> hist_;
    std::string cur_;                    // key of the call being played
    double curLast_ = -1e9;
    bool curPrio_ = false;
    std::function<double()> clock_;
    std::function<int64_t()> wall_;
    double now() const;
    void housekeepLocked(double t);
    void finishLocked(Live& l);
    void applyLocked();
    std::string labelLocked(const CallInfo& c) const;
    void openRecLocked(Live& l, int channels, int rate);
    uint16_t sessRx_ = 0;
    sess::SessionStore* sessStore_ = nullptr;
    bool sessNamed_ = false;
    sess::SessionStore* sessionStore() const;
    void sessionSyncLocked(Live& l, double t);   // begin or update (throttled) the call's session
    void sessionEndLocked(Live& l);              // end it, link its recording
};

// The monitor of a mode ("dmr", "p25", "nxdn", "dpmr", "dstar", "ysf", "tetra", "m17"): created on first use, lives for the program.
Monitor& monitor(const std::string& mode);
std::vector<std::string> monitorModes();          // the modes asked for so far
void setDefaultRecordDir(const std::string& dir); // the folder recordings go to when a mode names none (<dir>/<mode>/...)
std::string defaultRecordDir();
// All modes' settings as one text ("[mode]" sections) and back: the app keeps it in a file of its data folder.
std::string saveAllSettings();
void loadAllSettings(const std::string& text);

// For a receiver: opens `out` (48 kHz stereo) on the mode's output device, and opens it again when the user picked another device.
void ensureOutput(std::unique_ptr<AudioOut>& out, const Monitor& m, int rate = 48000, int startThreshold = 48000 / 5);

// The WAV file of a recording: 16 bit PCM. Tests read it back with readWav.
bool readWav(const std::string& path, int& rate, int& channels, std::vector<int16_t>& samples);

} // namespace voice
} // namespace dect2
