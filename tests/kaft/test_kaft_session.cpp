// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// KAFT session layer: hash and CRC known vectors, message round trips, ranges, stepper, compression rule, and a 2 MB transfer through a lossy
// link with an interruption and a resume
#include "dect2/kaft/kaft_session.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
using namespace dect2;
using namespace dect2::kaft;
static int fails = 0;
#define CHECK(c, fmt, ...) do { if (!(c)) { printf("FAIL line %d: " fmt "\n", __LINE__ __VA_OPT__(,) __VA_ARGS__); fails++; } } while (0)   // Flawfinder: ignore (the format is a literal)

struct Rng {
    kris::XorShift64 s;
    explicit Rng(uint64_t seed) : s{seed ? seed : 1} {}
    uint64_t next() { return s.next(); }
    double uni() { return (double)(s.next() >> 11) * (1.0 / 9007199254740992.0); }   // [0, 1)
};

static std::string hex(const Hash32& h) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : h) { s.push_back(d[b >> 4]); s.push_back(d[b & 15]); }
    return s;
}

static std::vector<uint8_t> bytesOf(const char* s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

static std::vector<uint8_t> makeText(size_t n, uint64_t seed) {
    static const char* words[] = {"the", "radio", "signal", "over", "band", "antenna", "hertz", "carrier", "noise", "frame", "burst", "block", "phase", "pilot",
                                  "ground", "wave", "mast", "tower", "hill", "valley", "weather", "report", "log", "call", "sign", "net", "check", "copy",
                                  "over", "out", "roger", "station", "mobile", "portable", "repeater", "tone", "squelch", "gain", "filter", "mixer"};
    Rng rng(seed);
    std::vector<uint8_t> t;
    t.reserve(n + 16);
    while (t.size() < n) {
        const char* w = words[rng.next() % (sizeof(words) / sizeof(words[0]))];
        t.insert(t.end(), w, w + std::strlen(w));
        t.push_back((rng.next() % 12 == 0) ? '\n' : ' ');
    }
    t.resize(n);
    return t;
}

// a small run-length coder standing in for zstd (none is linked): byte, count
static std::vector<uint8_t> rleCompress(const uint8_t* p, size_t n) {
    std::vector<uint8_t> o;
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j < n && p[j] == p[i] && j - i < 255) j++;
        o.push_back(p[i]);
        o.push_back((uint8_t)(j - i));
        i = j;
    }
    return o;
}
static bool rleDecompress(const uint8_t* p, size_t n, std::vector<uint8_t>& out) {
    out.clear();
    if (n % 2) return false;
    for (size_t i = 0; i < n; i += 2) out.insert(out.end(), p[i + 1], p[i]);
    return true;
}

struct Link {
    double loss = 0, ackLoss = 0;
    Rng rng{1};
    double esN0 = 40.0;
};
struct RunResult {
    bool done = false, failed = false, stopped = false;
};

// drive a sender against a receiver through the link; stop when stopAcked bytes are acknowledged (0 = run to the end)
static RunResult run(Sender& s, Receiver& r, Link& L, uint64_t stopAcked) {
    RunResult res;
    for (int iter = 0; iter < 400000; iter++) {
        if (stopAcked && s.bytesAcked() >= stopAcked) { res.stopped = true; return res; }
        const SendAction a = s.next();
        if (a.kind == SendAction::Done) { res.done = true; return res; }
        if (a.kind == SendAction::Failed || a.kind == SendAction::None) { res.failed = true; return res; }
        if (a.kind == SendAction::Ctrl) {
            const std::vector<uint8_t> reply = r.onCtrl(a.ctrl, L.esN0);
            if (!a.expectsReply) continue;
            if (reply.empty()) s.onTimeout();
            else s.onReply(reply);
            continue;
        }
        RxBurst rb;
        rb.session = a.burst.session;
        rb.burstNo = a.burst.burstNo;
        rb.profile = a.burst.profile;
        rb.esN0Db = (float)L.esN0;
        for (const auto& fr : a.burst.frames) {
            RxBurstFrame f;
            f.seen = true;
            for (const DataBlock& b : fr) {
                RxDataBlock d;
                d.ok = L.rng.uni() >= L.loss;
                d.blk = b;
                f.blocks.push_back(std::move(d));
            }
            rb.frames.push_back(std::move(f));
        }
        const std::vector<uint8_t> ack = r.onBurst(rb);
        if (ack.empty() || L.rng.uni() < L.ackLoss) s.onTimeout();
        else s.onReply(ack);
    }
    res.failed = true;
    return res;
}

int main() {
    // (e) known vectors
    {
        const auto abc = bytesOf("abc");
        CHECK(hex(blake2s256(abc.data(), 3)) == "508c5e8c327c14e2e1a72ba34eeb452f37458b209ed63a294d999b4c86675982", "BLAKE2s-256(abc) %s", hex(blake2s256(abc.data(), 3)).c_str());
        CHECK(hex(blake2s256(nullptr, 0)) == "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9", "BLAKE2s-256('') %s", hex(blake2s256(nullptr, 0)).c_str());
        CHECK(hex(sha256Hash(abc.data(), 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256(abc) %s", hex(sha256Hash(abc.data(), 3)).c_str());
        CHECK(hex(hashBytes(0, abc.data(), 3)) == hex(blake2s256(abc.data(), 3)) && hex(hashBytes(1, abc.data(), 3)) == hex(sha256Hash(abc.data(), 3)), "hash codes");
        // a message longer than one block, and exactly one and two blocks
        std::vector<uint8_t> a64(64, 'a'), a65(65, 'a'), a128(128, 'a');
        CHECK(blake2s256(a64.data(), 64) != blake2s256(a65.data(), 65) && blake2s256(a64.data(), 64) != blake2s256(a128.data(), 128), "block boundaries differ");
        const auto d = bytesOf("123456789");
        CHECK(kris::crc32(d.data(), 9) == 0xCBF43926u, "CRC-32 check value %08X", kris::crc32(d.data(), 9));
        auto x = d;
        const uint16_t c16 = kris::crc16(x.data(), x.size());
        x[3] ^= 0x04;
        CHECK(kris::crc16(x.data(), x.size()) != c16, "CRC-16 sees a flipped bit");
    }

    // (f) OPEN / ACCEPT and the other messages
    {
        OpenMsg o;
        o.session = 0xABCD; o.callTx = "LY1ABC"; o.callRx = "CQ"; o.width = 2; o.fileName = "C:\\photos\\sunset_0042.jpg"; o.payloadSize = 123456789012ull;
        o.origSize = 999999999999ull; o.hashCode = 1; o.compression = 1; o.startProfile = 6; o.resumeOffset = 77777; o.blockSize = 790;
        for (int i = 0; i < 32; i++) o.hash[(size_t)i] = (uint8_t)(i * 7 + 1);
        OpenMsg g;
        const auto e = encode(o);
        CHECK(msgType(e) == (int)MsgType::Open, "OPEN type");
        CHECK(decode(e, g) && g.session == o.session && g.callTx == "LY1ABC" && g.callRx == "CQ" && g.width == 2 && g.fileName == "sunset_0042.jpg" &&
                  g.payloadSize == o.payloadSize && g.origSize == o.origSize && g.hashCode == 1 && g.hash == o.hash && g.compression == 1 && g.startProfile == 6 &&
                  g.resumeOffset == 77777 && g.blockSize == 790,
              "OPEN round trip '%s'", g.fileName.c_str());
        auto cut = e;
        cut.resize(e.size() - 3);
        CHECK(!decode(cut, g), "OPEN truncated");
        AcceptMsg a;
        a.session = 0xABCD; a.callRx = "DL2XYZ"; a.esN0x10 = -57; a.profile = 3;
        a.have = {{0, 4096}, {10000, 20000}, {1000000, 2000000}};
        AcceptMsg h;
        CHECK(decode(encode(a), h) && h.session == a.session && h.callRx == "DL2XYZ" && h.esN0x10 == -57 && h.profile == 3 && h.have == a.have, "ACCEPT round trip");
        StartMsg st{7, 5}, st2;
        CHECK(decode(encode(st), st2) && st2.session == 7 && st2.profile == 5, "START");
        FinishMsg f;
        f.session = 9; f.payloadSize = 5; f.hashCode = 0; f.hash = o.hash; f.origHash = blake2s256(nullptr, 0);
        FinishMsg f2;
        CHECK(decode(encode(f), f2) && f2.payloadSize == 5 && f2.hash == f.hash && f2.origHash == f.origHash, "FINISH");
        VerifiedMsg v{9, true}, v2;
        CHECK(decode(encode(v), v2) && v2.ok && v2.session == 9, "VERIFIED");
        RejectMsg rj;
        rj.session = 4; rj.reason = 1; rj.have = {{5, 6}};
        RejectMsg rj2;
        CHECK(decode(encode(rj), rj2) && rj2.reason == 1 && rj2.have == rj.have, "REJECT");
        PollMsg p{3, 4001}, p2;
        CHECK(decode(encode(p), p2) && p2.burstNo == 4001, "POLL");
        AckMsg k;
        k.session = 3; k.burstNo = 11; k.esN0x10 = 231; k.lossPct = 12; k.requestedProfile = 5;
        k.seen = {1, 0, 1, 1};
        k.blockOk = {{1, 0, 1, 1, 1, 0, 0, 1, 1, 1}, {}, {1}, {0, 0, 0}};
        AckMsg k2;
        CHECK(decode(encode(k), k2) && k2.burstNo == 11 && k2.esN0x10 == 231 && k2.lossPct == 12 && k2.requestedProfile == 5 && k2.seen == k.seen && k2.blockOk == k.blockOk, "ACK");
        // the largest ack: 16 frames of 39 blocks must fit one control message
        AckMsg big;
        big.seen.assign(16, 1);
        big.blockOk.assign(16, std::vector<uint8_t>(39, 1));
        CHECK(encode(big).size() <= kMaxCtrlBytes, "big ack %zu bytes", encode(big).size());
        // control message through the frame glue (no modulation): too long fails, a long ACCEPT works
        AcceptMsg la;
        la.session = 5; la.callRx = "X";
        for (uint32_t i = 0; i < kMaxMapRanges; i++) la.have.push_back({i * 1000, i * 1000 + 500});
        const auto lb = encode(la);
        CHECK(lb.size() <= kMaxCtrlBytes, "ACCEPT with %zu ranges is %zu bytes", la.have.size(), lb.size());
        const FrameSpec ctl = frameForCtrl(lb, false, 5, 1, Width::W200k, "LY1ABC");
        CHECK(!ctl.blocks.empty() && (int)ctl.blocks.size() <= kCtrlMaxBlocks, "control frame blocks %zu", ctl.blocks.size());
        RxFrame rf;
        rf.header = ctl.header;
        rf.header.nBlocks = (uint8_t)ctl.blocks.size();
        for (const auto& b : ctl.blocks) {
            RxBlock rb;
            rb.ok = true;
            rb.payload = b;
            rb.payload.resize((size_t)payloadBytes(kCtrlProfile), 0);
            rf.blocks.push_back(std::move(rb));
        }
        std::vector<uint8_t> back;
        CHECK(msgFromFrame(rf, back) && back == lb, "control message through frames");
        rf.blocks[0].ok = false;
        CHECK(!msgFromFrame(rf, back), "a failed control block fails the message");
        CHECK(frameForCtrl(std::vector<uint8_t>(kMaxCtrlBytes + 10, 1), false, 5, 1, Width::W1M, "X").blocks.empty(), "oversize message refused");
    }

    // ranges
    {
        RangeSet r;
        r.add(10, 20); r.add(30, 40); r.add(20, 30);
        CHECK(r.ranges().size() == 1 && r.ranges()[0].first == 10 && r.ranges()[0].second == 40 && r.total() == 30, "ranges merge");
        r.remove(15, 25);
        CHECK(r.ranges().size() == 2 && r.covers(10, 15) && r.covers(25, 40) && !r.covers(14, 26) && r.total() == 20, "ranges split");
        r.add(0, 100);
        CHECK(r.ranges().size() == 1 && r.total() == 100, "ranges swallow");
    }

    // block accounting: payload per profile
    {
        for (int p = 0; p < kNumProfiles; p++)
            CHECK(payloadBytes(p) * 8 + kCrcBits <= blockInfoBits(p) && payloadBytes(p) > kBlockHdrBytes, "profile %d payload %d", p, payloadBytes(p));
        std::vector<uint8_t> d = {1, 2, 3};
        uint32_t off = 0;
        std::vector<uint8_t> out;
        CHECK(unpackDataBlock(packDataBlock(123456, d.data(), 3), off, out) && off == 123456 && out == d, "data block header");
    }

    // profile stepper
    {
        ProfileStepper st;
        int p = 4;
        p = st.update(p, 40, 0.0); CHECK(p == 4, "no step after 1 burst");
        p = st.update(p, 40, 0.0); CHECK(p == 4, "no step after 2 bursts");
        p = st.update(p, 40, 0.0); CHECK(p == 5, "step up after 3 good bursts, got %d", p);
        p = st.update(p, 40, 0.3); CHECK(p == 4, "step down at 30 percent loss, got %d", p);
        for (int i = 0; i < 9; i++) p = st.update(p, 40, 0.0);
        CHECK(p == 4, "hysteresis holds the profile, got %d", p);
        for (int i = 0; i < 6; i++) p = st.update(p, 40, 0.0);
        CHECK(p > 4, "steps up again after the hysteresis, got %d", p);
        ProfileStepper s2;
        int q = s2.update(6, 60.0, 0.6);
        CHECK(q == 4, "two steps down above 50 percent loss, got %d", q);
        ProfileStepper s3;
        q = s3.update(6, 20.0, 0.0);   // P6 needs 19.8 + 0.5
        CHECK(q == 5, "step down below requirement + 0.5 dB, got %d", q);
        CHECK(startingProfile(40.0) == 9 && startingProfile(12.0) == 4 && startingProfile(-10.0) == 0, "starting profile");
    }

    // compression rule and plumbing
    {
        const std::vector<uint8_t> flat(100000, 'x');
        auto rnd = makeText(100000, 5);
        Rng rr(9);
        for (uint8_t& b : rnd) b = (uint8_t)rr.next();
        CompressFn cfn = [](const uint8_t* p, size_t n) { return rleCompress(p, n); };
        CHECK(compressionWorthwhile(flat.data(), flat.size(), "log.txt", cfn), "repetitive data is worth compressing");
        CHECK(!compressionWorthwhile(rnd.data(), rnd.size(), "data.bin", cfn), "random data is skipped");
        CHECK(!compressionWorthwhile(flat.data(), flat.size(), "movie.MP4", cfn), "known compressed extension is skipped");
        CHECK(!compressionWorthwhile(flat.data(), flat.size(), "log.txt", CompressFn()), "no codec: skipped");

        SenderConfig sc;
        sc.callsign = "LY1ABC"; sc.fileName = "flat.txt"; sc.compress = cfn; sc.compressionId = 200; sc.session = 11;
        ReceiverConfig rc;
        rc.callsign = "DL2XYZ";
        rc.decompressors[200] = rleDecompress;
        Sender s(sc, flat);
        Receiver r(rc);
        Link L;
        CHECK(s.compression() == 200 && s.payloadSize() < flat.size() / 10, "compressed to %llu bytes", (unsigned long long)s.payloadSize());
        const RunResult res = run(s, r, L, 0);
        CHECK(res.done && r.complete() && r.file() == flat, "compressed transfer");
        // a receiver without the codec refuses
        Sender s2(sc, flat);
        Receiver r2(ReceiverConfig{});
        const RunResult res2 = run(s2, r2, L, 0);
        CHECK(res2.failed && !r2.complete(), "unsupported compression is refused");
    }

    // (d) 2 MB of pseudo-random text through a lossy link, with an interruption and a resume
    {
        const std::vector<uint8_t> file = makeText(2 * 1024 * 1024, 4242);
        const double losses[] = {0.0, 0.05, 0.2, 0.5};
        for (double loss : losses) {
            SenderConfig sc;
            sc.callsign = "LY1ABC"; sc.fileName = "report_2mb.txt"; sc.width = Width::W1M; sc.startProfile = 4; sc.session = 1;
            ReceiverConfig rc;
            rc.callsign = "DL2XYZ";
            Receiver r(rc);
            Link L;
            L.loss = loss;
            L.ackLoss = 0.01;
            L.rng = Rng(100 + (uint64_t)(loss * 1000));
            Sender s1(sc, file);
            const RunResult a = run(s1, r, L, file.size() * 4 / 10);   // interrupted at 40 percent
            CHECK(a.stopped, "loss %.2f: first session interrupted (done %d failed %d)", loss, a.done, a.failed);
            const SenderStats st1 = s1.stats();
            const uint64_t heldBefore = r.bytesHeld();
            sc.session = 2;
            Sender s2(sc, file);   // a new sender of the same file: the receiver's block map is returned in ACCEPT
            const RunResult b = run(s2, r, L, 0);
            const SenderStats st2 = s2.stats();
            CHECK(b.done, "loss %.2f: second session done %d failed %d", loss, b.done, b.failed);
            CHECK(r.complete() && r.file() == file, "loss %.2f: exact file delivered (hash verified)", loss);
            CHECK(s2.stats().dataBytesSent <= file.size() * 3 + 1000000, "loss %.2f: the resume did not start over", loss);
            const double sent = (double)(st1.dataBytesSent + st2.dataBytesSent);
            const double eff = (double)file.size() / sent;
            const double air = st1.airtime + st2.airtime;
            printf("loss %2.0f %%: held at resume %llu kB, sent %.0f kB, efficiency %.3f (1-loss %.3f), airtime %.1f s, goodput %.0f kbit/s, final profile P%d\n", loss * 100,
                   (unsigned long long)(heldBefore / 1024), sent / 1024.0, eff, 1.0 - loss, air, file.size() * 8.0 / air / 1000.0, s2.profile());
            if (loss <= 0.2) CHECK(eff >= 0.9 * (1.0 - loss), "loss %.2f: efficiency %.3f < %.3f", loss, eff, 0.9 * (1.0 - loss));
        }
    }

    // a lost ACCEPT / lost VERIFIED are recovered by the retries; an empty file works
    {
        const std::vector<uint8_t> file = makeText(5000, 3);
        SenderConfig sc;
        sc.callsign = "LY1ABC"; sc.fileName = "small.txt"; sc.session = 21;
        Sender s(sc, file);
        Receiver r(ReceiverConfig{});
        SendAction a = s.next();
        CHECK(a.kind == SendAction::Ctrl && msgType(a.ctrl) == (int)MsgType::Open, "first action is OPEN");
        r.onCtrl(a.ctrl, 20.0);   // the ACCEPT never arrives
        s.onTimeout();
        a = s.next();
        CHECK(a.kind == SendAction::Ctrl && msgType(a.ctrl) == (int)MsgType::Open, "OPEN is repeated");
        s.onReply(r.onCtrl(a.ctrl, 20.0));
        Link L;
        const RunResult res = run(s, r, L, 0);
        CHECK(res.done && r.file() == file, "small file delivered");

        Sender se(sc, std::vector<uint8_t>());
        Receiver re(ReceiverConfig{});
        sc.session = 22;
        const RunResult res2 = run(se, re, L, 0);
        CHECK(res2.done && re.complete() && re.file().empty(), "empty file");
    }

    if (fails) { printf("kaft_session: %d failure(s)\n", fails); return 1; }
    printf("kaft_session: ok\n");
    return 0;
}
