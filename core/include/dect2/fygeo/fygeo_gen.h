// FengYun geostationary test signals. Nothing is transmitted: the samples only exist in memory or in a file.
//
// FY-4 LRIT (FY-4A, FY-4B: modeOpt[0]) / FY-4A HRIT-II/III (makeFy4LritSynth ...): xRIT files in FY-4's header dialect (two AGRI channels of a full disk picture in two
// JPEG 2000 segments each, a second picture round after the first with a later time, and a text message) -> LRIT transport files -> CP_PDUs -> M_PDUs ->
// VCDUs of 1020 bytes (fill VCDUs between rounds) -> CADUs with the sync word -> transport stream packets on a PID -> the DVB-S2 transmitter
// chain of the DVB-S/S2 mode (QPSK 2/5 at 90 ksym/s, QPSK 8/9 at 120 ksym/s, QPSK 5/6 at 1 Msym/s; roll-off 0.25) with the noise, carrier and clock offsets of
// the SynthConfig.
//
// FY-2 S-VISSR (makeFy2SvissrSynth): scan lines of IR1 - IR4 and VIS (a test picture) in S-VISSR frames, each behind its PN preamble, scrambled,
// NRZ-M coded, BPSK at 660 ksym/s with a root raised cosine pulse (roll-off 0.5).
//
// SynthConfig: snrDb (Es/N0), cfoHz, sroPpm, modeOpt[0] (LRIT: 0 FY-4A, 1 FY-4B), modeVal[0] (symbol rate).
#pragma once
#include "dect2/engine/mode_synth.h"
#include "dect2/fygeo/fygeo_proto.h"
#include <memory>
#include <vector>

namespace dect2 {

// ---------------------------------------------------------------- FY-4
struct Fy4TestFile {
    int vcid = 0, apid = 0;
    fygeo::Fy4Headers h;                           // dataBits filled in
    std::vector<std::vector<uint8_t>> dataChunks;  // the data field as it is split over packets
    std::vector<uint8_t> file;                     // the whole file (headers and data), what the receiver must rebuild
    int channel = 0, segment = 0;                  // image files
};
// The files of round `round` (the time stamp and file names move on a minute each round). Pictures are kFy4TestW x kFy4TestH a segment, two segments, 12 bit.
constexpr int kFy4TestW = 96, kFy4TestH = 48, kFy4TestSegs = 2;
std::vector<Fy4TestFile> fy4TestFiles(int round = 0, const char* sat = "FY-4A");
// the pixels of channel `ch` of the whole test picture (kFy4TestW x kFy4TestH * kFy4TestSegs, 12 bit)
std::vector<uint16_t> fy4TestPicture(int ch);
const char* fy4TestText();
fygeo::Fy4Headers fy4TestImageHeaders(int round, int ch, int seg, const char* sat);
// packets of a test file: the transport header and the file headers, then the data chunks
goes::FileOut fy4FileOut(const Fy4TestFile& f, uint16_t fileCounter);

struct Fy4GenConfig {
    double rate = 2e6;
    int link = 0;                  // 0 FY-4A LRIT, 1 FY-4B LRIT, 2 FY-4A HRIT-II/III
    double symbolRate = 0;         // 0: the link's
    double snrDb = 30;             // Es/N0; >= 99 no noise
    double cfoHz = 0;
    double sroPpm = 0;
    int pid = 3004;
    int fillFrames = 40;           // fill VCDUs after each round; -1: one round only, then fill
    uint32_t seed = 1;
    int scid = 0x64;
};
class Fy4Generator {
public:
    explicit Fy4Generator(const Fy4GenConfig& c);
    ~Fy4Generator();
    void generate(cf32* out, size_t n);
    uint64_t cadusSent() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

// ---------------------------------------------------------------- S-VISSR
// The test picture of a frame: ir[k] (k = 0..3) kSvissrIrWidth values, vis 4 lines of kSvissrVisWidth values, for scan line `line`.
void svissrTestLine(int line, uint8_t* ir1, uint8_t* ir2, uint8_t* ir3, uint8_t* ir4, uint8_t* vis4);
struct SvissrGenConfig {
    double rate = 2.4e6;
    double symbolRate = 660e3;
    double snrDb = 30;             // Es/N0; >= 99 no noise
    double cfoHz = 0;
    double sroPpm = 0;
    double rolloff = 0.5;
    double amplitude = 0.2;
    int firstLine = 0;             // line counter of the first frame
    int lines = 8;                 // scan lines of one picture; the counter then starts again from firstLine
    double gapSec = 0;             // silence-less pause between pictures is not modelled: frames follow each other
    uint32_t seed = 1;
};
class SvissrGenerator {
public:
    explicit SvissrGenerator(const SvissrGenConfig& c);
    ~SvissrGenerator();
    void generate(cf32* out, size_t n);
    uint64_t framesSent() const;
    // the bits of the stream from the start of the next frame: useful for a test (preamble bits, then the scrambled frame)
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeFy4LritSynth(const SynthConfig& cfg, double sampleRate);    // SynthConfig::modeOpt[0]: 0 FY-4A, 1 FY-4B
std::unique_ptr<ModeSynth> makeFy4aHritSynth(const SynthConfig& cfg, double sampleRate);
std::unique_ptr<ModeSynth> makeFy2SvissrSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
