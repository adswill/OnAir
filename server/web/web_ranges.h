// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// OnAir Web: what frequencies the host lets listeners tune and see (docs/ONAIR-WEB.md, "Frequency ranges").
//
// The host sets an ALLOW-LIST (web_allow_range, web_range_preset) and a BLOCK-LIST (web_block_range, block presets). A block always wins. When at
// least one allow rule applies to a listener class, that class can tune only inside the allow rules (and only with the listed modes if the rule
// names modes). Rules can be limited to a listener class: open (no password), member (knew the password), invite (an invite token).
//
// Everything in this header and web_ranges.cpp is pure (no sockets, no hub, no clock) so tests/server/test_web_ranges.cpp can check it alone.
// The hub (web_hub.cpp) calls it for: tune / passband requests (checkPassband, snapFreq), decoder subscriptions (checkWindow), the host
// decoder's results (captureVisible), and every spectrum row (maskSpectrumRow).
//
// Edge handling (the rules the tests pin down):
//  * a range is [lo, hi] in Hz, both ends included; ranges that overlap or touch (within 1 mHz) are merged before anything is decided;
//  * the PASSBAND of a listener (tuned frequency + low_hz .. + high_hz) grows by a GUARD margin on both sides (guardMarginHz: 5 % of the
//    bandwidth, at least 200 Hz, at most 5 kHz, for the filter skirts) and that whole grown span must lie inside ONE merged allowed range
//    (never spread over a gap, even a 1 Hz gap) and must not touch a block (touching counts as overlapping);
//  * a rule that names modes counts only for those modes; a block that names modes blocks only those modes (but still hides the spectrum
//    only when it names no mode: a mode-limited block is about listening, not about seeing);
//  * with any rule configured, the passband must also lie inside the usable part of the capture (96 % of the sample rate): a passband that
//    runs over the capture edge would otherwise pick up the OTHER end of the capture through FFT wrap-around, which may be a blocked range;
//  * spectrum rows: a bin that is not wholly visible, and one bin on each side of it, is replaced by the noise floor of the visible part.
#pragma once
#include "web_policy.h"
#include "web_proto.h"
#include <string>
#include <vector>

namespace onair_server { class ConfigFile; }

namespace onair_web {

constexpr double kMaxRangeHz = 1e12;       // anything above is a typing mistake (a terahertz)
constexpr double kRangeEpsHz = 1e-3;       // ranges closer than this touch
constexpr unsigned kAllModes = (1u << (unsigned)Demod::Count) - 1;
constexpr unsigned kAllClasses = 7;        // bit 0 open, bit 1 member, bit 2 invite

enum class RangeClass { Open = 0, Member = 1, Invite = 2 };
RangeClass rangeClassOf(UserClass c);                 // Public -> Open, Member -> Member, Invited -> Invite
const char* rangeClassName(RangeClass c);             // "open", "member", "invite"
bool rangeClassFromName(const std::string& s, RangeClass& c);   // open|public, member|password, invite|invited
inline unsigned classBit(RangeClass c) { return 1u << (unsigned)c; }

struct FreqRange { double lo = 0, hi = 0; unsigned modes = kAllModes; std::string note; };   // note: a label for the browser (not merged away in views)
struct RangeRule { FreqRange r; unsigned classes = kAllClasses; std::string note; };

// ---------------------------------------------------------------- parsing
// "7.000", "144MHz", "14 kHz", "1e6" ... A number without a unit is in `defaultMul` Hz (1 for Hz). Units (any case): Hz, kHz, MHz, GHz (k, M, G alone work).
// Refused: empty, not a number, inf / nan, unknown unit, negative, above kMaxRangeHz. The result is rounded to 1 mHz.
bool parseFreqHz(const std::string& text, double defaultMul, double& hz, bool& hadUnit, std::string& err);
// "<start>-<end>[,mode...][,open|member|invite...][,note=free text to the end of the line]". A start without a unit takes the end's unit
// ("144-146MHz"); with no unit at all the numbers are Hz. Refused: start above end (reversed), start equal to end (empty), negative or huge
// values, unknown words. `modes` empty: all modes; no class tag: all classes.
bool parseRangeSpec(const std::string& line, RangeRule& out, std::string& err);
std::string formatRangeSpec(const RangeRule& r);   // the canonical line (plain Hz), parseRangeSpec(formatRangeSpec(x)) == x

// ---------------------------------------------------------------- set operations
// Sorted, overlapping and touching ranges joined (the mode sets are joined by union; use rangesForMode first to keep modes apart).
std::vector<FreqRange> mergeRanges(std::vector<FreqRange> v);
std::vector<FreqRange> rangesForMode(const std::vector<FreqRange>& v, Demod d);   // only the ranges that include this mode, merged
// a minus b over merged lists
std::vector<FreqRange> subtractRanges(const std::vector<FreqRange>& a, const std::vector<FreqRange>& b);

// What one listener class sees: the rules that apply to it, with the presets expanded.
struct RangeView {
    bool restricted = false;               // at least one allow rule: tuning only inside `allow`
    std::vector<FreqRange> allow;          // not merged (modes kept)
    std::vector<FreqRange> block;          // not merged (modes kept)
    std::vector<std::string> notes;        // the host's notes (rule notes and preset descriptions), without duplicates
    bool active() const { return restricted || !block.empty(); }
};

// ---------------------------------------------------------------- evaluation
double guardMarginHz(double bandwidthHz);
enum class RangeVerdict { Ok, NotAllowed, ModeNotAllowed, Blocked, OutsideCapture, Invalid };
const char* verdictText(RangeVerdict v);   // the English message sent with err::ForbiddenRange (Ok: "")

// The tuning (freq + passband in `demod`) against the view. `centreHz` / `rateHz` of the capture (rate 0: the capture is not checked).
RangeVerdict checkPassband(const RangeView& v, Demod demod, double freqHz, double lowHz, double highHz, double centreHz = 0, double rateHz = 0);
// A frequency window (a decoder's slice, the capture) in any mode: wholly visible. Same edge rules as checkPassband, without a guard.
RangeVerdict checkWindow(const RangeView& v, double loHz, double hiHz);
// The nearest tuned frequency to `freqHz` for which checkPassband is Ok. False: there is none (no allowed range is wide enough).
bool snapFreq(const RangeView& v, Demod demod, double freqHz, double lowHz, double highHz, double centreHz, double rateHz, double& out);
// The listener may see the host decoder's results / the whole capture (it is wholly inside the visible ranges).
bool captureVisible(const RangeView& v, double centreHz, double rateHz);

// Spectrum row of `bins` dB values over [startHz, stopHz] (absolute). Hides everything the view does not make visible (see the top of the file).
// Hidden bins get the noise floor of the visible ones (their 20th percentile); with no visible bin all become -150 dB. Returns how many were hidden.
size_t maskSpectrumRow(std::vector<float>& db, double startHz, double stopHz, const RangeView& v);

// ---------------------------------------------------------------- presets (data tables in web_ranges.cpp; typical values, NOT authoritative)
struct PresetInfo { const char* name; bool block; const char* description; };
const std::vector<PresetInfo>& rangePresets();
// The rules of a preset (class mask `classes`); false: no such preset.
bool presetRules(const std::string& name, unsigned classes, std::vector<RangeRule>& out, bool& isBlock);

// ---------------------------------------------------------------- the host's configuration
class WebRanges {
public:
    // web_allow_range, web_block_range, web_range_preset (all repeatable) and web_range_action (refuse | clamp). Bad lines are skipped and named in warnings.
    void load(const onair_server::ConfigFile& c, std::vector<std::string>* warnings = nullptr);
    void save(onair_server::ConfigFile& c) const;
    // Replaces everything at once (the admin API); nothing changes when any line is bad (err names the first).
    bool replace(const std::vector<std::string>& allow, const std::vector<std::string>& block, const std::vector<std::string>& presets,
                 const std::string& action, std::string& err);
    void clear();
    bool empty() const { return allow_.empty() && block_.empty() && presets_.empty(); }
    bool clampMode() const { return clamp_; }
    RangeView view(RangeClass c) const;
    std::vector<std::string> allowSpecs() const, blockSpecs() const, presetSpecs() const;   // the canonical lines (what save() writes)
    std::string json() const;            // the admin API's GET ranges
    unsigned generation() const { return gen_; }   // grows with every change

private:
    struct PresetUse { std::string name; unsigned classes = kAllClasses; };
    std::vector<RangeRule> allow_, block_;
    std::vector<PresetUse> presets_;
    bool clamp_ = false;
    unsigned gen_ = 1;
    bool addPreset(const std::string& token, std::string& err);
};

// What a listener is told (welcome.ranges and the "ranges" message): {"restricted":b,"action":"refuse|clamp","guard":{...},"allow":[...],"block":[...],"notes":[...]}
std::string rangesJson(const RangeView& v, bool clamp);

} // namespace onair_web
