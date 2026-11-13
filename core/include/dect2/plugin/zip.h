// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// A small ZIP reader for .onairplugin files (stored and deflate entries, no encryption, no zip64). Bounded: entry count, sizes and the
// total unpacked size are capped, names are checked so nothing can be written outside the target folder, and every entry's CRC-32 is verified.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2::plugin {

struct ZipEntry { std::string name; uint32_t method = 0, crc = 0; uint64_t csize = 0, usize = 0, localOffset = 0; bool dir = false; };

constexpr size_t kMaxZipEntries = 64;
constexpr uint64_t kMaxZipEntryBytes = 128ull << 20, kMaxZipTotalBytes = 256ull << 20;

class ZipReader {
public:
    bool open(const std::vector<uint8_t>& data, std::string& err);   // keeps its own copy
    const std::vector<ZipEntry>& entries() const { return entries_; }
    bool read(const ZipEntry& e, std::vector<uint8_t>& out, std::string& err) const;
private:
    std::vector<uint8_t> d_;
    std::vector<ZipEntry> entries_;
};

// Safe relative path inside the target folder: no "..", no absolute path, no drive letter, no backslash, no empty or dot parts, at most 4 levels.
bool safeZipName(const std::string& name);

// Builds a zip with stored entries (used by the tests and by the packaging helper).
std::vector<uint8_t> zipStore(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files);

} // namespace dect2::plugin
