// Radio clips: share a decoded moment. This header is the pure half (no GUI): the retroactive histories (waterfall lines, decoded rows), the range
// maths, and the generators (PNG, WAV, ZIP, one-file HTML, Discord text and picture). clips_ui.cpp is the dialog. Tests link clips_core.cpp.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../state/results_export.h"

namespace clips {

// ---- time range -------------------------------------------------------------------------------------------------------------------------------
struct Range { double t0 = 0, t1 = 0; double len() const { return t1 - t0; } };
Range lastSeconds(double now, double sec, double earliest);               // [now-sec, now], cut at the earliest time we have
Range clampRange(Range r, double earliest, double now, double minLen = 1.0);   // inside [earliest, now], at least minLen long, swapped ends fixed
Range fromFraction(double earliest, double now, double f0, double f1);   // a drag on the history strip: 0 = oldest, 1 = newest
double fractionOf(double earliest, double now, double t);

// ---- waterfall history: one byte per bin, 1024 bins, at most 10 rows a second ----------------------------------------------------------------
class WfHistory {
public:
    static constexpr int W = 1024;
    static constexpr double kDbLo = -160, kDbHi = 20;    // the byte range (0.7 dB a step)
    static constexpr double kMaxRate = 10;               // rows a second kept (the highest level of the frames between)
    void configure(bool on, double maxSeconds, size_t maxBytes);   // memory = min(maxSeconds * 10 rows, maxBytes / W) rows * (W + 8) bytes
    bool enabled() const { return on_; }
    void clear();
    // A spectrum frame (dB per bin, any length) arrives at wall time `epoch` (unix seconds).
    void feed(const float* db, size_t n, double epoch);
    size_t rows() const { return count_; }
    size_t bytes() const { return buf_.size() + stamp_.size() * sizeof(double); }
    double oldest() const;                               // epoch of the oldest row, 0 when empty
    double newest() const;
    // rows with t0 <= epoch < t1, oldest first: out gets rows*W bytes, times the epochs
    size_t extract(Range r, std::vector<uint8_t>& out, std::vector<double>& times) const;
private:
    bool on_ = false;
    size_t cap_ = 0, head_ = 0, count_ = 0;      // head_: next write
    std::vector<uint8_t> buf_;
    std::vector<double> stamp_;
    std::vector<uint8_t> acc_;
    double lastRow_ = 0;
    bool accHas_ = false;
};

// Colours a level byte range: lut256 maps 0..255 (the waterfall's colour table, 0xAABBGGRR). minDb/maxDb as the waterfall shows them.
// Result: width outW (<= W), height up to maxH (rows beyond are merged by their highest level), RGBA bytes.
struct Image { int w = 0, h = 0; std::vector<uint8_t> rgba; };
Image renderSpectrogram(const std::vector<uint8_t>& rows, size_t nRows, const uint32_t* lut256, float minDb, float maxDb, int outW, int maxH);

// ---- decoded rows history: what each results table showed, with the time each row first appeared --------------------------------------------
class TableLog {
public:
    void configure(bool on, double maxSeconds, size_t maxBytes);
    bool enabled() const { return on_; }
    bool due(const std::string& kind, double now) const;                          // would observe() take a table now? (lets a caller skip building one)
    void observe(const std::string& kind, const resexp::Table& t, double now);   // rate limited to once a second per kind; unknown rows get `now`
    resexp::Table range(Range r, std::string* kindsOut = nullptr) const;         // all kinds, rows first seen inside r, with a "table" column first
    size_t bytes() const { return bytes_; }
    double oldest() const;                                                       // first-seen time of the oldest row, 0 when none
    void clear();
private:
    struct Row { double t; std::vector<std::string> cells; uint64_t key; };
    struct Kind { std::string name; std::vector<std::string> cols; std::vector<Row> rows; double lastObs = 0; };
    std::vector<Kind> kinds_;
    bool on_ = true;
    double maxSec_ = 600;
    size_t maxBytes_ = 8u << 20, bytes_ = 0;
    void trim(double now);
};

// ---- generators -------------------------------------------------------------------------------------------------------------------------------
std::vector<uint8_t> encodePng(const uint8_t* rgba, int w, int h);                    // real deflate (fixed Huffman + LZ77), row filters
std::vector<uint8_t> encodeWav(const int16_t* mono, size_t n, int rate);
// Mono audio down to at most maxRate and maxBytes (WAV 16-bit): longer audio keeps its last seconds. rate gets the new rate.
std::vector<int16_t> fitAudio(const std::vector<int16_t>& in, int& rate, int maxRate, size_t maxBytes);
struct ZipEntry { std::string name; std::vector<uint8_t> data; };
std::vector<uint8_t> zipStore(const std::vector<ZipEntry>& files);                    // STORE-only zip with CRC32, UTF-8 names
uint32_t crc32(const uint8_t* p, size_t n, uint32_t c = 0);
std::string base64(const uint8_t* p, size_t n);
std::string htmlEscape(const std::string& s);                                         // & < > " ' and control characters

struct Meta {
    std::string mode;                // "ADS-B"
    double freqHz = 0;
    double t0 = 0, t1 = 0;           // unix seconds
    double snrDb = -1000;            // < -999: unknown
    std::string summary;             // "16 aircraft"
    std::string title, notes;        // the user's
    std::string version;             // OnAir version
    std::string grid;                // optional, empty = not shown
};
struct Content {
    Meta meta;
    bool wantImage = true, wantAudio = true, wantMessages = true;
    Image spectro;                   // empty = none
    std::vector<int16_t> audio; int audioRate = 0;
    resexp::Table table;
};
inline const char* kRepoUrl = "https://github.com/adswill/OnAir";
std::string freqText(double hz);                                    // "1090 MHz", "93.9 MHz", "7.074 MHz", "145.8 kHz"
std::string utcText(double t, bool seconds = false);                // "2026-10-17 14:32 UTC"
std::string buildHtml(const Content& c);                           // one file, no scripts, no external requests
std::string discordText(const Content& c);                         // ready to paste markdown
Image composite(const Content& c);                                 // spectrogram + caption strip with the watermark, width 1024
std::string fileStem(const Meta& m);                               // "onair_adsb_1090MHz_20261017-143200"

struct Written { std::string dir, html, zip; std::vector<std::string> files; uint64_t htmlBytes = 0, zipBytes = 0, discordPngBytes = 0; std::string error; };
// Writes the clip folder `parent/<stem>/` (files per the Want flags) and the zip next to it. Never throws.
Written writeClip(const Content& c, const std::string& parent);

}  // namespace clips

namespace clips {
WfHistory& wfHistory();   // the app's one history (UI thread only)
TableLog& tableLog();
}  // namespace clips
