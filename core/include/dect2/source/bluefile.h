// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#pragma once
// X-Midas BLUE files (.tmp, .prm, .blue, .bluefile): a 512-byte header, the samples, and an optional extended header of keywords.
// Written from the public format description (sigplot.lgsinnovations.com bluefile page); no third-party code is used.
// Reads types 1000 (1-D) and 2000 (2-D, read as one stream of samples); scalar (S) and complex (C) atoms of 8/16/32/64-bit integers,
// 32/64-bit floats; both byte orders (EEEI = little-endian, IEEE = big-endian), for the header and for the data separately.
#include <complex>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace dect2 {
using cf32 = std::complex<float>;
namespace blue {

struct Info {
    bool headBig = false, dataBig = false;   // IEEE = big-endian, EEEI = little-endian
    int32_t type = 1000;
    char spa = 'S';                          // first format letter: S scalar, C complex (others are not IQ)
    char kind = 'F';                         // second letter: B I L X (int8/16/32/64), O (uint8), F D (float32/64)
    bool complex = false;
    int bytesPerScalar = 4;
    uint64_t dataStart = 512;                // bytes
    uint64_t dataBytes = 0;                  // bytes actually usable (clamped to the file)
    uint64_t samples = 0;                    // atoms (complex samples, or real samples)
    bool truncated = false;                  // the header said more data than the file holds
    bool sizeFromFile = false;               // data_size was 0 or unusable: the size is what is left in the file
    double xstart = 0, xdelta = 0;
    int32_t xunits = 0;
    int32_t subsize = 0;                     // type 2000: points per frame
    double timecode = 0;
    double sampleRate = 0;                   // Hz: 1/xdelta when the axis is time (or unitless), else the SAMPLE_RATE keyword, else 0
    double centerHz = 0;                     // Hz from an RF/centre-frequency keyword, 0 = not given
    std::string description;                 // DESCRIPTION / COMMENT keyword
    std::vector<std::pair<std::string, std::string>> keywords;   // the extended header, values as text
    std::string format() const { return std::string(1, spa) + kind; }
    size_t atomBytes() const { return (size_t)bytesPerScalar * (complex ? 2 : 1); }
};

// Parses a header (the first 512 bytes) and, when given, the extended header bytes. fileSize bounds data_size. False with err set when
// the file is not a usable BLUE file.
bool parse(const uint8_t* hdr, size_t hdrLen, const uint8_t* ext, size_t extLen, uint64_t fileSize, Info& out, std::string& err);
// Reads a file's header (and keywords) without touching the samples.
bool probe(const std::string& path, Info& out, std::string& err);
// True when the first four bytes are "BLUE".
bool hasMagic(const std::string& path);
bool isBlueExtension(const std::string& path);   // .tmp .prm .blue .bluefile

// Streaming reader: any supported format, delivered as cf32 (full scale = 1.0; real data has Q = 0).
class Reader {
public:
    ~Reader() { close(); }
    bool open(const std::string& path, std::string& err);
    void close();
    const Info& info() const { return info_; }
    size_t read(cf32* dst, size_t n);        // returns the atoms delivered, 0 at the end
    void rewind();
private:
    Info info_;
    FILE* f_ = nullptr;
    uint64_t left_ = 0;
    std::vector<uint8_t> raw_;
    std::vector<int16_t> s16_;   // 16-bit samples, aligned for the conversion
};

struct WriteSpec {
    bool complex = true;                     // C or S
    char kind = 'F';                         // B I L D F
    bool bigEndian = false;                  // IEEE vs EEEI for header and data
    double sampleRate = 0;                   // stored as xdelta = 1/rate, xunits = 1 (seconds)
    double centerHz = 0;                     // RF_FREQ keyword, 0 leaves it out
    std::string description;
    double timecode = 0;                     // seconds since 1950-01-01, 0 = leave
};

// Streaming writer. The header is written at open (data_size 0), and rewritten with the true size and the keywords at close.
class Writer {
public:
    ~Writer() { std::string e; close(e); }
    bool open(const std::string& path, const WriteSpec& s, std::string& err);
    bool write(const cf32* x, size_t n, std::string& err);   // real files take the real part
    // Raw sample bytes already in the file's layout (little-endian complex int8 or float32, as the recorder queues them).
    bool writeBytes(const void* p, size_t bytes, std::string& err);
    bool close(std::string& err);
    bool isOpen() const { return f_ != nullptr; }
    uint64_t samples() const { return samples_; }
private:
    FILE* f_ = nullptr;
    WriteSpec s_;
    uint64_t samples_ = 0;
    std::vector<uint8_t> buf_;
};

// Converts a BLUE file of any supported format into a plain cf32 file (little-endian), streaming. Used to play the formats the file source
// does not read itself (real data, 32/64-bit integers, doubles, uint8).
bool convertToCf32(const std::string& src, const std::string& dstPath, std::string& err);

} // namespace blue
} // namespace dect2
