// Marine receiver telemetry (NAVTEX, DSC, weather fax), shared by the receiver and the UI.
// The first members are copied into RxTelemetry (history plot, top bar): keep them and their meaning.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// One NAVTEX message (ITU-R M.540): ZCZC B1 B2 B3 B4, text, NNNN.
struct NavtexMessage {
    char station = '?';              // B1: transmitter identity A..Z ('?' when the character was damaged)
    char subject = '?';              // B2: A navigational warning, B meteorological warning, C ice, D search and rescue, E forecast, ...
    int number = -1;                 // B3 B4: 00 (urgent) .. 99, -1 when damaged
    std::string subjectName;         // text for B2
    std::string header;              // as received, e.g. "EA01" ('*' where a character could not be read)
    std::string text;                // body, lines separated by '\n', '*' where a character could not be read
    bool textCut = false;            // the text was shortened to keep the report small
    uint32_t chars = 0, errors = 0;  // body characters and unreadable ones
    float cer = 0;                   // character error rate: errors / chars
    bool complete = false;           // NNNN was received
    int repeats = 1;                 // the same message heard this many times
    double rxSec = 0;                // signal time of the end of the message, seconds since the start
    int64_t rxTime = 0;              // wall clock (unix seconds)
};

// One DSC call (ITU-R M.493). Codes are the symbol numbers of the recommendation; -1 = not in the call or unreadable.
struct DscCall {
    int format = -1;                 // 112 distress, 116 all ships, 114 group, 120 individual, 102 geographic area, 123 individual (automatic)
    std::string formatName;
    int category = -1;               // 100 routine, 106 ship's business, 108 safety, 110 urgency, 112 distress
    std::string categoryName;
    std::string fromMmsi;            // 9 digits ("" when unreadable)
    std::string to;                  // MMSI of the called station, or the area, or "all ships"
    bool distress = false;           // distress alert, distress relay or a call with category distress
    int nature = -1;                 // nature of distress, 100..112
    std::string natureName;
    bool hasPos = false;             // position of the ship in distress / the sender
    double lat = 0, lon = 0;         // degrees, north and east positive
    std::string posText;
    bool hasTime = false;
    int utcHour = 0, utcMin = 0;
    int telecmd1 = -1, telecmd2 = -1;
    std::string telecmdText;
    std::string freqRx, freqTx;      // proposed frequency or channel ("8291.0 kHz", "VHF ch 16", "")
    std::string distressMmsi;        // station in distress, for acknowledgements and relays
    int eos = -1;                    // 117 acknowledgement requested, 122 acknowledgement, 127 other
    bool eccOk = false;              // the error check character matched
    int erasures = 0;                // symbols that could not be read from either copy
    bool vhf = false;                // 1200 bd on channel 70 (else 100 bd MF/HF)
    double rxSec = 0;
    int64_t rxTime = 0;
    std::string text;                // one line for the table
};

struct MarineFaxInfo {
    int state = 0;                   // 0 waiting for the start tone, 1 phasing, 2 receiving, 3 stopped
    int ioc = 576, lpm = 120;
    int lines = 0, width = 0;
    double slantPpm = 0;
    double toneHz = 0;               // last tone measured
    double blackHz = 1500;           // black level measured in the phasing lines (nominal 1500): how far the receiver is mistuned
    int phasingLines = 0;
    double snrDb = 0;
    uint64_t imageSeq = 0;           // changes when latestImage() has something new
    int thumbRows = 0;               // rows of the 256 px wide preview the viewer may show (lines / scale)
};

struct MarineTelemetry {
    // ---- the part every mode has
    uint64_t seq = 0;            // grows with every report, never restarts
    int state = 0;               // 0 searching, 1 signal, 2 decoding (a decoder is locked / a fax is being received)
    double cfoHz = 0;            // carrier error: where the FSK centre or the fax tones sit against the nominal place
    float snrDb = 0;             // signal to noise ratio (noise taken in 3 kHz)
    bool dataValid = false;      // a message, a call or a fax line has been decoded
    uint64_t blocksOk = 0;       // NAVTEX messages without damaged characters, DSC calls with a good check, fax pictures
    uint64_t blocksBad = 0;      // NAVTEX messages with damage or cut short, DSC calls with a bad check

    // ---- marine
    int serviceSetting = 0;      // the selector: 0 auto, 1 NAVTEX, 2 DSC, 3 weather fax
    int serviceActive = 0;       // what the receiver has heard last: 0 nothing, 1 NAVTEX, 2 DSC (MF/HF), 3 fax, 4 DSC (VHF)
    bool navtexLocked = false, dscLocked = false;
    bool dscVhfActive = false;
    // signal
    double toneHighHz = 0, toneLowHz = 0;   // FSK tones measured relative to the carrier (0 Hz = the tuned frequency); 0 when no FSK signal
    double shiftHz = 0;                      // measured shift
    double baudEst = 0;                      // symbol rate from the timing loop
    double fskLevelDb = 0;                   // how far the FSK tones stand above the noise (dB, peak to median)
    std::vector<float> spectrumDb;           // audio spectrum, 342 bins from -500 Hz to +3500 Hz (11.7 Hz each), dB
    float specLoHz = -500, specHiHz = 3500;
    // messages
    std::vector<NavtexMessage> navtex;       // newest last, at most 100
    std::vector<DscCall> dsc;                // newest last, at most 100
    uint64_t navtexCount = 0, dscCount = 0;  // totals since the start
    MarineFaxInfo fax;
};

inline std::string marineSummary(const MarineTelemetry& t) {
    char b[200];
    const char* sv = t.serviceActive == 1 ? "NAVTEX" : t.serviceActive == 2 ? "DSC" : t.serviceActive == 4 ? "DSC VHF" : t.serviceActive == 3 ? "Fax" : "";
    if (t.state == 0) return "Marine: searching";
    if (t.serviceActive == 3) {
        snprintf(b, sizeof b, "Marine fax: %s, IOC %d, %d lpm, %d lines", t.fax.state == 0 ? "waiting for start tone" : t.fax.state == 1 ? "phasing" : t.fax.state == 2 ? "receiving" : "stopped", t.fax.ioc, t.fax.lpm, t.fax.lines);
        return b;
    }
    if (t.serviceActive == 0) return t.state == 1 ? "Marine: signal" : "Marine: decoding";
    snprintf(b, sizeof b, "Marine %s: %s, %llu NAVTEX, %llu DSC", sv, t.state == 2 ? "decoding" : "signal", (unsigned long long)t.navtexCount, (unsigned long long)t.dscCount);
    return b;
}

} // namespace dect2
