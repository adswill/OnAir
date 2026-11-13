// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What the QO-100 tool reports (RxTelemetry::qo100, standard 54). Frequencies are true downlink RF (the beacon correction applied when
// there is one), except where a field says IF.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct Qo100Beacon {
    double nominalHz = 0;
    bool found = false;
    double rfHz = 0;            // where it is, apparent RF (nominal LO, no correction): nominal - LNB error
    double snrDb = 0;           // CW beacons: the fine estimate's peak over the median; PSK beacon: level over the floor of the narrowband spectrum
};

struct Qo100Signal {            // a carrier or an SSB signal on the narrowband transponder
    double rfHz = 0;            // top of the signal (CW: the carrier), corrected
    double widthHz = 0;         // -6 dB width
    float levelDb = -200, snrDb = 0;
    int beacon = -1;            // 0 lower CW, 1 PSK, 2 upper CW, -1 a user signal
    int cls = 0;                // what the tracker makes of it (Qo100Class)
    int track = 0;              // its Qo100Track id (0 none yet)
};

struct Qo100Datv {              // a carrier on the wideband transponder
    double rfHz = 0;            // centre, corrected
    double ifHz = 0;            // where it is at the radio (what the DVB-S/S2 receiver tunes to)
    double bw3Hz = 0, symRate = 0;
    float snrDb = 0;
    bool looksDvbs = false;     // flat-topped: a DVB-S / S2 carrier
    bool beacon = false;        // the DATV beacon (10491.5 MHz, 1.5 Msym/s)
};


// ---- the narrowband auto decoder and the SSB conversation manager (qo100_auto.h) --------------------------------------------------------------

// What a signal on the transponder is taken to be (Qo100Track::cls)
enum Qo100Class { kQoUnknown = 0, kQoBeacon, kQoCarrier, kQoCw, kQoSsb, kQoSlot15, kQoFt4, kQoPsk, kQoRtty, kQoSstv, kQoWide, kQoClasses };
const char* qo100ClassName(int cls);   // "unknown", "beacon", "carrier", "CW", "SSB", "FT8/JS8", "FT4", "PSK", "RTTY", "SSTV", "wide digital"

// The decoders the user ticks (Qo100Receiver::setAutoDecoders), one bit each
enum Qo100Dec { kQoDecSsb = 1, kQoDecFt8 = 2, kQoDecFt4 = 4, kQoDecSstv = 8, kQoDecCw = 16, kQoDecPsk = 32, kQoDecJs8 = 64, kQoDecRtty = 128 };
const char* qo100DecoderName(int bit);   // "SSB", "FT8", "FT4", "SSTV", "CW", "PSK31", "JS8", "RTTY"

struct Qo100Track {             // one signal found on the narrowband transponder and followed over time
    int id = 0;
    double rfHz = 0;            // middle of the signal, corrected downlink frequency
    double loHz = 0, hiHz = 0;  // its edges (an SSB signal: the lower edge is about the dial + 300 Hz)
    double widthHz = 0;
    float levelDb = -200, snrDb = 0;
    int cls = kQoUnknown;
    bool active = false;        // on the air now (else only remembered)
    double firstSec = 0, lastSec = 0;   // receiver seconds
    float duty = 0;             // share of the last 30 s it was on
    int conv = 0;               // the SSB conversation it belongs to (0 none)
    int decMask = 0;            // the decoders that run on it now (Qo100Dec bits)
    std::string idName;         // the signal identifier's opinion ("" not asked or no opinion)
};

struct Qo100Decode {            // a decoded FT8 / FT4 / JS8 message
    uint64_t seq = 0;
    double utc = 0;             // UTC seconds of its slot
    int kind = 0;               // Qo100Dec bit
    double rfHz = 0;            // the tone-0 frequency on the downlink
    float snrDb = 0;
    std::string text, call, grid;
};

struct Qo100Text {              // a PSK31 / CW / RTTY signal's text
    int kind = 0;               // Qo100Dec bit
    double rfHz = 0;
    float snrDb = 0;
    double lastSec = 0;
    std::string text;
    std::vector<std::string> calls;
};

struct Qo100Slot {              // one decoder channel cut out of the passband
    int id = 0;
    int kind = 0;               // Qo100Dec bit
    double dialHz = 0;          // its USB dial on the downlink
    int tracks = 0;             // signals it serves
    float load = 0;             // share of one core, measured
    uint64_t results = 0;       // decodes / characters / pictures so far
};

struct Qo100Conversation {
    int id = 0;
    bool active = false;        // somebody spoke in the last few seconds
    double startSec = 0, lastSec = 0;     // receiver seconds
    double startUtc = 0, lastUtc = 0;     // UTC seconds
    double dialHz = 0;          // USB dial that tunes it (lower edge - 300 Hz), corrected
    double loHz = 0;            // lower edge of the voice
    float levelDb = -200;       // now (or the last)
    float peakDb = -200;
    double driftHzPerMin = 0;
    bool driftValid = false;
    int stations = 1;           // estimated number of stations
    double talkSec = 0;         // time with a signal
    std::vector<int> tracks;
    std::string snippet;        // speech to text, when that feature is there
    std::vector<std::string> calls;       // callsigns found in it
    bool recording = false;
    std::string recordPath;
    bool listening = false;
};

struct Qo100AutoTelemetry {
    unsigned decoders = 0;      // the ticked decoders (Qo100Dec bits)
    int maxSlots = 0;
    float cpuLoad = 0;          // time spent in the auto decoders per second of signal (1 = one core)
    float cpuLimit = 0;
    int dropped = 0;            // channels given up because of the CPU limit or the slot limit
    std::vector<Qo100Track> tracks;
    std::vector<Qo100Slot> slots;
    std::vector<Qo100Decode> decodes;     // oldest first, the last 500
    std::vector<Qo100Text> texts;
    uint64_t decodeTotal = 0;
    std::vector<std::shared_ptr<const struct HfdigSstvImage>> images;   // SSTV pictures, newest first (the one being received first)
    uint64_t imageSeq = 0;
    std::vector<Qo100Conversation> convs;           // newest first
    int listenConv = 0;         // the conversation Listen follows (0: the dial)
    int autoListen = 0;         // 0 off, 1 the loudest, 2 the newest
};

struct Qo100Telemetry {
    uint64_t seq = 0;
    int state = 0;              // 0 waiting / the narrowband transponder is outside the radio's band, 1 searching for the beacons, 2 locked, 3 held (beacons lost: the last correction is kept)
    double rate = 0;            // input sample rate, Hz
    double centreIfHz = 0;      // the radio's frequency
    double loHz = 0;            // the nominal LNB oscillator
    bool inverted = false;      // the LO is above the downlink: the spectrum arrives mirrored (the receiver turns it back)
    bool nbOk = false, wbOk = false;   // the narrowband / wideband transponder lies inside the radio's band
    // the LNB's frequency error, measured on the CW beacons: actual LO = nominal + lnbErrHz
    bool corrValid = false;
    bool corrApplied = true;    // false: automatic frequency correction is switched off (the error is measured but not applied)
    double lnbErrHz = 0, lnbErrPpm = 0;
    double driftHzPerMin = 0;   // over the last 30 s
    bool driftValid = false;
    double lockSec = 0;         // since the lock was taken
    uint64_t measurements = 0;
    std::vector<float> histT, histErr;   // seconds since the start, lnbErrHz: one point per measurement (the newest last)
    Qo100Beacon beacons[3];     // lower CW, PSK, upper CW
    // the narrowband transponder: full-resolution averaged spectrum (dBFS per bin), bin k at nbF0Hz + k * nbBinHz (corrected RF)
    std::vector<float> nb;
    double nbF0Hz = 0, nbBinHz = 1;
    float nbFloorDb = -200, nbPeakDb = -200;
    std::vector<Qo100Signal> signals;
    // the radio's whole band: averaged spectrum, bin k at wbF0Hz + k * wbBinHz (corrected RF, lowest first even when inverted)
    std::vector<float> wb;
    double wbF0Hz = 0, wbBinHz = 1;
    float wbFloorDb = -200, wbPeakDb = -200;
    std::vector<Qo100Datv> datv;
    // SSB listening
    bool listening = false;
    double dialHz = 0;          // USB suppressed carrier (corrected RF)
    float audioDb = -200;       // level in the 2.7 kHz passband
    double powerDb = -200;      // total input power, dBFS
    Qo100AutoTelemetry autoDec;
};

} // namespace dect2
