// The conversation tracker of the voice modes (DMR, P25, NXDN, dPMR, D-STAR, Yaesu Fusion, TETRA, M17): turns the monitor's call list
// (voice_monitor.h) into conversations. A conversation is the run of calls on the same carrier and slot between the same parties (one
// talkgroup, or the same two radios in either direction) with no more than `gapSec` of quiet between two calls. Pure functions of the
// call list, so they are unit-tested without a receiver. Nothing here decodes or stores audio: a call's recording is whatever the monitor
// already wrote for it (never for an encrypted call).
#pragma once
#include "dect2/voice/voice_monitor.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::voice {

struct ConvFilter {
    std::string query;        // any part of target, source, alias, label, access or state (case-insensitive); empty: everything
    double minSeconds = 0;    // calls shorter than this are left out
    float maxErrorRate = -1;  // calls with a worse bit error rate are left out (0..1; <0: off; a call with an unknown rate stays)
    bool playedOnly = false;  // only calls that were played
    bool withAudioOnly = false;   // only calls with a recording
    bool hideEncrypted = false;   // leave encrypted calls out
    int slot = 0;             // only this slot / channel (0: all)
    int64_t sinceWall = 0;    // only calls that started at or after this Unix time (0: all)
};

bool callMatches(const CallEntry& c, const ConvFilter& f);

struct Talker { std::string who; size_t calls = 0; double seconds = 0; };

struct Conversation {
    std::string key;          // carrier|slot|parties
    std::string title;        // "TG 91" or "1234 <-> 5678" (the label of the talkgroup is in `label`)
    std::string label;        // the user's name for the talkgroup, if any
    std::string access;       // "CC 1", "NAC 293", ...
    int slot = 0;
    double carrierHz = 0;
    bool group = true;
    int64_t firstWall = 0, lastWall = 0;   // Unix time of the first call's start and the last call's end
    double talkSeconds = 0;   // the sum of the calls' durations
    size_t played = 0, withAudio = 0, encrypted = 0, emergency = 0;
    float avgError = -1.f, worstError = -1.f;   // bit error rate 0..1 over the calls that know it (-1: none)
    bool active = false;      // a call of it is still going on
    std::vector<Talker> talkers;     // busiest first
    std::vector<CallEntry> calls;    // oldest first
    double spanSeconds() const { return lastWall > firstWall ? double(lastWall - firstWall) : 0.0; }
};

// Groups `calls` (any order) into conversations, newest conversation first. `gapSec` is the quiet time that ends a conversation.
std::vector<Conversation> groupConversations(const std::vector<CallEntry>& calls, double gapSec = 60.0, const ConvFilter& f = ConvFilter());

// "good" (under 2 % bit errors), "fair" (under 8 %), "poor", or "-" when the rate is not known
const char* signalLabel(float errorRate);
// the who-talks-to-whom key of a call (the two radios of a private call give the same key in either direction)
std::string partiesKey(const CallInfo& c);

// Exports. CSV: one line per call with the conversation number; JSON: the conversations with their calls. Text that starts with = + - @ is
// prefixed with ' in the CSV so a spreadsheet does not run it as a formula. The recording is the file name, never the audio.
std::string conversationsCsv(const std::vector<Conversation>& conv);
std::string conversationsJson(const std::vector<Conversation>& conv, const std::string& mode = std::string());
// Clock text "2026-10-31 12:34:56" in UTC for a Unix time
std::string utcText(int64_t t);

} // namespace dect2::voice
