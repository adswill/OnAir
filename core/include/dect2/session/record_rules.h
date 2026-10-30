// Auto-record rules (docs/MODE-EXPERIENCE.md, 3.1 and 3.2): when a call, thread or track begins and a rule matches, the mode's recorder is asked
// to save audio and/or channel IQ. Everything is OFF by default (the master switch and the rule list are empty); the user turns it on with one
// click or adds a rule from a row ("Record this talkgroup"). A rule never blocks anything: it only says "record this", and an encrypted or
// unreadable session is never given an audio or IQ decision (metadata only, docs 3.6).
//
// Matching is by mode, an id/callsign/frequency match (a list: numbers, ranges "100-199", callsigns, "LY*" prefixes), an optional "emergency only",
// and the limits max length and disk cap. The set is thread safe; decide() is cheap (a few string compares) and is called by the voice monitor on
// the audio path and by the session store's writer thread.
#pragma once
#include "dect2/session/session.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dect2::sess {

enum RecAction : uint8_t { RecAudio = 1, RecIq = 2 };

// what the match text is compared with
enum class RecField : uint8_t { Any = 0, Target = 1, Source = 2, Callsign = 3, Frequency = 4 };
const char* recFieldName(RecField f);   // "Anything", "Talkgroup / channel / capcode", "Source", "Name or callsign", "Frequency"

struct RecordRule {
    uint32_t id = 0;
    bool enabled = true;
    std::string name;                // shown in the editor; filled from the match when empty
    uint16_t mode = 0;               // mode number, 0 = every mode
    RecField field = RecField::Any;
    std::string match;               // "" = everything of the mode; else a list ("91, 100-199, LY*")
    double freqMinMHz = 0, freqMaxMHz = 0;   // Frequency field: the range (max 0 = min only, +-12.5 kHz)
    uint8_t actions = RecAudio;      // RecAudio | RecIq
    bool emergencyOnly = false;
    uint32_t maxSeconds = 3600;      // longest recording of one call (0 = no limit)
    uint32_t iqSeconds = 10;         // length of an IQ clip (from the start of the call; no pre-roll exists for IQ)
    uint32_t capMB = 0;              // disk cap of all files this rule has made (0 = no cap); at the cap the rule stops asking and says so
    uint64_t usedBytes = 0;          // files made so far (kept in the rules file)
};

struct RecordDecision {
    bool audio = false, iq = false;
    uint32_t ruleId = 0;
    uint32_t maxSeconds = 0;         // audio
    uint32_t iqSeconds = 0;          // IQ clip
    bool any() const { return audio || iq; }
};

class RecordRules {
public:
    RecordRules();
    ~RecordRules();
    // master switch: default OFF
    bool enabled() const;
    void setEnabled(bool on);
    // the rules
    std::vector<RecordRule> list() const;
    uint32_t add(RecordRule r);                      // returns the id; an equal rule (same mode, field, match, actions) is not added twice (its id is returned)
    void update(const RecordRule& r);
    void remove(uint32_t id);
    bool has(uint16_t mode, RecField f, const std::string& match) const;
    // the answer for a session. Master off, no rule, encrypted / unreadable, or a rule at its cap -> nothing. The first matching rule per action wins.
    RecordDecision decide(const Session& s) const;
    // bytes a rule's recording used (the recorders call this when a file is finished)
    void addUsage(uint32_t ruleId, uint64_t bytes);
    // counters for the header: "N calls not recorded (cap)" and the rules that are at their cap now
    uint64_t cappedCount() const;
    void noteCapped();
    bool atCap(uint32_t ruleId) const;
    // size estimate of an IQ clip: rate x 8 bytes (cf32) x seconds
    static uint64_t iqEstimateBytes(double rateHz, uint32_t seconds) { return (uint64_t)(rateHz * 8.0 * seconds); }
    // changes with every edit (the store saves the rules file when it moved; the UI re-reads)
    uint64_t gen() const;
    std::string saveText() const;
    void loadText(const std::string& text);
    void clear();
    // handlers: called by the session store's writer thread when a session starts matching / ends. They must only queue work (never block).
    using StartFn = std::function<void(const Session&, const RecordDecision&)>;
    using EndFn = std::function<void(uint64_t sessionId)>;
    void setHandlers(StartFn onStart, EndFn onEnd);
    void notifyStart(const Session& s, const RecordDecision& d) const;
    void notifyEnd(uint64_t sessionId) const;
    // a human line for a rule: "Talkgroup 91 (DMR): audio, up to 1 h, 500 MB"
    static std::string describe(const RecordRule& r);

    // the match itself (tests use it)
    static bool matchList(const std::string& list, const std::string& field);
    static bool matches(const RecordRule& r, const Session& s);
private:
    struct Impl;
    Impl* p_;
};

// the program-wide rule set (the session store loads and saves it beside the sessions database)
RecordRules& rules();

}  // namespace dect2::sess
