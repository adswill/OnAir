// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham keyboard modes: PSK31 .. PSK250, QPSK31 .. QPSK125, RTTY, MT63, FSQ, IFKP, Olivia, Contestia, MFSK, THOR, DominoEX, Hellschreiber and CW. The text of a transmission becomes
// audio (kbdRender, one burst at a time, 48 kHz), the macros (CQ, answer, RST, QTH, name, 73) are filled from the station settings, the callsign
// of the other station is picked from the decoded text, and the QSO is logged. Nothing here transmits: the burst goes through HamSlotTx::sendAudio,
// the same gate (licence, Arm, amateur band) as the slot modes, plus the length and duty-cycle limits below.
// Protocols: PSK31 (G3PLX, varicode in psk_varicode.h), RTTY (ITA2 Baudot, 45.45 Bd / 170 Hz), Morse (ITU-R M.1677-1, cw_morse.h); the keyboard
// families use the modulators of hfdig_kbd*.cpp.
#pragma once
#include "dect2/ham/ham.h"
#include <string>
#include <vector>

namespace dect2::ham {

// Mt63: MT63 500 / 1000 / 2000 (variant 0 .. 5: bandwidth index * 2 + long interleave), Mt63Generator; Fsq: FSQ (variant = speed 0 .. 4) and IFKP
// (variant 10 + speed 0 .. 2), Kbd33Generator. FSQ text goes out as it is given: kbdFsqLine() makes the callsign header of a line.
enum class KbdKind { Psk, Rtty, Hfdig, Cw, Mt63, Fsq };

struct KbdPreset {
    const char* id;          // "psk31"
    const char* name;        // "PSK31"
    KbdKind kind;
    double baud;             // Psk 31.25 / 62.5, Rtty 45.45
    double shiftHz;          // Rtty
    int family, tones, bw, variant;   // Hfdig: KbdFamily (hfdig_kbd.h), Olivia / Contestia tones and bandwidth, the variant of the other families
    const char* adifMode;    // ADIF MODE ("PSK") and SUBMODE ("PSK31"; "" none)
    const char* adifSub;
    int rxFamily;            // the mode list entry that receives it (selectMode)
    const char* rxSub;       // its subId, nullptr: by family alone
    int pskMode;             // Psk: the receiver's mode (0 BPSK31, 1 BPSK63)
    double defaultHz;        // audio frequency of the signal's middle
};
const std::vector<KbdPreset>& kbdPresets();
const KbdPreset* kbdPresetById(const std::string& id);
double kbdBwHz(const KbdPreset& p);                    // occupied bandwidth for the TX gate

constexpr double kKbdMaxBurstSec = 3600;               // only the renderer's memory bound (an hour of audio), not a transmit limit
constexpr size_t kKbdMaxChars = 600;                   // nor longer than this many characters
constexpr int kKbdRate = 48000;

// The text as one burst of audio at kKbdRate, peak 1 (the caller sets the level). cwWpm: CW speed. An empty result and err say why not
// (nothing to send, too long, a character no mode can send is simply left out).
std::vector<float> kbdRender(const KbdPreset& p, const std::string& text, double centreHz, double cwWpm, std::string& err);
double kbdSeconds(const std::vector<float>& audio);   // its length

// Per-station limit on how much of the time the transmitter may be on: at most maxFraction of the last windowSec seconds.
class TxDuty {
public:
    explicit TxDuty(double windowSec = 600, double maxFraction = 0.5) : win_(windowSec), frac_(maxFraction) {}
    bool allow(double nowSec, double durSec) const;    // would a burst of durSec starting now keep the limit?
    void add(double nowSec, double durSec);             // a burst was sent
    double used(double nowSec) const;                   // seconds on air in the window
private:
    double win_, frac_;
    std::vector<std::pair<double, double>> b_;          // start, length
};

// One keyboard burst through the transmitter: gate (inside sendAudio), length, duty cycle. True only when the whole burst was played.
bool kbdSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gate, const KbdPreset& p, const std::string& text, double centreHz,
             double level, double cwWpm, double nowSec, std::string& err);

// FSQ / IFKP line as stations send it: " \n" + MYCALL + ":" + CRC of the call + body (to a station: "TOCALL " in front of the body)
std::string kbdFsqLine(const std::string& myCall, const std::string& toCall, const std::string& body);

// ---- macros ----
struct KbdStation {
    std::string myCall, myGrid, myName, myQth;
    std::string call, name, rst = "599";    // the station being worked
};
struct KbdMacro { const char* name; const char* text; };
const std::vector<KbdMacro>& kbdDefaultMacros();       // CQ, Answer, RST, QTH, Name, 73
// Fills {MYCALL} {MYGRID} {MYNAME} {MYQTH} {CALL} {NAME} {RST}. False when one the template uses is empty (the text is still returned, with that field left out).
bool kbdExpand(const std::string& tmpl, const KbdStation& s, std::string& out, std::string* missing = nullptr);

// ---- the decoded text ----
std::vector<std::string> kbdCallsInText(const std::string& text, const std::string& myCall);   // callsigns in it, first seen first, not your own
// The ADIF record of a keyboard QSO (mode name and submode of the preset).
QsoRecord kbdLogRecord(const KbdPreset& p, const KbdStation& s, double freqHz, double startUtc, double endUtc, const std::string& rstSent, const std::string& rstRcvd);

}  // namespace dect2::ham
