// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Auto subtitles: speech-to-text turned into timed caption cues for the sound of a receiver (FM, DAB, TV audio, SSB, ...).
// Pure data and rules (no radio, no UI, no speech engine): sentence segmentation of the transcriber's entries with times on the AUDIO clock
// (samples played, not the wall clock, so the cues line up with a recording), the cue list shown over the receiver, SRT / WebVTT export, the
// choice between broadcast subtitles and generated ones, and the caption look. docs/TRANSCRIBE.md ("Auto subtitles").
#pragma once
#include "dect2/transcribe/transcript.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace dect2 {

struct CaptionCue {
    double start = 0, end = 0;   // seconds on the audio clock of the track
    double shownAt = 0;          // clock when the cue arrived (it is on screen from then on)
    std::string text;            // as spoken
    std::string translation;     // English, "" when none
    std::string lang;
};

// Splits text into sentences (. ! ? and the CJK stops); a sentence longer than maxChars is cut at a comma or a space.
std::vector<std::string> splitSentences(const std::string& text, size_t maxChars = 84);

// Which subtitles to show on TV: the station's own (DVB, teletext), ours, both, or ours only where the station sends none.
enum class CaptionSource { Broadcast = 0, Generated = 1, Both = 2, Auto = 3 };
bool showBroadcastSubs(CaptionSource s);
bool showGeneratedSubs(CaptionSource s, bool broadcastPresent);

// The look of the caption overlay, clamped to what is readable
struct CaptionStyle {
    int sizePct = 100;           // 60..250 (percent of the normal text size)
    int bgAlpha = 75;            // 0..100 (percent opacity of the box behind the text)
    bool top = false;            // at the top of the picture instead of the bottom
    int lines = 2;               // 1..4 cue lines on screen
    CaptionStyle clamped() const;
};

// Whether the sound of a standard (the receiver's `standard` number) carries speech worth captioning. Airband and marine have their own
// transcripts (docs/TRANSCRIBE.md) and are not in the list.
bool captionStandardEligible(int standard);
const char* captionStandardName(int standard);   // the label for the settings list ("FM broadcast")
std::vector<int> captionStandards();             // every eligible standard, in list order

class CaptionTrack {
public:
    // The audio clock: n samples at `rate` were played, the last one at wall time endWallMs.
    void onAudio(size_t n, int rate, int64_t endWallMs);
    double clock() const;                               // seconds of audio since the track began
    double clockAt(int64_t wallMs) const;               // the audio clock at a wall time (interpolated from the last block)
    void reset();                                       // a new station: the cues and the clock start again
    // A transcript entry (startMs set) becomes one or more cues. Returns how many were added.
    int add(const TranscriptEntry& e);
    std::vector<CaptionCue> cues() const;
    // The cues on screen now: arrived and not yet held for long enough, newest last, at most maxLines.
    std::vector<CaptionCue> visible(size_t maxLines) const;
    size_t size() const;
    void markOrigin();                                  // a recording starts now: its time 0 is this clock value
    double origin() const;
    // Exports. from/to are clock seconds (to <= 0: to the end); times are rebased to `from`. translation: 0 original, 1 translation (when
    // there is one), 2 both (a second line).
    std::string srt(double from = 0, double to = 0, int translation = 0) const;
    std::string vtt(double from = 0, double to = 0, int translation = 0) const;

private:
    mutable std::mutex mu_;
    double clock_ = 0, origin_ = 0, anchorClock_ = 0;
    int64_t anchorWall_ = 0;
    std::vector<CaptionCue> cues_;
    static constexpr size_t kMaxCues = 4000;
};

std::string cueTimestamp(double sec, bool vtt);   // "00:01:02,345" (SRT) or "00:01:02.345" (WebVTT)

}  // namespace dect2
