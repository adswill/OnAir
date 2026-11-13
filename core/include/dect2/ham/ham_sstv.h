// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Ham SSTV send: a picture from a file (PNG, BMP, PPM) or from memory, fitted to the mode's size, the callsign stamped on it, as SSTV audio
// (VIS header, then the lines) for the rig, with an optional CW identification after it. Modes: every one of hfdig_sstv.h (Robot 36 / 72,
// Martin M1 / M2, Scottie S1 / S2 / DX, PD50 .. PD180, Wraase SC2-180), the same tables the receiver uses. Received pictures come from the
// SSTV decoder (HfdigSstv). Nothing here transmits: sstvSend() goes through HamSlotTx::sendAudio (licence, Arm, amateur band) and the duty limit.
#pragma once
#include "dect2/ham/ham.h"
#include "dect2/ham/ham_kbd.h"
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::ham {

struct SstvPicture { int w = 0, h = 0; std::vector<uint8_t> rgb; };   // RGB bytes, row by row

// PNG (any type), BMP (uncompressed 24 / 32 bit), PPM (P6 / P3). False and err when the file cannot be read or is not one of them.
bool sstvLoadPicture(const std::string& path, SstvPicture& out, std::string& err);
bool sstvDecodePicture(const std::vector<uint8_t>& bytes, SstvPicture& out, std::string& err);
// Scaled to w x h keeping the aspect: `fill` crops to fill the frame, otherwise black bars (letterbox). Bilinear.
SstvPicture sstvFit(const SstvPicture& in, int w, int h, bool fill = true);
int sstvModeByName(const std::string& name);   // index into sstvModeInfo(), "Martin M1" / "scottie s1" / "PD120" ..., -1 none

struct SstvTxCfg {
    int mode = 2;                  // sstvModeInfo index (2 = Martin M1)
    std::string stamp;             // drawn on the last rows ("" none); usually the callsign
    bool fill = true;
    std::string cwId;              // "DE LY1ABC" sent in CW after the picture ("" none)
    double cwWpm = 20, cwHz = 1500;
};
// The picture as 48 kHz audio, peak 1 (VIS, the lines, 0.4 s before and a short tail; the CW ID after a pause). Empty and err when not possible.
std::vector<float> sstvRender(const SstvPicture& pic, const SstvTxCfg& c, std::string& err);
double sstvSeconds(int mode);      // on-air length of the picture alone
constexpr double kSstvBwHz = 2300 - 1100 + 100;   // the tones: 1100 (VIS) .. 2300 Hz, plus the FM sidebands at the line rate

bool sstvSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gate, const SstvPicture& pic, const SstvTxCfg& c, double level, double nowSec, std::string& err);

}  // namespace dect2::ham
