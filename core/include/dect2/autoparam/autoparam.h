// Shared "Auto" detectors: what can be read from the signal itself, so a mode need not ask the user. Everything here is pure analysis of a stretch of
// audio; the decoders call it from a worker thread and apply the result (hfdig_rtty.cpp, hfdig_kbd.cpp).
//   findFskPairs     two spectral lines a known shift apart (the tones of an FSK signal)
//   measureFskBaud   the symbol rate of an FSK pair from the run lengths of the keying
//   analyseToneComb  occupied band and tone spacing of a multi-tone signal
//   readableFraction how much of a decoded string reads as text
// Results are verified by the decoder itself (a trial decode) before a mode trusts them: these only choose what is worth trying.
#pragma once
#include <cstddef>
#include <string>
#include <vector>

namespace dect2::autoparam {

struct FskPair {
    double lowHz = 0, shiftHz = 0;
    int shiftIdx = 0;
    double score = 0;    // weaker line against the floor
};
std::vector<FskPair> findFskPairs(const float* x, size_t n, double fs, const double* shifts, int nShifts, int maxPairs, double tolHz = 10);

struct FskBaud {
    int best = -1, second = -1;    // indices into the caller's table
    double confidence = 0;         // 0 .. 1: margin of the best one over the second
};
FskBaud measureFskBaud(const float* x, size_t n, double fs, double lowHz, double shiftHz, const double* bauds, int nBauds);

struct ToneComb {
    bool found = false;
    double loHz = 0, hiHz = 0;     // edges of the occupied band
    double occupiedHz = 0, centreHz = 0;
    double spacingHz = 0;          // pitch of the tones, 0 when no comb stands out
    double snrDb = 0;              // of the strongest stretch against the floor
};
ToneComb analyseToneComb(const float* x, size_t n, double fs);

double readableFraction(const std::string& text);

}  // namespace dect2::autoparam
