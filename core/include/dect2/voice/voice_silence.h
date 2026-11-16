// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// "No audio: why": the one reason a digital voice mode is silent although calls decode (DMR, P25, NXDN, ...), from what the voice monitor
// and the sound path know. Pure logic, no clocks or devices: the monitor fills SilenceIn (Monitor::silenceInput), the panels show the text.
// Same idea as voice_why.h (why nothing decodes); this one answers why nothing is HEARD.
#pragma once
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <string>

namespace dect2::voice {

enum class SilenceWhy {
    None,              // nothing to explain: playing, or no call heard lately
    AppMuted,          // the program's mute button
    AppVolume,         // the program's volume is 0
    ModeVolume,        // the mode's own volume slider is 0
    OutputError,       // the sound device did not open
    CodecUnavailable,  // the codec was found but no decoder could be started for a call
    CodecSilent,       // the codec decodes but only returns silence
    NoCodec,           // no voice codec installed
    Hold,              // a talkgroup hold is set and the calls heard are other talkgroups
    MuteList,          // the calls heard are on the mute list
    AllowList,         // "play only the allow list" is on and the calls heard are not on it
    Squelch,           // the calls heard have more bit errors than the squelch lets through
    Encrypted,         // the calls heard are encrypted (never decoded)
};

struct SilenceIn {
    bool live = false;              // the receiver of the mode runs
    bool codecReady = true;         // the voice codec is loaded
    float monVolume = 1.f;          // the mode's volume slider 0..1
    float appVolume = 1.f;          // the program's volume 0..1
    bool appMuted = false;
    bool outputFailed = false;      // the sound device could not be opened
    std::string outputError;        // what to tell about it (device name)
    std::string hold;               // the talkgroup hold ("" none)
    float squelchPct = 0.f;
    bool codecUnavailable = false;  // a call was played but its decoder could not start
    uint32_t silentRun = 0;         // decoded bursts in a row that were all zeros
    // seconds since a voice frame got each verdict (large: not lately)
    double sincePlay = 1e9, sinceNotHeld = 1e9, sinceMuted = 1e9, sinceFiltered = 1e9, sinceSquelched = 1e9, sinceEncrypted = 1e9, sinceNoCodec = 1e9;
};

struct Silence {
    SilenceWhy why = SilenceWhy::None;
    std::string text;               // "No audio: ..." (empty for None)
    bool canReleaseHold = false;    // the panel offers [Release hold]
    bool canUseDefaultOutput = false;   // the panel offers [Use default output]
};

constexpr double kSilenceRecentSec = 6.0;   // a verdict counts as "lately" for this long

inline Silence whySilent(const SilenceIn& in) {
    Silence s;
    const double R = kSilenceRecentSec;
    const double sinceAny = std::min({in.sincePlay, in.sinceNotHeld, in.sinceMuted, in.sinceFiltered, in.sinceSquelched, in.sinceEncrypted, in.sinceNoCodec});
    if (!in.live || sinceAny > 2 * R) return s;   // nothing is being received: nothing to explain
    char b[256];
    auto done = [&](SilenceWhy w, const std::string& t) { s.why = w; s.text = "No audio: " + t; return s; };
    if (in.appMuted) return done(SilenceWhy::AppMuted, "the program is muted (the speaker button in the top bar).");
    if (in.appVolume < 0.005f) return done(SilenceWhy::AppVolume, "output volume 0 (the program's volume slider).");
    if (in.monVolume < 0.005f) return done(SilenceWhy::ModeVolume, "output volume 0 (the vol slider of this mode).");
    if (in.sincePlay <= R) {   // calls are being played: the sound path is what is wrong
        if (in.outputFailed) {
            s = done(SilenceWhy::OutputError, "output device error" + (in.outputError.empty() ? std::string(".") : ": " + in.outputError));
            s.canUseDefaultOutput = true;
            return s;
        }
        if (in.codecUnavailable) return done(SilenceWhy::CodecUnavailable, "the codec plug-in could not start a decoder for the call (Install codec... > Rescan, or reinstall the plug-in).");
        if (in.silentRun >= 10) {
            snprintf(b, sizeof b, "the codec plug-in decoded %u voice bursts but returned only silence (wrong plug-in build or frame format).", (unsigned)in.silentRun);
            return done(SilenceWhy::CodecSilent, b);
        }
        return s;
    }
    if (!in.codecReady || in.sinceNoCodec <= R) return done(SilenceWhy::NoCodec, "no voice codec installed. Calls are listed but not heard (Install codec...).");
    struct C { double t; SilenceWhy w; };
    const C c[5] = {{in.sinceNotHeld, SilenceWhy::Hold}, {in.sinceMuted, SilenceWhy::MuteList}, {in.sinceFiltered, SilenceWhy::AllowList},
                    {in.sinceSquelched, SilenceWhy::Squelch}, {in.sinceEncrypted, SilenceWhy::Encrypted}};
    const C* best = nullptr;
    for (const C& x : c) if (x.t <= R && (!best || x.t < best->t)) best = &x;
    if (!best) return s;
    switch (best->w) {
    case SilenceWhy::Hold:
        s = done(SilenceWhy::Hold, "talkgroup hold is set to " + in.hold + " (other talkgroups are not played).");
        s.canReleaseHold = true;
        break;
    case SilenceWhy::MuteList: s = done(SilenceWhy::MuteList, "the calls heard are on the mute list (Voice monitor > Filters)."); break;
    case SilenceWhy::AllowList: s = done(SilenceWhy::AllowList, "\"play only the allow list\" is on and these calls are not on it (Voice monitor > Filters)."); break;
    case SilenceWhy::Squelch:
        snprintf(b, sizeof b, "the squelch (%.0f %% bit errors) blocks these calls; raise it in the Voice monitor.", in.squelchPct);
        s = done(SilenceWhy::Squelch, b);
        break;
    default: s = done(SilenceWhy::Encrypted, "the calls heard are encrypted; they are listed but never decoded."); break;
    }
    return s;
}

} // namespace dect2::voice
