// Multi-radio trunking (SDRTrunk style): one radio sits on the control channel, the others are retuned to the voice channels as grants arrive; calls are
// tracked, recorded and logged. Receive only. Plain logic, no DSP and no threads: the host calls onGrant() / tick() and supplies the radios through the
// VoiceReceiver interface (a real receiver in the app, a fake one in the tests).
//
//   GrantEvent        a voice grant from any control channel (P25 Phase 1 / 2, DMR Tier III / Capacity Plus / Connect Plus, TETRA, MPT 1327, EDACS, LTR)
//   ChannelPlan       system identity, P25 identifier table (iden_up), logical channel table; CSV import / export
//   AccessList        talkgroup / radio-ID allow, deny, priority and record rules
//   VoiceReceiver     what the pool needs from a radio: tune, state, audio, slices of a wide capture
//   CallRecorder      one WAV file plus a JSON sidecar per call
//   TrunkPool         the controller: device roles, allocation, call state machine, preemption, device loss, call log
// One radio alone: a grant inside its capture is taken as a slice of it (no retune); a grant outside it cannot be followed, the pool says so once per
// channel (status line + notice) and keeps the control channel. Encrypted calls are logged and never allocated or recorded.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace trunk { struct Grant; }
namespace tpool {

enum class Proto { P25p1, P25p2, DmrTier3, DmrCapPlus, DmrConnectPlus, Tetra, Mpt1327, Edacs, Ltr, Other, MotorolaTypeII };
const char* protoName(Proto p);
bool protoDigitalVoice(Proto p);

// ------------------------------------------------------------------------------------------------ grants

struct GrantEvent {
    Proto proto = Proto::Other;
    int tg = -1, src = -1;       // talkgroup (or called unit), calling unit
    double freqHz = 0;           // the voice channel; 0: the channel plan did not give one (the pool says so)
    int channel = -1;            // the channel number the control channel used (for messages)
    int slot = -1;               // TDMA slot, -1 not TDMA
    bool group = true, emergency = false, encrypted = false;
    int priority = 4;            // service options priority, 1 high ... 7 low
    int unplacedType = -1;       // freqHz is 0 because the channel type cannot be placed
    int site = -1;               // the site (RFSS / site, or any number the host gives each control channel) that sent the grant; -1 not known
    double qualityDb = -999;     // how well the host hears that site's control channel (SNR or RSSI, higher is better); -999 not known
};

// ------------------------------------------------------------------------------------------------ channel plan

struct IdenRow { int id = 0; double baseHz = 0, spacingHz = 0, txOffsetHz = 0, bwHz = 12500; int slots = 1; };

class ChannelPlan {
public:
    std::string name;
    int wacn = -1, sysId = -1, nac = -1;
    void setIden(const IdenRow& r);
    const IdenRow* iden(int id) const;
    // P25: channel = identifier (4 bits) << 12 | channel number. 0 when the identifier is not known. slot is set for a TDMA identifier (slots > 1) from the grant's slot argument.
    double p25Freq(int channel) const;
    void setChannel(int number, double hz);         // logical channel number (DMR LCN, EDACS, LTR, MPT 1327)
    double channelHz(int number) const;             // 0 unknown
    size_t idenCount() const { return iden_.size(); }
    size_t channelCount() const { return chan_.size(); }
    void clear();
    // CSV. Rows (case-insensitive first column, '#' comments):
    //   system,<name>,<wacn hex or dec>,<system id>,<nac>
    //   iden,<id>,<base Hz or MHz>,<spacing Hz or kHz>,<tx offset Hz or MHz>[,<bandwidth Hz or kHz>[,<slots>]]       (iden_up is accepted as the first column too)
    //   chan,<number>,<Hz or MHz>
    // or a table with a header row that has a channel column (Channel, LCN, Number) and a frequency column (Frequency, Freq, MHz).
    // Returns the number of rows taken; bad rows are counted in *skipped and described in err (first few).
    int importCsv(const std::string& text, std::string* err = nullptr, int* skipped = nullptr);
    std::string exportCsv() const;

private:
    std::map<int, IdenRow> iden_;
    std::map<int, double> chan_;
};

// ------------------------------------------------------------------------------------------------ access rules

struct AccessRule {
    enum Kind { Talkgroup, Unit } kind = Talkgroup;
    int lo = 0, hi = 0;          // an id or a range (inclusive)
    enum Act { Allow, Deny } act = Allow;
    int priority = 0;            // 0 none; 1 (highest) to 10: a talkgroup of higher priority may take a radio from a lower one
    bool record = true;
    std::string label;
};

struct AccessDecision { bool allow = true; int priority = 0; bool record = true; std::string reason; };

class AccessList {
public:
    bool allowByDefault = true;  // false: only talkgroups with an Allow rule are followed
    void add(const AccessRule& r) { rules_.push_back(r); }
    void clear() { rules_.clear(); }
    const std::vector<AccessRule>& rules() const { return rules_; }
    bool remove(size_t i);
    // A unit Deny beats everything; then the talkgroup rules (a later rule wins over an earlier one for the same id); then the default.
    AccessDecision decide(int tg, int src) const;
    // CSV, header row decides the columns: Kind (TG / Unit), Id (Decimal, TGID) or From,To, Action (Allow / Deny), Priority, Record, Label (Alpha Tag).
    // A header with only an Id column and a Mode column (the RadioReference talkgroup export: D, DE, T ...) is taken as talkgroup Allow rows with E rows denied.
    int importCsv(const std::string& text, std::string* err = nullptr, int* skipped = nullptr);
    std::string exportCsv() const;

private:
    std::vector<AccessRule> rules_;
};

// ------------------------------------------------------------------------------------------------ receivers

struct TuneRequest {
    double freqHz = 0;           // the channel
    Proto proto = Proto::Other;
    int slot = -1;
    int tg = -1, src = -1;
    bool control = false;        // the control channel (a radio promoted after the control radio was lost)
};

enum class RxState { Idle, Tuning, Locked, Failed };

// Implemented by the host. Everything is polled by TrunkPool::tick(); nothing blocks.
class VoiceReceiver {
public:
    virtual ~VoiceReceiver() {}
    virtual std::string name() const = 0;
    virtual bool present() const = 0;                       // false: unplugged, stopped or closed
    virtual bool canControl() const { return true; }        // can decode a control channel
    virtual double captureCentreHz() const = 0;
    virtual double captureSpanHz() const = 0;               // 0: only the tuned channel
    virtual int maxSlices() const { return 0; }             // channels it can decode at once inside its capture, without a retune
    virtual bool tune(const TuneRequest& r) = 0;            // false: refused (out of its range ...). The retune runs on; state() reports it.
    virtual RxState state() const = 0;                      // Tuning until it hears the channel (settled, synchronised); Failed when it never does
    virtual bool hearingVoice() const = 0;                  // voice (or a carrier for analog) on the channel now
    virtual bool terminated() { return false; }             // the end of the call was heard (a terminator); true once per call
    virtual bool encryptedHeard() const { return false; }   // the voice channel says the call is encrypted
    virtual void release() = 0;                             // stop following; park
    virtual bool openSlice(const TuneRequest& r, int* slice) { (void)r; (void)slice; return false; }
    virtual void closeSlice(int slice) { (void)slice; }
    virtual bool sliceHearing(int slice) const { (void)slice; return false; }
    // Decoded audio waiting (mono float). slice -1: the tuned channel. Returns the frames written; *rate is the sample rate.
    virtual int pullAudio(int slice, float* out, int maxFrames, int* rate) { (void)slice; (void)out; (void)maxFrames; (void)rate; return 0; }
};

// ------------------------------------------------------------------------------------------------ recorder

struct CallMeta {
    uint64_t id = 0;
    Proto proto = Proto::Other;
    std::string system, device, endReason;
    int tg = -1, src = -1, slot = -1;
    double freqHz = 0, durationSec = 0, audioSec = 0, setupSec = 0;
    int64_t wallStart = 0;
    bool emergency = false, group = true;
};

class CallRecorder {
public:
    ~CallRecorder() { finish(nullptr); }
    // <dir>/<system>_tg<TG>_<YYYYMMDD-HHMMSS>_<id>.wav; the directory is made. False when the file cannot be opened.
    bool begin(const std::string& dir, const CallMeta& m, std::string* err = nullptr);
    void feed(const float* mono, int frames, int rate);
    // Closes the files and writes the .json sidecar. Returns the wav path (empty when nothing was open); a call with no audio leaves no files.
    std::string finish(const CallMeta* final);
    bool open() const { return f_ != nullptr; }
    const std::string& path() const { return path_; }
    double seconds() const { return rate_ > 0 ? (double)frames_ / rate_ : 0; }
    static std::string metaJson(const CallMeta& m, const std::string& wav);
    static void finishMetaOnly(const std::string& dir, const CallMeta& m);   // the JSON alone: a call heard without sound (no vocoder installed)
    static std::string fileStem(const CallMeta& m);

private:
    void* f_ = nullptr;
    std::string path_;
    uint64_t frames_ = 0;
    int rate_ = 0;
    CallMeta meta_;
    void header();
};

// ------------------------------------------------------------------------------------------------ pool

enum class Role { Control, Voice, Idle, Lost, Off };
const char* roleName(Role r);

enum class CallState { Tuning, Active, Hold, Ended };
const char* callStateName(CallState s);

struct PoolConfig {
    double hangSec = 2.0;            // no voice and no grant repeat for this long: the call has ended
    double maxSetupSec = 3.0;        // the radio must hear the channel within this long of the retune, else the call is dropped
    double setupBackoffSec = 6.0;    // after a call nobody heard, the same talkgroup is not tried again for this long
    double endedCooldownSec = 4.0;   // a late grant for a call that just ended (control channel latency) does not open it again
    double maxCallSec = 1800;        // safety: a call longer than this is closed (a stuck carrier)
    double edgeGuardHz = 12500;      // a channel this close to the edge of a capture is not taken as inside it
    double ccLossSec = 5.0;          // no grant or control message for this long: controlLost() reports it
    double notFollowedLogSec = 30;   // an unfollowable call is logged again at most this often per channel
    int maxCalls = 8;
    // The same group call granted by two sites (or heard by two control radios of overlapping systems) is one call: the second grant opens no second
    // radio and no second log row; it is noted on the first, and if it comes from a clearly better-heard site within the first seconds the call moves there.
    bool dedupSites = true;
    double dedupSwapSec = 2.5;       // a copy that is this much better may take the call over only within this long of its start
    double dedupSwapDb = 6;
    bool record = true;
    bool recordMetaOnly = false;     // write the JSON even if there is no audio (no vocoder installed)
    std::string recordDir;           // empty: no files
    std::string systemName = "sys";
    bool preempt = true;
    bool allowSlices = true;         // calls inside a capture are decoded as slices of it (no retune)
    bool promoteControl = true;      // the control radio disappeared: the first idle capable radio takes over
};

struct CallRecord {
    uint64_t id = 0;
    int64_t wallStart = 0;
    double startSec = 0, lastVoiceSec = 0, lastGrantSec = 0, endSec = 0, setupSec = 0;
    CallState state = CallState::Tuning;
    Proto proto = Proto::Other;
    int tg = -1, src = -1, slot = -1, channel = -1, priority = 4;   // priority: the access rule's, 0 none
    double freqHz = 0;
    bool emergency = false, group = true, encrypted = false;
    bool followed = false, missed = false;
    int site = -1;                       // the site whose grant is being followed
    double qualityDb = -999;
    int duplicates = 0;                  // grants for the same call from other sites, merged into this row
    std::vector<int> altSites;           // the other sites that granted it
    int device = -1, slice = -1;         // pool device index that decodes it; slice -1: its tuned channel
    std::string deviceName, note, endReason, audioPath;
    double audioSec = 0;
    bool active() const { return state != CallState::Ended; }
    double durationSec() const { return (state == CallState::Ended ? endSec : lastVoiceSec) - startSec; }
};

struct DeviceInfo {
    int index = -1;
    std::string name;
    Role role = Role::Idle;
    bool present = false, enabled = true;
    RxState rx = RxState::Idle;
    int tg = -1;                         // the call it is on (0 none shown as -1)
    uint64_t call = 0;
    double freqHz = 0;                   // where it is tuned for the call (control: the control channel)
    int slices = 0;                      // calls decoded as slices of its capture
    uint64_t calls = 0;                  // calls taken since the start
};

struct PoolStats {
    uint64_t grants = 0, calls = 0, followed = 0, missedBusy = 0, missedOutside = 0, missedNoSignal = 0, missedDenied = 0, duplicates = 0, encrypted = 0, preempted = 0, deviceLost = 0, recordings = 0;
    double retuneAvgSec = 0, retuneMaxSec = 0;
    int retunes = 0;
};

class TrunkPool {
public:
    explicit TrunkPool(const PoolConfig& c = PoolConfig()) : cfg_(c) {}
    void setConfig(const PoolConfig& c) { cfg_ = c; }
    const PoolConfig& config() const { return cfg_; }
    ChannelPlan plan;
    AccessList access;

    // Takes ownership. The first device added that canControl() is the control radio unless setControl() says otherwise.
    int addDevice(std::unique_ptr<VoiceReceiver> rx);
    int addDeviceRef(VoiceReceiver* rx);             // not owned
    void removeDevice(int index);                    // the radio was closed: a call on it ends as "device lost"
    bool setControl(int index);
    void setEnabled(int index, bool on);
    int deviceCount() const { return (int)dev_.size(); }
    DeviceInfo device(int index) const;
    std::vector<DeviceInfo> devices() const;
    int controlDevice() const { return ctl_; }
    // Auto-assign: control = the radio the user is on (hint, or the first capable), every other present radio is a voice radio.
    void autoAssign(int controlHint = -1);

    // The control channel
    void setControlChannel(double hz) { ccHz_ = hz; }
    double controlChannelHz() const { return ccHz_; }
    void onControlSeen(double now) { lastCc_ = now; if (!ccSeen_) ccSeen_ = true; }
    bool controlLost(double now) const { return ccSeen_ && now - lastCc_ > cfg_.ccLossSec; }

    // Voice events
    void onGrant(const GrantEvent& g, double now);
    void tick(double now);

    const std::vector<CallRecord>& calls() const { return log_; }
    std::vector<const CallRecord*> nowTalking() const;
    const PoolStats& stats() const { return st_; }
    // One line for the UI: what the pool is doing now or why it cannot follow calls
    std::string statusLine(double now) const;
    std::vector<std::string> messages;               // newest last, at most 200
    std::string callLogCsv() const;
    std::function<void(const CallRecord&)> onCallEnd;   // the host's hook (a transcription, a feed)
    std::function<void(const CallRecord&)> onCallStart;
    int64_t wallBase = 0;                            // Unix time at now = 0 (the host sets it; tests leave it)
    void clear();

private:
    struct Dev {
        VoiceReceiver* rx = nullptr;
        std::unique_ptr<VoiceReceiver> own;
        bool enabled = true, wasPresent = true;
        Role role = Role::Idle;
        uint64_t call = 0;                           // the call on its tuned channel
        std::vector<uint64_t> sliceCalls;            // by slice number, 0 free
        uint64_t calls = 0;
        double tuneAt = 0;
    };
    struct Live {
        uint64_t id = 0;
        int dev = -1, slice = -1;
        std::unique_ptr<CallRecorder> recorder;
        double tuneAt = 0;
        bool record = true;
    };
    PoolConfig cfg_;
    std::vector<Dev> dev_;
    std::vector<CallRecord> log_;
    std::vector<Live> live_;
    int ctl_ = -1;
    double ccHz_ = 0, lastCc_ = 0;
    bool ccSeen_ = false;
    uint64_t next_ = 1;
    PoolStats st_;
    double retuneSum_ = 0;
    struct Ended { int tg; double freq; double sec; };
    std::vector<Ended> ended_;
    std::vector<std::pair<int, double>> block_;      // talkgroup -> not before this time (the last attempt was never heard)
    std::vector<std::pair<double, double>> noted_;   // channel -> when an unfollowable call was last logged
    std::string why_;                                // why nothing can be followed (the status line)
    double lastNow_ = 0;

    void note(const std::string& s);
    bool covers(const VoiceReceiver& r, double hz) const;
    int pickCover(double hz) const;
    int pickIdle(double hz) const;
    int weakest(int priority, bool emergency) const;
    int rank(int priority, bool emergency) const { return emergency ? 0 : priority <= 0 ? 11 : priority; }
    Live* liveById(uint64_t id);
    void start(const GrantEvent& g, const AccessDecision& d, double now);
    void assign(Live& l, CallRecord& r, int dev, int slice, const TuneRequest& tr, double now);
    void miss(CallRecord& r, const char* why, double now);
    void endCall(Live& l, const char* why, double now, bool cooldown = true);
    void freeDevice(Live& l);
    void deviceLost(int di, double now);
    void promote(double now);
    void pushAudio(Live& l, double now);
    CallRecord& addRecord(const GrantEvent& g, const AccessDecision& d, double now);
};

// ------------------------------------------------------------------------------------------------ adapters from the protocols' own grant records

// P25 Phase 1 / 2 and DMR Tier III from the existing control channel model (dect2/p25/trunk.h); DMR is Proto::DmrTier3
GrantEvent grantFromTrunk(const trunk::Grant& g);
// MPT 1327: the GTC names a channel number; the frequency follows from the control channel number and frequency and the spacing
GrantEvent grantMpt1327(int channel, int controlChannel, double controlHz, double spacingHz, int called, int caller, bool group = true);
// EDACS / LTR / DMR Connect Plus / Capacity Plus: a logical channel number looked up in the plan
GrantEvent grantFromLcn(Proto p, const ChannelPlan& plan, int lcn, int tg, int src, bool group = true, bool emergency = false, bool encrypted = false, int slot = -1);
// TETRA: a carrier number and band from the channel allocation of a D-SETUP / D-CONNECT (ETSI EN 300 392-2 clause 21.4.4.1): base + number x 25 kHz + offset
GrantEvent grantTetra(double baseHz, int carrier, int offsetCode, int slot, int ssi, int gssi, bool encrypted);

} // namespace tpool
} // namespace dect2
