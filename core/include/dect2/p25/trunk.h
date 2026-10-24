// Trunking follower for P25 (TIA-102) and, as far as the public specs allow, DMR Tier III (ETSI TS 102 361-4). Receive only.
//
// Pieces, all plain logic with no DSP and no threads of their own (the interface thread drives them):
//   SystemModel     what the control channel says: site (WACN, system, RFSS, site, NAC), the channel identifier table, adjacent sites, the
//                   control-channel candidates, and the voice grants (as Grant records) made of TSBKs.
//   TalkgroupList   names, tags, priority, lockout and scan lists; CSV import / export (the RadioReference-style columns, see docs/trunking.md).
//   Follower        the state machine: grant -> follow (a slice of the wide capture, or a retune of the one radio) -> end -> back to the control channel.
//   CallLog         every call heard on the control channel (followed or not), a live "now talking" view, CSV export.
//   Controller      the four together; the host executes the Actions it returns and reports what the radios hear.
// Encrypted calls (P25 service options bit 0x40 or ALGID != 0x80, DMR privacy flag) are logged as encrypted and never followed, decoded or recorded.
// Voice audio only exists when the user-installed vocoder add-on is present (docs/CODECS.md); IDs, talkgroups and the call log work without it.
#pragma once
#include "dect2/p25/p25_proto.h"
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace trunk {

// ------------------------------------------------------------------------------------------------ system model

struct Grant {
    bool dmr = false;
    bool group = true;
    int tg = -1, src = -1;       // talkgroup (or called unit), calling unit
    int channel = -1;            // P25: identifier + channel; DMR: the logical channel number
    double freqHz = 0;           // 0 when the identifier update / channel table for it is not known yet
    int slot = 0;                // TDMA (P25 Phase 2 channel type or DMR): 0 or 1; -1 not TDMA
    bool tdma = false;
    bool emergency = false;
    bool encrypted = false;      // reported only
    int algId = -1;              // when known
    int priority = 4;            // service options priority (1 high ... 7 low; 4 default)
    bool explicitUpdate = false; // came from an update message (a repeat of a running call), not a new grant
};

struct IdenInfo {
    bool valid = false, vu = false, tdma = false;
    int chanType = 0, slots = 1;
    double baseHz = 0, spacingHz = 0, txOffsetHz = 0, bwHz = 0;
};

struct SiteInfo {
    bool valid = false;
    int wacn = -1, sysId = -1, rfss = -1, site = -1, nac = -1;
    int ccChannel = -1;
    double ccHz = 0;
    int ssc = 0;
    double lat = 0, lon = 0;     // only when the user put coordinates into the site table (the control channel does not carry them)
    bool hasPosition = false;
    double lastSec = 0;
    bool adjacent = false;
};

class SystemModel {
public:
    // One decoded TSBK from the control channel. Grants are appended to *grants (may be null); the return value is a short text for the message list ("" for nothing new).
    std::string onTsbk(const p25::Tsbk& t, int nac, double nowSec, std::vector<Grant>* grants);
    double channelFreq(int channel, int* slot = nullptr, bool* tdma = nullptr) const;   // 0 when unknown
    const SiteInfo& site() const { return site_; }
    const std::vector<SiteInfo>& adjacent() const { return adj_; }
    const IdenInfo& iden(int id) const { return idens_[id & 15]; }
    // The control channel and the others the system announces (secondary control channels, adjacent sites): the frequencies to try, current one first
    std::vector<double> controlCandidates() const;
    void setSitePosition(int sysId, int rfss, int site, double lat, double lon);
    uint64_t tsbks() const { return nTsbk_; }
    uint64_t grants() const { return nGrant_; }
    void clear();

private:
    SiteInfo site_;
    std::vector<SiteInfo> adj_;
    IdenInfo idens_[16];
    std::vector<double> secondary_;
    std::vector<std::pair<std::pair<int, std::pair<int, int>>, std::pair<double, double>>> positions_;
    uint64_t nTsbk_ = 0, nGrant_ = 0;
};

// Extra opcodes (TIA-102.AABC) next to the ones in p25_proto.h
constexpr int kOpGrpVGrantUpdExp = 0x03, kOpIdenUpTdma = 0x33, kOpSccb = 0x39;

// ------------------------------------------------------------------------------------------------ talkgroup list

struct TgEntry {
    int id = 0;
    std::string alpha, mode, desc, tag, category, list;   // short name, D / DE / T / A, description, tag (Law Dispatch ...), group, scan list
    int priority = 0;            // 0 none; 1 (highest) to 10
    bool lockout = false;
    bool listen = true;          // taken into the scan lists when scan-only is on
    bool encryptedMode() const { return !mode.empty() && mode.find('E') != std::string::npos; }
};

class TalkgroupList {
public:
    TgEntry* find(int id);
    const TgEntry* find(int id) const;
    TgEntry& upsert(int id);
    bool remove(int id);
    const std::vector<TgEntry>& entries() const { return v_; }
    void clear() { v_.clear(); }
    // CSV: header row decides the columns (case-insensitive; Decimal / DEC / TGID, Hex, Alpha Tag, Mode, Description, Tag, Category / Group, Priority, Lockout, List).
    // Quoted fields, "" escapes, CR / LF / CRLF accepted. Returns the number of rows taken; bad rows are counted in *skipped and described in err (first few).
    int importCsv(const std::string& text, std::string* err = nullptr, int* skipped = nullptr, bool merge = true);
    std::string exportCsv() const;
    // Scan lists: when scanOnly is on only talkgroups with listen set (and in an enabled list, if any list is enabled) are followed.
    void setScanOnly(bool on) { scanOnly_ = on; }
    bool scanOnly() const { return scanOnly_; }
    void enableList(const std::string& name, bool on);
    bool listEnabled(const std::string& name) const;
    std::vector<std::string> lists() const;
    struct Decision { bool follow = true; int priority = 0; const char* reason = ""; };
    Decision decide(int tg) const;

private:
    std::vector<TgEntry> v_;
    bool scanOnly_ = false;
    std::map<std::string, bool> enabled_;
};

// ------------------------------------------------------------------------------------------------ call log

struct CallRecord {
    uint64_t id = 0;
    int64_t wallTime = 0;        // Unix time of the start
    double startSec = 0, lastSec = 0, durSec = 0;
    bool active = false;
    bool dmr = false, group = true;
    int tg = -1, src = -1;
    int channel = -1; double freqHz = 0; int slot = -1;
    bool emergency = false, encrypted = false;
    bool followed = false;       // a slice or the radio was on it
    bool missed = false;         // not followed because it needed a wider capture or a second radio
    std::string note;            // why it was not followed: lockout, encrypted, busy, out of span ...
    std::string audioPath;       // the recording, when one was made
};

class CallLog {
public:
    CallRecord* byId(uint64_t id);
    const std::vector<CallRecord>& records() const { return v_; }   // oldest first
    CallRecord& add(const CallRecord& r);
    void trim(size_t keep);
    std::string exportCsv(const TalkgroupList* names = nullptr) const;
    void clear() { v_.clear(); next_ = 1; }

private:
    std::vector<CallRecord> v_;
    uint64_t next_ = 1;
};

// ------------------------------------------------------------------------------------------------ follower

struct Action {
    enum Kind { OpenSlice, CloseSlice, Retune, Listen, RecordStart, RecordStop, Notice } kind = Notice;
    uint64_t callId = 0;
    int slice = -1;              // OpenSlice / CloseSlice / Listen: the follower's own slice number
    double freqHz = 0;           // OpenSlice / Retune
    bool tdma = false;           // OpenSlice / Retune: a P25 Phase 2 voice channel (needs the Phase 2 receiver, seeded with the site's WACN / system / NAC)
    int slot = -1;               // ... its time slot, 0 or 1
    std::string text;            // Notice; RecordStart: the file name stem
};

struct FollowConfig {
    int maxSlices = 2;           // simultaneous followed calls inside the capture (CPU limit)
    double hangSec = 2.0;        // a call with no grant repeat or voice for this long has ended
    double ccLossSec = 4.0;      // no control message for this long: the control channel is lost
    double ccDwellSec = 3.0;     // time spent on each candidate while looking for the control channel
    double returnTimeoutSec = 3.0;   // back on the control channel but nothing heard for this long: start looking
    double edgeGuardHz = 12500;  // a voice channel this close to the edge of the capture is not taken as inside
    double sliceBandwidthHz = 12500;
    bool allowRetune = false;    // the single radio may leave the control channel for a call (and look for the control channel when it is lost); off: it stays
    bool followPhase2 = true;    // follow P25 Phase 2 (TDMA) grants (inside the capture: one at a time, the engine runs one Phase 2 receiver)
    bool record = true;
};

enum class FollowState { NoControl, ControlHold, Following, Hopped, Returning, Searching };
const char* followStateName(FollowState s);

class Follower {
public:
    explicit Follower(const FollowConfig& c = FollowConfig()) : cfg_(c) {}
    void setConfig(const FollowConfig& c) { cfg_ = c; }
    const FollowConfig& config() const { return cfg_; }
    // The capture: centre and span of what the radio delivers now. spanHz 0: only the tuned channel (every other frequency needs a retune).
    // slices: Phase 1 calls inside it can be decoded there (whole-span decoding is on). A Phase 2 call inside it needs no such thing: the Phase 2
    // receiver runs as the second mode anywhere in the capture.
    void setCapture(double centerHz, double spanHz, bool slices = true) { centerHz_ = centerHz; spanHz_ = spanHz; slices_ = slices; }
    void setControlChannel(double hz, const std::vector<double>& candidates);
    // A control channel message arrived (any good TSBK)
    void onControlSeen(double now);
    // A grant (new, or a repeat of a running call). decision comes from the talkgroup list.
    void onGrant(const Grant& g, const TalkgroupList::Decision& d, double now, CallLog& log);
    // The slice (or the hopped radio) hears voice from the call, or its terminator
    void onVoice(uint64_t callId, double now);
    void onTerminator(uint64_t callId, double now);
    // Stop following the call but keep it in the log as running (its grant repeats keep it there, not followed); it ends by silence on the
    // control channel like any call nobody follows
    void leave(uint64_t callId, double now);
    // Time passes: ends calls, returns the radio, rotates through control channel candidates
    void tick(double now, CallLog& log);
    std::vector<Action> takeActions();
    FollowState state() const { return state_; }
    int activeSlices() const;
    uint64_t audibleCall() const;      // the call whose sound plays (highest priority, then oldest); 0 none
    uint64_t missedCalls() const { return missed_; }
    bool inSpan(double freqHz) const;
    double currentControlHz() const { return ccHz_; }
    double radioHz() const { return radioHz_; }   // where the single radio was last told to be (0: not told)
    std::vector<double> sliceFreqs(bool tdma) const;   // the open slices (inside the capture) of Phase 1 (tdma false) or Phase 2 calls
    void reset();

private:
    struct Slot { uint64_t callId = 0; int prio = 0; double freqHz = 0, startSec = 0, lastSec = 0; bool used = false, hopped = false, tdma = false, leave = false; };
    std::vector<Slot> slots_;
    FollowConfig cfg_;
    FollowState state_ = FollowState::NoControl;
    double centerHz_ = 0, spanHz_ = 0, ccHz_ = 0, radioHz_ = 0, homeHz_ = 0;
    bool slices_ = true;
    std::vector<double> cands_;
    size_t candIdx_ = 0;
    double lastCcSec_ = -1e9, stateSince_ = 0, lastNow_ = 0, dwellStart_ = 0;
    std::vector<Action> out_;
    uint64_t missed_ = 0;
    std::vector<std::pair<double, double>> noticed_;   // out-of-span frequencies already announced
    void note(const std::string& s) { Action a; a.text = s; out_.push_back(a); }
    void close(Slot& s, CallLog& log, double now, const char* why);
    void goHome(double now);
    void setState(FollowState s, double now) { if (state_ != s) { state_ = s; stateSince_ = now; } }
    Slot* freeSlot();
};

// ------------------------------------------------------------------------------------------------ DMR Tier III

// The Tier III channel grants as CSBKs (ETSI TS 102 361-4). The opcode numbers are kept in one table; see docs/trunking.md for what is verified.
struct DmrOpcodes { int aloha = 0x19, tvGrant = 0x30, pvGrant = 0x31, btvGrant = 0x32; };
struct DmrLcn { int lcn = 0; double freqHz = 0; };
class DmrModel {
public:
    void setOpcodes(const DmrOpcodes& o) { ops_ = o; }
    void setChannel(int lcn, double freqHz);    // the user's logical channel table (the grants carry channel numbers only)
    double lcnFreq(int lcn) const;
    // csbk: opcode, feature ID, 8 data bytes. The grant layout used: logical channel (12 bits, bits 0-11) + slot (bit 12), then 24-bit target and 24-bit source.
    bool onCsbk(int opcode, int fid, const uint8_t data[8], double nowSec, Grant& out);
    uint64_t grants() const { return n_; }

private:
    DmrOpcodes ops_;
    std::map<int, double> lcn_;
    uint64_t n_ = 0;
};

// ------------------------------------------------------------------------------------------------ controller

struct ControllerStatus {
    FollowState state = FollowState::NoControl;
    uint64_t tsbkGood = 0, tsbkBad = 0;
    double crcOkPct = 0;         // control channel message success rate
    bool controlLocked = false;
    int activeCalls = 0;
    uint64_t missed = 0;
};

class Controller {
public:
    Controller();
    SystemModel sys;
    TalkgroupList tgs;
    CallLog log;
    Follower follower;
    DmrModel dmr;

    // From the P25 receiver: a good TSBK (nac = the NID's), or a failed one (counts toward the success rate)
    void onTsbk(const p25::Tsbk& t, int nac, double nowSec);
    void onTsbkBad(double nowSec);
    void onDmrCsbk(int opcode, int fid, const uint8_t data[8], double nowSec);
    void setCapture(double centerHz, double spanHz, bool slices = true) { follower.setCapture(centerHz, spanHz, slices); }
    void tick(double nowSec);
    std::vector<Action> takeActions() { return follower.takeActions(); }
    void onVoice(uint64_t callId, double now) { follower.onVoice(callId, now); }
    void onTerminator(uint64_t callId, double now) { follower.onTerminator(callId, now); }
    // The voice channel says the call is encrypted (the Phase 2 ESS: grants do not always say so): logged as encrypted, and left
    void onEncrypted(uint64_t callId, double now) {
        if (CallRecord* r = log.byId(callId)) { r->encrypted = true; r->followed = false; if (r->note.empty()) r->note = "encrypted (voice channel): left"; }
        follower.leave(callId, now);   // its grant repeats must not open it again
    }
    ControllerStatus status(double nowSec) const;
    std::vector<std::string> messages;   // newest last, at most 200
    void clear();

    // The live "now talking" view: calls that are active
    std::vector<const CallRecord*> nowTalking() const;

private:
    uint64_t good_ = 0, bad_ = 0;
    double lastGood_ = -1e9;
    void pushMsg(const std::string& s);
    void dispatch(const std::vector<Grant>& gs, double now);
};

// A TSBK generator for tests and for the demo: the messages a control channel sends, as Tsbk records (they go through tsbkBits / tsbkParse like the real ones)
namespace gen {
p25::Tsbk idenUp(int id, double baseHz, double spacingHz, double txOffsetHz, double bwHz);          // IDEN_UP (0x3D)
p25::Tsbk idenUpVu(int id, double baseHz, double spacingHz, double txOffsetHz, int bwCode);          // IDEN_UP_VU (0x34)
p25::Tsbk idenUpTdma(int id, int chanType, double baseHz, double spacingHz, double txOffsetHz);     // IDEN_UP_TDMA (0x33)
p25::Tsbk netSts(int wacn, int sysId, int ccChannel, int ssc);
p25::Tsbk rfssSts(int sysId, int rfss, int site, int ccChannel, int ssc);
p25::Tsbk adjSts(int sysId, int rfss, int site, int ccChannel, int ssc);
p25::Tsbk sccb(int rfss, int site, int ch1, int ch2);
p25::Tsbk grpVGrant(int svcOpts, int channel, int tg, int src);
p25::Tsbk uuVGrant(int channel, int dst, int src);
p25::Tsbk grpVGrantUpd(int ch1, int tg1, int ch2, int tg2);
p25::Tsbk grpVGrantUpdExp(int svcOpts, int chT, int chR, int tg);
}

} // namespace trunk
} // namespace dect2
