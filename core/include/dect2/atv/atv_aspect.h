// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Picture aspect ratio of analogue television (issue #504: the 16:9 video of an Icom IC-905 FM-ATV transceiver shows squeezed, a 4:3 picture stretched).
// Both frame formats of the signal have the same lines and the same active line time; 16:9 is "anamorphic": the whole 16:9 picture is squeezed into the
// 4:3 frame (the width of the active line) and the display stretches it back. So the aspect is a matter of how the picture is shown (receive) and how it is
// fitted into the frame (transmit); the wide screen signalling of EN 300 294 (625 lines, line 23) says which one a transmission is.
//
// The setting, saved per mode: 0 Auto, 1 4:3, 2 16:9.
//   Receive: Auto shows 16:9 when line 23 carries "full format 16:9 (anamorphic)", else 4:3. The letterbox formats (box 16:9, box 14:9) are 4:3 frames
//            with bars in the picture itself and are shown as 4:3.
//   Transmit: the source picture (a camera or file of its own aspect, or the 4:3 test card) is fitted into the frame of the chosen aspect without distortion:
//            letterbox bars above and below, or pillarbox bars at the sides. Auto takes the frame that matches the source (16:9 for a source of 1.55 or wider).
//            A 16:9 frame sends WSS 1110 (full format 16:9); an explicit 4:3 sends WSS 0001 (full format 4:3). 625-line systems only (the others have no WSS).
#pragma once
#include <string>

namespace dect2 {

constexpr int kAtvAspectAuto = 0, kAtvAspect43 = 1, kAtvAspect169 = 2;
constexpr int kAtvWssFull43 = 0b0001, kAtvWssFull169 = 0b1110;   // EN 300 294 table 1, b0 b1 b2 b3 (b0 the most significant bit here)
constexpr double kAtvAspectValue43 = 4.0 / 3.0, kAtvAspectValue169 = 16.0 / 9.0;

// the WSS bits (or -1: none) say the picture is an anamorphic 16:9 frame; the same from the text the receiver reports (atvWssAspectName)
bool atvWssIsWide(int wssBits);
bool atvWssTextIsWide(const std::string& wssText);

// Receive: the aspect (width / height) the picture is shown with. mode is the setting (anything else is taken as Auto).
double atvDisplayAspect(int mode, int wssBits);
double atvDisplayAspect(int mode, const std::string& wssText);

// Transmit: the frame the picture is sent in, kAtvAspect43 or kAtvAspect169. srcAspect: the source's width / height (<= 0: unknown, taken as 4:3).
int atvTxFrame(int mode, double srcAspect);
// the WSS to send for that choice, or -1 for none (Auto with a 4:3 frame sends none, as before)
int atvTxWss(int mode, int frame);

// The part of the frame (0 .. 1 in x and y) a source of aspect srcAspect takes in a frame of aspect frameAspect, centred, the whole source visible, no distortion
struct AtvFit { double x0 = 0, x1 = 1, y0 = 0, y1 = 1; bool identity() const { return x0 == 0 && x1 == 1 && y0 == 0 && y1 == 1; } };
AtvFit atvFitSource(double srcAspect, double frameAspect);

// Fits the picture src (w x h samples, the whole source stretched over them) into the frame dst (w x h samples) as `fit` says: bilinear inside the
// rectangle, black outside (Y 0, U and V 0). src and dst are three planes each; dst may not be src.
void atvFitPlanes(const float* sy, const float* su, const float* sv, int w, int h, const AtvFit& fit, float* dy, float* du, float* dv);

} // namespace dect2
