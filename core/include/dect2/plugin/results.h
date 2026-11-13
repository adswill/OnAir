// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// What a plugin shows: tables, status lamps, a log, plots, images and map points, kept in a bounded store the generic "Plugin mode" panel
// draws from. Plugin authors write no UI code. Every size is capped here, so a plugin that floods results cannot grow the program's memory:
// extra rows push out the oldest, oversized text is cut, and what is refused is counted.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace dect2::plugin {

constexpr size_t kMaxTables = 8, kMaxCols = 24, kMaxRows = 2000, kMaxCellText = 512, kMaxStatus = 64, kMaxLogLines = 500, kMaxLogLine = 1000,
                 kMaxPlots = 8, kMaxPlotFloats = 65536, kMaxImages = 4, kMaxImageBytes = 4u << 20, kMaxImageSide = 4096, kMaxPoints = 2000, kMaxIdLen = 48, kMaxTitleLen = 100;

// codes returned by the store (and so by the C host API): 0 ok
constexpr int kOk = 0, kBadArg = -1, kLimit = -2, kDenied = -3;

struct Col { std::string name; int32_t type = 0; };
struct Cell { int32_t type = 0; int64_t i = 0; double d = 0; std::string s; };
struct Table { std::string id, title; std::vector<Col> cols; std::deque<std::vector<Cell>> rows; uint64_t rowsTotal = 0; };
struct Status { std::string key, text; int32_t lamp = 0; };
struct Plot { std::string id, title; int32_t kind = 0; std::vector<float> data; double x0 = 0, dx = 1; };
struct Image { std::string id; int32_t format = 0; uint32_t w = 0, h = 0; std::vector<uint8_t> bytes; };
struct MapPoint { std::string id, label; double lat = 0, lon = 0; };

struct Snapshot {
    std::vector<Table> tables;
    std::vector<Status> status;
    std::vector<std::string> log;
    std::vector<Plot> plots;
    std::vector<Image> images;
    std::vector<MapPoint> points;
    uint64_t seq = 0, refused = 0;
};

class Results {
public:
    int defineTable(const std::string& id, const std::string& title, const std::vector<Col>& cols);
    int addRow(const std::string& id, const std::vector<Cell>& cells);
    int clearTable(const std::string& id);
    int setStatus(const std::string& key, const std::string& text, int32_t lamp);
    int addLog(const std::string& line);
    int setPlot(const std::string& id, const std::string& title, int32_t kind, const float* data, size_t n, double x0, double dx);
    int setImage(const std::string& id, int32_t format, uint32_t w, uint32_t h, const uint8_t* data, size_t size);
    int setPoint(const std::string& id, const std::string& label, double lat, double lon);
    void clear();
    uint64_t seq() const;                 // changes whenever anything changed (the panel redraws only then)
    uint64_t refused() const;
    Snapshot snapshot() const;
    std::vector<std::string> logTail(size_t n) const;
    size_t approxBytes() const;

private:
    mutable std::mutex mu_;
    std::vector<Table> tables_;
    std::vector<Status> status_;
    std::deque<std::string> log_;
    std::vector<Plot> plots_;
    std::vector<Image> images_;
    std::vector<MapPoint> points_;
    uint64_t seq_ = 0, refused_ = 0;
    int refuse() { refused_++; return kLimit; }
};

// Compact binary form of one result call, for the out-of-process host (the child writes it to a pipe, the app applies it to its store).
// Messages are [u32 length][u8 type][body]; every length is checked, a truncated or hostile message is refused.
enum class Wire : uint8_t { Log = 1, TableDefine, TableRow, TableClear, Status, ResultLog, Plot, Image, MapPoint, Event, Audio, RequestIq, Hello, Fatal, Ack,
                        ToChildInit = 32, ToChildSetting, ToChildConfigure, ToChildIq, ToChildEvent, ToChildTick, ToChildQuit };   // up to the app (child to parent) / down to the child
void wirePut(std::vector<uint8_t>& out, Wire type, const std::vector<uint8_t>& body);
struct WireWriter {
    std::vector<uint8_t> b;
    void u8(uint32_t v) { b.push_back((uint8_t)v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) b.push_back((uint8_t)(v >> (8 * i))); }
    void f64(double v);
    void str(const std::string& s, size_t maxLen = 4096);
    void bytes(const uint8_t* p, size_t n) { u32((uint32_t)n); b.insert(b.end(), p, p + n); }
};
struct WireReader {
    const uint8_t* p; size_t n, pos = 0; bool ok = true;
    WireReader(const uint8_t* d, size_t size) : p(d), n(size) {}
    uint32_t u8();
    uint32_t u32();
    uint64_t u64();
    double f64();
    std::string str(size_t maxLen = 4096);
    bool bytes(std::vector<uint8_t>& out, size_t maxLen);
};
// Applies one decoded result message (Wire::TableDefine ... Wire::MapPoint) to a store. False: not a result message or malformed.
bool wireApply(Results& r, Wire type, const uint8_t* body, size_t size);
// Builders used by the child process; same layout wireApply reads.
std::vector<uint8_t> wireTableDefine(const std::string& id, const std::string& title, const std::vector<Col>& cols);
std::vector<uint8_t> wireTableRow(const std::string& id, const std::vector<Cell>& cells);
std::vector<uint8_t> wirePlot(const std::string& id, const std::string& title, int32_t kind, const float* data, size_t n, double x0, double dx);
std::vector<uint8_t> wireImage(const std::string& id, int32_t format, uint32_t w, uint32_t h, const uint8_t* data, size_t size);

} // namespace dect2::plugin
