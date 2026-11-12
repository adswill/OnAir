// The per-mode transmit parameter table (tx_params.h). One builder per kind of signal; every row names where its value goes, and the rows that do not change
// the samples say so (meta). docs/TX-4.0.md has the audit that this table came from.
#include "dect2/tx/tx_params.h"
#include "dect2/audio/audio_control.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/mesh/mesh_rx.h"
#include "dect2/hfdig/hfdig_sstv.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/t2/t2.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace dect2 {

thread_local std::vector<std::string>* TxRequest::txKeyTrace = nullptr;
void txTraceKeys(std::vector<std::string>* sink) { TxRequest::txKeyTrace = sink; }

namespace {

using D = TxParamDef;

// ---------------------------------------------------------------- row makers
D num(const char* key, const char* label, const char* help, const char* unit, double def, double lo, double hi, const char* fmt) {
    D d; d.key = key; d.label = label; d.help = help; d.unit = unit; d.type = TxPType::Float; d.def = def; d.lo = lo; d.hi = hi; d.fmt = fmt;
    return d;
}
D intr(const char* key, const char* label, const char* help, const char* unit, double def, double lo, double hi) {
    D d = num(key, label, help, unit, def, lo, hi, "%.0f");
    d.type = TxPType::Int;
    return d;
}
D hexr(const char* key, const char* label, const char* help, double def, double lo, double hi, const char* fmt) {
    D d = num(key, label, help, "", def, lo, hi, fmt);
    d.type = TxPType::Hex;
    return d;
}
D flag(const char* key, const char* label, const char* help, bool def) {
    D d = num(key, label, help, "", def ? 1 : 0, 0, 1, "");
    d.type = TxPType::Bool;
    return d;
}
D pick(const char* key, const char* label, const char* help, double defIdx, std::vector<const char*> names, double base = 0, std::vector<double> values = {}) {
    D d = num(key, label, help, "", defIdx, 0, (double)names.size() - 1, "");
    d.type = TxPType::Choice; d.choices = std::move(names); d.base = base; d.values = std::move(values);
    return d;
}
D text(const char* key, const char* label, const char* help, TxPField f) {
    D d; d.key = key; d.label = label; d.help = help; d.type = TxPType::Text; d.field = f; d.group = TxPGroup::Payload; d.level = TxPLevel::Simple;
    return d;
}
D file(const char* key, const char* label, const char* help) {
    D d = text(key, label, help, TxPField::File);
    d.type = TxPType::File;
    return d;
}
D custom(const char* key, const char* label, const char* help, TxPField f = TxPField::None) {
    D d; d.key = key; d.label = label; d.help = help; d.custom = true; d.field = f; d.group = TxPGroup::Payload; d.level = TxPLevel::Simple;
    d.meta = "drawn by the mode's own widget; checked by that mode's own tests";
    return d;
}

// modifiers
D G(D d, TxPGroup g) { d.group = g; return d; }
D S(D d) { d.level = TxPLevel::Simple; return d; }
D A(D d, TxPGroup g) { d.level = TxPLevel::Advanced; d.group = g; return d; }
D FLD(D d, TxPField f) { d.field = f; return d; }
D SET(D d, void (*f)(TxRequest&, double)) { d.set = f; return d; }
D W(D d, const char* when) { d.when = when; return d; }
D META(D d, const char* why) { d.meta = why; return d; }
D TV(D d, double v) { d.testValue = v; d.hasTestValue = true; return d; }
D TT(D d, const char* t) { d.testText = t; return d; }
D PROBE(D d, double s) { d.probeSec = s; return d; }
D AUD(D d) { d.needsAudio = true; return d; }
D HID(D d) { d.hidden = true; return d; }

void add(std::vector<D>& v, std::initializer_list<D> l) { v.insert(v.end(), l.begin(), l.end()); }

const char* const kSideband[] = {"Upper sideband (USB)", "Lower sideband (LSB)"};

std::vector<const char*> ctcssNames() {
    static std::vector<std::string> names;
    static std::vector<const char*> ptrs;
    static std::once_flag once;
    std::call_once(once, [] {
        names.push_back("Off");
        for (float f : actl::kCtcssHz) { char b[24]; snprintf(b, sizeof b, "%.1f Hz", f); names.push_back(b); }
        for (const auto& n : names) ptrs.push_back(n.c_str());
    });
    return ptrs;
}
std::vector<const char*> dcsNames() {
    static std::vector<std::string> names;
    static std::vector<const char*> ptrs;
    static std::once_flag once;
    std::call_once(once, [] {
        names.push_back("Off");
        for (int c : txDcsCodes()) { char b[16]; snprintf(b, sizeof b, "D%03d", c); names.push_back(b); }
        for (const auto& n : names) ptrs.push_back(n.c_str());
    });
    return ptrs;
}

// ---------------------------------------------------------------- shared groups of rows

// the audio input: microphone, file, test tone or silence (the panel's audio-source widget; the file name is the request's file)
void audioSource(std::vector<D>& v) {
    v.push_back(custom("audio.source", "Audio input", "The microphone, a WAV / MP3 file, the test tone or silence.", TxPField::File));
}

// the audio chain every analog voice shares (tx_voice.cpp AnalogVoice)
void voiceAudio(std::vector<D>& v, double hp, double lp) {
    add(v, {
        A(FLD(num("audio.tone", "Test tone", "The pitch of the test tone, used when the audio input is the test tone.", "Hz", 1000, 100, 5000, "%.0f"), TxPField::ToneHz), TxPGroup::Audio),
        A(num("audio.toneLevelPct", "Test tone level", "How loud the test tone is, as a share of full audio.", "%", 50, 0, 100, "%.0f"), TxPGroup::Audio),
        A(num("audio.tone2Hz", "Second test tone", "A second tone added to the first (0 = off): a two-tone linearity test.", "Hz", 0, 0, 5000, "%.0f"), TxPGroup::Audio),
        A(flag("audio.compOn", "Speech compressor", "Evens out loud and quiet speech so the signal stays strong without over-modulating.", false), TxPGroup::Audio),
        W(A(num("audio.compThreshDb", "Threshold", "Audio above this level is turned down.", "dB", -18, -40, 0, "%.0f"), TxPGroup::Audio), "audio.compOn=1"),
        W(A(num("audio.compRatio", "Ratio", "How strongly audio above the threshold is turned down; 20 is close to a hard limiter.", ": 1", 4, 1, 20, "%.1f"), TxPGroup::Audio), "audio.compOn=1"),
        W(A(num("audio.compMakeupDb", "Make-up gain", "Gain added after the compressor.", "dB", 0, 0, 20, "%.0f"), TxPGroup::Audio), "audio.compOn=1"),
        W(A(num("audio.compReleaseMs", "Release", "How fast the compressor lets go after a loud passage.", "ms", 200, 20, 2000, "%.0f"), TxPGroup::Audio), "audio.compOn=1"),
        A(num("audio.hpHz", "High-pass (low cut)", "Removes rumble and hum below this pitch (0 = off).", "Hz", hp, 0, 1000, "%.0f"), TxPGroup::Filters),
        A(num("audio.lpHz", "Low-pass (audio bandwidth)", "Cuts the audio above this pitch, which sets how wide the signal is (0 = off).", "Hz", lp, 0, 12000, "%.0f"), TxPGroup::Filters),
        META(A(flag("vox.on", "VOX (transmit on voice)", "The signal goes out only while there is sound on the audio input.", false), TxPGroup::Timing),
             "opens and closes with the audio level; a steady test tone keeps it open (tests/tx/test_tx_params.cpp checks it with speech gaps)"),
        W(META(A(num("vox.threshDb", "VOX threshold", "How loud the audio must be to open the VOX.", "dB", -35, -60, 0, "%.0f"), TxPGroup::Timing), "with VOX on and speech gaps only (test_tx_params)"), "vox.on=1"),
        W(META(A(num("vox.hangMs", "VOX hang time", "How long the VOX stays open after the sound stops.", "ms", 500, 50, 3000, "%.0f"), TxPGroup::Timing), "with VOX on and speech gaps only (test_tx_params)"), "vox.on=1"),
        META(A(flag("dtmf.off", "Ignore the DTMF keypad", "The keypad's tones are not mixed into this transmission.", false), TxPGroup::Audio), "only while keypad keys are pressed (tests/tx/test_tx_dtmf.cpp)"),
    });
}

// narrow FM voice (the analog fall-back of the digital modes and the scene generators)
void nfmVoice(std::vector<D>& v) {
    audioSource(v);
    add(v, {
        A(FLD(num("fm.deviation", "Deviation", "How far the audio swings the frequency. 2.5 kHz is a 12.5 kHz channel, 5 kHz a 25 kHz channel.", "Hz", 5000, 500, 25000, "%.0f"), TxPField::Deviation), TxPGroup::Modulation),
        AUD(A(num("fm.preemphUs", "Pre-emphasis", "Boosts the high notes of the microphone or file before sending (0 = off). The test tone is sent flat.", "µs", 50, 0, 150, "%.0f"), TxPGroup::Modulation)),
        A(pick("fm.ctcssIdx", "CTCSS tone", "A sub-audible tone that opens a receiver's squelch: one of the 50 standard tones (67.0 to 254.1 Hz). Off sends none.", 0, ctcssNames()), TxPGroup::Modulation),
        W(A(num("fm.ctcssHz", "Own CTCSS tone", "A tone that is not on the standard list (0 = none). The list above wins when it is set.", "Hz", 0, 0, 300, "%.1f"), TxPGroup::Modulation), "fm.ctcssIdx=0"),
        W(A(num("fm.ctcssPct", "CTCSS level", "The tone's deviation as a share of the full deviation (12 % is the usual 600 Hz at 5 kHz).", "%", 12, 2, 30, "%.0f"), TxPGroup::Modulation), "fm.ctcssIdx>0"),
        A(pick("fm.dcsIdx", "DCS code", "Digital-coded squelch: a 23-bit code word at 134.4 bit/s below the voice. It replaces the CTCSS tone when both are set.", 0, dcsNames()), TxPGroup::Modulation),
        W(TV(A(intr("fm.dcsCode", "Own DCS code", "A code that is not on the list, written as its three octal digits (0 = none). The list above wins when it is set.", "", 0, 0, 777), TxPGroup::Modulation), 23), "fm.dcsIdx=0"),
        W(A(flag("fm.dcsInv", "DCS inverted", "Sends the code with the opposite polarity (the codes written with I instead of N).", false), TxPGroup::Modulation), "fm.dcsIdx>0"),
        W(A(num("fm.dcsPct", "DCS level", "The code's deviation as a share of the full deviation.", "%", 12, 2, 30, "%.0f"), TxPGroup::Modulation), "fm.dcsIdx>0"),
        A(num("fm.level", "RF level", "The size of the signal as a share of full scale.", "", 0.4, 0.05, 1.0, "%.2f"), TxPGroup::Output),
    });
    voiceAudio(v, 300, 3000);
}

// "send analog NFM of the microphone or a file instead": the switch is the panel's own (it changes this table)
void analogSwitch(std::vector<D>& v, const TxParamCtx& c) {
    v.push_back(custom("analog.voice", "Send analog voice instead", "Sends analog NFM of the microphone or a file instead of the mode's own signal."));
    if (c.analogVoice) nfmVoice(v);
}

void outputCommon(std::vector<D>& v) {
    add(v, {
        A(num("out.trimDb", "Output trim", "Gain applied to the whole signal just before the radio. Lower it when the meter shows clipping.", "dB", 0, -40, 6, "%.1f"), TxPGroup::Output),
        A(flag("out.swapIq", "Mirror the spectrum", "Swaps I and Q so the signal is turned over: an upper sideband becomes a lower one. For a radio or mixer that inverts.", false), TxPGroup::Output),
        TV(A(num("out.rampMs", "Soft start (fade-in)", "The signal fades in over this time after TRANSMIT (and out at Stop): a few ms stop the key click, a few seconds bring an amplifier up gently.", "ms", kTxDefaultRampMs, 0, 10000, "%.0f"), TxPGroup::Timing), 5000),
        META(A(num("tx.totSec", "Timeout timer", "Ends the transmission by itself after this many seconds, with the key-down fade (0 = off). Your own limit: OnAir sets none.", "s", 0, 0, 3600, "%.0f"), TxPGroup::Timing),
             "ends the session; tests/tx/test_tx_control.cpp checks it"),
    });
}

void hfAudioPos(std::vector<D>& v, double def, const char* label, const char* lsbKey) {
    v.push_back(A(FLD(num("audio.tone", label, "Where the signal sits in the audio passband, as an offset from the dial frequency.", "Hz", def, 200, 3000, "%.0f"), TxPField::ToneHz), TxPGroup::Modulation));
    if (lsbKey) v.push_back(A(pick(lsbKey, "Sideband", "Upper sideband puts the signal above the dial frequency, lower sideband below it.", 0, {kSideband[0], kSideband[1]}), TxPGroup::Modulation));
}

// ---------------------------------------------------------------- the DVB / ISDB / DVB-S rows

void dvbt2Rows(std::vector<D>& v) {
    add(v, {
        PROBE(A(SET(TV(pick("t2.fft", "FFT size", "More carriers are more robust against echoes, fewer against movement.", 3, {"1K", "2K", "4K", "8K", "16K", "32K"}, 0, {3, 0, 2, 1, 4, 5}), 4),
                    [](TxRequest& r, double x) { r.synth.tx.s2field1 = (int)x; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(pick("t2.gi", "Guard interval", "The pause before each symbol that absorbs echoes. Not every pilot pattern is allowed with every guard (EN 302 755 table 59).", 4,
                         {"1/32", "1/16", "1/8", "1/4", "1/128", "19/128", "19/256"}),
                    [](TxRequest& r, double x) { r.synth.tx.giIdx = (int)x; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(pick("t2.pp", "Pilot pattern", "PP1 to PP8: denser pilots follow faster channels; the allowed ones depend on the FFT size and guard.", 6, {"PP1", "PP2", "PP3", "PP4", "PP5", "PP6", "PP7", "PP8"}),
                    [](TxRequest& r, double x) { r.synth.tx.pp = (int)x; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(pick("t2.plpMod", "Constellation", "The modulation of the data PLP.", 2, {"QPSK", "16-QAM", "64-QAM", "256-QAM"}),
                    [](TxRequest& r, double x) { r.synth.tx.plpMod = (int)x; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(pick("t2.plpCod", "Code rate (FEC)", "The LDPC code rate of the data PLP.", 2, {"1/2", "3/5", "2/3", "3/4", "4/5", "5/6"}),
                    [](TxRequest& r, double x) { r.synth.tx.plpCod = (int)x; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(flag("t2.rot", "Rotated constellation", "Rotates and delays the constellation so a faded carrier can still be recovered.", true),
                    [](TxRequest& r, double x) { r.synth.tx.plpRot = x > 0.5; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(flag("t2.short", "Short FEC frames", "16200-bit FEC frames instead of 64800: lower latency, a little less efficient.", false),
                    [](TxRequest& r, double x) { r.synth.tx.plpShort = x > 0.5; }), TxPGroup::Framing), 0.12),
        PROBE(A(SET(flag("t2.ext", "Extended carriers", "More carriers at the band edges (8K, 16K and 32K only).", false),
                    [](TxRequest& r, double x) { r.synth.tx.ext = x > 0.5; }), TxPGroup::Modulation), 0.12),
        PROBE(A(SET(intr("t2.ti", "Time-interleaver blocks", "Time-interleaver blocks per frame (0 = none). More spreads a burst error wider.", "", 3, 0, 8),
                    [](TxRequest& r, double x) { r.synth.tx.plpTi = (int)x; }), TxPGroup::Framing), 0.12),
        PROBE(A(SET(pick("t2.l1Mod", "L1 signalling modulation", "The modulation of the L1-post signalling.", 1, {"BPSK", "QPSK", "16-QAM", "64-QAM"}),
                    [](TxRequest& r, double x) { r.synth.tx.l1Mod = (int)x; }), TxPGroup::Framing), 0.12),
        PROBE(A(SET(hexr("t2.cellId", "Cell ID", "CELL_ID of the L1 signalling (16 bits).", 0x1001, 0, 0xFFFF, "0x%04X"),
                    [](TxRequest& r, double x) { r.synth.tx.cellId = (int)x; }), TxPGroup::Framing), 0.12),
        PROBE(A(SET(hexr("t2.networkId", "Network ID", "NETWORK_ID of the L1 signalling (16 bits).", 0x3085, 0, 0xFFFF, "0x%04X"),
                    [](TxRequest& r, double x) { r.synth.tx.networkId = (int)x; }), TxPGroup::Framing), 0.12),
        PROBE(A(SET(hexr("t2.systemId", "T2 system ID", "T2_SYSTEM_ID of the L1 signalling (16 bits).", 0x8001, 0, 0xFFFF, "0x%04X"),
                    [](TxRequest& r, double x) { r.synth.tx.systemId = (int)x; }), TxPGroup::Framing), 0.12),
    });
}

void dvbtRows(std::vector<D>& v) {
    add(v, {
        PROBE(A(SET(pick("dvbt.mode", "FFT mode", "2K or 8K carriers.", 1, {"2K", "8K"}), [](TxRequest& r, double x) { r.synth.dvbtMode = (int)x; }), TxPGroup::Modulation), 0.05),
        PROBE(A(SET(pick("dvbt.guard", "Guard interval", "The pause before each symbol that absorbs echoes.", 2, {"1/32", "1/16", "1/8", "1/4"}),
                    [](TxRequest& r, double x) { r.synth.dvbtGuard = (int)x; }), TxPGroup::Modulation), 0.05),
        PROBE(A(SET(pick("dvbt.mod", "Constellation", "The modulation of the carriers.", 2, {"QPSK", "16-QAM", "64-QAM"}), [](TxRequest& r, double x) { r.synth.dvbtMod = (int)x; }), TxPGroup::Modulation), 0.05),
        PROBE(A(SET(pick("dvbt.rate", "Code rate (FEC)", "The convolutional code rate.", 1, {"1/2", "2/3", "3/4", "5/6", "7/8"}), [](TxRequest& r, double x) { r.synth.dvbtRate = (int)x; }), TxPGroup::Modulation), 0.05),
    });
}

void isdbtRows(std::vector<D>& v) {
    add(v, {
        PROBE(S(SET(pick("isdbt.bw", "Channel width", "6 MHz in Japan and the Americas, 7 or 8 MHz elsewhere.", 0, {"6 MHz", "7 MHz", "8 MHz"}, 6), [](TxRequest& r, double x) { r.synth.isdbtBwMhz = (int)x; })), 0.05),
        PROBE(A(SET(pick("isdbt.mode", "FFT mode", "Mode 1 (2K), 2 (4K) or 3 (8K) carriers.", 2, {"mode 1 (2K)", "mode 2 (4K)", "mode 3 (8K)"}, 1),
                    [](TxRequest& r, double x) { r.synth.isdbtMode = (int)x; }), TxPGroup::Modulation), 0.05),
        PROBE(A(SET(pick("isdbt.guard", "Guard interval", "The pause before each symbol that absorbs echoes.", 2, {"1/32", "1/16", "1/8", "1/4"}),
                    [](TxRequest& r, double x) { r.synth.isdbtGuard = (int)x; }), TxPGroup::Modulation), 0.05),
        PROBE(A(SET(pick("isdbt.mod", "Constellation (layer B)", "The modulation of the programme layer; the one-segment layer A stays QPSK 2/3.", 3, {"DQPSK", "QPSK", "16-QAM", "64-QAM"}),
                    [](TxRequest& r, double x) { r.synth.isdbtMod = (int)x; }), TxPGroup::Modulation), 0.05),
        PROBE(A(SET(pick("isdbt.rate", "Code rate (layer B)", "The convolutional code rate of the programme layer.", 2, {"1/2", "2/3", "3/4", "5/6", "7/8"}),
                    [](TxRequest& r, double x) { r.synth.isdbtRate = (int)x; }), TxPGroup::Modulation), 0.05),
    });
}

void dvbsRows(std::vector<D>& v, double symDef = 5e6) {
    add(v, {
        S(SET(TV(num("dvbs.symrate", "Symbol rate", "Symbols a second (QO-100 DATV uses 333 k to 2 M, satellites 22 M to 30 M).", "sym/s", symDef, 25e3, 60e6, "%.0f"), symDef * 0.5),
              [](TxRequest& r, double x) { r.synth.modeVal[0] = x; })),
        A(SET(pick("dvbs.std", "Standard", "DVB-S2, the older DVB-S, or DVB-S2X.", 0, {"DVB-S2", "DVB-S", "DVB-S2X"}), [](TxRequest& r, double x) { r.synth.modeOpt[0] = (int)x; }), TxPGroup::Modulation),
        W(A(SET(pick("dvbs.mod", "Modulation", "QPSK is the most robust; the APSK constellations carry more.", 0, {"QPSK", "8PSK", "16APSK", "32APSK"}),
                [](TxRequest& r, double x) { r.synth.modeOpt[1] = (int)x; }), TxPGroup::Modulation), "dvbs.std!=1"),
        W(A(SET(pick("dvbs.cr", "Code rate", "The DVB-S2 LDPC code rate (2/3 when set to default).", 0, {"default (2/3)", "1/4", "1/3", "2/5", "1/2", "3/5", "2/3", "3/4", "4/5", "5/6", "8/9", "9/10"}),
                [](TxRequest& r, double x) { r.synth.modeOpt[2] = (int)x; }), TxPGroup::Modulation), "dvbs.std!=1"),
        W(TV(A(SET(pick("dvbs.crS", "Code rate", "The DVB-S convolutional code rate.", 0, {"default (2/3)", "1/2", "2/3", "3/4", "5/6", "7/8"}),
                   [](TxRequest& r, double x) { r.synth.modeOpt[2] = (int)x; }), TxPGroup::Modulation), 3), "dvbs.std=1"),
        A(SET(pick("dvbs.rolloff", "Roll-off", "The spectrum's edge: smaller is narrower and needs a better receiver.", 0, {"0.35", "0.25", "0.20", "0.15", "0.10", "0.05"}),
              [](TxRequest& r, double x) { r.synth.modeOpt[3] = (int)x; }), TxPGroup::Filters),
        W(A(SET(pick("dvbs.frame", "Frame size", "Normal (64800 bits) or short (16200 bits) FEC frames.", 0, {"normal", "short"}), [](TxRequest& r, double x) { r.synth.modeOpt[4] = (int)x; }), TxPGroup::Framing), "dvbs.std!=1"),
        W(A(SET(flag("dvbs.pilots", "Pilots", "Pilot blocks in the frame help a receiver with phase noise and low SNR.", false), [](TxRequest& r, double x) { r.synth.modeOpt[5] = x > 0.5 ? 1 : 0; }), TxPGroup::Framing), "dvbs.std!=1"),
        A(SET(flag("dvbs.invert", "Spectral inversion", "Turns the spectrum over (for an up-converter with a high-side oscillator).", false), [](TxRequest& r, double x) { r.synth.modeOpt[6] = x > 0.5 ? 1 : 0; }), TxPGroup::Output),
    });
}

// the TV payload: a transport stream file or the test card with the names of the multiplex
void tvPayload(std::vector<D>& v, const std::string& id) {
    if (id == "atsc3") return;   // ATSC 3.0 sends its own ROUTE / fMP4 test programme: no file, no names
    v.push_back(META(file("tv.file", "MPEG-TS / video file", "Your .ts / .m2ts is sent as it is; other video is remuxed when FFmpeg can. Empty: a test card with the names below."),
                     "a file of the test machine; tests/tx/test_tx_mux.cpp sends files"));
    v.push_back(custom("tv.mux", "Multiplex", "Network, provider, first service ID and up to four programmes (name and picture source each).", TxPField::Name));
    v.push_back(HID(custom("tv.network", "Network name", "", TxPField::Network)));
}

// ---------------------------------------------------------------- the builder

std::vector<D> build(const TxModeSpec& m, const TxParamCtx& c) {
    std::vector<D> v;
    const std::string id = m.id ? m.id : "";
    const TxPayloadKind k = txPayloadKind(id);
    switch (k) {
    case TxPayloadKind::TvFile:
        if (id == "dvbt2" || id == "dvbt") {
            v.push_back(S(FLD(pick("dvb.bw", "Channel width", "The width of the channel the multiplex is sent in.", 0, {"8 MHz", "7 MHz", "6 MHz", "5 MHz"}, 0, {8, 7, 6, 5}), TxPField::BandwidthMhz)));
            v.back().probeSec = 0.05;
        }
        tvPayload(v, id);
        if (id == "dvbt2") dvbt2Rows(v);
        if (id == "dvbt") dvbtRows(v);
        if (id == "isdbt") isdbtRows(v);
        if (id == "dvbs") dvbsRows(v);
        if (id == "dtmb" || id == "dvbc" || id == "atsc3" || id == "atsc")
            v.push_back(A(custom("scene.options", "Generator settings", "The generator's own settings (shared with the receiver's test signal); its noise and offset controls do not apply here."), TxPGroup::Modulation));
        break;
    case TxPayloadKind::AnalogTv:
        add(v, {
            custom("atv.picture", "Picture source", "The test card, a video file or a camera / capture card.", TxPField::File),
            text("atv.ident", "Ident on the card", "The station name drawn on the test card (empty: ONAIR TEST).", TxPField::Name),
            custom("atv.sound", "Sound carrier", "A tone, a melody, an unmodulated carrier, none, the microphone or a file."),
            A(SET(pick("atv.system", "System", "The line standard and the channel: B/G, I, D/K, M (NTSC) ...", 0, {"B/G", "B (7 MHz)", "I", "D/K", "M (NTSC)", "N", "A (405)", "C", "E (819)", "H", "K'", "L", "L'"}),
                  [](TxRequest& r, double x) { r.synth.modeOpt[0] = (int)x; }), TxPGroup::Modulation),
            A(SET(pick("atv.colour", "Colour", "The colour system; usual for the system is PAL, or NTSC for M.", 0, {"usual for the system", "PAL", "NTSC", "SECAM", "monochrome"}),
                  [](TxRequest& r, double x) { r.synth.modeOpt[1] = (int)x; }), TxPGroup::Modulation),
            A(SET(pick("atv.pattern", "Test picture", "What the test card source draws.", 0, {"test card", "colour bars", "grey ramp"}), [](TxRequest& r, double x) { r.synth.modeOpt[2] = (int)x; }), TxPGroup::Modulation),
        });
        for (auto& d : v) if (!d.custom) d.probeSec = 0.03;
        break;
    case TxPayloadKind::BroadcastAudio:
        if (id == "dab") add(v, {
            TT(text("dab.ensemble", "Ensemble name", "The ensemble label (up to 16 characters) in the FIC.", TxPField::Network), "ONAIR ENS"),
            custom("dab.services", "Service names", "Up to four service labels in the FIC.", TxPField::Name),
        });
        else if (id == "drm") add(v, {
            TT(text("drm.label", "Station label", "The station label in the SDC (up to 16 characters).", TxPField::Name), "ONAIR DRM"),
            text("drm.text", "Text message", "The text message the receiver shows.", TxPField::Text),
        });
        for (auto& d : v) d.probeSec = 3.0;   // the SDC and the text come once a superframe (1.2 s)
        // HD Radio and CDR send their built-in demo programme: nothing to type
        break;
    case TxPayloadKind::Ftx: {
        add(v, {
            text("ftx.call", "Callsign", "Your callsign (a standard callsign: prefix, one digit, up to three letters).", TxPField::Callsign),
            TT(text("ftx.grid", "Grid", "Your four-character Maidenhead locator.", TxPField::Grid), "KO25"),
        });
        if (id == "wspr") v.push_back(S(FLD(TV(intr("wspr.dbm", "Power", "The power written in the message, in dBm (WSPR rounds to 0, 3, 7 ...).", "dBm", 37, 0, 60), 30), TxPField::Dbm)));
        else add(v, {
            S(FLD(pick("ftx.kind", "Message", "CQ with your grid, a signal report to another station, or free text.", 0, {"CQ", "Report", "Free text"}), TxPField::Kind)),
            W(TT(text("ftx.to", "To", "The station the report is for.", TxPField::Dest), "SV1ABC"), "ftx.kind=1"),
            W(TT(text("ftx.text", "Text", "Free text (up to 13 characters).", TxPField::Text), "TNX 73"), "ftx.kind=2"),
        });
        hfAudioPos(v, 1500, "Audio frequency", nullptr);
        v.push_back(A(FLD(pick("hf.sideband", "Sideband", "Upper sideband puts the signal above the dial frequency, lower sideband below it.", 0, {kSideband[0], kSideband[1]}), TxPField::Variant), TxPGroup::Modulation));
        for (auto& d : v) d.probeSec = 1.0;
        break;
    }
    case TxPayloadKind::Rtty:
        add(v, {
            text("rtty.text", "Text", "What is sent (Baudot: letters, figures and a few signs).", TxPField::Text),
            A(FLD(pick("rtty.baud", "Speed", "45.45 baud is the amateur standard.", 0, {"45.45 baud", "50 baud", "75 baud", "100 baud"}), TxPField::BaudIdx), TxPGroup::Modulation),
            A(FLD(pick("rtty.shift", "Shift", "The distance between mark and space; 170 Hz is the amateur standard.", 0, {"170 Hz", "200 Hz", "425 Hz", "450 Hz", "850 Hz"}), TxPField::ShiftIdx), TxPGroup::Modulation),
            A(num("rtty.markHz", "Mark tone", "The audio frequency of the mark tone (2125 Hz is the usual one).", "Hz", 2125, 300, 3000, "%.0f"), TxPGroup::Modulation),
            A(FLD(flag("rtty.reverse", "Reverse", "Mark is the upper tone.", false), TxPField::Reverse), TxPGroup::Modulation),
            A(FLD(pick("hf.sideband", "Sideband", "Upper sideband puts the tones above the dial frequency, lower sideband below it.", 0, {kSideband[0], kSideband[1]}), TxPField::Variant), TxPGroup::Modulation),
        });
        for (auto& d : v) d.probeSec = 1.5;
        break;
    case TxPayloadKind::Keyboard:
        if (id == "cp16") {   // makeCp16TxGenerator (cp16_gen.h)
            add(v, {
                text("cp16.text", "Text", "The characters drawn on the waterfall (the font of the mode's screen).", TxPField::Text),
                A(num("cp16.char", "Seconds a character", "How long one 16-row character and its blank row take; slower is stronger.", "s", 2, 0.5, 8, "%.1f"), TxPGroup::Modulation),
                A(num("cp16.spacing", "Carrier spacing", "The distance between the 16 carriers (CRAC uses 10 to 20 Hz).", "Hz", 15, 5, 30, "%.1f"), TxPGroup::Modulation),
                A(num("cp16.base", "Lowest carrier", "The audio frequency of the left column of the character.", "Hz", 1000, 200, 3000, "%.0f"), TxPGroup::Modulation),
                A(flag("cp16.lsb", "Lower sideband", "Send below the dial frequency instead of above it.", false), TxPGroup::Modulation),
            });
            for (auto& d : v) d.probeSec = 1.5;
            break;
        }
        if (id == "esubghz") {   // makeEsubghzTxGenerator
            add(v, {
                text("esubghz.text", "Message", "One plain chat line (encrypted lines are sent from the mode's Chat screen).", TxPField::Text),
                A(num("esubghz.ifHz", "Offset from the centre", "Where the signal sits in the samples, from the radio's centre frequency.", "Hz", 0, -500000, 500000, "%.0f"), TxPGroup::Modulation),
            });
            break;
        }
        if (id == "meshcore") {   // makeMeshCoreTxGenerator
            add(v, {
                text("meshcore.text", "Message", "One line to the Public channel; the MeshCore screen sends adverts and direct messages.", TxPField::Text),
                A(intr("meshcore.sf", "Spreading factor", "SF5 to SF12 (the EU narrow preset uses SF8).", "", 8, 5, 12), TxPGroup::Modulation),
                A(pick("meshcore.bwHz", "Bandwidth", "The LoRa bandwidth (the EU narrow preset uses 62.5 kHz).", 1, {"31.25 kHz", "62.5 kHz", "125 kHz", "250 kHz", "500 kHz"}, 0, {31250, 62500, 125000, 250000, 500000}), TxPGroup::Modulation),
                A(pick("meshcore.cr", "Coding rate", "The forward error correction: 4/5 is the least, 4/8 the most.", 3, {"4/5", "4/6", "4/7", "4/8"}, 5), TxPGroup::Modulation),
                TV(A(intr("meshcore.preamble", "Preamble", "The up-chirps before the sync word (32 at SF8 and below, else 16).", "", 32, 6, 64), TxPGroup::Framing), 16),
                META(A(num("meshcore.gapSec", "Pause between frames", "Silence between two frames of the same burst.", "s", 0.05, 0, 10, "%.2f"), TxPGroup::Timing), "only between the frames of a burst; one text line is one frame"),
                HID(custom("meshcore.name", "Sender name", "The node name the MeshCore screen sets.", TxPField::Name)),
                A(num("meshcore.ifHz", "Offset from the centre", "Where the signal sits in the samples (default: where the Transmit window expects a LoRa signal).", "Hz", -meshTuning().tuneOffsetHz, -500000, 500000, "%.0f"), TxPGroup::Modulation),
            });
            for (auto& d : v) d.probeSec = 0.6;
            break;
        }
        {   // Olivia, Contestia, MFSK, THOR, DominoEX, Feld Hell (makeKbdTx)
            const int fam = m.variant - 26;
            v.push_back(text("kbd.text", "Text", "What is sent, repeated until Stop.", TxPField::Text));
            if (fam == KbdOlivia || fam == KbdContestia) {
                v.push_back(A(pick("kbd.tones", "Tones", "More tones are more robust and slower.", 4, {"2", "4", "8", "16", "32", "64", "128", "256"}, 0, {2, 4, 8, 16, 32, 64, 128, 256}), TxPGroup::Modulation));
                v.push_back(A(pick("kbd.bw", "Bandwidth", "The width of the signal.", 3, {"125 Hz", "250 Hz", "500 Hz", "1000 Hz", "2000 Hz"}, 0, {125, 250, 500, 1000, 2000}), TxPGroup::Modulation));
            } else if (kbdVariantCount(fam) > 1) {
                static std::vector<std::vector<const char*>> names(kKbdFamilies);
                static std::once_flag once;
                std::call_once(once, [] { for (int f = 0; f < kKbdFamilies; f++) for (int i = 0; i < kbdVariantCount(f); i++) names[(size_t)f].push_back(kbdVariantName(f, i)); });
                v.push_back(A(pick("kbd.variant", "Variant", "The speed and tone set of the family.", kbdDefaultVariant(fam), names[(size_t)fam]), TxPGroup::Modulation));
            }
            hfAudioPos(v, 1500, "Centre frequency", "kbd.lsb");
            for (auto& d : v) d.probeSec = 1.0;
        }
        break;
    case TxPayloadKind::Cw:
        add(v, {
            text("cw.text", "Text", "What is keyed, repeated until Stop.", TxPField::Text),
            S(FLD(TV(intr("cw.wpm", "Speed", "Words a minute.", "wpm", 20, 5, 60), 30), TxPField::Wpm)),
        });
        hfAudioPos(v, 700, "Pitch", nullptr);
        for (auto& d : v) d.probeSec = 0.6;
        break;
    case TxPayloadKind::Psk: {
        static std::vector<const char*> names;
        static std::once_flag once;
        std::call_once(once, [] { for (int i = 0; i < kPskModes; i++) names.push_back(pskModeName(i)); });
        add(v, {
            text("psk.text", "Text", "What is sent, repeated until Stop.", TxPField::Text),
            S(FLD(pick("psk.mode", "Mode", "BPSK31 is the usual one; the faster and QPSK / 8PSK modes carry more.", 0, names), TxPField::Variant)),
        });
        hfAudioPos(v, 1000, "Audio frequency", nullptr);
        for (auto& d : v) d.probeSec = 2.0;
        break;
    }
    case TxPayloadKind::Js8:
        add(v, {
            text("js8.call", "Callsign", "Your callsign.", TxPField::Callsign),
            TT(text("js8.grid", "Grid", "Your four-character Maidenhead locator.", TxPField::Grid), "KO25"),
            TT(text("js8.to", "To", "The station or group the text is for (empty with a text: @ALLCALL).", TxPField::Dest), "@HB"),
            text("js8.text", "Text", "A directed message; empty: a heartbeat with your grid.", TxPField::Text),
            META(A(FLD(num("audio.tone", "Audio frequency", "Where the signal sits in the audio passband, as an offset above the dial frequency.", "Hz", 1000, 200, 3000, "%.0f"), TxPField::ToneHz), TxPGroup::Modulation),
                 "JS8 frames wait for the next 15 s UTC slot; tests/js8 checks the frequency"),
        });
        for (auto& d : v) if (!d.meta) d.meta = "JS8 frames wait for the next 15 s UTC slot; the time-dependent start makes a sample compare meaningless";
        break;
    case TxPayloadKind::Hamdrm:
        add(v, {
            text("hamdrm.call", "Callsign", "Your callsign, sent with the file.", TxPField::Callsign),
            META(file("hamdrm.file", "File to send", "Any file, sent as a HamDRM object. Empty: the test picture."), "a file of the test machine; tests/hfdig sends files"),
        });
        for (auto& d : v) d.probeSec = 3.0;
        break;
    case TxPayloadKind::Sstv: {
        static std::vector<const char*> names;
        static std::once_flag once;
        std::call_once(once, [] { for (int i = 0; i < sstvModeCount(); i++) names.push_back(sstvModeInfo(i).name); });
        add(v, {
            PROBE(text("sstv.call", "Callsign", "Stamped on the picture.", TxPField::Callsign), 8.0),
            META(file("sstv.file", "Picture (PPM)", "A P6 PPM; any other or empty file sends the test picture."), "a file of the test machine"),
            S(FLD(TV(pick("sstv.mode", "SSTV mode", "The picture format and speed (Martin M1 and Scottie S1 are common).", 0, names), 2), TxPField::Variant)),
        });
        for (auto& d : v) if (!d.probeSec) d.probeSec = 1.5;
        break;
    }
    case TxPayloadKind::Freedv:   // the speech goes to Codec 2 at 8 kHz (txPullPcm8k): the analog audio chain does not apply
        audioSource(v);
        v.push_back(META(A(FLD(num("audio.tone", "Test tone", "The pitch of the test tone, used when the audio input is the test tone.", "Hz", 1000, 100, 3500, "%.0f"), TxPField::ToneHz), TxPGroup::Audio),
                         "the FreeDV encoder needs libcodec2 (user-installed), not on the test machine"));
        break;
    case TxPayloadKind::Pager:
        add(v, {
            FLD(TV(intr("pager.capcode", "Capcode", "The pager's address (POCSAG: up to 2097151).", "", 0, 0, 2097151), 1234567), TxPField::Address),
            text("pager.text", "Text", "An alphanumeric page; empty: a tone-only page.", TxPField::Text),
            S(FLD(TV(pick("pager.speed", "Speed", "POCSAG at 512, 1200 or 2400 baud, or FLEX.", 1, {"POCSAG 512", "POCSAG 1200", "POCSAG 2400", "FLEX 1600/2", "FLEX 3200/2", "FLEX 3200/4", "FLEX 6400/4"}), 0), TxPField::BaudIdx)),
            W(A(FLD(intr("pager.function", "Function bits", "The POCSAG function (0 to 3): which beep or display the pager uses (3 is alphanumeric).", "", 3, 0, 3), TxPField::Function), TxPGroup::Framing), "pager.speed<3"),
        });
        v[0].group = TxPGroup::Payload; v[0].level = TxPLevel::Simple;
        for (auto& d : v) d.probeSec = 1.5;
        break;
    case TxPayloadKind::Packet:
        add(v, {
            text("aprs.call", "Callsign", "Your callsign with SSID (A61AB-9).", TxPField::Callsign),
            TT(text("aprs.dest", "Destination", "The AX.25 destination (APRS).", TxPField::Dest), "APZ001"),
            TT(text("aprs.path", "Path", "The digipeater path (WIDE1-1,WIDE2-1).", TxPField::Path), "WIDE2-2"),
            text("aprs.text", "Comment", "The status or position comment.", TxPField::Text),
            S(FLD(TV(num("aprs.lat", "Latitude", "Your position (0 and 0: a status instead of a position).", "°", 0, -90, 90, "%.5f"), 25.2), TxPField::Lat)),
            S(FLD(TV(num("aprs.lon", "Longitude", "Your position.", "°", 0, -180, 180, "%.5f"), 55.3), TxPField::Lon)),
            A(FLD(pick("aprs.baud", "Speed", "1200 baud AFSK (VHF APRS) or 9600 baud G3RUH.", 0, {"1200 baud", "9600 baud"}), TxPField::BaudIdx), TxPGroup::Modulation),
        });
        for (auto& d : v) d.probeSec = 1.0;
        break;
    case TxPayloadKind::Ais:
        add(v, {
            S(FLD(TV(intr("ais.mmsi", "MMSI", "The vessel's nine-digit identity.", "", 0, 0, 999999999), 247123456), TxPField::Address)),
            text("ais.call", "Callsign", "The vessel's callsign (static data).", TxPField::Callsign),
            TT(text("ais.name", "Vessel name", "The vessel's name (static data).", TxPField::Name), "ONAIR TEST"),
            S(FLD(TV(num("ais.lat", "Latitude", "The vessel's position.", "°", 0, -90, 90, "%.5f"), 25.2), TxPField::Lat)),
            S(FLD(TV(num("ais.lon", "Longitude", "The vessel's position.", "°", 0, -180, 180, "%.5f"), 55.3), TxPField::Lon)),
        });
        for (auto& d : v) d.probeSec = 2.5;
        break;
    case TxPayloadKind::AdsB:
        add(v, {
            text("adsb.call", "Flight ID", "The identification sent (up to eight characters); empty: the demo aircraft.", TxPField::Callsign),
            W(S(FLD(TV(hexr("adsb.icao", "ICAO address", "The 24-bit aircraft address (used with a flight ID).", 0, 0, 0xFFFFFF, "%06X"), 0x4B1234), TxPField::Address)), nullptr),
        });
        for (auto& d : v) d.probeSec = 0.1;
        break;
    case TxPayloadKind::VoiceFm:
        add(v, {
            TT(text("bfm.ps", "Station name (PS)", "Eight characters the radio shows.", TxPField::Name), "ONAIRFM"),
            PROBE(text("bfm.rt", "Radio text (RT)", "Up to 64 characters of scrolling text.", TxPField::Text), 1.0),
            S(FLD(TV(hexr("bfm.pi", "RDS PI", "The programme identification code (four hex digits).", 0x4A21, 0, 0xFFFF, "%04X"), 0x1234), TxPField::RdsPi)),
            custom("bfm.live", "Live RDS control", "Programme type, flags, the clock and the command port while on the air."),
        });
        audioSource(v);
        add(v, {
            A(FLD(num("fm.deviation", "Deviation", "How far the audio swings the frequency. Broadcast FM uses 75 kHz.", "Hz", 75000, 5000, 100000, "%.0f"), TxPField::Deviation), TxPGroup::Modulation),
            AUD(A(FLD(num("fm.preemph", "Pre-emphasis", "Boosts the high notes before sending: 50 µs in Europe and most of the world, 75 µs in the Americas.", "µs", 50, 25, 100, "%.0f"), TxPField::Preemph), TxPGroup::Modulation)),
            A(flag("bfm.stereo", "Stereo", "Sends the 19 kHz pilot and the stereo difference signal.", true), TxPGroup::Modulation),
            W(A(num("bfm.pilotPct", "Pilot level", "The 19 kHz stereo pilot, as a share of the full deviation.", "%", 9, 0, 20, "%.1f"), TxPGroup::Modulation), "bfm.stereo=1"),
            A(flag("bfm.rds", "RDS", "Sends the station name, radio text and programme type on a 57 kHz subcarrier.", true), TxPGroup::Framing),
            W(A(num("bfm.rdsDevKhz", "RDS level", "How far the RDS subcarrier deviates the carrier.", "kHz", 2, 0.2, 7.5, "%.1f"), TxPGroup::Framing), "bfm.rds=1"),
            W(A(intr("bfm.pty", "Programme type", "The RDS programme type code (0 to 31; 10 is pop music).", "", 10, 0, 31), TxPGroup::Framing), "bfm.rds=1"),
            W(A(flag("bfm.tp", "Traffic programme", "The RDS flag for a station that carries traffic announcements.", true), TxPGroup::Framing), "bfm.rds=1"),
            W(A(flag("bfm.ta", "Traffic announcement now", "The RDS flag that a traffic announcement is on the air.", false), TxPGroup::Framing), "bfm.rds=1"),
            W(META(A(flag("bfm.ct", "Send the clock time", "Sends the computer's UTC time in the RDS clock group so radios set their clock.", false), TxPGroup::Framing),
                   "the clock group goes out at the start of each minute; tests/fm checks it"), "bfm.rds=1"),
            W(A(flag("bfm.music", "Music (not speech)", "The RDS flag for music instead of speech.", true), TxPGroup::Framing), "bfm.rds=1"),
            A(FLD(num("audio.tone", "Left test tone", "The pitch on the left channel, used when the audio input is the test tone.", "Hz", 1000, 100, 15000, "%.0f"), TxPField::ToneHz), TxPGroup::Audio),
            A(num("bfm.rightHz", "Right test tone", "The pitch on the right channel (0 = three times the left).", "Hz", 0, 0, 15000, "%.0f"), TxPGroup::Audio),
            A(num("bfm.leftAmp", "Left level", "The left test tone, as a share of full audio.", "", 0.5, 0, 1, "%.2f"), TxPGroup::Audio),
            A(num("bfm.rightAmp", "Right level", "The right test tone, as a share of full audio.", "", 0.5, 0, 1, "%.2f"), TxPGroup::Audio),
            A(num("bfm.level", "RF level", "The size of the signal as a share of full scale.", "", 0.3, 0.05, 1.0, "%.2f"), TxPGroup::Output),
        });
        for (auto& d : v) if (!d.probeSec) d.probeSec = 0.15;
        break;
    case TxPayloadKind::VoiceAir:
        audioSource(v);
        add(v, {
            A(pick("am.type", "Type", "AM sends the carrier and both sidebands; DSB only the sidebands.", 0, {"AM (carrier and sidebands)", "DSB (suppressed carrier)"}), TxPGroup::Modulation),
            A(num("am.depthPct", "Modulation depth", "How far the audio swings the signal; more than 100 % over-modulates.", "%", 85, 0, 120, "%.0f"), TxPGroup::Modulation),
            A(num("am.carrierLevel", "Carrier level", "The carrier as a share of full scale (at 85 % depth the peaks reach about full scale).", "", 0.55, 0.05, 1.0, "%.2f"), TxPGroup::Output),
        });
        voiceAudio(v, 0, 4500);
        break;
    case TxPayloadKind::Qo100:
        v.push_back(custom("qo100.path", "QO-100 path", "SSB of the microphone, or DATV (DVB-S/S2) of a file."));
        if (c.variant == 1) {
            tvPayload(v, "dvbs");
            dvbsRows(v, 1e6);   // QO-100 DATV: 1 Msym/s in the wide transponder
            break;
        }
        audioSource(v);
        add(v, {
            A(FLD(pick("ssb.sideband", "Sideband", "Which sideband is sent (QO-100 SSB uses USB).", 0, {kSideband[0], kSideband[1]}, 2), TxPField::Kind), TxPGroup::Modulation),
            A(num("ssb.carrierPct", "Carrier injection", "0 is the normal suppressed carrier; a little carrier helps a receiver lock.", "%", 0, 0, 100, "%.0f"), TxPGroup::Modulation),
            A(num("ssb.level", "Peak level", "The largest signal as a share of full scale.", "", 0.4, 0.05, 1.0, "%.2f"), TxPGroup::Output),
        });
        voiceAudio(v, 300, 2700);
        break;
    case TxPayloadKind::VoiceDigital: {
        v.push_back(custom("voice.note", "Vocoder", "What the vocoder can encode here, and the analog fall-back."));
        if (id == "dmr") {
            add(v, {
                text("dmr.call", "Callsign", "Your callsign (sent in the talker alias).", TxPField::Callsign),
                S(FLD(TV(intr("dmr.id", "DMR ID", "Your radio ID (up to 16776415).", "", 0, 0, 16776415), 2345678), TxPField::Address)),
                S(FLD(pick("dmr.call.kind", "What to send", "A group or private voice call, an SMS, a GPS position, a kerchunk, or Tier III registration.", 0,
                           {"Group voice call", "Private voice call", "All call", "SMS text message", "LRRP / GPS position", "Kerchunk (open repeater)", "Tier III registration", "Tier III voice request"}), TxPField::Kind)),
                S(FLD(TV(intr("dmr.tg", "Talkgroup", "The talkgroup of a group call, SMS or position.", "", 0, 0, 16776415), 91), TxPField::Talkgroup)),
                W(TT(text("dmr.dest", "Radio ID called", "The radio ID of a private call or the Tier III system.", TxPField::Dest), "3100001"), "dmr.call.kind=1|6|7"),
                W(TT(text("dmr.sms", "Message text", "The SMS text.", TxPField::Text), "HELLO DMR"), "dmr.call.kind=3"),
                W(FLD(TV(num("dmr.lat", "Latitude", "The position sent.", "°", 0, -90, 90, "%.6f"), 25.2), TxPField::Lat), "dmr.call.kind=4"),
                W(FLD(TV(num("dmr.lon", "Longitude", "The position sent.", "°", 0, -180, 180, "%.6f"), 55.3), TxPField::Lon), "dmr.call.kind=4"),
                A(FLD(TV(intr("dmr.cc", "Colour code", "The repeater's colour code (0 to 15).", "", 1, 0, 15), 7), TxPField::ColorCode), TxPGroup::Framing),
                A(FLD(pick("dmr.slot", "Timeslot", "The repeater timeslot.", 0, {"1", "2"}, 1), TxPField::Slot), TxPGroup::Framing),
                META(custom("dmr.slotSync", "Slot sync", "Waits for the repeater's inbound slot when the DMR receiver heard it in the last 10 s."), "only when the DMR receiver heard a repeater (tests/dmr slot sync)"),
                HID(META(A(flag("dmr.aligned", "Aligned channel timing", "The repeater pairs inbound slot 2 with outbound slot 1.", false), TxPGroup::Timing), "with slot sync only")),
                HID(META(A(num("dmr.latencyMs", "Radio latency", "The radio's delay to the air.", "ms", 0, -100, 200, "%.2f"), TxPGroup::Timing), "with slot sync only")),
            });
        } else if (id == "p25" || id == "nxdn" || id == "dpmr") {
            add(v, {
                S(FLD(TV(intr("dv.id", "Radio ID", id == "p25" ? "Your unit ID (24 bits)." : "Your radio ID.", "", 0, 0, 16777215), 123), TxPField::Address)),
                W(TT(text("dv.dest", "Talkgroup / called ID", "The talkgroup or radio called, as a number (empty: the mode's test group). Used with your radio ID.", TxPField::Dest), "777"), "dv.id>0"),
            });
            if (id != "p25") v.push_back(W(TT(text("dv.text", "Short data text", "A short text sent with the call. Used with your radio ID.", TxPField::Text), "HELLO"), "dv.id>0"));
        } else if (id == "dstar") {
            add(v, {
                text("dstar.my", "My callsign", "MYCALL in the header.", TxPField::Callsign),
                TT(text("dstar.ur", "Your callsign", "URCALL (CQCQCQ, or a callsign / reflector command).", TxPField::Dest), "CQCQCQ"),
                text("dstar.text", "Slow-data text", "The 20-character message in the slow data.", TxPField::Text),
            });
        } else if (id == "m17") {
            add(v, {
                text("m17.src", "Callsign", "The source callsign.", TxPField::Callsign),
                TT(text("m17.dst", "Destination", "The destination (@ALL, a callsign or a reflector module).", TxPField::Dest), "N0ABC"),
                text("m17.text", "Packet text", "Sent as a packet when there is no live voice.", TxPField::Text),
            });
        }
        // TETRA: the downlink test pattern only; nothing to type
        if (!txCanEncodeVoice(m) || c.analogVoice) analogSwitch(v, c);
        else { audioSource(v); }
        for (auto& d : v) if (!d.probeSec) d.probeSec = 0.6;
        break;
    }
    case TxPayloadKind::Marine:
        if (c.subId == "wefax") { analogSwitch(v, c); break; }
        add(v, {
            S(SET(TV(pick("marine.service", "Service", "NAVTEX text, DSC on MF/HF or VHF channel 70, or the weather fax test chart.", 0, {"NAVTEX", "DSC MF/HF", "Weather fax (test chart)", "DSC VHF ch 70"}, 1), 3),
                  [](TxRequest& r, double x) { r.synth.modeOpt[0] = (int)x; })),
            W(text("navtex.text", "Message", "The NAVTEX message (ZCZC and NNNN are added when missing).", TxPField::Text), "marine.service=0"),
            W(S(FLD(TV(intr("dsc.mmsi", "MMSI", "The calling station's nine-digit identity.", "", 0, 0, 999999999), 247123456), TxPField::Address)), "marine.service=1|3"),
        });
        for (auto& d : v) d.probeSec = 8.0;   // NAVTEX phasing and the DSC dot pattern come first
        break;
    case TxPayloadKind::AcarsFamily:
        add(v, {
            TT(text("acars.reg", "Aircraft registration", "The registration sent in the downlink.", TxPField::Callsign), "A6-ONA"),
            text("acars.text", "Text", "The free text of the downlink.", TxPField::Text),
        });
        for (auto& d : v) d.probeSec = 1.5;
        break;
    case TxPayloadKind::Sonde:
        add(v, {
            TT(text("sonde.serial", "Serial", "The RS41 serial number (S1234567).", TxPField::Callsign), "S7654321"),
            S(FLD(TV(num("sonde.lat", "Latitude", "The sonde's position.", "°", 0, -90, 90, "%.5f"), 25.2), TxPField::Lat)),
            S(FLD(TV(num("sonde.lon", "Longitude", "The sonde's position.", "°", 0, -180, 180, "%.5f"), 55.3), TxPField::Lon)),
        });
        for (auto& d : v) d.probeSec = 1.2;
        break;
    case TxPayloadKind::Same:
        v.push_back(TT(text("same.text", "Header", "The SAME header after ZCZC- (ZCZC-WXR-RWT-000000+ is added when it does not start with ZCZC).", TxPField::Text), "0030-3101200-ONAIR"));
        v.back().probeSec = 1.0;
        break;
    case TxPayloadKind::Ale:
        add(v, {
            text("ale.from", "Callsign", "Your ALE address.", TxPField::Callsign),
            TT(text("ale.to", "Called station", "The address called (empty: CQ).", TxPField::Dest), "N0ABC"),
            text("ale.text", "AMD message", "The automatic message display text.", TxPField::Text),
        });
        for (auto& d : v) d.probeSec = 4.0;
        break;
    case TxPayloadKind::VorIls:
        add(v, {
            TT(text("vor.ident", "Ident", "The Morse identification, up to four letters.", TxPField::Callsign), "DXB"),
            S(SET(pick("vor.kind", "Navaid", "A VOR, a localizer, a glide slope or a marker beacon.", 0, {"conventional VOR", "Doppler VOR", "localizer", "glide slope", "outer marker", "middle marker", "inner marker"}),
                  [](TxRequest& r, double x) { r.synth.modeOpt[0] = (int)x; })),
            W(S(SET(TV(num("vor.radial", "Radial", "The bearing a receiver reads from the VOR.", "°", 0, 0, 359.9, "%.1f"), 90), [](TxRequest& r, double x) { r.synth.modeVal[0] = x; })), "vor.kind<2"),
            W(S(SET(TV(num("ils.ddm", "DDM", "The difference in depth of modulation (90 Hz minus 150 Hz): 0 is on the course.", "", 0, -0.4, 0.4, "%.3f"), 0.1), [](TxRequest& r, double x) { r.synth.modeVal[0] = x; })), "vor.kind=2|3"),
        });
        for (auto& d : v) d.probeSec = 2.0;
        break;
    case TxPayloadKind::LoraRaw:
        add(v, {
            text("loraraw.text", "Payload", "The bytes of one LoRa frame (as text).", TxPField::Text),
            A(intr("lora.sf", "Spreading factor", "SF7 to SF12: each step up doubles the time on air and reaches further.", "", 7, 6, 12), TxPGroup::Modulation),
            A(pick("lora.bwIdx", "Bandwidth", "The LoRa bandwidth.", 1, {"62.5 kHz", "125 kHz", "250 kHz", "500 kHz"}), TxPGroup::Modulation),
            A(pick("lora.cr", "Coding rate", "The forward error correction: 4/5 is the least, 4/8 the most.", 0, {"4/5", "4/6", "4/7", "4/8"}, 5), TxPGroup::Modulation),
            A(intr("lora.preamble", "Preamble", "The up-chirps before the sync word.", "", 8, 6, 64), TxPGroup::Framing),
            A(hexr("lora.sync", "Sync word", "The sync word (0x12 private, 0x34 LoRaWAN, 0x2B Meshtastic).", 0x12, 0, 255, "0x%02X"), TxPGroup::Framing),
            A(flag("lora.crc", "CRC", "A checksum after the payload.", true), TxPGroup::Framing),
            TV(A(num("lora.gapSec", "Pause between frames", "The frame is sent again after this pause.", "s", 2, 0.1, 600, "%.1f"), TxPGroup::Timing), 0.1),
        });
        for (auto& d : v) d.probeSec = 0.4;
        break;
    case TxPayloadKind::Dect:
        v.push_back(S(FLD(TV(hexr("dect.rfpi", "RFPI", "The base station's identity (40 bits, here up to 32).", 0, 0, 0xFFFFFFFF, "%08X"), 0x12345678), TxPField::Address)));
        v.back().group = TxPGroup::Payload;
        v.back().probeSec = 0.1;
        break;
    case TxPayloadKind::CubeSat:
        add(v, {
            text("cubesat.call", "Callsign", "The source of the AX.25 frame.", TxPField::Callsign),
            TT(text("cubesat.dest", "Destination", "The AX.25 destination (empty: CQ).", TxPField::Dest), "N0ABC"),
            text("cubesat.text", "Payload text", "The information field of the frame.", TxPField::Text),
            A(FLD(pick("cubesat.baud", "Speed", "1200 baud AFSK or 9600 baud G3RUH.", 0, {"1200 baud AFSK", "9600 baud G3RUH"}), TxPField::BaudIdx), TxPGroup::Modulation),
        });
        for (auto& d : v) d.probeSec = 1.0;
        break;
    case TxPayloadKind::Mesh:
        add(v, {
            text("mesh.text", "Text", "Empty: the demo LoRa / mesh scene. With a text: your own Meshtastic node sends it.", TxPField::Text),
            custom("mesh.node", "Your node", "Long and short name, node ID, callsign, channel, PSK and hop limit.", TxPField::Callsign),
        });
        for (auto& d : v) d.probeSec = 0.5;
        analogSwitch(v, c);
        break;
    case TxPayloadKind::LoraAprs: {   // txLoraAprsRadio (tx.h)
        add(v, {
            text("laprs.call", "Callsign", "Your callsign with SSID.", TxPField::Callsign),
            TT(text("laprs.dest", "Destination", "The TNC2 destination (empty: APRS).", TxPField::Dest), "APLRT1"),
            TT(text("laprs.path", "Path", "The digipeater path (empty: WIDE1-1; - for none).", TxPField::Path), "WIDE2-1"),
            text("laprs.text", "Comment", "The status or position comment.", TxPField::Text),
            S(FLD(TV(num("laprs.lat", "Latitude", "Your position (0 and 0: a status instead).", "°", 0, -90, 90, "%.5f"), 25.2), TxPField::Lat)),
            S(FLD(TV(num("laprs.lon", "Longitude", "Your position.", "°", 0, -180, 180, "%.5f"), 55.3), TxPField::Lon)),
            S(pick("lora.preset", "LoRa settings", "The LoRa APRS networks: 125 kHz, preamble 8, sync word 0x12, CRC on. Own settings: every value below can be changed.", 0,
                   {"EU: 433.775 MHz, SF12, 4/5", "PL: 434.855 MHz, SF9, 4/7", "UK: 439.9125 MHz, SF12, 4/5", "915 MHz, SF12, 4/5", "Own settings"})),
            W(A(intr("lora.sf", "Spreading factor", "SF7 to SF12: each step up doubles the time on air and reaches further.", "", 12, 7, 12), TxPGroup::Modulation), "lora.preset=4"),
            W(A(pick("lora.bwIdx", "Bandwidth", "The LoRa bandwidth; LoRa APRS uses 125 kHz.", 1, {"62.5 kHz", "125 kHz", "250 kHz", "500 kHz"}), TxPGroup::Modulation), "lora.preset=4"),
            W(A(pick("lora.cr", "Coding rate", "The forward error correction: 4/5 is the least, 4/8 the most.", 0, {"4/5", "4/6", "4/7", "4/8"}, 5), TxPGroup::Modulation), "lora.preset=4"),
            W(A(intr("lora.preamble", "Preamble", "The up-chirps before the sync word; LoRa APRS uses 8.", "", 8, 6, 64), TxPGroup::Framing), "lora.preset=4"),
            W(A(hexr("lora.sync", "Sync word", "LoRa APRS uses 0x12; LoRaWAN 0x34, Meshtastic and MeshCom 0x2B.", 0x12, 0, 255, "0x%02X"), TxPGroup::Framing), "lora.preset=4"),
            W(A(flag("lora.crc", "CRC", "A checksum after the packet. LoRa APRS receivers expect it.", true), TxPGroup::Framing), "lora.preset=4"),
            META(A(num("lora.gapSec", "Pause between packets", "The packet is sent again after this pause, until Stop.", "s", 10, 1, 600, "%.0f"), TxPGroup::Timing), "the second packet starts after several seconds; tests/tx/test_tx_loraaprs.cpp"),
        });
        for (auto& d : v) d.probeSec = 0.3;
        break;
    }
    case TxPayloadKind::Kaal:
        add(v, {
            custom("kaal.panel", "KAAL", "Callsign, grid, position, power, bio text, beacon, voice codec and frame version (the KAAL panel).", TxPField::Callsign),
            HID(custom("kaal.grid", "Grid", "", TxPField::Grid)), HID(custom("kaal.lat", "Latitude", "", TxPField::Lat)), HID(custom("kaal.lon", "Longitude", "", TxPField::Lon)),
            HID(custom("kaal.text", "Bio text", "", TxPField::Text)),
            HID(custom("kaal.powerDbm", "Power", "")), HID(custom("kaal.beacon", "Beacon", "")), HID(custom("kaal.bitrate", "Bit rate", "")), HID(custom("kaal.frame", "Frame version", "")),
            HID(custom("kaal.v4profile", "v4 profile", "")), HID(custom("kaal.v4voice", "v4 voice slots", "")), HID(custom("kaal.v4robust", "v4 robust data", "")), HID(custom("kaal.linkQuality", "Link quality", "")),
        });
        audioSource(v);
        break;
    case TxPayloadKind::Kait:
        add(v, {
            custom("kait.panel", "KAIT", "Callsign, picture and profile (the KAIT panel).", TxPField::Callsign),
            HID(custom("kait.file", "Picture", "", TxPField::File)), HID(custom("kait.profile", "Profile", "", TxPField::Variant)), HID(custom("kait.gap", "Pause between bursts", "")),
        });
        break;
    case TxPayloadKind::Kavb:
        add(v, {
            custom("kavb.panel", "KAVB", "Callsign, file or test stream, profile and FEC (the KAVB panel).", TxPField::Callsign),
            HID(custom("kavb.file", "File", "", TxPField::File)), HID(custom("kavb.profile", "Profile", "")), HID(custom("kavb.fec", "FEC profile", "")), HID(custom("kavb.seconds", "Test stream", "")),
        });
        break;
    case TxPayloadKind::Kapp:
        add(v, {
            custom("kapp.panel", "KAPP", "Callsign, message or file, profile and passes (the KAPP panel).", TxPField::Callsign),
            HID(custom("kapp.text", "Message", "", TxPField::Text)), HID(custom("kapp.file", "File", "", TxPField::File)), HID(custom("kapp.profile", "Profile", "")), HID(custom("kapp.repeat", "Passes", "")),
        });
        break;
    case TxPayloadKind::Kaft:
        add(v, {
            custom("kaft.panel", "KAFT", "Callsign, file, width, profile and passes (the KAFT panel).", TxPField::Callsign),
            HID(custom("kaft.file", "File", "", TxPField::File)), HID(custom("kaft.profile", "Profile", "")), HID(custom("kaft.width", "Width", "")), HID(custom("kaft.repeat", "Passes", "")),
            HID(custom("kaft.session", "Session", "")),
        });
        break;
    case TxPayloadKind::Kaws:
        add(v, {
            custom("kaws.panel", "KAWS", "Callsign, grid, recipient, message, kind, profile, passes and slot timing (the KAWS panel).", TxPField::Callsign),
            HID(custom("kaws.grid", "Grid", "", TxPField::Grid)), HID(custom("kaws.dest", "Recipient", "", TxPField::Dest)), HID(custom("kaws.text", "Message", "", TxPField::Text)),
            HID(custom("kaws.kind", "Kind", "")), HID(custom("kaws.profile", "Profile", "")), HID(custom("kaws.repeat", "Passes", "")), HID(custom("kaws.eirp", "EIRP", "")),
            HID(custom("kaws.lat", "Latitude", "", TxPField::Lat)), HID(custom("kaws.lon", "Longitude", "", TxPField::Lon)),
            HID(custom("kaws.align", "Wait for the slot", "")), HID(custom("kaws.msgid", "Message ID", "")), HID(custom("kaws.utc", "Start time (test hook)", "")),
        });
        break;
    case TxPayloadKind::WeatherScene: case TxPayloadKind::SatScene: case TxPayloadKind::Flarm: case TxPayloadKind::Zigbee: case TxPayloadKind::Dji:
    case TxPayloadKind::HfBeacon: case TxPayloadKind::Vdes: case TxPayloadKind::Uplink: case TxPayloadKind::Mac:
        if (id == "winlinkpkt" || id == "winlinkardop") {   // the Winlink screen fills the frames
            v.push_back(custom("winlink.session", "Winlink session", "The frames of the Winlink screen's session (empty: one test frame).", TxPField::Text));
            break;
        }
        if (id == "nfc") break;   // the reader frames are fixed
        v.push_back(A(custom("scene.options", "Test scene", "This generator sends its built-in test scene; its settings are shared with the receiver's test signal (noise and offsets do not apply here)."), TxPGroup::Modulation));
        analogSwitch(v, c);
        break;
    }
    outputCommon(v);
    return v;
}

bool cond(const std::string& w, const std::vector<D>& rows, const std::map<std::string, double>& num) {
    size_t opPos = w.find_first_of("=<>!");
    if (opPos == std::string::npos) return true;
    const std::string key = w.substr(0, opPos);
    std::string op(1, w[opPos]);
    size_t valPos = opPos + 1;
    if (op == "!" && valPos < w.size() && w[valPos] == '=') { op = "!="; valPos++; }
    double have = 0;
    const auto it = num.find(key);
    if (it != num.end()) have = it->second;
    else if (const D* d = txParamFind(rows, key)) have = d->def;
    if (op == ">") return have > atof(w.c_str() + valPos);
    if (op == "<") return have < atof(w.c_str() + valPos);
    bool any = false;
    size_t p = valPos;
    while (p <= w.size()) {
        size_t e = w.find('|', p);
        if (e == std::string::npos) e = w.size();
        if (std::fabs(have - atof(w.substr(p, e - p).c_str())) < 1e-9) any = true;
        p = e + 1;
    }
    return op == "!=" ? !any : any;
}

} // namespace

const std::vector<TxParamDef>& txParams(const TxModeSpec& m, const TxParamCtx& ctx) {
    static std::mutex mu;
    static std::map<std::string, std::vector<TxParamDef>> cache;
    const std::string key = std::string(m.id ? m.id : "") + (ctx.analogVoice ? "+a" : "") + (ctx.variant == 1 ? "+v" : "") + "/" + ctx.subId;
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, build(m, ctx)).first;
    return it->second;
}

const TxParamDef* txParamFind(const std::vector<TxParamDef>& rows, const std::string& key) {
    for (const auto& d : rows) if (key == d.key) return &d;
    return nullptr;
}

bool txParamShown(const TxParamDef& d, const std::vector<TxParamDef>& rows, const std::map<std::string, double>& num) {
    if (d.hidden) return false;
    if (!d.when) return true;
    return cond(d.when, rows, num);
}

double txParamValue(const TxParamDef& d, double v) {
    if (d.type != TxPType::Choice) return v;
    const int i = std::max(0, std::min((int)d.choices.size() - 1, (int)std::lround(v)));
    return d.values.empty() ? d.base + i : d.values[(size_t)i];
}

void txParamApply(const TxParamDef& d, double v, const std::string& txt, TxRequest& r) {
    if (d.type == TxPType::Text || d.type == TxPType::File) {
        switch (d.field) {
        case TxPField::Callsign: r.callsign = txt; break;
        case TxPField::Grid: r.grid = txt; break;
        case TxPField::Text: r.text = txt; break;
        case TxPField::Dest: r.dest = txt; break;
        case TxPField::Path: r.path = txt; break;
        case TxPField::File: r.file = txt; break;
        case TxPField::Name: r.name = txt; break;
        case TxPField::Network: r.network = txt; break;
        default: break;
        }
        return;
    }
    if (d.custom) return;   // the mode's own widget writes its values
    const double x = txParamValue(d, v);
    if (d.set) { d.set(r, x); return; }
    switch (d.field) {
    case TxPField::None: r.opt[d.key] = x; break;
    case TxPField::Address: r.address = (uint32_t)std::max(0.0, std::min(4294967295.0, std::round(x))); break;
    case TxPField::Lat: r.lat = x; break;
    case TxPField::Lon: r.lon = x; break;
    case TxPField::ToneHz: r.toneHz = x; break;
    case TxPField::Variant: r.variant = (int)std::lround(x); break;
    case TxPField::Kind: r.kind = (int)std::lround(x); break;
    case TxPField::BaudIdx: r.baudIdx = (int)std::lround(x); break;
    case TxPField::ShiftIdx: r.shiftIdx = (int)std::lround(x); break;
    case TxPField::Wpm: r.wpm = (int)std::lround(x); break;
    case TxPField::Dbm: r.dbm = (int)std::lround(x); break;
    case TxPField::Function: r.function = (int)std::lround(x); break;
    case TxPField::Reverse: r.reverse = x > 0.5; break;
    case TxPField::RdsPi: r.rdsPi = (int)std::lround(x); break;
    case TxPField::Slot: r.slot = (int)std::lround(x); break;
    case TxPField::ColorCode: r.colorCode = (int)std::lround(x); break;
    case TxPField::Talkgroup: r.talkgroup = (int)std::lround(x); break;
    case TxPField::BandwidthMhz: r.bandwidthMhz = x; break;
    case TxPField::Deviation: r.deviationHz = x; break;
    case TxPField::Preemph: r.preemphUs = x; break;
    default: r.opt[d.key] = x; break;
    }
}

void txParamsApplyNumbers(const std::vector<TxParamDef>& rows, const std::map<std::string, double>& num, TxRequest& r) {
    for (const auto& d : rows) {
        if (d.type == TxPType::Text || d.type == TxPType::File || d.custom) continue;
        const auto it = num.find(d.key);
        txParamApply(d, it == num.end() ? d.def : it->second, "", r);
    }
}

void txParamsSanitize(const TxModeSpec& m, const TxParamCtx& ctx, TxRequest& r) {
    const auto& rows = txParams(m, ctx);
    auto has = [&](TxPField f) { for (const auto& d : rows) if (d.field == f) return true; return false; };
    const TxRequest def;
    if (!has(TxPField::Callsign)) r.callsign.clear();
    if (!has(TxPField::Grid)) r.grid.clear();
    if (!has(TxPField::Text)) r.text.clear();
    if (!has(TxPField::Dest)) r.dest.clear();
    if (!has(TxPField::Path)) r.path.clear();
    if (!has(TxPField::File)) r.file.clear();
    if (!has(TxPField::Name)) r.name.clear();
    if (!has(TxPField::Network)) r.network.clear();
    if (!has(TxPField::Address)) r.address = 0;
    if (!has(TxPField::Lat)) r.lat = 0;
    if (!has(TxPField::Lon)) r.lon = 0;
    if (!has(TxPField::ToneHz)) r.toneHz = def.toneHz;
    if (!has(TxPField::Kind) && txPayloadKind(m.id ? m.id : "") != TxPayloadKind::VoiceAir) r.kind = 0;
    if (!has(TxPField::BaudIdx)) r.baudIdx = 0;
    if (!has(TxPField::ShiftIdx)) r.shiftIdx = 0;
    if (!has(TxPField::Reverse)) r.reverse = false;
    if (!has(TxPField::Talkgroup)) r.talkgroup = 0;
    if (!has(TxPField::Deviation)) r.deviationHz = 0;
    if (!has(TxPField::Variant) && !(m.id && std::string(m.id) == "qo100")) r.variant = 0;   // QO-100: the path (SSB / DATV) is the variant
}

const std::vector<std::string>& txParamRadioKeys() {
    static const std::vector<std::string> k = {"out.dcI", "out.dcQ", "out.iqGainDb", "out.iqPhaseDeg", "out.trimDb", "out.swapIq", "out.rampMs", "tx.totSec"};
    return k;
}

const char* txParamGroupName(TxPGroup g) {
    switch (g) {
    case TxPGroup::Payload: return "What to send";
    case TxPGroup::Audio: return "Audio";
    case TxPGroup::Modulation: return "Modulation";
    case TxPGroup::Filters: return "Filters";
    case TxPGroup::Framing: return "Framing";
    case TxPGroup::Timing: return "Timing";
    case TxPGroup::Output: return "Output";
    }
    return "";
}

std::vector<std::string> txParamWarnings(const TxModeSpec& m, const TxRequest& r) {
    std::vector<std::string> w;
    const std::string id = m.id ? m.id : "";
    if (id == "dvbt2") {
        const int fft = r.synth.tx.s2field1, gi = r.synth.tx.giIdx, pp = r.synth.tx.pp;
        // EN 302 755 table 59 (SISO): the pilot patterns allowed for each FFT size and guard interval
        static const char* const allowed[6][7] = {
            // 1/32      1/16         1/8          1/4      1/128   19/128      19/256
            {"PP4 PP5", "PP2 PP3", "PP2 PP3", "PP1", "", "", ""},                                 // 2K (0)
            {"PP4 PP5 PP7", "PP2 PP3 PP8", "PP2 PP3 PP8", "PP1 PP8", "PP7", "PP2 PP3 PP8", "PP2 PP3 PP8"},   // 8K (1)
            {"PP4 PP5", "PP2 PP3", "PP2 PP3", "PP1", "", "", ""},                                 // 4K (2)
            {"PP4 PP5", "PP2 PP3", "PP2 PP3", "PP1", "", "", ""},                                 // 1K (3)
            {"PP4 PP5 PP6 PP7", "PP2 PP3 PP4 PP8", "PP2 PP3 PP8", "PP1 PP8", "PP7", "PP2 PP3 PP8", "PP2 PP3 PP4 PP8"},   // 16K (4)
            {"PP4 PP6 PP7", "PP2 PP4 PP8", "PP2 PP8", "", "PP7", "PP2 PP8", "PP2 PP4 PP8"},            // 32K (5)
        };
        if (fft >= 0 && fft < 6 && gi >= 0 && gi < 7 && pp >= 0 && pp < 8) {
            const std::string a = allowed[fft][gi];
            char name[8]; snprintf(name, sizeof name, "PP%d", pp + 1);
            if (a.empty()) w.push_back("this guard interval is not allowed with this FFT size (EN 302 755 table 59); receivers may not lock");
            else if (a.find(name) == std::string::npos) w.push_back(std::string(name) + " is not allowed with this FFT size and guard interval (allowed: " + a + "); receivers may not lock");
        }
        if (r.synth.tx.ext && (fft == 0 || fft == 2 || fft == 3)) w.push_back("extended carriers exist only in 8K, 16K and 32K: the setting is ignored");
    }
    if (txPayloadKind(id) == TxPayloadKind::Ftx && r.kind == 2 && r.text.size() > 13) w.push_back("free text longer than 13 characters cannot be sent");
    if (id == "fm" && r.name.size() > 8) w.push_back("the station name has 8 characters: the rest is cut");
    if ((id == "packet" || id == "loraaprs") && r.callsign.empty()) w.push_back("no callsign: the demo stations are sent");
    return w;
}

} // namespace dect2
