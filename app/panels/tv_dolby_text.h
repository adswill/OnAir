// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The translated words for Dolby TV sound (dolby_audio.h): the format of a track (Dolby Atmos in E-AC-3 JOC or AC-4, the channel bed),
// what goes to the sound device (stereo, 5.1, 7.1, Dolby Surround matrix, bitstream passthrough) and why the Atmos objects are not heard.
// Used by every panel that shows the TV player: DVB-T/T2, ATSC 1.0, ISDB-T, DTMB, DVB-S (tv.cpp), QO-100 (qo100_ui.cpp), IPTV.
#pragma once
#include "dect2/tv/player.h"
#include "../i18n/i18n.h"
#include <string>

namespace dolbytext {

// "Dolby Atmos (E-AC-3 JOC), 5.1 bed" / "Dolby Digital Plus (E-AC-3) 5.1" / "" (not Dolby)
inline std::string trackFormat(const std::string& codec, const std::string& bed, int atmos) {
    std::string r;
    if (atmos == 1) r = tr("Dolby Atmos (E-AC-3 JOC)");
    else if (codec == "eac3") r = tr("Dolby Digital Plus (E-AC-3)");
    else if (codec == "ac3") r = tr("Dolby Digital (AC-3)");
    else return r;
    if (!bed.empty()) { r += atmos ? ", " : " "; r += bed; if (atmos) { r += " "; r += tr("bed"); } }
    return r;
}

inline std::string trackFormat(const dect2::AudioTrackInfo& t) { return trackFormat(t.codec, t.bed, t.atmos); }

// what plays: "5.1 to the speakers", "stereo downmix", "bitstream to the receiver (experimental)"
inline std::string output(const dect2::PlayerStats& ps) {
    if (ps.passthrough) return tr("bitstream to the receiver (experimental)");
    if (ps.audioOutChannels == 8) return tr("7.1 to the speakers");
    if (ps.audioOutChannels == 6) return tr("5.1 to the speakers");
    if (ps.audioOutChannels == 2 && ps.audioChannels > 2) return ps.matrixStereo ? tr("stereo, Dolby Surround matrix") : tr("stereo downmix");
    return std::string();
}

// One line for the media info, empty when the sound is not Dolby: the format, then what plays
inline std::string line(const dect2::PlayerStats& ps) {
    const std::string f = trackFormat(ps.audioCodec, ps.audioBed, ps.atmos);
    if (f.empty()) return f;
    const std::string o = output(ps);
    return o.empty() ? f : f + "  ->  " + o;
}

// Why the Atmos height and object sound is not heard, when it is not passed through (empty otherwise)
inline const char* atmosNote(const dect2::PlayerStats& ps) {
    if (ps.atmos != 1 || ps.passthrough) return "";
    return tr("Dolby Atmos: OnAir plays the channel bed; the objects need Dolby's renderer. A receiver or soundbar renders them: Settings > Audio > Pass Dolby audio through.");
}

} // namespace dolbytext
