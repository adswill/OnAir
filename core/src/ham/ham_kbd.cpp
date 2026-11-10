// Ham keyboard modes: text to audio for PSK31 / PSK63, RTTY, CW and the hfdig keyboard families, the transmit limits and the macros.
#include "dect2/ham/ham_kbd.h"
#include "dect2/cw/cw_morse.h"
#include "dect2/psk/psk_varicode.h"
#include "dect2/psk/psk_tel.h"
#include <complex>
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/mt63/mt63_gen.h"
#include "dect2/mt63/mt63_proto.h"
#include "dect2/fsq/kbd33.h"
#include "dect2/fsq/kbd33_gen.h"
#include "../hfdig/hfdig_kbd_int.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace dect2::ham {

namespace {
constexpr double kPi = 3.14159265358979323846;
std::string upper(std::string s) { for (auto& c : s) c = (char)toupper((unsigned char)c); return s; }
}

const std::vector<KbdPreset>& kbdPresets() {
    //                        id             name           kind              baud   shift  fam  tones bw    var  ADIF mode / submode          rx family, sub   psk  Hz
    static const std::vector<KbdPreset> v = {
        {"psk31",        "PSK31",        KbdKind::Psk,   31.25, 0,   0,   0,  0,    0, "PSK",      "PSK31",           36, nullptr,  0, 1000},
        {"psk63",        "PSK63",        KbdKind::Psk,   62.5,  0,   0,   0,  0,    0, "PSK",      "PSK63",           36, nullptr,  1, 1000},
        {"psk125",       "PSK125",       KbdKind::Psk,   125,   0,   0,   0,  0,    0, "PSK",      "PSK125",          36, nullptr,  4, 1000},
        {"psk250",       "PSK250",       KbdKind::Psk,   250,   0,   0,   0,  0,    0, "PSK",      "PSK250",          36, nullptr,  5, 1000},
        {"qpsk31",       "QPSK31",       KbdKind::Psk,   31.25, 0,   0,   0,  0,    0, "PSK",      "QPSK31",          36, nullptr,  2, 1000},
        {"qpsk63",       "QPSK63",       KbdKind::Psk,   62.5,  0,   0,   0,  0,    0, "PSK",      "QPSK63",          36, nullptr,  3, 1000},
        {"qpsk125",      "QPSK125",      KbdKind::Psk,   125,   0,   0,   0,  0,    0, "PSK",      "QPSK125",         36, nullptr,  6, 1000},
        {"rtty",         "RTTY",         KbdKind::Rtty,  45.45, 170, 0,   0,  0,    0, "RTTY",     "",                25, "rtty",   0, 2210},
        {"cw",           "CW",           KbdKind::Cw,    0,     0,   0,   0,  0,    0, "CW",       "",                34, nullptr,  0, 700},
        {"olivia-4-125", "Olivia 4/125", KbdKind::Hfdig, 0,     0,   KbdOlivia,    4,  125,  0, "OLIVIA",   "OLIVIA 4/125",    25, "olivia", 0, 1500},
        {"olivia-4-250", "Olivia 4/250", KbdKind::Hfdig, 0,     0,   KbdOlivia,    4,  250,  0, "OLIVIA",   "OLIVIA 4/250",    25, "olivia", 0, 1500},
        {"olivia-8-250", "Olivia 8/250", KbdKind::Hfdig, 0,     0,   KbdOlivia,    8,  250,  0, "OLIVIA",   "OLIVIA 8/250",    25, "olivia", 0, 1500},
        {"olivia-8-500", "Olivia 8/500", KbdKind::Hfdig, 0,     0,   KbdOlivia,    8,  500,  0, "OLIVIA",   "OLIVIA 8/500",    25, "olivia", 0, 1500},
        {"olivia-16-1000","Olivia 16/1000",KbdKind::Hfdig,0,    0,   KbdOlivia,    16, 1000, 0, "OLIVIA",   "OLIVIA 16/1000",  25, "olivia", 0, 1500},
        {"olivia-16-500","Olivia 16/500",KbdKind::Hfdig, 0,     0,   KbdOlivia,    16, 500,  0, "OLIVIA",   "OLIVIA 16/500",   25, "olivia", 0, 1500},
        {"olivia-32-1000","Olivia 32/1000",KbdKind::Hfdig,0,    0,   KbdOlivia,    32, 1000, 0, "OLIVIA",   "OLIVIA 32/1000",  25, "olivia", 0, 1500},
        {"olivia-64-2000","Olivia 64/2000",KbdKind::Hfdig,0,    0,   KbdOlivia,    64, 2000, 0, "OLIVIA",   "OLIVIA 64/2000",  25, "olivia", 0, 1500},
        {"contestia-4-125","Contestia 4/125",KbdKind::Hfdig,0, 0,   KbdContestia,  4,  125,  0, "CONTESTI", "CONTESTI 4/125",  25, "contestia", 0, 1500},
        {"contestia-4-250","Contestia 4/250",KbdKind::Hfdig,0, 0,   KbdContestia,  4,  250,  0, "CONTESTI", "CONTESTI 4/250",  25, "contestia", 0, 1500},
        {"contestia-8-250","Contestia 8/250",KbdKind::Hfdig,0, 0,   KbdContestia,  8,  250,  0, "CONTESTI", "CONTESTI 8/250",  25, "contestia", 0, 1500},
        {"contestia-16-500","Contestia 16/500",KbdKind::Hfdig,0,0,  KbdContestia,  16, 500,  0, "CONTESTI", "CONTESTI 16/500", 25, "contestia", 0, 1500},
        {"contestia-8-500","Contestia 8/500",KbdKind::Hfdig,0, 0,   KbdContestia,  8,  500,  0, "CONTESTI", "CONTESTI 8/500",  25, "contestia", 0, 1500},
        {"contestia-16-1000","Contestia 16/1000",KbdKind::Hfdig,0,0,KbdContestia,  16, 1000, 0, "CONTESTI", "CONTESTI 16/1000",25, "contestia", 0, 1500},
        {"contestia-32-1000","Contestia 32/1000",KbdKind::Hfdig,0,0,KbdContestia,  32, 1000, 0, "CONTESTI", "CONTESTI 32/1000",25, "contestia", 0, 1500},
        // MFSK4 .. MFSK128 (kMfskNames order; the L modes are in the HF digital decoder only), THOR 4 .. THOR 100, DominoEX 4 .. 22, the Hell variants; submodes from the ADIF 3.1 list
        {"mfsk4", "MFSK4", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 0, "MFSK", "MFSK4", 25, "mfsk", 0, 1500},
        {"mfsk8", "MFSK8", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 1, "MFSK", "MFSK8", 25, "mfsk", 0, 1500},
        {"mfsk11", "MFSK11", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 2, "MFSK", "MFSK11", 25, "mfsk", 0, 1500},
        {"mfsk16", "MFSK16", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 3, "MFSK", "MFSK16", 25, "mfsk", 0, 1500},
        {"mfsk22", "MFSK22", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 4, "MFSK", "MFSK22", 25, "mfsk", 0, 1500},
        {"mfsk31", "MFSK31", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 5, "MFSK", "MFSK31", 25, "mfsk", 0, 1500},
        {"mfsk32", "MFSK32", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 6, "MFSK", "MFSK32", 25, "mfsk", 0, 1500},
        {"mfsk64", "MFSK64", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 7, "MFSK", "MFSK64", 25, "mfsk", 0, 1500},
        {"mfsk128", "MFSK128", KbdKind::Hfdig, 0, 0, KbdMfsk, 0, 0, 8, "MFSK", "MFSK128", 25, "mfsk", 0, 1500},
        {"thor4", "THOR 4", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 0, "THOR", "THOR4", 25, "thor", 0, 1500},
        {"thor5", "THOR 5", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 1, "THOR", "THOR5", 25, "thor", 0, 1500},
        {"thor8", "THOR 8", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 2, "THOR", "THOR8", 25, "thor", 0, 1500},
        {"thor11", "THOR 11", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 3, "THOR", "THOR11", 25, "thor", 0, 1500},
        {"thor16", "THOR 16", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 4, "THOR", "THOR16", 25, "thor", 0, 1500},
        {"thor22", "THOR 22", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 5, "THOR", "THOR22", 25, "thor", 0, 1500},
        {"thor25x4", "THOR 25x4", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 6, "THOR", "THOR25X4", 25, "thor", 0, 1500},
        {"thor50x1", "THOR 50x1", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 7, "THOR", "THOR50X1", 25, "thor", 0, 1500},
        {"thor50x2", "THOR 50x2", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 8, "THOR", "THOR50X2", 25, "thor", 0, 1500},
        {"thor100", "THOR 100", KbdKind::Hfdig, 0, 0, KbdThor, 0, 0, 9, "THOR", "THOR100", 25, "thor", 0, 1500},
        {"dominoex4", "DominoEX 4", KbdKind::Hfdig, 0, 0, KbdDominoex, 0, 0, 0, "DOMINO", "DOMINO4", 25, "dominoex", 0, 1500},
        {"dominoex5", "DominoEX 5", KbdKind::Hfdig, 0, 0, KbdDominoex, 0, 0, 1, "DOMINO", "DOMINO5", 25, "dominoex", 0, 1500},
        {"dominoex8", "DominoEX 8", KbdKind::Hfdig, 0, 0, KbdDominoex, 0, 0, 2, "DOMINO", "DOMINO8", 25, "dominoex", 0, 1500},
        {"dominoex11", "DominoEX 11", KbdKind::Hfdig, 0, 0, KbdDominoex, 0, 0, 3, "DOMINO", "DOMINO11", 25, "dominoex", 0, 1500},
        {"dominoex16", "DominoEX 16", KbdKind::Hfdig, 0, 0, KbdDominoex, 0, 0, 4, "DOMINO", "DOMINO16", 25, "dominoex", 0, 1500},
        {"dominoex22", "DominoEX 22", KbdKind::Hfdig, 0, 0, KbdDominoex, 0, 0, 5, "DOMINO", "DOMINO22", 25, "dominoex", 0, 1500},
        {"feldhell", "Feld Hell", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 0, "HELL", "FELDHELL", 25, "hell", 0, 1500},
        {"slowhell", "Slow Hell", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 1, "HELL", "SLOWHELL", 25, "hell", 0, 1500},
        {"hell80", "Hell 80", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 2, "HELL", "HELL80", 25, "hell", 0, 1500},
        {"hellx5", "HellX5", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 3, "HELL", "HELLX5", 25, "hell", 0, 1500},
        {"hellx9", "HellX9", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 4, "HELL", "HELLX9", 25, "hell", 0, 1500},
        {"fskhell245", "FSK Hell 245", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 5, "HELL", "FSKHELL", 25, "hell", 0, 1500},
        {"fskhell105", "FSK Hell 105", KbdKind::Hfdig, 0, 0, KbdHell, 0, 0, 6, "HELL", "HFSK", 25, "hell", 0, 1500},
        // MT63 (centre: the lower edge at 500 Hz audio), FSQ and IFKP (ADIF: FSQ is MFSK / FSQCALL; IFKP has no ADIF submode)
        {"mt63-500s", "MT63-500S", KbdKind::Mt63, 0, 0, 0, 0, 500, 0, "MT63", "", 95, nullptr, 0, 750},
        {"mt63-500l", "MT63-500L", KbdKind::Mt63, 0, 0, 0, 0, 500, 1, "MT63", "", 95, nullptr, 0, 750},
        {"mt63-1000s", "MT63-1000S", KbdKind::Mt63, 0, 0, 0, 0, 1000, 2, "MT63", "", 95, nullptr, 0, 1000},
        {"mt63-1000l", "MT63-1000L", KbdKind::Mt63, 0, 0, 0, 0, 1000, 3, "MT63", "", 95, nullptr, 0, 1000},
        {"mt63-2000s", "MT63-2000S", KbdKind::Mt63, 0, 0, 0, 0, 2000, 4, "MT63", "", 95, nullptr, 0, 2000},
        {"mt63-2000l", "MT63-2000L", KbdKind::Mt63, 0, 0, 0, 0, 2000, 5, "MT63", "", 95, nullptr, 0, 2000},
        {"fsq15", "FSQ 1.5", KbdKind::Fsq, 0, 0, 0, 0, 0, 0, "MFSK", "FSQCALL", 113, nullptr, 0, 1500},
        {"fsq2", "FSQ 2", KbdKind::Fsq, 0, 0, 0, 0, 0, 1, "MFSK", "FSQCALL", 113, nullptr, 0, 1500},
        {"fsq3", "FSQ 3", KbdKind::Fsq, 0, 0, 0, 0, 0, 2, "MFSK", "FSQCALL", 113, nullptr, 0, 1500},
        {"fsq45", "FSQ 4.5", KbdKind::Fsq, 0, 0, 0, 0, 0, 3, "MFSK", "FSQCALL", 113, nullptr, 0, 1500},
        {"fsq6", "FSQ 6", KbdKind::Fsq, 0, 0, 0, 0, 0, 4, "MFSK", "FSQCALL", 113, nullptr, 0, 1500},
        {"ifkp05", "IFKP 0.5", KbdKind::Fsq, 0, 0, 0, 0, 0, 10, "MFSK", "", 116, nullptr, 0, 1500},
        {"ifkp10", "IFKP 1.0", KbdKind::Fsq, 0, 0, 0, 0, 0, 11, "MFSK", "", 116, nullptr, 0, 1500},
        {"ifkp20", "IFKP 2.0", KbdKind::Fsq, 0, 0, 0, 0, 0, 12, "MFSK", "", 116, nullptr, 0, 1500},
    };
    return v;
}
const KbdPreset* kbdPresetById(const std::string& id) {
    for (const auto& p : kbdPresets()) if (id == p.id) return &p;
    return nullptr;
}

static KbdConfig hfdigConfig(const KbdPreset& p, double centreHz) {
    KbdConfig k;
    k.family = p.family;
    k.tones = p.tones ? p.tones : 32;
    k.bandwidth = p.bw ? p.bw : 1000;
    k.variant = p.variant;
    k.centreHz = centreHz;
    k.autoCentre = false;
    return k;
}

double kbdBwHz(const KbdPreset& p) {
    switch (p.kind) {
    case KbdKind::Psk: return 2 * pskBaud(p.pskMode);
    case KbdKind::Rtty: return p.shiftHz + 1.5 * p.baud + 10;
    case KbdKind::Cw: return 200;
    case KbdKind::Mt63: return p.bw;
    case KbdKind::Fsq: return kbd33::specOf(p.variant >= 10, p.variant % 10).bandHz();
    default: return kbdBandwidthHz(hfdigConfig(p, 1500));
    }
}

double kbdSeconds(const std::vector<float>& a) { return (double)a.size() / kKbdRate; }

namespace {

// 10 ms fades at both ends (no key clicks), 0.15 s of silence before (PTT to audio) and 0.1 s after
void finish(std::vector<float>& a) {
    const size_t f = std::min<size_t>(a.size() / 2, kKbdRate / 100);
    for (size_t i = 0; i < f; i++) {
        const float g = 0.5f - 0.5f * (float)std::cos(kPi * (double)i / (double)f);
        a[i] *= g;
        a[a.size() - 1 - i] *= g;
    }
    std::vector<float> o((size_t)(0.15 * kKbdRate), 0.f);
    o.insert(o.end(), a.begin(), a.end());
    o.insert(o.end(), (size_t)(0.1 * kKbdRate), 0.f);
    a.swap(o);
}

// ---- PSK31 .. PSK250 and QPSK31 .. QPSK125 (G3PLX): BPSK, a "0" is a phase reversal; QPSK, each bit through the K = 5 code and fldigi's phase
// mapping (psk_varicode.h); the carrier amplitude follows half a cosine across each change; idle reversals before and after ----
std::vector<float> renderPsk(const KbdPreset& p, const std::string& text, double hz) {
    std::string t;
    for (char c : text) if ((unsigned char)c < 128) t += c;
    const bool qpsk = pskQpsk(p.pskMode);
    const double baud = pskBaud(p.pskMode);
    const int idle = std::max(32, (int)std::lround(baud));   // a second of reversals (32 symbols at least): the receiver's AFC and clock
    std::vector<uint8_t> bits((size_t)idle, 0);
    const auto d = psk::varicodeBits(t);
    bits.insert(bits.end(), d.begin(), d.end());
    bits.insert(bits.end(), (size_t)std::max(qpsk ? 48 : 16, idle / 2), 0);   // postamble (QPSK: longer than the receiver's 40 symbol Viterbi traceback)
    std::vector<int> turns;                                     // eighth turns per symbol
    if (psk8psk(p.pskMode)) {
        std::string u;
        for (char c : text) u += c;                             // 8PSK: the MFSK varicode has all 256 codes
        turns = psk::psk8Steps(u, idle, std::max(16, idle / 2));
    } else if (qpsk) {
        psk::ConvEncoder enc;
        for (int k = 0; k < 8; k++) bits.push_back(0);          // flush the code
        for (uint8_t b : bits) turns.push_back(2 * psk::qpskTurns(enc.encode(b)));
    } else {
        for (uint8_t b : bits) turns.push_back(b ? 0 : 4);
    }
    const double N = kKbdRate / baud;
    std::vector<float> a;
    a.reserve((size_t)((double)turns.size() * N) + 8);
    std::complex<double> prev(1, 0);
    const double w = 2 * kPi * hz / kKbdRate;
    double ph = 0, tEnd = 0;
    for (int tq : turns) {
        const std::complex<double> cur = prev * std::polar(1.0, tq * kPi / 4);
        const size_t s0 = a.size();
        tEnd += N;
        const size_t n = (size_t)std::llround(tEnd) - s0;
        for (size_t k = 0; k < n; k++) {
            const double sh = 0.5 + 0.5 * std::cos(kPi * (double)k / (double)n);   // weight of the previous symbol across the interval
            const std::complex<double> v = (prev * sh + cur * (1 - sh)) * std::polar(1.0, ph);
            a.push_back((float)v.real());
            ph += w;
            if (ph > 2 * kPi) ph -= 2 * kPi;
        }
        prev = cur;
    }
    return a;
}

// ---- RTTY: ITA2 Baudot, 5 data bits LSB first, 1 start bit, 1.5 stop bits, phase continuous FSK; mark is the lower tone ----
std::vector<float> renderRtty(const KbdPreset& p, const std::string& text, double centre) {
    static const char kL[] = "\0E\nA SIU\rDRJNFCKTZLWHYPQOBG\0MXV\0";   // letters; 27 is FIGS, 31 is LTRS
    static const char kF[] = {0, '3', '\n', '-', ' ', '\'', '8', '7', '\r', '$', '4', 0, ',', '!', ':', '(', '5', '"', ')', '2', '#', '6', '0', '1', '9', '?', '&', 0, '.', '/', ';', 0};
    std::vector<int> codes;
    bool figs = false;
    codes.push_back(31);
    for (char ch0 : upper(text)) {
        if (ch0 == '\n') { codes.push_back(8); codes.push_back(2); continue; }
        int code = -1;
        bool needFigs = false;
        for (int k = 0; k < 32 && code < 0; k++) if (kL[k] == ch0 && ch0 != 0 && k != 27 && k != 31) code = k;
        if (code < 0) { for (int k = 0; k < 32 && code < 0; k++) if (kF[k] == ch0 && ch0 != 0) code = k; needFigs = true; }
        if (code < 0) continue;
        if (code != 4 && code != 8 && code != 2 && needFigs != figs) { codes.push_back(needFigs ? 27 : 31); figs = needFigs; }
        codes.push_back(code);
    }
    codes.push_back(31);
    struct Seg { bool mark; double bits; };
    std::vector<Seg> segs;
    segs.push_back({true, 20});
    for (int c : codes) {
        segs.push_back({false, 1});
        for (int b = 0; b < 5; b++) segs.push_back({((c >> b) & 1) != 0, 1});
        segs.push_back({true, 1.5});
    }
    segs.push_back({true, 10});
    const double lo = centre - p.shiftHz / 2, hi = centre + p.shiftHz / 2;
    const double spb = kKbdRate / p.baud;
    std::vector<float> a;
    double ph = 0;
    for (const Seg& s : segs) {
        const size_t n = (size_t)std::llround(s.bits * spb);
        const double w = 2 * kPi * (s.mark ? lo : hi) / kKbdRate;
        for (size_t i = 0; i < n; i++) {
            a.push_back((float)std::sin(ph));
            ph += w;
            if (ph > 2 * kPi) ph -= 2 * kPi;
        }
    }
    return a;
}

// ---- CW: raised-cosine keyed tone (5 ms edges), the usual 1 : 3 : 7 timing, prosigns as <AR> ----
std::vector<float> renderCw(const std::string& text, double hz, double wpm) {
    wpm = std::max(5.0, std::min(60.0, wpm));
    const double dot = 1.2 / wpm;
    const size_t ed = (size_t)(0.005 * kKbdRate);
    std::vector<float> a;
    double ph = 0;
    const double w = 2 * kPi * hz / kKbdRate;
    auto tone = [&](double sec) {
        const size_t n = (size_t)std::llround(sec * kKbdRate);
        for (size_t i = 0; i < n; i++) {
            double g = 1;
            if (i < ed) g = 0.5 - 0.5 * std::cos(kPi * (double)i / (double)ed);
            else if (n - 1 - i < ed) g = 0.5 - 0.5 * std::cos(kPi * (double)(n - 1 - i) / (double)ed);
            a.push_back((float)(g * std::sin(ph)));
            ph += w;
            if (ph > 2 * kPi) ph -= 2 * kPi;
        }
    };
    auto gap = [&](double sec) { a.insert(a.end(), (size_t)std::llround(sec * kKbdRate), 0.f); };
    bool first = true;
    for (const std::string& tok : cw::tokenize(text)) {
        if (tok == " ") { gap(4 * dot); continue; }   // a word space is 7 dots: 3 after the character and these 4
        const std::string code = cw::encodeSymbol(tok);
        if (code.empty()) continue;
        if (!first) gap(0);
        first = false;
        for (size_t i = 0; i < code.size(); i++) {
            tone(code[i] == '-' ? 3 * dot : dot);
            gap(i + 1 < code.size() ? dot : 3 * dot);
        }
    }
    return a;
}

// ---- the keyboard families of hfdig: their modulators make 8 kHz audio; interpolated to 48 kHz ----
std::vector<float> renderHfdig(const KbdPreset& p, const std::string& text, double hz) {
    const KbdConfig k = hfdigConfig(p, hz);
    std::unique_ptr<kbd::Modulator> mod;
    switch (k.family) {
    case KbdOlivia: case KbdContestia: mod = kbd::makeOliviaMod(k, hz); break;
    case KbdMfsk: mod = kbd::makeMfskMod(k, hz); break;
    case KbdThor: case KbdDominoex: mod = kbd::makeIfkMod(k, hz); break;
    default: mod = kbd::makeHellMod(k, hz); break;
    }
    if (!mod) return {};
    kbd::TextSrc src;
    // the characters after the text that push the last one out of the receiver: MFSK and THOR hold it in the interleaver (4 x 4 x depth bits, depth 10,
    // 50 for THOR 25x4 .. 100) and the Viterbi decoder; the 11025 Hz modes also in the resampler (THOR 22 lost its last character with 16)
    const int depth = k.family == KbdThor && k.variant >= 6 ? 50 : 10;
    const uint64_t want = text.size() + (k.family == KbdMfsk ? 40 : k.family == KbdThor ? 16 + 3 * depth : 16);
    src.text = text + std::string((size_t)(want - text.size()) + 64, ' ');   // the padding fills the last block; the text never wraps
    std::vector<float> raw((size_t)(0.1 * kbd::kFs), 0.f);
    while (src.sent < want && raw.size() < (size_t)(kKbdMaxBurstSec * 2 * kbd::kFs)) mod->next(raw, src);
    // 8 kHz -> 48 kHz: windowed sinc, 6 phases
    static std::vector<double> h;
    constexpr int N = 48;
    if (h.empty()) {
        for (int i = -N; i <= N; i++) {
            const double x = kPi * i * 2 * 3600.0 / kKbdRate;
            h.push_back((i == 0 ? 1.0 : std::sin(x) / x) * (0.5 + 0.5 * std::cos(kPi * i / (N + 1))) * 2 * 3600.0 / kKbdRate * 6);
        }
    }
    std::vector<float> o(raw.size() * 6, 0.f);
    for (size_t n = 0; n < o.size(); n++) {
        double acc = 0;
        for (int i = -N; i <= N; i++) {
            const long m = (long)n - i;
            if (m < 0 || m % 6) continue;
            const size_t j = (size_t)(m / 6);
            if (j < raw.size()) acc += h[(size_t)(i + N)] * raw[j];
        }
        o[n] = (float)acc;
    }
    return o;
}

// ---- MT63, FSQ, IFKP: the repo's generators (16 kHz inside, interpolated) at 48 kHz without noise; the real part is the USB audio. The text is sent once;
// the burst ends one second after the generator falls silent (FSQ, IFKP: the carrier is off between texts), or after maxSec (MT63 keeps sending
// idle characters) ----
template <class Gen, class Cfg>
std::vector<float> renderGen(Cfg cfg, double maxSec = kKbdMaxBurstSec + 5) {
    cfg.rate = kKbdRate;
    cfg.dialOffsetHz = 0;
    cfg.noiseSigma = 0;
    Gen g(cfg);
    std::vector<cf32> b(kKbdRate / 10);
    std::vector<float> a;
    size_t lastOn = 0;
    bool seen = false;
    while (a.size() < (size_t)(maxSec * kKbdRate)) {
        g.generate(b.data(), b.size());
        for (const cf32& z : b) {
            a.push_back(z.real());
            if (std::fabs(z.real()) > 1e-6f || std::fabs(z.imag()) > 1e-6f) { lastOn = a.size(); seen = true; }
        }
        if (seen && a.size() > lastOn + (size_t)kKbdRate) break;
    }
    if (!seen) return {};
    a.resize(std::min(a.size(), lastOn + (size_t)(0.05 * kKbdRate)));
    return a;
}
std::vector<float> renderMt63(const KbdPreset& p, const std::string& text, double hz) {
    Mt63GenConfig c;
    Mt63GenSignal s;
    s.bandwidth = p.bw; s.longInterleave = (p.variant & 1) != 0; s.centreHz = hz; s.text = text; s.repeat = false; s.snrDb = 20;
    // a preamble of idle characters as long as the interleaver and a little more, 5.5 s at least: the receiver finds the band, the symbol clock and the
    // interleaver phase in it (MT63-2000 has a short interleaver in time but the receiver's band search needs a few seconds)
    s.startSec = std::max(5.5, ((s.longInterleave ? mt63::kLongIntl : mt63::kShortIntl) + 16) / mt63::charsPerSec(p.bw));
    c.signals.push_back(s);
    // the text, then the interleaver's depth (32 or 64 symbols, one character each) of idle characters and a few more for the receiver's character sync
    const double sec = s.startSec + ((double)text.size() + (s.longInterleave ? mt63::kLongIntl : mt63::kShortIntl) + 12) / mt63::charsPerSec(p.bw);
    return renderGen<Mt63Generator>(c, sec);
}
std::vector<float> renderFsq(const KbdPreset& p, const std::string& text, double hz) {
    Kbd33GenConfig c;
    Kbd33GenSignal s;
    s.ifkp = p.variant >= 10; s.speed = p.variant % 10; s.centreHz = hz; s.text = text; s.repeat = false; s.startSec = 0.1; s.snrDb = 20;
    c.signals.push_back(s);
    return renderGen<Kbd33Generator>(c);
}

}  // namespace

std::string kbdFsqLine(const std::string& myCall, const std::string& toCall, const std::string& body) {
    const std::string to = upper(toCall);
    return fsqFrame(upper(myCall), to.empty() ? body : to + " " + body, !to.empty());
}

std::vector<float> kbdRender(const KbdPreset& p, const std::string& text, double centreHz, double cwWpm, std::string& err) {
    err.clear();
    if (text.empty()) { err = "nothing to send"; return {}; }
    if (text.size() > kKbdMaxChars) { err = "the text is too long for one transmission (" + std::to_string(kKbdMaxChars) + " characters at most)"; return {}; }
    std::vector<float> a;
    switch (p.kind) {
    case KbdKind::Psk: a = renderPsk(p, text, centreHz); break;
    case KbdKind::Rtty: a = renderRtty(p, text, centreHz); break;
    case KbdKind::Cw: a = renderCw(text, centreHz, cwWpm); break;
    case KbdKind::Mt63: a = renderMt63(p, text, centreHz); break;
    case KbdKind::Fsq: a = renderFsq(p, text, centreHz); break;
    default: a = renderHfdig(p, text, centreHz); break;
    }
    if (a.empty()) { err = "nothing in the text can be sent in this mode"; return {}; }
    float pk = 0;
    for (float v : a) pk = std::max(pk, std::fabs(v));
    if (pk <= 0) { err = "nothing in the text can be sent in this mode"; return {}; }
    for (auto& v : a) v /= pk;
    finish(a);
    return a;
}

// ---- limits ----
double TxDuty::used(double now) const {
    double u = 0;
    for (const auto& b : b_) {
        const double s = std::max(b.first, now - win_), e = std::min(b.first + b.second, now);
        if (e > s) u += e - s;
    }
    return u;
}
bool TxDuty::allow(double now, double dur) const { return used(now) + dur <= frac_ * win_; }
void TxDuty::add(double now, double dur) {
    b_.erase(std::remove_if(b_.begin(), b_.end(), [&](const std::pair<double, double>& x) { return x.first + x.second < now - win_; }), b_.end());
    b_.push_back({now, dur});
}

bool kbdSend(HamSlotTx& tx, TxDuty& duty, const HamTxCheck& gateIn, const KbdPreset& p, const std::string& text, double centreHz,
             double level, double cwWpm, double nowSec, std::string& err) {
    std::vector<float> a = kbdRender(p, text, centreHz, cwWpm, err);
    if (a.empty()) return false;
    const double dur = kbdSeconds(a);
    HamTxCheck g = gateIn;
    const double bw = kbdBwHz(p);
    g.audioHz = std::max(0.0, centreHz - bw / 2);   // the gate takes the lowest frequency of the signal
    g.bwHz = bw;
    if (!tx.sendAudio(g, std::move(a), bw, level, err)) return false;
    duty.add(nowSec, dur);
    return true;
}

// ---- macros ----
const std::vector<KbdMacro>& kbdDefaultMacros() {
    static const std::vector<KbdMacro> m = {
        {"CQ", "CQ CQ CQ DE {MYCALL} {MYCALL} {MYCALL} PSE K\n"},
        {"Answer", "{CALL} DE {MYCALL} {MYCALL} K\n"},
        {"RST", "{CALL} DE {MYCALL} UR RST {RST} {RST} K\n"},
        {"QTH", "MY QTH IS {MYQTH} GRID {MYGRID} K\n"},
        {"Name", "MY NAME IS {MYNAME} {MYNAME} K\n"},
        {"73", "{CALL} DE {MYCALL} TNX FER QSO 73 SK\n"},
    };
    return m;
}

bool kbdExpand(const std::string& t, const KbdStation& s, std::string& out, std::string* missing) {
    out.clear();
    bool ok = true;
    for (size_t i = 0; i < t.size();) {
        const size_t e = t[i] == '{' ? t.find('}', i) : std::string::npos;
        if (e == std::string::npos) { out += t[i++]; continue; }
        const std::string k = t.substr(i + 1, e - i - 1);
        const std::string* v = k == "MYCALL" ? &s.myCall : k == "MYGRID" ? &s.myGrid : k == "MYNAME" ? &s.myName : k == "MYQTH" ? &s.myQth
                             : k == "CALL" ? &s.call : k == "NAME" ? &s.name : k == "RST" ? &s.rst : nullptr;
        if (!v) { out += t.substr(i, e - i + 1); i = e + 1; continue; }
        if (v->empty()) { ok = false; if (missing && missing->find(k) == std::string::npos) *missing += (missing->empty() ? "" : ", ") + k; }
        out += upper(*v);
        i = e + 1;
    }
    return ok;
}

std::vector<std::string> kbdCallsInText(const std::string& text, const std::string& myCall) {
    std::vector<std::string> r;
    const std::string me = upper(myCall);
    for (const auto& c : cw::callsigns(upper(text))) if (c != me && hamCallValid(c)) r.push_back(c);
    return r;
}

QsoRecord kbdLogRecord(const KbdPreset& p, const KbdStation& s, double freqHz, double startUtc, double endUtc, const std::string& rstSent, const std::string& rstRcvd) {
    QsoRecord q;
    q.call = upper(s.call);
    q.mode = p.adifMode;
    q.submode = p.adifSub;
    q.rstSent = rstSent; q.rstRcvd = rstRcvd;
    q.myCall = upper(s.myCall); q.myGrid = upper(s.myGrid);
    q.freqHz = freqHz; q.startUtc = startUtc; q.endUtc = endUtc;
    return q;
}

}  // namespace dect2::ham
