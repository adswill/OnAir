// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The recordings manager core: finding and describing recordings in the known folders, the user's tags and notes (a small index file),
// search / sort, the automatic cleanup rule, exact-sample trimming of IQ and WAV, SigMF export, format conversion and the trash with undo.
// All on files and numbers, no window: tests/sched/test_recmgr.cpp. The window is app/panels/recmgr_ui.cpp.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace dect2::recmgr {

enum class Kind { Iq, Audio, Video, Image, Log, SigMF, Other };
const char* kindName(Kind k);
Kind classify(const std::string& path);   // by extension: .cs8 .cu8 .cf32 .iq .raw -> Iq; .sigmf-meta -> SigMF; .wav .flac .mp3 .ogg -> Audio; .ts .mp4 .mkv -> Video; .png .jpg .gif .bmp -> Image; .txt .log .csv .json .jsonl -> Log

struct Meta {                  // what the user (or the scheduler) said about a file; stored in the index, keyed by the path under the root
    std::vector<std::string> tags;
    std::string notes, mode;
    double freqHz = 0;
    bool favourite = false;
};

struct Entry {
    std::string path, name, folder;     // folder: the first folder under the root ("" = the root itself)
    Kind kind = Kind::Other;
    int64_t mtime = 0;                  // seconds since the Unix epoch
    uint64_t bytes = 0;                 // all the files of the entry (a SigMF recording is two)
    std::string mode;
    double freqHz = 0, sampleRate = 0, seconds = 0;
    uint64_t samples = 0;               // IQ: complex samples
    std::string datatype;               // SigMF datatype, "cf32_le" ...
    std::vector<std::string> tags;
    std::string notes;
    bool favourite = false;
    std::vector<std::string> files;     // every file of the entry, for rename / trash
    bool isProtected() const;           // favourite, or tagged "favourite" / "favorite" / "keep"
};

class Index {
public:
    std::map<std::string, Meta> meta;   // key: path relative to the root, '/' separated
    bool load(const std::string& path);                 // false when missing or damaged (the index is then empty)
    bool save(const std::string& path, std::string& err) const;
    void rename(const std::string& oldKey, const std::string& newKey);
    std::string toJson() const;
    bool fromJson(const std::string& text, std::string& err);
};

// Every recording under root (to the given depth), newest first. The side-cars give the rate, centre frequency and length; the index gives
// tags and notes. Only the index file itself and the trash folder are left out.
std::vector<Entry> scan(const std::string& root, const Index& idx, int maxDepth = 4);
std::string relKey(const std::string& root, const std::string& path);

// ---- search, sort ------------------------------------------------------------------------------------------------------------------
// Words are all required, case-insensitive, in the name, mode, tags or notes. tag:x  kind:iq  mode:fm  fav  >100MB  <5MB  >10min  <30s
bool matches(const Entry& e, const std::string& query);
enum class Column { Name, Date, Mode, Frequency, Length, Size, Kind };
void sortEntries(std::vector<Entry>& v, Column c, bool ascending);

// ---- the cleanup rule --------------------------------------------------------------------------------------------------------------
struct CleanupRule {
    uint64_t keepNewestBytes = 0;   // 0 = no size budget: keep at most this much, the newest first
    int olderThanDays = 0;          // 0 = no age rule: delete what is older
};
struct CleanupPlan { std::vector<size_t> remove; uint64_t freedBytes = 0; };
// Which entries of one folder to delete. Favourites and tagged keepers are never deleted (they still count towards the budget).
CleanupPlan planCleanup(const std::vector<Entry>& entries, const CleanupRule& rule, int64_t nowSec);
// The same with a rule per folder: the entries are grouped by Entry::folder and each group gets rule(folder).
CleanupPlan planCleanupByFolder(const std::vector<Entry>& entries, const std::function<CleanupRule(const std::string&)>& rule, int64_t nowSec);

// ---- trimming and exporting --------------------------------------------------------------------------------------------------------
struct SampleRange { uint64_t first = 0, end = 0; };   // [first, end)
// The exact sample positions of a time range: first = round(start * rate), end = round(end * rate), both clamped to the recording.
bool trimRange(double startSec, double endSec, double rate, uint64_t totalSamples, SampleRange& out, std::string& err);
int bytesPerComplexSample(const std::string& sigmfDatatype);   // "cf32_le" 8, "ci16_le" 4, "cu16_le" 4, "ci8" 2, "cu8" 2; 0 when not complex
// A raw IQ file, bytesPerSample per complex sample, cut to the range
bool trimRawIq(const std::string& src, const std::string& dst, int bytesPerSample, SampleRange r, std::string& err);
// A SigMF recording (give the .sigmf-meta): writes dstBase + ".sigmf-meta" and ".sigmf-data" with the range and a capture that starts at it
bool trimSigmf(const std::string& metaPath, const std::string& dstBase, double startSec, double endSec, SampleRange* used, std::string& err);
// A PCM WAV cut to the time range (frames at the exact positions); keeps the format
bool trimWav(const std::string& src, const std::string& dst, double startSec, double endSec, SampleRange* used, std::string& err);
struct WavInfo { int channels = 0, bits = 0, format = 0; uint32_t rate = 0; uint64_t dataOffset = 0, dataBytes = 0; uint64_t frames() const { return channels && bits ? dataBytes / ((bits / 8) * channels) : 0; } };
bool readWavInfo(const std::string& path, WavInfo& out, std::string& err);

// SigMF metadata for a raw IQ file: writes <rawPath>.sigmf-meta next to it (the raw file is the data; it is not renamed)
bool writeSigmfSidecar(const std::string& metaPath, const std::string& datatype, double rate, double freqHz, const std::string& description, std::string& err);
// Raw IQ to another sample format: "cs8" "cu8" "ci16" "cf32" (little endian, I then Q)
bool convertIq(const std::string& src, const std::string& srcFmt, const std::string& dst, const std::string& dstFmt, std::string& err);
bool copyFile(const std::string& src, const std::string& dst, std::string& err);

// ---- rename, trash ------------------------------------------------------------------------------------------------------------------
// Renames every file of the entry to the new base name (no path, no extension); false when a target exists or the name is empty after cleaning.
bool renameEntry(const Entry& e, const std::string& newBase, std::string& newPath, std::string& err);

struct TrashItem { std::string group, original, stored; int64_t when = 0; };
// Moves the files into trashDir (the originals are gone, the data is kept) and writes the journal. One group per call, for undo.
bool moveToTrash(const std::vector<std::string>& files, const std::string& trashDir, int64_t nowSec, std::string& group, std::string& err);
std::vector<TrashItem> listTrash(const std::string& trashDir);
// Puts a group back where it was. False when an original name is taken (nothing is moved then).
bool restoreFromTrash(const std::string& trashDir, const std::string& group, std::string& err);
// Permanently removes what has been in the trash longer than days (0 = everything). Returns the bytes freed.
uint64_t purgeTrash(const std::string& trashDir, int days, int64_t nowSec);
uint64_t folderBytes(const std::string& dir);

}  // namespace dect2::recmgr
