// GOES HRIT transport layer and file assembly (goes_proto.h).
#include "dect2/goes/goes_proto.h"
#include "dect2/ccsds/ccsds.h"
#include "dect2/common/inflate.h"
#include "dect2/common/des.h"
#include "dect2/image/j2k.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include "dect2/common/endian.h"

namespace dect2::goes {

namespace {
inline uint32_t be16(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe16(p); }
inline uint32_t be32(const uint8_t* p) { return (uint32_t)dect2::endian::loadBe32(p); }
inline uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
inline void put16(std::vector<uint8_t>& v, uint32_t x) { dect2::endian::putBe16(v, x); }
inline void put32(std::vector<uint8_t>& v, uint32_t x) { put16(v, x >> 16); put16(v, x & 0xFFFF); }
inline void put64(std::vector<uint8_t>& v, uint64_t x) { put32(v, (uint32_t)(x >> 32)); put32(v, (uint32_t)x); }
std::string text(const uint8_t* p, size_t n) {
    std::string s(reinterpret_cast<const char*>(p), n);
    while (!s.empty() && (s.back() == 0 || s.back() == ' ')) s.pop_back();
    return s;
}
bool contains(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }
}

// ---------------------------------------------------------------- headers
bool parseHeaders(const uint8_t* p, size_t n, Headers& h, int dialect) {
    h = Headers();
    if (n < 16 || p[0] != 0 || be16(p + 1) != 16) return false;
    h.fileType = p[3];
    h.headerLength = be32(p + 4);
    h.dataBits = be64(p + 8);
    if (h.headerLength < 16 || h.headerLength > n) return false;
    size_t i = 16;
    while (i + 3 <= h.headerLength) {
        const int type = p[i];
        const size_t len = be16(p + i + 1);
        if (len < 3 || i + len > h.headerLength) return false;
        const uint8_t* r = p + i + 3;
        const size_t rl = len - 3;
        switch (type) {
        case 1: if (rl >= 6) { h.hasImage = true; h.bpp = r[0]; h.columns = (int)be16(r + 1); h.lines = (int)be16(r + 3); h.compression = r[5]; } break;
        case 2: if (rl >= 48) { h.hasNav = true; h.projection = text(r, 32); h.cfac = (int32_t)be32(r + 32); h.lfac = (int32_t)be32(r + 36); h.coff = (int32_t)be32(r + 40); h.loff = (int32_t)be32(r + 44); } break;
        case 4: h.annotation = text(r, rl); break;
        case 5: if (rl >= 7) { h.hasTime = true; h.days = (int)be16(r + 1); h.msOfDay = be32(r + 3); } break;
        case 6: h.ancillary = text(r, rl); break;
        case 7: if (dialect == 1 && rl >= 4) h.keyNo = (int)(be32(r) & 0xFFFF); break;
        case 128:
            if (dialect == 1) { if (rl >= 4) { h.hasSeg = true; h.segSeq = r[0]; h.maxSeg = r[1]; h.startLine = (int)be16(r + 2); } break; }
            if (rl >= 14) { h.hasSeg = true; h.imageId = (int)be16(r); h.segSeq = (int)be16(r + 2); h.startCol = (int)be16(r + 4); h.startLine = (int)be16(r + 6); h.maxSeg = (int)be16(r + 8); h.maxCol = (int)be16(r + 10); h.maxRow = (int)be16(r + 12); } break;
        case 129: if (dialect == 1) break;
            if (rl >= 11) { h.hasNoaa = true; h.productId = (int)be16(r + 4); h.subId = (int)be16(r + 6); h.parameter = (int)be16(r + 8); h.noaaCompression = r[10]; } break;
        case 130: h.headerText = text(r, rl); break;
        case 131: if (dialect == 1) { h.obsTime = text(r, rl); break; }
            if (rl >= 4) { h.hasRice = true; h.riceFlags = (int)be16(r); h.ricePpb = r[2]; h.riceLines = r[3]; } break;
        default: break;
        }
        i += len;
    }
    h.ok = true;
    return true;
}

std::vector<uint8_t> buildHeaders(const Headers& h, int dialect) {
    std::vector<uint8_t> s;
    auto rec = [&](int type, const std::vector<uint8_t>& body) { s.push_back((uint8_t)type); put16(s, (uint32_t)(body.size() + 3)); s.insert(s.end(), body.begin(), body.end()); };
    auto str = [](const std::string& t, size_t pad = 0) { std::vector<uint8_t> b(t.begin(), t.end()); if (pad) b.resize(pad, ' '); return b; };
    if (h.hasImage) { std::vector<uint8_t> b{(uint8_t)h.bpp}; put16(b, (uint32_t)h.columns); put16(b, (uint32_t)h.lines); b.push_back((uint8_t)h.compression); rec(1, b); }
    if (h.hasNav) { std::vector<uint8_t> b = str(h.projection, 32); put32(b, (uint32_t)h.cfac); put32(b, (uint32_t)h.lfac); put32(b, (uint32_t)h.coff); put32(b, (uint32_t)h.loff); rec(2, b); }
    if (!h.annotation.empty()) rec(4, str(h.annotation));
    if (h.hasTime) { std::vector<uint8_t> b{0x40}; put16(b, (uint32_t)h.days); put32(b, h.msOfDay); rec(5, b); }
    if (!h.ancillary.empty()) rec(6, str(h.ancillary));
    if (dialect == 1) {
        if (h.hasImage || h.keyNo) { std::vector<uint8_t> b; put32(b, (uint32_t)h.keyNo); rec(7, b); }
        if (h.hasSeg) { std::vector<uint8_t> b{(uint8_t)h.segSeq, (uint8_t)h.maxSeg}; put16(b, (uint32_t)h.startLine); rec(128, b); }
    } else if (h.hasSeg) { std::vector<uint8_t> b; for (int v : {h.imageId, h.segSeq, h.startCol, h.startLine, h.maxSeg, h.maxCol, h.maxRow}) put16(b, (uint32_t)v); rec(128, b); }
    if (h.hasNoaa) { std::vector<uint8_t> b{'N', 'O', 'A', 'A'}; put16(b, (uint32_t)h.productId); put16(b, (uint32_t)h.subId); put16(b, (uint32_t)h.parameter); b.push_back((uint8_t)h.noaaCompression); rec(129, b); }
    if (!h.headerText.empty()) rec(130, str(h.headerText));
    if (h.hasRice) { std::vector<uint8_t> b; put16(b, (uint32_t)h.riceFlags); b.push_back((uint8_t)h.ricePpb); b.push_back((uint8_t)h.riceLines); rec(131, b); }
    std::vector<uint8_t> out{0};
    put16(out, 16);
    out.push_back((uint8_t)h.fileType);
    put32(out, (uint32_t)(16 + s.size()));
    put64(out, h.dataBits);
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

double headerTime(const Headers& h) {
    if (!h.hasTime) return 0;
    // 1958-01-01 is 4383 days before 1970-01-01
    return ((double)h.days - 4383.0) * 86400.0 + h.msOfDay / 1000.0;
}

const char* kindName(int k) {
    static const char* n[KNumKinds] = {"Full disk", "Mesoscale", "CONUS", "Image", "EMWIN", "Admin message", "DCS", "Text", "Other"};
    return k >= 0 && k < KNumKinds ? n[k] : "?";
}

int classify(const Headers& h) {
    const std::string& a = h.annotation;
    if (h.fileType == 2) return KAdmin;
    if (h.fileType == 130) return KDcs;
    if (h.fileType == 214 || (h.hasNoaa && h.productId == 9) || contains(a, ".ZIS") || contains(a, ".TXT") || contains(a, "EMWIN")) return KEmwin;
    if (h.fileType == 0 || h.hasImage) {
        if (contains(a, "_LA_") || contains(a, "_ELA_") || contains(a, "_KO_")) return KMeso;   // GK-2A local area / Korea
        if (contains(a, "CMIPM") || contains(a, "-M1") || contains(a, "-M2") || contains(a, "_M1_") || contains(a, "_M2_") || contains(a, "Meso")) return KMeso;
        if (contains(a, "CMIPF") || contains(a, "FD") || contains(a, "Full")) return KFullDisk;
        if (contains(a, "CMIPC") || contains(a, "CONUS")) return KConus;
        return KImage;
    }
    if (h.fileType == 1) return KText;
    return KOther;
}

const char* compressionName(const Headers& h) {
    if (h.hasNoaa) switch (h.noaaCompression) { case 0: return "none"; case 1: return "Rice"; case 2: return "JPEG"; case 5: return "GIF"; case 10: return "ZIP"; default: return "?"; }
    if (h.hasRice || h.compression == 1) return "Rice";
    if (h.compression == 2) return "lossy";
    return "none";
}

// ---------------------------------------------------------------- GK-2A helpers
static const char* const kGkChan[16] = {"VI004", "VI005", "VI006", "VI008", "NR013", "NR016", "SW038", "WV063", "WV069", "WV073", "IR087", "IR096", "IR105", "IR112", "IR123", "IR133"};
const char* gk2aChannelName(int i) { return i >= 0 && i < 16 ? kGkChan[i] : "?"; }
int gk2aChannelIndex(const std::string& a) {
    if (a.compare(0, 4, "IMG_") != 0) return -1;
    for (int i = 0; i < 16; i++) if (a.find(kGkChan[i]) != std::string::npos) return i;
    return -1;
}
std::vector<std::pair<int, std::array<uint8_t, 8>>> parseKeyFile(const std::vector<uint8_t>& d) {
    std::vector<std::pair<int, std::array<uint8_t, 8>>> out;
    auto isText = [&] { for (uint8_t c : d) if (c > 126 || (c < 32 && c != '\n' && c != '\r' && c != '\t')) return false; return !d.empty(); };
    if (isText()) {
        const std::string t(d.begin(), d.end());
        size_t i = 0;
        while (i < t.size()) {
            size_t e = t.find('\n', i);
            if (e == std::string::npos) e = t.size();
            const std::string line = t.substr(i, e - i);
            i = e + 1;
            const size_t c = line.find_first_of(":= \t");
            if (c == std::string::npos) continue;
            std::string num = line.substr(0, c), hex;
            for (char ch : line.substr(c + 1)) if (isxdigit((unsigned char)ch)) hex += ch;
            if (hex.size() != 16 || num.empty()) continue;
            char* endp = nullptr;
            const long idx = strtol(num.c_str(), &endp, 16);
            if (endp == num.c_str() || idx <= 0 || idx > 0xFFFF) continue;
            std::array<uint8_t, 8> k{};
            for (int b = 0; b < 8; b++) k[(size_t)b] = (uint8_t)strtol(hex.substr((size_t)b * 2, 2).c_str(), nullptr, 16);
            out.emplace_back((int)idx, k);
        }
        return out;
    }
    if (d.size() < 2) return out;
    const size_t cnt = be16(d.data());
    if (d.size() < 2 + cnt * 10) return out;
    for (size_t i = 0; i < cnt; i++) {
        std::array<uint8_t, 8> k{};
        memcpy(k.data(), d.data() + 2 + i * 10 + 2, 8);
        out.emplace_back((int)be16(d.data() + 2 + i * 10), k);
    }
    return out;
}

bool unzipFirst(const std::vector<uint8_t>& z, std::vector<uint8_t>& out, std::string& name) {
    if (z.size() < 30 || z[0] != 'P' || z[1] != 'K' || z[2] != 3 || z[3] != 4) return false;
    auto le16 = [&](size_t i) { return (uint32_t)z[i] | (uint32_t)z[i + 1] << 8; };
    auto le32 = [&](size_t i) { return le16(i) | le16(i + 2) << 16; };
    const uint32_t method = le16(8), csize = le32(18), nlen = le16(26), elen = le16(28);
    const size_t start = 30 + (size_t)nlen + elen;
    if (start > z.size()) return false;
    name.assign(reinterpret_cast<const char*>(z.data() + 30), nlen);
    const size_t avail = z.size() - start;
    if (method == 0) { const size_t n = csize && csize <= avail ? csize : avail; out.assign(z.begin() + (std::ptrdiff_t)start, z.begin() + (std::ptrdiff_t)(start + n)); return true; }
    if (method == 8) return inflateRaw(z.data() + start, csize && csize <= avail ? csize : avail, out, 16u << 20);
    return false;
}

// ---------------------------------------------------------------- assembler
struct Assembler::Impl {
    struct Vc { int64_t last = -1; std::vector<uint8_t> buf; bool have = false; };
    struct FileState {
        bool active = false;
        std::vector<uint8_t> file;        // after the transport header
        Headers h;
        bool headersDone = false, rice = false, rawImage = false, errors = false;
        bool rawStop = false;             // plain image: data were lost, the offsets of what follows are unknown
        int lastSeq = -1, line = 0;
        size_t rowsWritten = 0;
        std::shared_ptr<Product> prod;
    };
    Assembler* self;
    Vc vc[64];
    std::map<int, FileState> files;      // vcid * 2048 + apid
    std::map<int64_t, std::weak_ptr<Product>> images;   // segmented images being put together: vcid, image id
    mutable std::mutex listMu;
    std::vector<std::shared_ptr<Product>> list;           // newest first
    size_t maxProducts = 80;
    uint64_t nextId = 1;
    double now = 0;
    int dialect = 0;
    // GK-2A: the DES keys, and the pictures (JPEG 2000) decoded on a worker thread so the demodulator never waits for them
    struct Job { std::shared_ptr<Product> p; Headers h; std::vector<uint8_t> data; };
    std::mutex keyMu;
    std::vector<Job> waiting;             // encrypted pictures whose key was not loaded (keyMu)
    std::map<int, std::array<uint8_t, 8>> keys;
    std::mutex jobMu;
    std::condition_variable jobCv;
    std::deque<Job> jobs;
    std::vector<std::string> pendingLogs;
    std::thread worker;
    bool stopWorker = false;
    int zone = kZoneBytes;            // bytes of packet zone in a VCDU (884 GOES; 1012 FengYun-4)
    bool raw = false;                 // whole files only (their own headers are the caller's: FengYun-4), no products here
    bool chkCrc = true;

    void log(const std::string& s) { if (self->onLog) self->onLog(s); }
    ~Impl() {
        { std::lock_guard<std::mutex> lk(jobMu); stopWorker = true; }
        jobCv.notify_all();
        if (worker.joinable()) worker.join();
    }
    void drainLogs() {
        std::vector<std::string> v;
        { std::lock_guard<std::mutex> lk(jobMu); v.swap(pendingLogs); }
        for (const auto& l : v) log(l);
    }
    void queueJob(Job j) {
        {
            std::lock_guard<std::mutex> lk(jobMu);
            if (!worker.joinable()) worker = std::thread([this] { workLoop(); });
            jobs.push_back(std::move(j));
        }
        jobCv.notify_one();
    }
    void workLoop() {
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> lk(jobMu);
                jobCv.wait(lk, [&] { return stopWorker || !jobs.empty(); });
                if (stopWorker) return;
                j = std::move(jobs.front());
                jobs.pop_front();
            }
            runJob(j);
        }
    }
    // a decoded segment into the product's picture
    void runJob(Job& j) {
        Product& p = *j.p;
        const Headers& h = j.h;
        j2k::Image im;
        size_t off = 0;
        if (j2k::findCodestream(j.data.data(), j.data.size(), off)) im = j2k::decode(j.data.data(), j.data.size());
        else if (h.compression == 0 && (h.bpp == 8 || h.bpp == 16) && h.columns > 0 && h.lines > 0) {
            const size_t bps = (size_t)h.bpp / 8;
            im.width = h.columns; im.height = h.lines; im.precision = h.bpp; im.ok = j.data.size() >= (size_t)h.columns * h.lines * bps;
            im.pix.assign((size_t)im.width * im.height, 0);
            for (size_t i = 0; i < im.pix.size() && (i + 1) * bps <= j.data.size(); i++) im.pix[i] = bps == 2 ? (uint16_t)be16(j.data.data() + 2 * i) : j.data[i];
            if (!im.ok) im.error = "short raw picture";
        } else im.error = "the picture is not JPEG 2000 and not raw";
        std::lock_guard<std::mutex> lk(p.mu);
        if (im.pix.empty() || im.width <= 0) {
            p.errors = true;
            p.note = "picture not decoded: " + im.error;
            p.segsDone++;
            p.version++;
            return;
        }
        if (!im.ok) p.errors = true;
        const int prec = h.bpp >= 8 && h.bpp <= 16 && h.bpp <= im.precision ? h.bpp : im.precision;
        const int shift = std::max(0, prec - 8);
        const int y0 = h.segSeq >= 1 ? (h.segSeq - 1) * std::max(1, h.lines) : h.startLine;
        for (int y = 0; y < im.height; y++) {
            const int py = y0 + y;
            if (py < 0 || py >= p.h) continue;
            uint8_t* d = p.pix.data() + (size_t)py * p.w;
            const uint16_t* sp = im.pix.data() + (size_t)y * im.width;
            for (int x = 0; x < im.width && x < p.w; x++) d[x] = (uint8_t)std::min(255, sp[x] >> shift);
            if (!p.rowDone[(size_t)py]) { p.rowDone[(size_t)py] = 1; p.rowsDone++; }
        }
        p.segsDone++;
        if (p.segsDone >= p.segsTotal) p.complete = true;
        p.version++;
    }

    void addProduct(const std::shared_ptr<Product>& p) {
        std::lock_guard<std::mutex> lk(listMu);
        list.insert(list.begin(), p);
        if (list.size() > maxProducts) list.resize(maxProducts);
    }

    std::shared_ptr<Product> productFor(FileState& f, int vcid, int apid) {
        const Headers& h = f.h;
        if (h.hasImage && h.hasSeg && h.maxSeg > 1) {
            int64_t key = (int64_t)vcid << 32 | (uint32_t)h.imageId;
            if (dialect == 1) {      // the file name without its segment number names the picture
                std::string a = h.annotation;
                const size_t u = a.rfind('_');
                if (u != std::string::npos) a.resize(u);
                key = (int64_t)vcid << 32 | (uint32_t)std::hash<std::string>{}(a);
            }
            auto it = images.find(key);
            if (it != images.end()) {
                if (auto p = it->second.lock()) {
                    std::lock_guard<std::mutex> lk(p->mu);
                    if (!p->complete && p->segsTotal == h.maxSeg) return p;
                }
            }
            auto p = newProduct(f, vcid, apid);
            images[key] = p;
            return p;
        }
        return newProduct(f, vcid, apid);
    }
    std::shared_ptr<Product> newProduct(FileState& f, int vcid, int apid) {
        auto p = std::make_shared<Product>();
        const Headers& h = f.h;
        p->id = nextId++;
        p->kind = classify(h);
        p->vcid = vcid; p->apid = apid; p->hdr = h;
        p->startSec = now;
        p->name = !h.annotation.empty() ? h.annotation : std::string(kindName(p->kind)) + " " + std::to_string(p->id);
        if (h.hasImage && (f.rice || f.rawImage)) {
            p->w = h.hasSeg && h.maxCol > 0 ? h.maxCol : h.columns;
            p->h = h.hasSeg && h.maxRow > 0 ? h.maxRow : h.lines;
            if (p->w <= 0 || p->h <= 0 || (int64_t)p->w * p->h > (int64_t)12000 * 12000) { p->w = p->h = 0; }
            p->pix.assign((size_t)p->w * p->h, 0);
            p->rowDone.assign((size_t)p->h, 0);
            p->segsTotal = h.hasSeg ? std::max(1, h.maxSeg) : 1;
        }
        if (dialect == 1 && h.hasImage) {
            p->channelIdx = gk2aChannelIndex(h.annotation);
            p->keyNo = h.keyNo;
            p->segsTotal = std::max(1, h.maxSeg);
            p->w = h.columns; p->h = h.lines * p->segsTotal;
            if (p->w <= 0 || p->h <= 0 || (int64_t)p->w * p->h > (int64_t)40000 * 40000) { p->w = p->h = 0; }
            else { p->pix.assign((size_t)p->w * p->h, 0); p->rowDone.assign((size_t)p->h, 0); }
        }
        addProduct(p);
        return p;
    }

    void writeRow(FileState& f, int row, const uint16_t* s, int n) {
        Product& p = *f.prod;
        std::lock_guard<std::mutex> lk(p.mu);
        const int y = (f.h.hasSeg ? f.h.startLine : 0) + row, x0 = f.h.hasSeg ? f.h.startCol : 0;
        if (y < 0 || y >= p.h) return;
        const int shift = std::max(0, f.h.bpp - 8);
        uint8_t* d = p.pix.data() + (size_t)y * p.w;
        for (int i = 0; i < n && x0 + i < p.w; i++) d[x0 + i] = (uint8_t)std::min(255, s[i] >> shift);
        if (!p.rowDone[(size_t)y]) { p.rowDone[(size_t)y] = 1; p.rowsDone++; }
        p.version++;
    }

    void headersReady(FileState& f, int vcid, int apid) {
        f.headersDone = true;
        const Headers& h = f.h;
        const int nc = h.hasNoaa ? h.noaaCompression : (h.hasRice || h.compression == 1 ? 1 : 0);
        f.rice = h.hasImage && nc == 1 && dialect == 0;
        f.rawImage = h.hasImage && nc == 0 && h.compression == 0 && (h.bpp == 8 || h.bpp == 16) && dialect == 0;
        f.prod = productFor(f, vcid, apid);
        f.line = 0; f.rowsWritten = 0;
    }

    void riceLine(FileState& f, const uint8_t* d, size_t n) {
        const Headers& h = f.h;
        ccsds::RiceParams rp;
        rp.bitsPerSample = std::max(1, std::min(16, h.bpp));
        rp.blockSize = h.ricePpb > 0 ? h.ricePpb : 16;
        rp.rsiBlocks = std::max(1, (h.columns + rp.blockSize - 1) / rp.blockSize);
        rp.preprocess = !h.hasRice || (h.riceFlags & ccsds::kSzNn) != 0;
        const int lines = std::max(1, h.hasRice ? h.riceLines : 1);
        std::vector<uint16_t> s((size_t)rp.rsiBlocks * rp.blockSize * lines);
        const size_t got = ccsds::riceDecode(d, n, rp, s.data(), s.size());
        const size_t per = (size_t)rp.rsiBlocks * rp.blockSize;
        for (int l = 0; l < lines; l++) {
            if (got < per * (l + 1)) { f.errors = true; break; }
            writeRow(f, f.line + l, s.data() + per * l, h.columns);
        }
        f.line += lines;
    }

    void rawRows(FileState& f) {
        const Headers& h = f.h;
        const size_t rowBytes = (size_t)h.columns * (h.bpp / 8);
        if (!rowBytes || f.file.size() < h.headerLength) return;
        const size_t avail = (f.file.size() - h.headerLength) / rowBytes;
        std::vector<uint16_t> s((size_t)h.columns);
        for (; f.rowsWritten < avail && (int)f.rowsWritten < h.lines; f.rowsWritten++) {
            const uint8_t* r = f.file.data() + h.headerLength + f.rowsWritten * rowBytes;
            for (int i = 0; i < h.columns; i++) s[(size_t)i] = h.bpp == 16 ? (uint16_t)be16(r + 2 * i) : r[i];
            writeRow(f, (int)f.rowsWritten, s.data(), h.columns);
        }
    }

    // GK-2A: decrypt the data field with the key of header 7 when it is loaded, then pictures go to the worker (JPEG 2000) and text is kept.
    // True when the file is dealt with here.
    bool finishGk2a(FileState& f, int vcid, int apid) {
        Product& p = *f.prod;
        const Headers& h = f.h;
        if (f.file.size() < h.headerLength) return false;
        const size_t len = std::min<size_t>(f.file.size() - h.headerLength, (size_t)((h.dataBits + 7) / 8));
        std::vector<uint8_t> data(f.file.begin() + (std::ptrdiff_t)h.headerLength, f.file.begin() + (std::ptrdiff_t)(h.headerLength + len));
        bool locked = false;
        if (h.keyNo != 0) {
            std::array<uint8_t, 8> k{};
            bool have = false;
            { std::lock_guard<std::mutex> lk(keyMu); auto it = keys.find(h.keyNo); if (it != keys.end()) { k = it->second; have = true; } }
            if (have) { Des d; d.setKey(k.data()); d.ecb(data.data(), data.size(), true); }
            else locked = true;
        }
        {
            std::lock_guard<std::mutex> lk(p.mu);
            p.bytes += f.file.size();
            if (f.errors) p.errors = true;
            p.keyNo = h.keyNo;
            if (locked) { p.needKey = true; p.note = "encrypted (KMA key " + std::to_string(h.keyNo) + "), key not loaded"; }
            p.version++;
        }
        if (h.hasImage) {
            if (locked) {
                { std::lock_guard<std::mutex> lk(p.mu); p.segsLocked++; p.version++; }
                std::lock_guard<std::mutex> lk(keyMu);            // kept (encrypted) until the key is imported
                if (waiting.size() < 96) waiting.push_back(Job{f.prod, h, std::move(data)});
            } else queueJob(Job{f.prod, h, std::move(data)});
        } else {
            std::lock_guard<std::mutex> lk(p.mu);
            if (!locked) {
                p.data = std::move(data);
                if (p.kind == KAdmin || p.kind == KText) { p.text.assign(p.data.begin(), p.data.end()); while (!p.text.empty() && p.text.back() == 0) p.text.pop_back(); }
            }
            p.complete = true;
            p.version++;
        }
        char b[256];
        snprintf(b, sizeof b, "LRIT: %s %s (VC %d, APID %d, %zu bytes%s%s)", kindName(p.kind), p.name.c_str(), vcid, apid, f.file.size(), f.errors ? ", with errors" : "", locked ? ", encrypted" : "");
        log(b);
        if (self->onFile) self->onFile(f.file, vcid, apid);
        return true;
    }

    void finish(FileState& f, int vcid, int apid, bool truncated) {
        f.active = false;
        if (truncated) f.errors = true;
        self->files++;
        if (f.errors) self->fileErrors++;
        if (!f.headersDone) { log("HRIT: a file ended before its headers"); return; }
        Product& p = *f.prod;
        const Headers& h = f.h;
        if (dialect == 1 && finishGk2a(f, vcid, apid)) return;
        {
            std::lock_guard<std::mutex> lk(p.mu);
            p.bytes += f.file.size();
            if (f.errors) p.errors = true;
            if (f.rice || f.rawImage) {
                p.segsDone++;
                if (p.segsDone >= p.segsTotal) p.complete = true;
            } else {
                const size_t len = std::min<size_t>(f.file.size() - h.headerLength, (size_t)((h.dataBits + 7) / 8));
                p.data.assign(f.file.begin() + h.headerLength, f.file.begin() + (std::ptrdiff_t)(h.headerLength + len));
                std::vector<uint8_t> inner;
                std::string nm;
                if ((h.hasNoaa && h.noaaCompression == 10) || (p.data.size() > 4 && p.data[0] == 'P' && p.data[1] == 'K')) {
                    if (unzipFirst(p.data, inner, nm)) { p.innerName = nm; p.text.assign(inner.begin(), inner.end()); }
                } else if (p.kind == KEmwin || p.kind == KAdmin || p.kind == KText || p.kind == KDcs) {
                    p.text.assign(p.data.begin(), p.data.end());
                }
                p.complete = true;
            }
            p.version++;
        }
        char b[256];
        snprintf(b, sizeof b, "HRIT: %s %s (VC %d, APID %d, %zu bytes%s)", kindName(p.kind), p.name.c_str(), vcid, apid, f.file.size(), f.errors ? ", with errors" : "");
        log(b);
        if (self->onFile) self->onFile(f.file, vcid, apid);
    }

    void finishRaw(FileState& f, int vcid, int apid) {
        f.active = false;
        self->files++;
        if (f.errors) self->fileErrors++;
        if (self->onFile) self->onFile(f.file, vcid, apid);
    }

    void packet(int vcid, const uint8_t* pk, size_t len) {
        self->packets++;
        const int apid = (int)((pk[0] & 7) << 8 | pk[1]);
        if (apid == kIdleApid) return;
        const int flags = pk[2] >> 6, seq = (int)((pk[2] & 0x3F) << 8 | pk[3]);
        if (len < 8) return;
        const uint8_t* u = pk + 6;
        const size_t ulen = len - 6 - 2;
        const bool crcOk = !chkCrc || ccsds::crc16(u, ulen) == be16(u + ulen);
        if (!crcOk) self->crcErrors++;
        FileState& f = files[vcid * 2048 + apid];
        if (flags == 1 || flags == 3) {
            if (f.active) finish(f, vcid, apid, true);
            f = FileState();
            if (!crcOk || ulen < (size_t)kTpHeader) return;
            f.active = true;
            f.lastSeq = seq;
            f.file.assign(u + kTpHeader, u + ulen);
        } else {
            if (!f.active) return;
            if (seq != ((f.lastSeq + 1) & 0x3FFF)) {
                f.errors = true;
                f.rawStop = true;
                if (f.rice) f.line += ((seq - f.lastSeq - 1) & 0x3FFF) * std::max(1, f.h.riceLines);
            }
            f.lastSeq = seq;
            if (!crcOk) { f.errors = true; f.rawStop = true; if (f.rice) f.line += std::max(1, f.h.riceLines); }
            else {
                f.file.insert(f.file.end(), u, u + ulen);
                if (f.headersDone && f.rice) riceLine(f, u, ulen);
            }
        }
        if (raw) {
            if (f.active && (flags == 2 || flags == 3)) finishRaw(f, vcid, apid);
            return;
        }
        if (!f.headersDone && f.file.size() >= 16) {
            Headers h;
            const uint32_t hl = be32(f.file.data() + 4);
            if (f.file[0] != 0 || hl < 16 || hl > (1u << 20)) { f.active = false; return; }
            if (f.file.size() >= hl) {
                if (!parseHeaders(f.file.data(), f.file.size(), h, dialect)) { f.active = false; return; }
                f.h = h;
                headersReady(f, vcid, apid);
                // Rice: whatever follows the headers in this packet is a scan line
                if (f.rice && f.file.size() > hl) riceLine(f, f.file.data() + hl, f.file.size() - hl);
            }
        }
        if (f.headersDone && f.rawImage && !f.rawStop) rawRows(f);
        if (f.active && (flags == 2 || flags == 3)) finish(f, vcid, apid, false);
    }

    void take(int vcid, Vc& v) {
        size_t off = 0;
        while (v.buf.size() - off >= 6) {
            const size_t len = 7 + be16(v.buf.data() + off + 4);
            if (v.buf.size() - off < len) break;
            packet(vcid, v.buf.data() + off, len);
            off += len;
        }
        v.buf.erase(v.buf.begin(), v.buf.begin() + (std::ptrdiff_t)off);
    }

    void push(const uint8_t* d) {
        self->vcdus++;
        const int scid = (d[0] & 0x3F) << 2 | d[1] >> 6, vcid = d[1] & 0x3F;
        const int64_t counter = (int64_t)((uint32_t)d[2] << 16 | (uint32_t)d[3] << 8 | d[4]);
        self->perVcid[vcid]++;
        if (vcid == kFillVcid) { self->fillVcdus++; return; }
        self->scid = scid;
        Vc& v = vc[vcid];
        if (v.last >= 0 && counter != ((v.last + 1) & 0xFFFFFF)) { self->vcduGaps++; v.buf.clear(); v.have = false; }
        v.last = counter;
        const int fhp = (int)((d[6] & 7) << 8 | d[7]);
        const uint8_t* z = d + 8;
        if (fhp == 2047) {
            if (v.have) { v.buf.insert(v.buf.end(), z, z + zone); take(vcid, v); }
            return;
        }
        if (fhp >= zone) { v.buf.clear(); v.have = false; return; }
        if (v.have) {
            v.buf.insert(v.buf.end(), z, z + fhp);
            take(vcid, v);
        }
        v.buf.assign(z + fhp, z + zone);
        v.have = true;
        take(vcid, v);
    }
};

Assembler::Assembler() : p_(std::make_unique<Impl>()) { p_->self = this; }
Assembler::~Assembler() = default;
void Assembler::reset() { for (auto& v : p_->vc) v = Impl::Vc(); p_->files.clear(); p_->images.clear(); }
void Assembler::clear() {
    reset();
    std::lock_guard<std::mutex> lk(p_->listMu);
    p_->list.clear();
    vcdus = fillVcdus = vcduGaps = packets = crcErrors = files = fileErrors = 0;
    { std::lock_guard<std::mutex> kl(p_->keyMu); p_->waiting.clear(); }
    scid = -1;
    for (auto& c : perVcid) c = 0;
}
void Assembler::push(const uint8_t* vcdu, double nowSec) { p_->now = nowSec; p_->drainLogs(); p_->push(vcdu); }
void Assembler::setDialect(int d) { p_->dialect = d; }
void Assembler::setKeys(const std::vector<std::pair<int, std::array<uint8_t, 8>>>& k) {
    std::lock_guard<std::mutex> lk(p_->keyMu);
    p_->keys.clear();
    for (const auto& e : k) p_->keys[e.first] = e.second;
    // pictures that were waiting for a key
    std::vector<Impl::Job> keep;
    for (auto& j : p_->waiting) {
        auto it = p_->keys.find(j.h.keyNo);
        if (it == p_->keys.end()) { keep.push_back(std::move(j)); continue; }
        Des d; d.setKey(it->second.data()); d.ecb(j.data.data(), j.data.size(), true);
        {
            std::lock_guard<std::mutex> pl(j.p->mu);
            j.p->segsLocked = std::max(0, j.p->segsLocked - 1);
            if (j.p->segsLocked == 0) { j.p->needKey = false; j.p->note.clear(); }
            j.p->version++;
        }
        p_->queueJob(std::move(j));
    }
    p_->waiting.swap(keep);
}
void Assembler::setRawFiles(bool raw, int zoneBytes, bool checkCrc) { p_->raw = raw; p_->zone = std::max(16, std::min(zoneBytes, 4096)); p_->chkCrc = checkCrc; }
std::vector<std::shared_ptr<Product>> Assembler::products() const { std::lock_guard<std::mutex> lk(p_->listMu); return p_->list; }
void Assembler::adopt(std::vector<std::shared_ptr<Product>> v) {
    std::lock_guard<std::mutex> lk(p_->listMu);
    for (const auto& q : v) if (q) p_->nextId = std::max(p_->nextId, q->id + 1);
    if (v.size() > p_->maxProducts) v.resize(p_->maxProducts);
    p_->list = std::move(v);
}
void Assembler::setMaxProducts(size_t n) { std::lock_guard<std::mutex> lk(p_->listMu); p_->maxProducts = std::max<size_t>(1, n); }

// ---------------------------------------------------------------- mux (test signals)
std::vector<uint8_t> makeFile(Headers h, const std::vector<uint8_t>& data, int dialect) {
    h.dataBits = (uint64_t)data.size() * 8;
    std::vector<uint8_t> f = buildHeaders(h, dialect);
    f.insert(f.end(), data.begin(), data.end());
    return f;
}

void Mux::addFile(int vcid, const FileOut& f) {
    Vc& v = vc_[vcid & 63];
    const size_t n = f.chunks.size();
    for (size_t i = 0; i < n; i++) {
        const auto& u = f.chunks[i];
        const int flags = n == 1 ? 3 : i == 0 ? 1 : i + 1 == n ? 2 : 0;
        int& sq = seq_[f.apid & 2047];
        v.starts.push_back(v.q.size());
        v.q.push_back((uint8_t)((f.apid >> 8) & 7));
        v.q.push_back((uint8_t)f.apid);
        v.q.push_back((uint8_t)(flags << 6 | ((sq >> 8) & 0x3F)));
        v.q.push_back((uint8_t)sq);
        sq = (sq + 1) & 0x3FFF;
        put16(v.q, (uint32_t)(u.size() + 2 - 1));
        v.q.insert(v.q.end(), u.begin(), u.end());
        put16(v.q, ccsds::crc16(u.data(), u.size()));
    }
    flushVc(v);
    if (std::find(order_.begin(), order_.end(), vcid & 63) == order_.end()) order_.push_back(vcid & 63);
}

void Mux::flushVc(Vc& v) {
    size_t rem = (zone_ - v.q.size() % zone_) % zone_;
    if (rem == 0) return;
    if (rem < 7) rem += zone_;
    v.starts.push_back(v.q.size());
    v.q.push_back((uint8_t)((kIdleApid >> 8) & 7));
    v.q.push_back((uint8_t)kIdleApid);
    v.q.push_back(0xC0);
    v.q.push_back(0);
    put16(v.q, (uint32_t)(rem - 7));
    v.q.insert(v.q.end(), rem - 6, 0x55);
}

namespace {
void header(uint8_t* d, int scid, int vcid, uint32_t counter, int fhp) {
    d[0] = (uint8_t)(0x40 | ((scid >> 2) & 0x3F));
    d[1] = (uint8_t)(((scid & 3) << 6) | (vcid & 63));
    d[2] = (uint8_t)(counter >> 16); d[3] = (uint8_t)(counter >> 8); d[4] = (uint8_t)counter;
    d[5] = 0;
    d[6] = (uint8_t)((fhp >> 8) & 7); d[7] = (uint8_t)fhp;
}
}

bool Mux::next(uint8_t* d) {
    while (!order_.empty() && vc_[order_.front()].q.empty()) order_.erase(order_.begin());
    if (order_.empty()) return false;
    const int id = order_.front();
    Vc& v = vc_[id];
    int fhp = 2047;
    for (size_t s : v.starts) if (s < (size_t)zone_) { fhp = (int)s; break; }
    header(d, scid_, id, v.counter, fhp);
    v.counter = (v.counter + 1) & 0xFFFFFF;
    memcpy(d + 8, v.q.data(), zone_);
    v.q.erase(v.q.begin(), v.q.begin() + zone_);
    std::vector<size_t> ns;
    for (size_t s : v.starts) if (s >= (size_t)zone_) ns.push_back(s - zone_);
    v.starts.swap(ns);
    if (v.q.empty()) order_.erase(order_.begin());
    return true;
}

void Mux::fill(uint8_t* d) {
    header(d, scid_, kFillVcid, fillCounter_, 2047);
    fillCounter_ = (fillCounter_ + 1) & 0xFFFFFF;
    memset(d + 8, 0, zone_);
}

} // namespace dect2::goes
