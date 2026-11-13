// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Iridium bursts as gr-iridium's iridium-extractor writes them (".bits" files, one "RAW:" line a burst), so iridium-toolkit's
// iridium-parser.py reads our files unchanged: `iridium-parser.py output.bits > output.parsed`. OnAir writes bits only; whatever the
// toolkit does with them afterwards (voice included) happens outside OnAir.
//
// Line format (gr-iridium lib/iridium_frame_printer_impl.cc):
//   RAW: <file_info> <ms %012.4f> <Hz %010d> N:<magnitude dB %05.2f><noise dB %+06.2f> I:<id %011d> <confidence %3d>% <level %.5f>
//        <data symbols %3d> <bits: unique word + data, '0'/'1'>
//   file_info = "i-<unix second of the first burst>-t1", ms = milliseconds since that second, data symbols = symbols after the
//   12-symbol unique word. No header line. iridium-toolkit bitsparser.py reads it with
//   (RAW|RWA|NC1): ([^ ]*) (-?[\d.]+) (\d+) (?:N:([+-]?\d+(?:\.\d+)?)([+-]\d+(?:\.\d+)?)|A:(\w+)) [IL]:(\w+) +(\d+)% ([\d.]+|inf|nan) +(\d+) ([\[\]<> 01]+)(.*)
#pragma once
#include "iridium_frame.h"
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace dect2 {

// One RAW line (no newline). unixTime: when the burst's unique word started; t0: whole unix second of file_info.
std::string iridiumRawLine(const IridiumBurstBits& b, long long t0, double unixTime, uint64_t id);

// The regex above as a std::regex ECMAScript string (tests).
extern const char* const kIridiumRawRegex;

class IridiumBitsWriter {
public:
    ~IridiumBitsWriter();
    bool open(const std::string& path, std::string* err = nullptr);   // truncates
    void close();
    bool isOpen() const;
    // unixTime of the burst: the caller maps its sample clock to the wall clock (or the recording's start time)
    void write(const IridiumBurstBits& b, double unixTime);
    uint64_t lines() const;
    std::string path() const;
    // Optional live copy of every line (text + '\n') to host:port: UDP (one datagram a line) or a TCP connection made to the
    // listener (re-made every few seconds when it drops). Works with or without the file.
    bool openStream(bool tcp, const std::string& host, int port, std::string* err = nullptr);
    void closeStream();
    bool streamOpen() const;
    uint64_t streamLines() const;
    bool active() const;                 // file or stream open
private:
    void streamSend(const std::string& line);
    mutable std::mutex mu_;
    FILE* f_ = nullptr;
    std::string path_;
    long long t0_ = -1;
    uint64_t lines_ = 0;
    int sock_ = -1; bool tcp_ = false; std::string host_; int port_ = 0; double retryAt_ = 0; uint64_t streamed_ = 0; bool stream_ = false;
};

} // namespace dect2
