#include "dect2/common/u8file.h"
#include "dect2/source/iq_file.h"
#include "dect2/files/files_proto.h"
#include "dect2/source/bluefile.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "dect2/common/str.h"
#include "dect2/common/endian.h"

namespace dect2 {
namespace {

std::string lower(std::string x) { return dect2::str::lower(std::move(x)); }
bool endsWith(const std::string& s, const char* e) { size_t n = strlen(e); return s.size() >= n && lower(s.substr(s.size() - n)) == e; }
std::filesystem::path fsPath(const std::string& p) { return dect2::u8file(p); }

bool readAll(const std::string& path, std::string& out) {
    std::ifstream f(fsPath(path), std::ios::binary);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str();
    return true;
}

size_t bytesPerSample(FileFormat f) { return f == FileFormat::CF32 ? 8 : (f == FileFormat::CI16 || f == FileFormat::CU16) ? 4 : 2; }

bool probeSigmf(const std::string& metaPath, const std::string& dataPathIn, IqFileInfo& out, std::string& err) {
    std::string txt;
    if (!readAll(metaPath, txt)) { err = "cannot read " + metaPath; return false; }
    // the meta text is read by core/src/files/files_proto.cpp (C++) or the Rust crate onair_files
    const filesp::SigmfMeta m = filesp::sigmfParse(txt);
    if (m.status == 1) { err = "not a valid SigMF meta file: " + metaPath; return false; }
    if (m.status == 2) { err = "SigMF meta has no \"global\" section"; return false; }
    if (m.status == 3) { err = "SigMF meta has no core:datatype"; return false; }
    if (m.status == 4) { err = "SigMF datatype \"" + m.datatype + "\" is not supported (complex cf32, ci16, cu16, ci8, cu8 are)"; return false; }
    IqFileInfo r;
    r.format = static_cast<FileFormat>(m.format);   // the same order: CS8, CU8, CF32, CI16, CU16
    r.bigEndian = m.bigEndian;
    r.sampleRate = m.sampleRate;
    r.description = m.description;
    r.centerHz = m.centerHz;
    r.annotations = m.annotations;
    const uint64_t sampleStart = m.sampleStart;
    std::string data = dataPathIn;
    if (data.empty()) {
        const std::string base = metaPath.substr(0, metaPath.size() - strlen(".sigmf-meta"));
        const std::filesystem::path metaDir = fsPath(metaPath).parent_path();
        if (!m.dataset.empty()) {
            std::filesystem::path dp = fsPath(m.dataset);
            if (!dp.is_absolute()) dp = metaDir / dp;
            if (std::filesystem::exists(dp)) data = dp.string();
        }
        if (data.empty())
            for (const char* ext : {".sigmf-data", ".SIGMF-DATA"}) if (std::filesystem::exists(fsPath(base + ext))) { data = base + ext; break; }
        if (data.empty()) { err = "no IQ data file for " + metaPath + " (expected .sigmf-data or global core:dataset)"; return false; }
    }
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(fsPath(data), ec);
    if (ec) { err = "cannot read " + data; return false; }
    const uint64_t bps = bytesPerSample(r.format);
    const uint64_t hdr = m.headerBytes;
    const uint64_t trail = m.trailingBytes;
    r.offsetBytes = hdr + sampleStart * bps;
    if (r.offsetBytes > size) { err = "SigMF sample_start is past the end of the data file"; return false; }
    if (trail && trail < size) r.endBytes = std::max(size - trail, r.offsetBytes);
    r.dataPath = data;
    out = r;
    return true;
}

uint32_t rd32(const uint8_t* p) { return (uint32_t)dect2::endian::loadLe32(p); }

bool probeWav(const std::string& path, IqFileInfo& out, std::string& err) {
    std::ifstream f(fsPath(path), std::ios::binary);
    if (!f) { err = "cannot read " + path; return false; }
    uint8_t h[12];
    if (!f.read((char*)h, 12) || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { err = "not a WAV file: " + path; return false; }
    uint16_t tag = 0, ch = 0, bits = 0; uint32_t rate = 0;
    bool haveFmt = false;
    IqFileInfo r;
    uint8_t ck[8];
    while (f.read((char*)ck, 8)) {
        const uint32_t len = rd32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            std::vector<uint8_t> b(std::max<uint32_t>(len, 16));
            if (!f.read((char*)b.data(), len)) break;
            const filesp::WavFmt w = filesp::wavFmt(b.data(), b.size(), len);   // the fields of the chunk: files_proto.cpp (C++) or the Rust crate onair_files
            tag = (uint16_t)w.tag; ch = (uint16_t)w.channels; rate = w.rate; bits = (uint16_t)w.bits;
            haveFmt = true;
            if (len & 1) f.seekg(1, std::ios::cur);
        } else if (!memcmp(ck, "data", 4)) {
            if (!haveFmt) { err = "WAV data before its format"; return false; }
            int fmtCode = 0;
            const int cls = filesp::wavClassify(filesp::WavFmt{tag, ch, rate, bits}, fmtCode);
            if (cls == 1) { err = "a WAV needs two channels (I and Q) to play as IQ, this one has " + std::to_string(ch); return false; }
            if (cls != 0) { err = "WAV sample format not supported (16-bit, 8-bit or 32-bit float are)"; return false; }
            r.format = static_cast<FileFormat>(fmtCode);
            r.sampleRate = rate;
            r.offsetBytes = (uint64_t)f.tellg();
            std::error_code ec;
            const uint64_t size = std::filesystem::file_size(fsPath(path), ec);
            r.endBytes = filesp::wavDataEnd(r.offsetBytes, len, size);
            r.dataPath = path;
            // SDR programs put the centre frequency in the name: "..._100.000MHz_..."
            r.centerHz = filesp::wavCenterFromName(fsPath(path).filename().string());
            out = r;
            return true;
        } else f.seekg((std::streamoff)len + (len & 1), std::ios::cur);
        if (!f) break;
    }
    err = "no audio data in " + path;
    return false;
}

// A folder for converted copies that only this user can enter: <temp>/onair-<uid>, mode 0700, owned by us, not a symlink. Another user on the
// same machine can then neither pre-create the copy (data injection) nor plant a symlink (overwrite of a file we may write). Windows temp
// folders are per user already. Empty path: no safe folder.
static std::filesystem::path privateTempDir() {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) return std::filesystem::path();
#ifdef _WIN32
    return base;
#else
    const std::filesystem::path dir = base / ("onair-" + std::to_string((unsigned long)getuid()));
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return std::filesystem::path();
    struct stat st;
    if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) return std::filesystem::path();
    if ((st.st_mode & 077) != 0 && chmod(dir.c_str(), 0700) != 0) return std::filesystem::path();
    return dir;
#endif
}


// Converts into a file with an unpredictable name (mkstemp: created O_EXCL, 0600) inside the private folder, then renames it to `target`.
static bool convertPrivately(const std::string& src, const std::filesystem::path& dir, const std::filesystem::path& target, std::string& err) {
#ifdef _WIN32
    return blue::convertToCf32(src, target.string(), err);
#else
    std::string tmpl = (dir / "onair-blue-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back(0);
    const int fd = mkstemp(buf.data());
    if (fd < 0) { err = "cannot create a private temporary file for the converted copy"; return false; }
    fchmod(fd, 0600);
    close(fd);
    const std::string tmp = buf.data();
    if (!blue::convertToCf32(src, tmp, err)) { std::remove(tmp.c_str()); return false; }
    std::error_code ec;
    std::filesystem::rename(tmp, target, ec);
    if (ec) { std::remove(tmp.c_str()); err = "cannot place the converted copy"; return false; }
    return true;
#endif
}

bool probeBlue(const std::string& path, IqFileInfo& out, std::string& err, bool allowConvert) {
    blue::Info b;
    if (!blue::probe(path, b, err)) return false;
    if (!(b.sampleRate >= 1.0 && b.sampleRate <= 1e12)) { err = "the BLUE file does not give a sample rate (xdelta): " + path; return false; }
    IqFileInfo r;
    r.blue = true;
    r.sampleRate = b.sampleRate;
    r.centerHz = b.centerHz;
    r.numSamples = b.samples;
    r.description = b.description + (b.truncated ? (b.description.empty() ? "" : ", ") + std::string("file is shorter than its header says") : "");
    r.dataPath = path;
    // complex int8 / int16 / float32 play from the file itself, in the file's byte order
    const bool direct = b.complex && (b.kind == 'B' || b.kind == 'I' || b.kind == 'F');
    if (direct) {
        r.format = b.kind == 'B' ? FileFormat::CS8 : b.kind == 'I' ? FileFormat::CI16 : FileFormat::CF32;
        r.bigEndian = b.dataBig && b.kind != 'B';
        r.offsetBytes = b.dataStart;
        r.endBytes = b.dataStart + b.dataBytes;
        out = r;
        return true;
    }
    // everything else (real data plays as I with Q = 0): a cf32 copy, reused while the source is unchanged
    r.format = FileFormat::CF32;
    r.converted = true;
    r.offsetBytes = 0; r.endBytes = 0;
    if (!allowConvert) { out = r; return true; }
    std::error_code ec;
    const auto stamp = std::filesystem::last_write_time(fsPath(path), ec).time_since_epoch().count();
    const uint64_t h = std::hash<std::string>{}(path) ^ ((uint64_t)stamp * 1099511628211ull) ^ (b.dataBytes * 31ull);
    char nm[64];
    snprintf(nm, sizeof nm, "onair-blue-%016llx.cf32", (unsigned long long)h);
    const std::filesystem::path dir = privateTempDir();
    if (dir.empty()) { err = "no private temporary folder for the converted copy"; return false; }
    const std::filesystem::path tmp = dir / nm;
    const std::string tmpS = tmp.string();
    const uint64_t want = b.samples * 8;
    bool reuse = false;
    {   // reuse only a plain file of ours with the expected size (never a symlink)
        std::error_code e2;
        const auto st = std::filesystem::symlink_status(tmp, e2);
        reuse = !e2 && std::filesystem::is_regular_file(st) && std::filesystem::file_size(tmp, e2) == want;
    }
    if (!reuse && !convertPrivately(path, dir, tmp, err)) return false;
    r.dataPath = tmpS;
    out = r;
    return true;
}

} // namespace

std::string iqPrivateTempDir() { return privateTempDir().string(); }

bool probeIqFile(const std::string& path, IqFileInfo& out, std::string& err, bool allowConvert) {
    err.clear();
    if (blue::isBlueExtension(path)) return probeBlue(path, out, err, allowConvert);
    if (endsWith(path, ".sigmf-meta")) return probeSigmf(path, "", out, err);
    if (endsWith(path, ".sigmf-data")) {
        const std::string meta = path.substr(0, path.size() - strlen(".sigmf-data")) + ".sigmf-meta";
        if (!std::filesystem::exists(fsPath(meta))) { err = "no .sigmf-meta file beside " + path; return false; }
        return probeSigmf(meta, path, out, err);
    }
    if (endsWith(path, ".wav")) return probeWav(path, out, err);
    if (!endsWith(path, ".cs8") && !endsWith(path, ".cu8") && !endsWith(path, ".cf32") && blue::hasMagic(path)) return probeBlue(path, out, err, allowConvert);   // a BLUE file under another name
    return false;
}

std::vector<RecordingListing> listRecordingFiles(const std::string& dir) {
    std::vector<RecordingListing> out;
    std::error_code ec;
    const std::filesystem::path d = fsPath(dir);
    if (!std::filesystem::is_directory(d, ec)) return out;
    static const char* const kRaw[] = {".cs8", ".cu8", ".cf32", ".ci16", ".cu16", ".wav", ".tmp", ".prm", ".blue", ".bluefile"};
    std::vector<std::string> names;
    for (std::filesystem::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) names.push_back(it->path().filename().string());
    // a recording with a .sigmf-meta is listed once, by the meta; its data (the hard link, or the .sigmf-data) and the raw copy are skipped
    std::map<std::string, bool> metaBase;
    for (const std::string& n : names) if (endsWith(n, ".sigmf-meta")) metaBase[n.substr(0, n.size() - strlen(".sigmf-meta"))] = true;
    for (const std::string& n : names) {
        const bool meta = endsWith(n, ".sigmf-meta");
        if (!meta) {
            if (endsWith(n, ".sigmf-data")) continue;   // listed by its meta, or not a recording without one
            bool raw = false;
            for (const char* e : kRaw) if (endsWith(n, e)) raw = true;
            if (!raw) continue;
            const std::string base = n.substr(0, n.find_last_of('.'));
            if (metaBase.count(base)) continue;
        }
        const std::string path = (d / n).string();
        RecordingListing r;
        r.fileName = n; r.path = path; r.sigmf = meta;
        IqFileInfo info; std::string err;
        std::string dataPath = path;
        if (probeIqFile(path, info, err, false)) {
            r.blue = info.blue;
            r.format = info.format; r.sampleRate = info.sampleRate; r.centerHz = info.centerHz; r.mode = info.description;
            dataPath = info.dataPath;
            const uint64_t size = std::filesystem::file_size(fsPath(dataPath), ec);
            ec.clear();
            const uint64_t end = info.endBytes ? info.endBytes : size;
            if (info.numSamples && r.sampleRate > 0) r.seconds = (double)info.numSamples / r.sampleRate;
            else if (end > info.offsetBytes && r.sampleRate > 0) r.seconds = (double)((end - info.offsetBytes) / bytesPerSample(r.format)) / r.sampleRate;
        } else {
            if (!err.empty() || meta || blue::isBlueExtension(n)) continue;   // a WAV or SigMF that cannot be played: not listed
            r.format = guessFormat(path);          // a raw file: the rate and format its name says
            r.sampleRate = guessSampleRate(path);
            const uint64_t size = std::filesystem::file_size(fsPath(path), ec);
            ec.clear();
            if (r.sampleRate > 0) r.seconds = (double)(size / bytesPerSample(r.format)) / r.sampleRate;
        }
        const auto ft = std::filesystem::last_write_time(fsPath(path), ec);
        if (!ec) {
            const auto sys = std::chrono::time_point_cast<std::chrono::seconds>(ft - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
            r.modified = (int64_t)sys.time_since_epoch().count();
        }
        ec.clear();
        out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const RecordingListing& a, const RecordingListing& b) { return a.modified > b.modified; });
    return out;
}

const char* sigmfDatatype(FileFormat fmt, bool be) {
    switch (fmt) {
    case FileFormat::CF32: return be ? "cf32_be" : "cf32_le";
    case FileFormat::CI16: return be ? "ci16_be" : "ci16_le";
    case FileFormat::CU16: return be ? "cu16_be" : "cu16_le";
    case FileFormat::CU8: return "cu8";
    default: return "ci8";
    }
}

bool writeSigmfMeta(const std::string& metaPath, FileFormat fmt, bool be, double rate, double center, uint64_t sampleStart,
                    const std::string& description, std::string& err) {
    std::ofstream f(fsPath(metaPath), std::ios::binary);
    if (!f) { err = "cannot create " + metaPath; return false; }
    const std::string b = filesp::sigmfToolMeta(sigmfDatatype(fmt, be), rate, center, sampleStart, description);   // the text: files_proto.cpp (C++) or the Rust crate onair_files
    f << b;
    if (!f) { err = "writing failed: " + metaPath; return false; }
    return true;
}

} // namespace dect2
