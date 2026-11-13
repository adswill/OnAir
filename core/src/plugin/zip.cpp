// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/plugin/zip.h"
#include "dect2/common/inflate.h"
#include <cstring>

namespace dect2::plugin {

namespace {
uint32_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
void wr16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(x & 255); v.push_back((x >> 8) & 255); }
void wr32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; i++) v.push_back((x >> (8 * i)) & 255); }
} // namespace

bool safeZipName(const std::string& n) {
    if (n.empty() || n.size() > 200 || n[0] == '/' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos) return false;
    size_t depth = 0, start = 0;
    for (size_t i = 0; i <= n.size(); i++) {
        if (i == n.size() || n[i] == '/') {
            const std::string part = n.substr(start, i - start);
            if (i == n.size() && part.empty()) break;   // trailing slash = a directory entry
            if (part.empty() || part == "." || part == "..") return false;
            if (++depth > 4) return false;
            start = i + 1;
        }
    }
    for (unsigned char c : n) if (c < 32) return false;
    return true;
}

bool ZipReader::open(const std::vector<uint8_t>& data, std::string& err) {
    d_ = data;
    entries_.clear();
    if (d_.size() < 22) { err = "not a zip file"; return false; }
    // end of central directory: scan back (a comment may follow it)
    size_t eocd = std::string::npos;
    const size_t lo = d_.size() > 22 + 65535 ? d_.size() - 22 - 65535 : 0;
    for (size_t i = d_.size() - 22 + 1; i-- > lo;) if (rd32(&d_[i]) == 0x06054b50) { eocd = i; break; }
    if (eocd == std::string::npos) { err = "not a zip file (no directory)"; return false; }
    const uint32_t count = rd16(&d_[eocd + 10]), cdSize = rd32(&d_[eocd + 12]), cdOff = rd32(&d_[eocd + 16]);
    if (count == 0xFFFF || cdOff == 0xFFFFFFFFu) { err = "zip64 files are not supported"; return false; }
    if (count > kMaxZipEntries) { err = "the file has too many entries"; return false; }
    if ((uint64_t)cdOff + cdSize > d_.size()) { err = "damaged zip directory"; return false; }
    size_t p = cdOff;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (p + 46 > d_.size() || rd32(&d_[p]) != 0x02014b50) { err = "damaged zip directory"; return false; }
        ZipEntry e;
        const uint32_t flags = rd16(&d_[p + 8]);
        e.method = rd16(&d_[p + 10]); e.crc = rd32(&d_[p + 16]); e.csize = rd32(&d_[p + 20]); e.usize = rd32(&d_[p + 24]);
        const uint32_t nl = rd16(&d_[p + 28]), xl = rd16(&d_[p + 30]), cl = rd16(&d_[p + 32]);
        e.localOffset = rd32(&d_[p + 42]);
        if (p + 46 + nl + xl + cl > d_.size()) { err = "damaged zip directory"; return false; }
        e.name.assign((const char*)&d_[p + 46], nl);
        p += 46 + nl + xl + cl;
        if (flags & 1) { err = "encrypted zip entries are not supported"; return false; }
        if (!safeZipName(e.name)) { err = "unsafe file name in the zip: " + e.name.substr(0, 60); return false; }
        e.dir = !e.name.empty() && e.name.back() == '/';
        if (e.usize > kMaxZipEntryBytes || (total += e.usize) > kMaxZipTotalBytes) { err = "the zip unpacks to more than the size limit"; return false; }
        if (!e.dir && e.method != 0 && e.method != 8) { err = "unsupported compression in " + e.name; return false; }
        entries_.push_back(std::move(e));
    }
    return true;
}

bool ZipReader::read(const ZipEntry& e, std::vector<uint8_t>& out, std::string& err) const {
    out.clear();
    const uint64_t lo = e.localOffset;
    if (lo + 30 > d_.size() || rd32(&d_[lo]) != 0x04034b50) { err = "damaged zip entry " + e.name; return false; }
    const uint64_t start = lo + 30 + rd16(&d_[lo + 26]) + rd16(&d_[lo + 28]);
    if (start + e.csize > d_.size()) { err = "damaged zip entry " + e.name; return false; }
    const uint8_t* src = &d_[start];
    if (e.method == 0) {
        if (e.csize != e.usize) { err = "damaged zip entry " + e.name; return false; }
        out.assign(src, src + e.csize);
    } else {
        if (!inflateRaw(src, (size_t)e.csize, out, (size_t)e.usize) || out.size() != e.usize) { err = "cannot unpack " + e.name; return false; }
    }
    if (crc32Ieee(out.data(), out.size()) != e.crc) { err = "checksum error in " + e.name; return false; }
    return true;
}

std::vector<uint8_t> zipStore(const std::vector<std::pair<std::string, std::vector<uint8_t>>>& files) {
    std::vector<uint8_t> z, cd;
    for (const auto& f : files) {
        const uint32_t off = (uint32_t)z.size(), crc = crc32Ieee(f.second.data(), f.second.size());
        wr32(z, 0x04034b50); wr16(z, 20); wr16(z, 0); wr16(z, 0); wr16(z, 0); wr16(z, 0x21); wr32(z, crc);
        wr32(z, (uint32_t)f.second.size()); wr32(z, (uint32_t)f.second.size()); wr16(z, (uint32_t)f.first.size()); wr16(z, 0);
        z.insert(z.end(), f.first.begin(), f.first.end());
        z.insert(z.end(), f.second.begin(), f.second.end());
        wr32(cd, 0x02014b50); wr16(cd, 20); wr16(cd, 20); wr16(cd, 0); wr16(cd, 0); wr16(cd, 0); wr16(cd, 0x21); wr32(cd, crc);
        wr32(cd, (uint32_t)f.second.size()); wr32(cd, (uint32_t)f.second.size()); wr16(cd, (uint32_t)f.first.size()); wr16(cd, 0); wr16(cd, 0);
        wr16(cd, 0); wr16(cd, 0); wr32(cd, 0); wr32(cd, off);
        cd.insert(cd.end(), f.first.begin(), f.first.end());
    }
    const uint32_t cdOff = (uint32_t)z.size();
    z.insert(z.end(), cd.begin(), cd.end());
    wr32(z, 0x06054b50); wr16(z, 0); wr16(z, 0); wr16(z, (uint32_t)files.size()); wr16(z, (uint32_t)files.size());
    wr32(z, (uint32_t)cd.size()); wr32(z, cdOff); wr16(z, 0);
    return z;
}

} // namespace dect2::plugin
