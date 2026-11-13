// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT for the engine: see kaft_live.h.
#include "dect2/kaft/kaft_live.h"
#include "dect2/kaft/kaft_frame.h"
#include "dect2/kaft/kaft_rx.h"
#include "dect2/kaft/kaft_session.h"
#include "dect2/kaft/kaft_tel.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace dect2 {

namespace {

constexpr size_t kMaxFiles = 32;
constexpr double kFrameTimeoutSec = 5.0;       // no frame for this long: back to "searching"
constexpr uint64_t kMaxFileBytes = 256ull << 20;

// A file name from the air made safe: no path, no leading dot, ASCII letters, digits and . _ - ( ) + space only, at most 100 characters, not a reserved
// Windows device name.
std::string safeName(const std::string& in) {
    size_t cut = in.find_last_of("/\\");
    std::string s = cut == std::string::npos ? in : in.substr(cut + 1);
    std::string o;
    for (unsigned char c : s) {
        const bool ok = std::isalnum(c) && c < 0x80;
        o += (ok || c == '.' || c == '_' || c == '-' || c == '(' || c == ')' || c == '+' || c == ' ') ? (char)c : '_';
    }
    while (!o.empty() && (o.front() == '.' || o.front() == ' ')) o.erase(o.begin());
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    if (o.size() > 100) {   // keep the extension
        const size_t dot = o.find_last_of('.');
        const std::string ext = dot != std::string::npos && o.size() - dot <= 12 ? o.substr(dot) : std::string();
        o = o.substr(0, 100 - ext.size()) + ext;
    }
    if (o.empty()) o = "received.bin";
    std::string stem = o.substr(0, o.find('.'));
    for (char& c : stem) c = (char)std::toupper((unsigned char)c);
    static const char* const kRes[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "LPT1", "LPT2", "LPT3"};
    for (const char* r : kRes) if (stem == r) { o = "_" + o; break; }
    return o;
}

std::string uniquePath(const std::filesystem::path& dir, const std::string& name) {
    namespace fs = std::filesystem;
    fs::path p = dir / name;
    std::error_code ec;
    if (!fs::exists(p, ec)) return p.string();
    const std::string stem = fs::path(name).stem().string(), ext = fs::path(name).extension().string();
    for (int i = 1; i < 10000; i++) {
        p = dir / (stem + " (" + std::to_string(i) + ")" + ext);
        if (!fs::exists(p, ec)) return p.string();
    }
    return std::string();
}

std::string openKey(const kaft::OpenMsg& o) {
    std::string k = o.fileName;
    k += '|' + std::to_string(o.payloadSize) + '|' + std::to_string(o.origSize) + '|';
    k.append((const char*)o.hash.data(), o.hash.size());
    return k;
}

} // namespace

struct KaftLiveReceiver::Impl {
    std::mutex feedMu;                       // the processing state below
    std::mutex pubMu;                        // the published telemetry, the callback and the folder
    std::unique_ptr<kaft::FrameReceiver> fr;
    std::unique_ptr<kaft::Receiver> rcv;
    double rate = 0;
    bool ready = false;
    uint64_t samples = 0, nextReport = 0, seq = 0;

    std::vector<kaft::RxFrame> pending;      // data frames of the burst being collected
    double pendingSec = 0;
    std::string openKeyCur, doneKey;         // the session announced, the last file completed
    bool ignoring = false;                   // the current OPEN repeats the file already delivered
    std::deque<std::pair<double, uint32_t>> goodBytes;

    KaftTelemetry cur;
    double lastFrameSec = -1e9;
    bool wasReceiving = false;
    KaftTelemetry pub;
    std::string folder;
    std::function<void(const std::string&)> log;

    double now() const { return rate > 0 ? (double)samples / rate : 0.0; }

    void say(const std::string& s) {
        std::function<void(const std::string&)> lg;
        { std::lock_guard<std::mutex> pk(pubMu); lg = log; }
        if (lg) lg(s);
    }

    void newReceiver() {
        kaft::ReceiverConfig rc;
        rc.maxPayload = kMaxFileBytes;
        rcv = std::make_unique<kaft::Receiver>(rc);
    }

    void startStream() {
        kaft::FrameReceiverConfig c;
        c.inputRate = (int)std::llround(rate);
        fr = std::make_unique<kaft::FrameReceiver>(c);
        newReceiver();
        samples = 0; nextReport = (uint64_t)(0.25 * rate);
        pending.clear(); goodBytes.clear();
        openKeyCur.clear(); doneKey.clear(); ignoring = false;
        cur = KaftTelemetry();
        lastFrameSec = -1e9;
        wasReceiving = false;
    }

    void flushPending() {
        if (pending.empty()) return;
        kaft::RxBurst rb;
        if (kaft::burstFromFrames(pending, rb)) {
            const std::vector<uint8_t> reply = rcv->onBurst(rb);
            if (kaft::msgType(reply) == (int)kaft::MsgType::Ack) {
                kaft::AckMsg a;
                if (kaft::decode(reply, a)) cur.requestedProfile = a.requestedProfile;
            }
            cur.haveBytes = rcv->bytesHeld();
            if (cur.session == 1) cur.session = 2;
        }
        pending.clear();
    }

    void save() {
        const std::vector<uint8_t>& data = rcv->file();
        KaftFile f;
        f.name = safeName(rcv->fileName());
        f.bytes = data.size();
        f.atSec = now();
        std::string dir;
        { std::lock_guard<std::mutex> pk(pubMu); dir = folder; }
        if (dir.empty()) {
            f.note = "no folder chosen: not saved";
        } else {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::create_directories(fs::path(dir), ec);
            const std::string path = uniquePath(fs::path(dir), f.name);
            if (path.empty()) {
                f.note = "could not find a free file name";
            } else {
                std::ofstream o(path, std::ios::binary);
                o.write((const char*)data.data(), (std::streamsize)data.size());
                o.close();
                if (o.fail()) { f.note = "could not write " + path; std::error_code e2; fs::remove(path, e2); }
                else { f.saved = true; f.path = path; f.note = "saved"; }
            }
        }
        say("KAFT file " + f.name + ", " + std::to_string(f.bytes) + " bytes, " + f.note);
        cur.files.insert(cur.files.begin(), f);
        if (cur.files.size() > kMaxFiles) cur.files.resize(kMaxFiles);
    }

    void onCtrl(const kaft::RxFrame& f) {
        std::vector<uint8_t> msg;
        if (!kaft::msgFromFrame(f, msg)) return;
        switch (kaft::msgType(msg)) {
        case (int)kaft::MsgType::Open: {
            kaft::OpenMsg o;
            if (!kaft::decode(msg, o)) return;
            const std::string key = openKey(o);
            if (!doneKey.empty() && key == doneKey) { ignoring = true; return; }   // the file we already have, sent again
            ignoring = false;
            const bool again = key == openKeyCur && o.session == cur.sessionId && cur.session != 0;   // the same session announced again (the sender repeats OPEN)
            const std::vector<uint8_t> reply = rcv->onCtrl(msg, (double)f.esN0Db);
            if (kaft::msgType(reply) != (int)kaft::MsgType::Accept) return;   // refused (too big, compressed): no session
            if (!again) {
                openKeyCur = key;
                cur.session = 1;
                cur.sessionId = o.session;
                cur.peer = o.callTx;
                cur.fileName = safeName(o.fileName);
                cur.fileBytes = o.payloadSize;
                cur.requestedProfile = -1;
                kaft::AcceptMsg a;
                uint64_t held = 0;
                if (kaft::decode(reply, a)) for (const auto& r : a.have) held += r.second - r.first;
                cur.resumedBytes = held;
                say("KAFT session " + std::to_string(o.session) + ": " + cur.fileName + ", " + std::to_string(o.payloadSize) + " bytes" + (held ? ", resumed" : ""));
            }
            cur.haveBytes = rcv->bytesHeld();
            cur.heldSessions = rcv->heldSessions();
            break;
        }
        case (int)kaft::MsgType::Start:
            if (!ignoring) rcv->onCtrl(msg, (double)f.esN0Db);
            break;
        case (int)kaft::MsgType::Abort:
            if (!ignoring) { rcv->onCtrl(msg, (double)f.esN0Db); cur.heldSessions = rcv->heldSessions(); }
            break;
        case (int)kaft::MsgType::Finish: {
            if (ignoring) return;
            flushPending();
            const std::vector<uint8_t> reply = rcv->onCtrl(msg, (double)f.esN0Db);
            cur.haveBytes = rcv->bytesHeld();
            const int rt = kaft::msgType(reply);
            if (rt == (int)kaft::MsgType::Verified && rcv->complete()) {
                save();
                doneKey = openKeyCur;
                cur.session = 3;
            } else if (rt == (int)kaft::MsgType::Reject) {
                kaft::RejectMsg r;
                if (kaft::decode(reply, r) && r.reason != 0) cur.session = 4;   // 0: blocks are missing, a later pass can bring them
            }
            cur.heldSessions = rcv->heldSessions();
            break;
        }
        default: break;
        }
    }

    void onFrame(const kaft::RxFrame& f) {
        const double t = now();
        lastFrameSec = t;
        cur.width = (int)f.header.width;
        cur.callsign = f.header.callsign;
        cur.esN0Db = f.esN0Db;
        cur.cfoHz = f.cfoHz;
        if (f.header.type == kaft::FrameType::Ack) { cur.framesAck++; return; }
        if (f.header.type == kaft::FrameType::Ctrl) { cur.framesCtrl++; onCtrl(f); return; }
        cur.framesData++;
        cur.profile = f.header.profile;
        if (f.header.profile >= 0 && f.header.profile < kaft::kNumProfiles) cur.profileFrames[f.header.profile]++;
        const int ok = f.blocksOk();
        cur.blocksOk += (uint64_t)ok;
        cur.blocksBad += (uint64_t)((int)f.blocks.size() - ok);
        if (ok < (int)f.blocks.size()) cur.framesBad++;
        if (ok > 0) goodBytes.emplace_back(t, (uint32_t)((size_t)ok * (size_t)kaft::dataBytesPerBlock(f.header.profile)));
        if (ignoring) return;
        if (!pending.empty()) {
            const kaft::RxFrame& p = pending.front();
            if (p.header.session != f.header.session || ((p.header.frameNo >> 4) & 0x0FFF) != ((f.header.frameNo >> 4) & 0x0FFF)) flushPending();
        }
        pending.push_back(f);
        pendingSec = t;
        if (f.header.lastInBurst) flushPending();
    }

    void report() {
        const double t = now();
        if (!pending.empty() && t - pendingSec > 1.0) flushPending();   // the last frame of that burst never came
        while (!goodBytes.empty() && t - goodBytes.front().first > 10.0) goodBytes.pop_front();
        double bytes = 0;
        for (const auto& g : goodBytes) bytes += g.second;
        const bool receiving = t - lastFrameSec <= kFrameTimeoutSec;
        KaftTelemetry o = cur;
        o.seq = ++seq;
        o.state = receiving ? 2 : samples ? 1 : 0;
        o.inputRate = rate;
        o.dataValid = receiving && o.blocksOk > 0;
        o.snrDb = o.esN0Db;
        o.timeSec = t;
        o.lastFrameAgeSec = lastFrameSec > -1e8 ? std::max(0.0, t - lastFrameSec) : 0.0;
        o.throughputBps = bytes * 8.0 / std::max(1.0, std::min(10.0, t));
        std::function<void(const std::string&)> lg;
        {
            std::lock_guard<std::mutex> lk(pubMu);
            o.saveFolder = folder;
            pub = std::move(o);
            lg = log;
        }
        if (lg && receiving != wasReceiving) lg(receiving ? "KAFT frames found" : "KAFT frames stopped");
        wasReceiving = receiving;
    }
};

KaftLiveReceiver::KaftLiveReceiver() : p_(std::make_unique<Impl>()) {}
KaftLiveReceiver::~KaftLiveReceiver() = default;

void KaftLiveReceiver::configure(double r) {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.rate = r;
    m.ready = r >= kaftTuning().minSampleRate - 1;
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KaftTelemetry();
    m.pub.saveFolder = m.folder;
    m.pub.seq = ++m.seq;
    m.pub.inputRate = r;
}

bool KaftLiveReceiver::ready() const {
    std::lock_guard<std::mutex> lk(p_->feedMu);
    return p_->ready;
}

void KaftLiveReceiver::reset() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (m.ready) m.startStream();
    std::lock_guard<std::mutex> pk(m.pubMu);
    m.pub = KaftTelemetry();
    m.pub.saveFolder = m.folder;
    m.pub.seq = ++m.seq;
    m.pub.inputRate = m.rate;
}

void KaftLiveReceiver::clearFiles() {
    Impl& m = *p_;
    std::lock_guard<std::mutex> lk(m.feedMu);
    m.cur.files.clear();
}

void KaftLiveReceiver::feed(const cf32* x, size_t n) {
    Impl& m = *p_;
    if (n == 0) return;
    std::lock_guard<std::mutex> lk(m.feedMu);
    if (!m.ready) return;
    m.fr->push(x, n);
    m.samples += n;
    for (const kaft::RxFrame& f : m.fr->take()) m.onFrame(f);
    if (m.samples >= m.nextReport) {
        m.nextReport = m.samples + (uint64_t)(0.25 * m.rate);
        m.report();
    }
}

bool KaftLiveReceiver::telemetry(KaftTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(p_->pubMu);
    if (p_->pub.seq == 0 || p_->pub.seq == lastSeq) return false;
    out = p_->pub;
    return true;
}

void KaftLiveReceiver::setLogCallback(std::function<void(const std::string&)> cb) { std::lock_guard<std::mutex> lk(p_->pubMu); p_->log = std::move(cb); }
void KaftLiveReceiver::setSaveFolder(const std::string& f) { std::lock_guard<std::mutex> lk(p_->pubMu); p_->folder = f; }

ModeTuning kaftTuning() {
    ModeTuning t;
    t.stdMode = 127; t.id = "kaft"; t.name = "KAFT";
    t.minMhz = 100; t.maxMhz = 6000; t.defMhz = 1290;
    t.sampleRate = 2.4e6; t.bandwidthMhz = 1.0; t.minSampleRate = 1.25e6;
    return t;
}

} // namespace dect2
