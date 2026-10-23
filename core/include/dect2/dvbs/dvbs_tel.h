// DVB-S/S2 receiver telemetry, shared by the receiver and the UI.
#pragma once
#include "dect2/common/ring.h"
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

// IP data carried by the signal (GSE, and MPE or ULE in the transport stream): see dvbs_ip.h. Only headers, counts and addresses: never payload.
struct DvbsIpFlow {
    std::string src, dst, protocol;
    int sport = -1, dport = -1;
    uint64_t packets = 0, bytes = 0;
    bool encrypted = false;          // the headers are good but the UDP checksums fail: the payload was changed in transit (link encryption); not decoded
};
struct DvbsIpLabel {                 // a GSE label: a terminal's address on an outbound
    std::string label;
    uint64_t pdus = 0, bytes = 0, ip = 0;
    std::vector<std::string> addrs;
    std::string modcods;             // "QPSK 5/6 41 %, 16APSK 3/4 59 %": where the bytes went (the terminal's link as the hub sees it)
};
struct DvbsMediaFlow {               // a multicast stream found in the IP traffic
    std::string dst, src, kind, codec;
    int port = 0;
    uint64_t packets = 0, bytes = 0, lost = 0;
    double kbps = 0;
    std::vector<std::string> services;   // MPEG-TS flows: the programmes found in them (names from their SDT), with their stream kinds
    bool scrambled = false;              // transport packets of the flow are scrambled: only labelled
};
struct DvbsSdp {                     // an announcement (SAP/SDP)
    std::string name, info, address, source, media;   // media: "audio 5004 (MPEG audio), video 5006 (H.264)"
    uint64_t seen = 0;
};
struct DvbsIpTel {
    bool active = false;             // IP packets are being decoded
    std::string source;              // "GSE (standard)", "GSE (length incl. header)", "GSE blind search", "MPE", "ULE", ... (several joined)
    uint64_t gseFields = 0, packets = 0, bytes = 0, ipv4 = 0, ipv6 = 0, fragments = 0, reassembled = 0, mpePackets = 0, ulePackets = 0;
    std::vector<std::pair<std::string, uint64_t>> protocols, udpPorts, tcpPorts;   // by name or port, most packets first
    std::vector<DvbsIpFlow> flows;   // the biggest, by bytes
    size_t flowCount = 0, encryptedFlows = 0;
    std::vector<std::pair<std::string, uint64_t>> otherPdus;   // non-IP GSE PDUs by protocol type
    double otherEntropy = -1;        // bits per byte of those (negative: not judged yet)
    bool otherEncrypted = false;     // they read as random bytes: ciphertext, not decoded
    std::vector<DvbsIpLabel> labels; // the busiest GSE labels
    std::vector<DvbsMediaFlow> media;   // multicast RTP and MPEG-TS streams
    std::vector<DvbsSdp> sdp;           // SAP/SDP announcements (session names)
    size_t labelCount = 0;
    std::string pcapPath;            // the PCAP file being written ("" none)
    uint64_t pcapPackets = 0, pcapBytes = 0;
};

// One input stream of a DVB-S2 carrier (a multistream carrier interleaves several, told apart by the ISI)
struct DvbsStream {
    int isi = 0;                     // 0 for a single stream
    bool single = true;
    bool decoded = false;            // the transport stream that is being decoded (the ISI choice)
    std::string content;             // "transport stream", "GSE", "GSE-HEM", "GSE-Lite", "transport packets in a generic stream (sync BBh)", "12-byte packets (sync 00h)"
    bool ccm = true, issyi = false, npd = false, hem = false;
    uint64_t frames = 0;
    std::string modcods;             // "QPSK 1/3 100 %"
    double bps = 0;                  // useful bit rate (DFL)
};

// One carrier in the survey of the input spectrum
struct DvbsCarrier {
    double centerHz = 0;     // offset from the centre of the input
    double rateHz = 0;       // symbol rate from the -3 dB width
    double rollOff = 0;      // 0: not measured
    float snrDb = 0;         // plateau over the noise floor
    float levelDb = 0;       // plateau level relative to the strongest carrier (0 dB = strongest)
    bool selected = false;   // the receiver works on this one
};

// DVB-CID: the carrier identification signal under the host carrier (ETSI TS 103 129)
struct DvbsCid {
    bool on = false;            // looking for it
    bool acquired = false;
    double offsetHz = 0;        // against the host carrier's centre
    float snrDb = 0;            // despread, per bit
    uint64_t bits = 0, frames = 0, badFrames = 0;
    double clockPpm = 0;
    bool haveGuid = false;
    std::string guid;           // "CC:XX:...:XX"
    std::string mac;            // the MAC address the identifier encapsulates, if it does
    bool havePosition = false;
    double latitude = 0, longitude = 0;
    std::string telephone, text;
};

struct DvbsTelemetry {
    // ---- the part every mode has: the engine copies it into RxTelemetry; keep these members and what they mean
    uint64_t seq = 0;            // grows with every report and never restarts (not even after reset())
    int state = 0;               // 0 searching, 1 partly locked, 2 locked and decoding
    double cfoHz = 0;            // carrier offset: where the carrier is relative to the centre of the input
    float snrDb = 0;             // signal-to-noise ratio of what the mode decodes: Es/N0 in dB, from the symbols (MER)
    bool dataValid = false;      // payload is coming out (transport stream packets)
    uint64_t blocksOk = 0, blocksBad = 0;   // FEC frames (DVB-S2) or Reed-Solomon packets (DVB-S) decoded well and badly since the start

    // ---- this mode's own fields below
    // what was found
    int standard = 0;            // 0 not known yet, 1 DVB-S, 2 DVB-S2, 3 DVB-S2X
    std::string signalNote;      // plain text about the signal: "S2X signalling seen: ..., not decoded", "GSE stream: not converted", ...
    double symbolRate = 0;       // Hz, the rate the receiver is using now (timing loop included)
    bool symbolRateManual = false;
    double symbolRateSpectrum = 0;   // Hz, from the -3 dB width of the spectrum (0: not measured)
    double symbolRateLoop = 0;       // Hz, what the timing loop settled on (0: not locked yet)
    float spectrumSnrDb = 0;         // plateau of the carrier over the noise floor in the spectrum
    float spectrumFitRms = 0;        // how well the spectrum looks like a raised cosine carrier (about 0.1 is good, 1 or more is not)
    float rollOff = 0;               // the roll-off the matched filter uses
    int rollOffSource = 0;           // 0 assumed, 1 measured from the spectrum, 2 signalled in the stream
    int modulation = -1;             // 0 QPSK, 1 8PSK (or 8APSK), 2 16APSK, 3 32APSK, 4 64APSK, 5 128APSK, 6 256APSK, -1 not known
    std::string modulationName;      // "QPSK" ... (S2X: the canonical name of EN 302 307-2 table 17a, "8APSK" ...)
    std::string codeRate;            // "2/3" ... (S2X: the canonical code rate, "5/9-L" ...)
    int frameSize = 0;               // DVB-S2: 0 not known, 1 normal (64800), 2 short (16200), 3 medium (VL-SNR, S2X: 32400)
    int modcod = -1;                 // DVB-S2 MODCOD number (1..28), 0 dummy frame, -1 not known; DVB-S2X: the PLS code value of table 17a (132..248); 129 / 131 VL-SNR frames (modulationName and codeRate then name the mode of table 18a)
    bool pilots = false;
    bool inverted = false;           // the spectrum is turned around (an LNB with a high-side oscillator)
    bool vcm = false;                // the MODCOD changes from frame to frame (VCM or ACM)
    int isi = -1;                    // DVB-S2 multiple input streams: the stream that is decoded, -1 single stream or not known
    int plScramblingCode = 0;        // DVB-S2 physical layer scrambling code (Gold code number n) in use
    double netBitrate = 0;           // transport stream bit rate (bits per second), averaged over the last second

    // quality
    float merDb = 0;                 // modulation error ratio of the symbols
    float preFecBer = -1;            // bit error rate before the error correction (estimate), -1 not known
    float ldpcIterAvg = 0;           // DVB-S2: average LDPC iterations per frame (recent)
    int computeMode = 2;             // the choice that is set: 0 CPU, 1 GPU, 2 auto
    bool gpuAvailable = false;       // a graphics processor that the error correction can use
    bool onGpu = false;              // the last frames (DVB-S2 LDPC) or windows (DVB-S Viterbi) were decoded on it
    uint64_t bchOk = 0, bchBad = 0;                 // DVB-S2 BCH decoder: frames fixed or clean / not decodable
    uint64_t rsClean = 0, rsCorrected = 0, rsFailed = 0;   // DVB-S Reed-Solomon (204,188) packets
    uint64_t packets = 0;            // transport stream packets delivered
    uint64_t packetsBad = 0;         // ... of which carry the transport error indicator
    uint64_t framesSeen = 0;         // DVB-S2 PLFRAMEs seen
    uint64_t framesDummy = 0;        // dummy PLFRAMEs among them
    uint64_t framesUnsupported = 0;  // DVB-S2X frames among them that are followed but not decoded (VL-SNR, reserved PLS codes)
    uint64_t crcErrors = 0;          // DVB-S2 user packet CRC-8 mismatches
    uint64_t gseFrames = 0;          // DVB-S2 frames that carry a generic stream (reported, not converted)

    // lock stages: the receiver passes through them in this order
    bool lockSpectrum = false;       // a carrier was found in the spectrum
    bool lockTiming = false;         // the symbol timing loop has settled
    bool lockCarrier = false;        // the carrier phase is tracked
    bool lockFrame = false;          // DVB-S2: PLFRAME headers are found where they should be; DVB-S: the packet sync bytes
    bool lockFec = false;            // the error correction delivers (LDPC/BCH frames or Viterbi + Reed-Solomon)
    bool tsLock = false;             // transport stream packets are flowing
    double secsSinceLock = 0;        // seconds since the transport stream lock was gained (0: no lock)
    // progress of the automatic search in plain words (narrow carriers take seconds: a DVB-S2 frame at 10 kSym/s lasts over three seconds)
    double huntSecs = 0;             // seconds of signal the receiver has spent on the carrier it found (0 while searching or locked)
    double huntSecsExpected = 0;     // about how long the frame search needs at this symbol rate (0: not known)
    std::string searchStatus;        // what the receiver is doing or why it does not lock ("carrier too weak", "not DVB-S/S2", ...); empty when locked

    // pictures
    std::vector<cf32> cells;                 // recent symbols after carrier recovery, at most 2048, unit mean power
    std::vector<float> psdDb;                // spectrum of the input, 0 dB at the peak, over the whole input rate (fft shifted)
    double psdSpanHz = 0;                    // the width psdDb covers
    double carrierLoHz = 0, carrierHiHz = 0; // the -3 dB edges of the carrier in the spectrum, relative to the centre (0, 0 when not found)
    std::vector<float> merHistory;           // MER of the last frames or blocks of symbols, oldest first
    DvbsIpTel ip;                            // IP data in the stream
    std::string superframe;                  // the super-frames found (Annex E): format, pilots, count; empty when none
    std::vector<DvbsStream> streams;         // the input streams of a DVB-S2 carrier
    DvbsCid cid;                             // carrier identification, when looked for
    std::vector<DvbsCarrier> carriers;       // every carrier seen in the spectrum (survey), by frequency
    std::string symbolDumpPath;              // the symbol file being written (empty: none)
    uint64_t symbolDumpCount = 0;            // symbols written
    bool carrierPicked = false;              // the user chose a carrier from the survey
    double carrierPickHz = 0;                // ... at this offset
};

// One line for the command line and the log
inline std::string dvbsSummary(const DvbsTelemetry& t) {
    char b[240];
    const char* st = t.standard == 1 ? "DVB-S" : t.standard == 2 ? "DVB-S2" : t.standard == 3 ? "DVB-S2X" : "DVB-S/S2";
    if (t.standard == 0) {
        snprintf(b, sizeof b, "%s: state %d  carrier %s  %.3f Msym/s  SNR %.1f dB", st, t.state, t.lockSpectrum ? "found" : "not found", t.symbolRate / 1e6, t.snrDb);
        return b;
    }
    snprintf(b, sizeof b, "%s %s %s%s%s  %.3f Msym/s  Es/N0 %.1f dB  ok %llu  bad %llu  packets %llu%s", st, t.modulationName.c_str(), t.codeRate.c_str(),
             t.frameSize == 2 ? " short" : "", t.pilots ? " pilots" : "", t.symbolRate / 1e6, t.snrDb, (unsigned long long)t.blocksOk, (unsigned long long)t.blocksBad,
             (unsigned long long)t.packets, t.tsLock ? "  TS lock" : "");
    return b;
}

} // namespace dect2
