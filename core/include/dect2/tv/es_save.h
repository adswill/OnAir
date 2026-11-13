// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Saves a raw audio elementary stream (DRA, MPEG audio) to a file that FFmpeg can play, for sound OnAir cannot decode itself.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace dect2 {

// A thread-safe file sink for an elementary stream.
class EsSink {
public:
    ~EsSink() { close(); }
    bool open(const std::string& path);
    void close();
    bool isOpen() const;
    void write(const uint8_t* p, size_t n);
    uint64_t bytes() const;
    std::string path() const;
private:
    mutable std::mutex mu_;
    FILE* f_ = nullptr;
    uint64_t n_ = 0;
    std::string path_;
};

// Takes the transport-stream packets of one PID and writes the payload of its PES packets (the PES headers removed).
class TsEsExtractor {
public:
    void setPid(int pid) { pid_ = pid; inPes_ = false; skip_ = 0; hdrN_ = 0; }
    int pid() const { return pid_; }
    void feed(const uint8_t* pkt, EsSink& sink);   // one 188-byte packet
private:
    int pid_ = -1;
    bool inPes_ = false;
    int skip_ = 0;          // PES header bytes still to drop; -1 while the fixed header is read
    uint8_t hdr_[16] = {};
    int hdrN_ = 0;
};

// The file extension FFmpeg expects for a stream: "dra" for DRA, "mp2" for MPEG audio.
const char* esExtension(const std::string& codec);

} // namespace dect2
