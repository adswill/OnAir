// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// AVS+ video (DTMB television in China, PMT stream_type 0x42, sequence header profile_id 0x48) through an OPTIONAL FFmpeg add-on.
// The FFmpeg inside OnAir decodes AVS1 only in its JiZhun profile (profile 0x20) and OnAir ships no patented codec. As with AC-4 (ac4_dec.h): the
// user may put libavcodec + libavutil, built from an FFmpeg patched for AVS+ (github.com/maliwen2015/ffmpeg_cavs_dra: a patch for FFmpeg 7.1.2 that
// adds AVS+ and DRA decoding), into the OnAir codecs folders. They are loaded at run time (dlopen / LoadLibrary) and used through a handful of
// functions and the stable first fields of AVPacket (data, size, pts) and AVFrame (data, linesize, width, height, format) only, so the FFmpeg may
// be of another major version than the one OnAir was built with. Without them everything else works and the service says "install the AVS+ add-on".
//
// Search: DECT2_AVSPLUS_DIR (a folder; development and tests), then the OnAir codecs folders (native::codecFolders). In each folder the newest
// libavcodec[.N].dylib / libavcodec.so[.N] / avcodec-N.dll is tried, with the libavutil of the same folder loaded first. The decoder is looked up by
// name ("cavs" unless DECT2_AVSPLUS_DECODER or avsPlusSetDecoderName() says otherwise; the add-on's README does not name it).
//
// NOT verified against a real AVS+ stream: only the loading, the calls and the player plumbing are tested (with a stock decoder standing in).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

struct AvsPlusLibInfo {
    bool found = false;        // a libavcodec and a libavutil were loaded
    bool hasDecoder = false;   // and it has the decoder
    std::string path, utilPath;
    int avcodecVersion = 0;    // libavcodec_version(): major << 16 | minor << 8 | micro
    std::string message;       // what is wrong, in plain words; empty when all is well
};
// Looks once (later calls give the same answer unless rescan is true: "Look again"). Thread-safe.
AvsPlusLibInfo avsPlusLibrary(bool rescan = false);
inline bool avsPlusAvailable() { return avsPlusLibrary().hasDecoder; }
// The player calls this while it shows "install the add-on": looks again at most every `minIntervalSec`, so an add-on installed while the program
// runs is picked up. True when a decoder is there now.
bool avsPlusPoll(double minIntervalSec = 3.0);
// For tests: look only in this folder (and not in the codecs folders); empty = back to the normal search. Forces a new search.
void avsPlusSetLibraryDir(const std::string& dir);
// For tests: the name of the decoder looked up in the library. A test can borrow another decoder (mpeg1video) of a stock FFmpeg.
void avsPlusSetDecoderName(const std::string& name);
std::vector<std::string> avsPlusSearchFolders();

// Whether an avcodec_version() major number is one the stable-field use fits (FFmpeg 5 .. 9)
bool avsPlusAbiOk(unsigned avcodecVersion);

// The text for the TV panel when the picture is AVS+ and cannot be shown. `info` from avsPlusLibrary().
std::string avsPlusNote(const AvsPlusLibInfo& info);

struct AvsPicture {
    int w = 0, h = 0;
    int format = -1;                 // AVPixelFormat of the build's headers (YUV420P and its full range twin are the ones OnAir handles)
    const uint8_t* data[4] = {};     // valid until the next send / receive / flush
    int linesize[4] = {};
    int64_t pts = INT64_MIN;         // the presentation time of the oldest still unassigned packet, in the units the packets came in
    bool key = false;
};

class AvsPlusDecoder {
public:
    AvsPlusDecoder();
    ~AvsPlusDecoder();
    AvsPlusDecoder(const AvsPlusDecoder&) = delete;
    AvsPlusDecoder& operator=(const AvsPlusDecoder&) = delete;
    bool open();                     // false: no add-on / no decoder / would not open (error() says why)
    bool isOpen() const;
    void flush();
    // One demuxed packet. pts in any monotonic unit (INT64_MIN: none). Returns false on a hard error.
    bool send(const uint8_t* data, size_t size, int64_t pts, bool key);
    // The next picture, false when the decoder wants more data. Times are given back in display order from the packet times (a heap of the pending
    // ones), so no AVFrame field beyond the stable ones is read.
    bool receive(AvsPicture& out);
    std::string error() const;
    uint64_t sent = 0, pictures = 0;
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
