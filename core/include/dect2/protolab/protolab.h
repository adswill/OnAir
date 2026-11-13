// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Protocol lab: a workbench for working out an unknown digital signal. Pipeline: bursts -> demodulator -> baud rate -> bit slicer
// (clock recovery) -> line coding -> preamble / sync -> bytes -> CRC search -> field analysis -> export. Every stage is a plain
// function so it can be tested alone. Nothing here decrypts anything: an encrypted payload simply shows up as high-entropy bytes.
#pragma once
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::protolab {

using cf = std::complex<float>;

// ---------------------------------------------------------------- samples, bursts, demodulators
enum class Demod { Auto, OOK, FSK2, FSK4, PSK2, PSK4, FM, AM };
const char* demodName(Demod d);

enum class IqFormat { Cf32, Cs16, Cu8, Cs8 };
bool loadIq(const std::string& path, IqFormat fmt, std::vector<cf>& out, std::string& err, size_t maxSamples = 40000000);

// One signal out of a wide capture: mix offsetHz to zero, low-pass to about bandwidthHz and decimate (3-stage moving average). The new rate
// is at least 4 times the bandwidth (and at least 40 kHz) when the capture allows it.
std::vector<cf> channelise(const std::vector<cf>& iq, double fs, double offsetHz, double bandwidthHz, double& outRate);

struct Burst { size_t start = 0, end = 0; };   // sample range [start, end)
struct BurstParams {
    double minGapSec = 0.004;      // a pause shorter than this does not split a packet
    double minLenSec = 0.0005;     // shorter bursts are noise
    double threshold = 0.30;       // between the noise floor (0) and the strong level (1)
};
std::vector<Burst> findBursts(const std::vector<cf>& iq, double fs, const BurstParams& p = {});

struct Soft {                       // demodulated burst: one channel (two for QPSK)
    std::vector<float> a, b;
    bool twoChannels() const { return !b.empty(); }
};
Soft demodulate(const cf* x, size_t n, Demod d, double fs);

// ---------------------------------------------------------------- baud rate and slicing
struct BaudEstimate {
    double baud = 0;               // 0: not found
    double quality = 0;            // 0..1, how well the transition intervals are whole multiples of one symbol
    double fromAutocorr = 0;       // second opinion
    int smoothing = 1;             // samples of smoothing that gave the cleanest intervals
};
BaudEstimate estimateBaud(const std::vector<const Soft*>& bursts, double fs, int levels = 2);   // levels 4: refined on the four-level signal

// Sample the soft signal at the bit centres with a clock that follows the transitions. levels 2 (1 bit/symbol), 4 (2 bits, Gray).
std::vector<uint8_t> sliceBits(const Soft& s, double fs, double baud, int levels, bool invert);

// ---------------------------------------------------------------- line coding and framing
enum class Coding { NRZ, NRZI, ManchesterIEEE, ManchesterThomas, DiffManchester, Hdlc };
const char* codingName(Coding c);
std::vector<uint8_t> decodeLine(const std::vector<uint8_t>& bits, Coding c, int phase = 0);
// HDLC: bits are the on-air stream after NRZI. Returns the frames between flags with stuffing removed (bit order as sent).
std::vector<std::vector<uint8_t>> hdlcFrames(const std::vector<uint8_t>& nrziDecodedBits);

std::vector<uint8_t> bitsToBytes(const std::vector<uint8_t>& bits, size_t from, bool lsbFirst);
std::vector<uint8_t> bytesToBits(const std::vector<uint8_t>& bytes, bool lsbFirst);

struct SyncInfo {
    int gridDelta = 0;              // bits between the last whole preamble byte boundary and the end of the preamble pattern (a guess for the byte grid)
    int preambleBits = 0;           // length of the leading repeating pattern (0: none)
    std::vector<uint8_t> preamblePattern;   // one period
    std::vector<uint8_t> sync;      // common bits after the preamble (<= 32)
    int found = 0;                  // packets that contain it
    int total = 0;
    std::vector<int> start;         // per packet, bit index of the sync word (-1: missing)
};
SyncInfo findSync(const std::vector<std::vector<uint8_t>>& packets);

// whitening (LFSR) candidates
struct Whitening { const char* name; int width; uint32_t poly; uint32_t seed; bool byteWise; };
const std::vector<Whitening>& whiteningCandidates();
void whiten(std::vector<uint8_t>& bytes, const Whitening& w);   // XOR with the sequence; the same call undoes it

// ---------------------------------------------------------------- CRC and checksums
struct CrcModel { const char* name; int width; uint32_t poly, init; bool refin, refout; uint32_t xorout; };
uint32_t crcCompute(const CrcModel& m, const uint8_t* d, size_t n);
const std::vector<CrcModel>& crcCatalog();

struct CrcHit {
    std::string name;              // catalog name, or "CRC-16/unnamed"
    int width = 0;
    uint32_t poly = 0, init = 0, xorout = 0;
    bool refin = false, refout = false;
    bool crcBigEndian = true;      // byte order of the CRC value in the packet
    int dataStart = 0;             // first covered byte (0 = first byte of the packet, sync included)
    int endSlack = 0;              // bytes after the CRC
    int coveredBytes = 0;          // bytes the CRC covers (of the first packet that has it)
    int matches = 0, total = 0;    // packets where it holds
    bool isChecksum = false;       // plain sum / xor, not a CRC
};
struct CrcOptions { bool generic = true; int maxStart = 12; int maxSlack = 2; int minPackets = 3; int minWidth = 8; };
std::vector<CrcHit> searchCrc(const std::vector<std::vector<uint8_t>>& packets, int hintStart, const CrcOptions& o = {});

// ---------------------------------------------------------------- field analysis
enum class FieldKind { Constant, Counter, Checksum, Enum, Varying };
struct Column {
    int index = 0;
    FieldKind kind = FieldKind::Varying;
    double entropy = 0;            // bits per byte, 0..8
    int distinct = 0;
    int counterStep = 0;
    int correlatedWith = -1;       // another column with |r| >= 0.9
    double correlation = 0;
    uint8_t constantValue = 0;
};
struct FieldReport {
    std::vector<Column> cols;
    std::string layout;            // one line: "C C C K K V V V X X"
};
FieldReport analyseFields(const std::vector<std::vector<uint8_t>>& payloads, int crcBytes);

struct Interpretation { int offset = 0; std::string kind, text; };
std::vector<Interpretation> interpret(const std::vector<uint8_t>& bytes);   // ASCII runs, BCD, ints, floats
std::string bitDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b);   // two lines of bits, '^' under the differences
std::string hexDump(const std::vector<uint8_t>& b);

// ---------------------------------------------------------------- the whole pipeline
struct Candidate {
    Demod demod = Demod::OOK;
    Coding coding = Coding::NRZ;
    bool invert = false;
    bool lsbFirst = false;
    double baud = 0, baudQuality = 0;
    int levels = 2;
    SyncInfo sync;
    std::string whitening;         // "" none
    std::vector<std::vector<uint8_t>> packets;   // bytes from the sync word on
    std::vector<CrcHit> crc;
    double score = 0;
};
struct AnalyseOptions {
    Demod demod = Demod::Auto;
    double baud = 0;               // 0: estimate
    bool tryCrc = true;
    bool genericCrc = true;
    BurstParams bursts;
};
struct Report {
    size_t packetsFound = 0;
    std::vector<Burst> bursts;
    std::vector<Candidate> candidates;   // best first
    FieldReport fields;
    std::string note;
};
Report analyse(const std::vector<cf>& iq, double fs, const AnalyseOptions& o = {});
// the same from packets whose bits are already known (hex / bit text pasted in)
Report analysePackets(const std::vector<std::vector<uint8_t>>& bitPackets, double baud, const AnalyseOptions& o = {});

// ---------------------------------------------------------------- export
std::string protocolJson(const Candidate& c, const FieldReport& f, const std::string& name, double fs);
std::string rtl433Spec(const Candidate& c, const std::string& name);       // an rtl_433 "-X" flex decoder line, best effort
std::string packetText(const Candidate& c);                                // one packet per line, bits (URH-style text)

// ---------------------------------------------------------------- test signal generators (also used for the demo button)
enum class TestProto { FskCrc16, ManchesterOok, HdlcFsk, WhitenedFsk, Fsk4Crc8 };
const char* testProtoName(TestProto p);
struct GenOptions { double fs = 250000; int packets = 8; double snrDb = 25; double cfoHz = 0; uint32_t seed = 1; double baudScale = 1.0; /* transmitter clock error: 1.03 = 3 % fast */ };
std::vector<cf> generateTestSignal(TestProto p, const GenOptions& o, std::vector<std::vector<uint8_t>>* payloadsOut = nullptr);

}   // namespace dect2::protolab
