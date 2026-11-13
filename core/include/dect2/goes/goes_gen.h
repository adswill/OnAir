// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// GOES HRIT test signal. Nothing is transmitted: the samples only exist in memory or in a file.
//
// Test products (goesTestFiles): a full disk image in two segments, Rice compressed (one scan line a packet, 100 x 64 pixels so the
// last Rice block of each line is padded), a mesoscale image sent plain (60 x 40), a relayed JPEG picture, an administrative text
// message and an EMWIN text product in a ZIP file. They go through the real layers: LRIT headers -> transport file -> CP_PDUs with
// CRC -> M_PDUs of their virtual channels -> VCDUs (fill VCDUs between rounds) -> Reed-Solomon (255,223) x 4, randomiser, sync word ->
// rate 1/2 K = 7 convolutional code (G2 inverted) -> BPSK at 927 ksym/s, root raised cosine (roll-off 0.5), carrier offset, symbol rate
// offset and noise (snrDb is Es/N0).
//
// SynthConfig (makeGoesSynth): snrDb, cfoHz, sroPpm.
#pragma once
#include "goes_proto.h"
#include "dect2/engine/mode_synth.h"
#include <memory>
#include <vector>

namespace dect2 {

struct GoesTestFile {
    int vcid = 0, apid = 0;
    goes::Headers h;                               // dataBits filled in
    std::vector<std::vector<uint8_t>> dataChunks;  // the data field, as it is split over packets (Rice: one compressed line each)
    int dialect = 0;                               // 1: GK-2A header records
    std::vector<uint8_t> file;                     // the whole LRIT file (headers and data field), what the receiver must rebuild
};
std::vector<GoesTestFile> goesTestFiles();
// the pixels of the test images (8 bits): the full disk (both segments) and the mesoscale image
std::vector<uint8_t> goesTestFullDisk(int& w, int& h);
std::vector<uint8_t> goesTestMeso(int& w, int& h);
// the text of the admin message and of the EMWIN product (inside the ZIP)
const char* goesTestAdminText();
const char* goesTestEmwinText();
// packets of a test file: the transport header and the LRIT headers, then the data chunks
goes::FileOut goesFileOut(const GoesTestFile& f, uint16_t fileCounter);

struct GoesGenConfig {
    double rate = 4e6;
    double symbolRate = 927e3;
    double snrDb = 30;          // Es/N0; >= 99 no noise
    double cfoHz = 0;
    double sroPpm = 0;          // symbol clock offset
    double rolloff = 0.5;
    double amplitude = 0.2;     // rms of the signal
    int fillFrames = 250;       // fill VCDUs after each round of the test files (about 4.4 s); -1: the files only once, then fill
    int scid = 0x7E;
    bool invertG2 = true;       // false: the second code output sent as it is (the receiver finds out which)
    bool nrzm = false;          // differential (NRZ-M) coding of the frame bits before the convolutional encoder, as the real GOES-19 HRIT signal sends them (the receiver finds out which)
    bool dualBasis = true;      // Reed-Solomon symbols in the CCSDS dual basis (the receiver finds out which)
    uint32_t seed = 1;
    bool qpsk = false;          // QPSK (GK-2A HRIT): G1 on I and G2 on Q, symbolRate is the QPSK symbol rate
    bool oqpsk = false;         // with qpsk: offset QPSK, the Q rail half a symbol later (Elektro-L HRIT 1.15 Msym/s)
    std::vector<GoesTestFile> files;   // empty: goesTestFiles()
    int vcduCounterStart = 0;
};

class GoesGenerator {
public:
    explicit GoesGenerator(const GoesGenConfig& c);
    ~GoesGenerator();
    void generate(cf32* out, size_t n);
    uint64_t framesSent() const;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

std::unique_ptr<ModeSynth> makeGoesSynth(const SynthConfig& cfg, double sampleRate);

} // namespace dect2
