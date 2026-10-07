// DRM receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One service of the multiplex (FAC and SDC combined)
struct DrmService {
    int shortId = 0;
    uint32_t id = 0;               // 24 bit service identifier
    bool audio = true;             // false: data service
    std::string label;             // SDC type 1 ("" until received)
    int language = 0;              // FAC code, Table 18
    std::string languageName;      // from the FAC code, or the ISO 639-2 code of SDC type 12 when that came
    std::string country;           // ISO 3166 code of SDC type 12
    int programmeType = 0;         // Table 19 (audio services); for data services the application identifier
    std::string programmeName;
    bool conditionalAccess = false;
    // audio description (SDC type 9)
    int audioCoding = -1;          // 0 AAC, 3 xHE-AAC, -1 not known yet
    bool sbr = false;
    int audioMode = -1;            // 0 mono, 1 parametric stereo, 2 stereo
    int audioRateHz = 0;           // sampling rate of the core coder
    bool textMessage = false;
    int streamId = -1;
    int bitrateBps = 0;            // net rate of the stream that carries the service
    std::string codecText;         // "AAC + SBR + PS, 24 kHz, mono core"
};

struct DrmTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching, 1 partly locked (synchronised, the FAC is read), 2 locked and decoding (SDC and MSC)
    double cfoHz = 0;            // carrier offset: how far the reference frequency (carrier 0) is from the tuned frequency
    float snrDb = 0;             // signal to noise ratio of the cells, from the pilots (MER); 0 while there is no signal
    bool dataValid = false;      // the main service channel decodes (audio super frames come out)
    uint64_t blocksOk = 0, blocksBad = 0;   // audio frames received well and badly since the start (blocks = frames of the audio super frames)

    // ---- this mode's own fields below
    // signal and multiplex (from the FAC and SDC)
    int mode = -1;               // robustness mode 0 A, 1 B, 2 C, 3 D, 4 E; -1 until detected
    char modeName = '-';
    int occupancy = -1;          // spectrum occupancy code (Table 48); -1 until the FAC is read
    float bandwidthKhz = 0;      // nominal channel width
    int longInterleave = -1;     // 1: long (2 s), 0: short (400 ms); -1 unknown
    int mscQam = 0;              // 4, 16 or 64; 0 unknown
    int sdcQam = 0;
    int protA = 0, protB = 0;    // protection levels (SDC type 0)
    int protRateA = 0, protRateB = 0;   // overall code rate in percent (50, 60, 71, ...) of each part
    int numStreams = 0;
    int streamLenA[4] = {}, streamLenB[4] = {};   // bytes per logical frame
    int identity = -1;           // FAC identity of the frame being received (0 first of the super frame)
    int reconfiguration = 0;
    bool hierarchicalUnsupported = false;   // the FAC asks for a mapping this receiver does not decode
    std::vector<DrmService> services;
    int serviceAudioSelected = -1;   // short id of the service that is played, -1 none
    // time and date (SDC type 8)
    bool timeValid = false;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0;
    bool hasLocalOffset = false;
    int localOffsetHalfHours = 0;
    // measurements
    float levelDbfs = -120;      // level of the signal in the 48 kHz channel (relative to full scale)
    float sroPpm = 0;            // sample rate offset between the transmitter and the radio
    float dopplerHz = 0;         // Doppler spread (rate of change of the channel)
    float delaySpreadMs = 0;     // delay spread of the channel (rms of the impulse response)
    float quality = 0;           // 0 .. 1: share of the audio frames or FAC blocks received well over the last seconds, weighted by the SNR
    float timingMs = 0;          // position of the strongest path relative to the symbol start
    float correlation = 0;       // guard interval correlation (0 .. 1) at the symbol timing: how clean the OFDM structure is
    // counters
    uint64_t facOk = 0, facBad = 0, sdcOk = 0, sdcBad = 0, mscFramesOk = 0, mscFramesBad = 0;   // FAC blocks, SDC blocks, multiplex frames (CRC of the audio super frame header)
    // plots (capped)
    std::vector<float> chanDb;   // |H(k)| in dB over the occupied carriers (decimated to at most 512 points)
    int chanFirstCarrier = 0;    // carrier number of chanDb[0]
    float chanCarrierStep = 1;   // carriers per point
    float chanSpacingHz = 0;     // carrier spacing 1/Tu
    std::vector<float> cirDb;    // impulse response |h(t)|^2 in dB, from the pilots (at most 256 points)
    float cirStartMs = 0, cirStepMs = 0;   // delay of the first point and spacing
    std::vector<float> specDb;   // spectrum of the 48 kHz channel (256 points over -24 .. +24 kHz)
    std::vector<cf32> facConst, sdcConst, mscConst;   // equalised cells of the last FAC, SDC and MSC blocks (at most 1024 each)
    // audio
    int audioState = 0;          // 0 no audio stream, 1 frames received but no decoder for this format, 2 decoding
    std::string audioInfo;       // what is played, or why not
    uint64_t audioSamples = 0;   // 48 kHz samples produced
    std::string textMessage;     // text message of the audio service
    uint64_t textSegmentsOk = 0, textSegmentsBad = 0;
    std::string status;          // one line: what the receiver is doing
};

inline const char* drmServiceKind(const DrmService& s) { return s.audio ? "audio" : "data"; }

// One line for the command line and the log
inline std::string drmSummary(const DrmTelemetry& t) {
    char b[320];
    if (t.mode < 0) { snprintf(b, sizeof b, "DRM: searching  level %.0f dBFS", t.levelDbfs); return b; }
    const char* lab = t.services.empty() ? "" : t.services[0].label.c_str();
    snprintf(b, sizeof b, "DRM: mode %c %.1f kHz  state %d  SNR %.1f dB  CFO %+.1f Hz  FAC %llu/%llu  SDC %llu/%llu  audio ok %llu bad %llu  '%s'",
             t.modeName, t.bandwidthKhz, t.state, t.snrDb, t.cfoHz, (unsigned long long)t.facOk, (unsigned long long)t.facBad, (unsigned long long)t.sdcOk, (unsigned long long)t.sdcBad,
             (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad, lab);
    return b;
}

} // namespace dect2
