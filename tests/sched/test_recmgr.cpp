// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The recordings manager core: classify, scan with SigMF side-cars and the index, search, sort, the cleanup rule, exact trimming of IQ, SigMF
// and WAV, conversion, rename, trash and undo.
#include "dect2/sched/recmgr.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unistd.h>

using namespace dect2::recmgr;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << data;
}
static std::string get(const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); }

static Entry E(const char* name, int64_t t, uint64_t bytes, bool fav = false, const char* folder = "") {
    Entry e; e.name = name; e.path = name; e.mtime = t; e.bytes = bytes; e.favourite = fav; e.folder = folder; return e;
}

static void testClassify() {
    CHECK(classify("a.cs8") == Kind::Iq && classify("A.SIGMF-META") == Kind::SigMF && classify("x.wav") == Kind::Audio && classify("x.ts") == Kind::Video);
    CHECK(classify("x.png") == Kind::Image && classify("x.log") == Kind::Log && classify("x.xyz") == Kind::Other && classify("noext") == Kind::Other);
}

static void testCleanup() {
    const int64_t day = 86400, now = 100 * day;
    std::vector<Entry> v = {E("new", now - 1 * day, 100), E("mid", now - 5 * day, 100), E("old", now - 40 * day, 100), E("oldfav", now - 50 * day, 100, true), E("ancient", now - 90 * day, 100)};
    v[4].tags = {"Keep"};   // tagged
    CleanupRule r; r.olderThanDays = 30;
    auto p = planCleanup(v, r, now);
    CHECK(p.remove.size() == 1 && v[p.remove[0]].name == "old" && p.freedBytes == 100);
    // budget: keep the newest 250 bytes: new + mid fit, the next (old) is over, protected ones stay however old
    r = CleanupRule(); r.keepNewestBytes = 250;
    p = planCleanup(v, r, now);
    CHECK(p.remove.size() == 1 && v[p.remove[0]].name == "old");
    // a smaller older file does not jump the queue: strictly newest first
    std::vector<Entry> w = {E("a", 300, 200), E("b", 200, 200), E("c", 100, 10)};
    r.keepNewestBytes = 250;
    p = planCleanup(w, r, 1000);
    CHECK(p.remove.size() == 2);
    // protected files count against the budget
    std::vector<Entry> z = {E("fav", 300, 240, true), E("x", 200, 20), E("y", 100, 5)};
    p = planCleanup(z, r, 1000);
    CHECK(p.remove.size() == 2 && z[p.remove[0]].name == "x");
    // both rules; budget 0 and days 0 do nothing
    CHECK(planCleanup(v, CleanupRule(), now).remove.empty());
    // per folder
    std::vector<Entry> f = {E("a1", 300, 100, false, "iq"), E("a2", 200, 100, false, "iq"), E("b1", 300, 100, false, "audio"), E("b2", 200, 100, false, "audio")};
    auto pf = planCleanupByFolder(f, [](const std::string& d) { CleanupRule c; if (d == "iq") c.keepNewestBytes = 150; return c; }, 1000);
    CHECK(pf.remove.size() == 1 && f[pf.remove[0]].name == "a2");
}

static void testSearch() {
    Entry e = E("noor_93.9MHz_2026.wav", 0, 5000000); e.mode = "FM"; e.tags = {"Radio", "demo"}; e.notes = "morning show"; e.seconds = 90; e.kind = Kind::Audio;
    CHECK(matches(e, "") && matches(e, "noor") && matches(e, "NOOR fm") && matches(e, "morning") && matches(e, "radio"));
    CHECK(!matches(e, "noor am") && matches(e, "tag:demo") && !matches(e, "tag:dem") && matches(e, "kind:audio") && !matches(e, "kind:iq"));
    CHECK(matches(e, "mode:fm") && matches(e, ">1MB") && !matches(e, ">10MB") && matches(e, "<2min") && matches(e, ">60s") && !matches(e, "fav"));
    e.favourite = true;
    CHECK(matches(e, "fav"));
    std::vector<Entry> v = {E("b", 2, 30), E("A", 3, 10), E("c", 1, 20)};
    sortEntries(v, Column::Name, true);
    CHECK(v[0].name == "A" && v[2].name == "c");
    sortEntries(v, Column::Size, false);
    CHECK(v[0].bytes == 30);
    sortEntries(v, Column::Date, true);
    CHECK(v[0].mtime == 1);
}

static void testTrim() {
    std::string err; SampleRange r;
    CHECK(trimRange(1.0, 2.0, 1000.0, 5000, r, err) && r.first == 1000 && r.end == 2000);
    CHECK(trimRange(0.0004, 0.0016, 1000.0, 5000, r, err) && r.first == 0 && r.end == 2);   // rounded to the nearest sample
    CHECK(trimRange(-3, 100, 1000.0, 5000, r, err) && r.first == 0 && r.end == 5000);
    CHECK(!trimRange(2, 2, 1000.0, 5000, r, err) && !trimRange(6, 7, 1000.0, 5000, r, err) && !trimRange(0, 1, 0, 5000, r, err));
    CHECK(bytesPerComplexSample("cf32_le") == 8 && bytesPerComplexSample("ci16_le") == 4 && bytesPerComplexSample("cu8") == 2 && bytesPerComplexSample("rf32_le") == 0);
}

int main() {
    testClassify(); testCleanup(); testSearch(); testTrim();
    const fs::path root = fs::temp_directory_path() / ("recmgr_test_" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    // an IQ recording as SigMF: 1000 samples of cs8 (2 bytes) at 1 kHz, sample i = (i % 100, i % 100 + 1)
    std::string iq;
    for (int i = 0; i < 1000; i++) { iq += (char)(i % 100); iq += (char)(i % 100 + 1); }
    put(root / "iq" / "cap1.sigmf-data", iq);
    std::string err;
    CHECK(writeSigmfSidecar((root / "iq" / "cap1.sigmf-meta").string(), "ci8", 1000, 93.9e6, "FM", err));
    put(root / "iq" / "raw_2.5Msps.cs8", std::string(5000000, 'x'));
    // a WAV: 8 kHz mono 16-bit, 8000 frames, frame i has value i
    {
        std::string w = "RIFF"; auto w32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) w += (char)(v >> (8 * i)); };
        auto w16 = [&](uint16_t v) { w += (char)v; w += (char)(v >> 8); };
        w32(36 + 16000); w += "WAVEfmt "; w32(16); w16(1); w16(1); w32(8000); w32(16000); w16(2); w16(16); w += "data"; w32(16000);
        for (int i = 0; i < 8000; i++) w16((uint16_t)(i & 0xffff));
        put(root / "audio" / "talk.wav", w);
    }
    put(root / "images" / "pic.png", "PNG");
    put(root / "notes.log", "hello");

    Index idx;
    Meta m; m.tags = {"favourite", "demo"}; m.notes = "good one"; idx.meta["audio/talk.wav"] = m;
    auto list = scan(root.string(), idx);
    CHECK(list.size() == 5);   // cap1 (meta+data as one), raw, wav, png, log
    const Entry* cap = nullptr; const Entry* wav = nullptr; const Entry* raw = nullptr;
    for (auto& e : list) { if (e.name == "cap1.sigmf-meta") cap = &e; if (e.name == "talk.wav") wav = &e; if (e.name == "raw_2.5Msps.cs8") raw = &e; }
    CHECK(cap && cap->kind == Kind::SigMF && cap->samples == 1000 && cap->seconds == 1.0 && cap->freqHz == 93.9e6 && cap->mode == "FM" && cap->files.size() == 2 && cap->folder == "iq");
    CHECK(wav && wav->seconds == 1.0 && wav->isProtected() && wav->notes == "good one" && wav->tags.size() == 2);
    CHECK(raw && raw->sampleRate == 2.5e6 && raw->samples == 2500000 && raw->seconds == 1.0);
    CHECK(relKey(root.string(), (root / "audio" / "talk.wav").string()) == "audio/talk.wav");

    // the index round trip and rename
    const std::string ip = (root / "recindex.json").string();
    CHECK(idx.save(ip, err));
    Index i2;
    CHECK(i2.load(ip) && i2.meta.size() == 1 && i2.meta["audio/talk.wav"].notes == "good one" && i2.meta["audio/talk.wav"].tags[0] == "favourite");
    i2.rename("audio/talk.wav", "audio/chat.wav");
    CHECK(i2.meta.count("audio/chat.wav") == 1 && i2.meta.count("audio/talk.wav") == 0);
    Index none; CHECK(!none.load((root / "missing.json").string()));
    put(root / "bad.json", "{oops");
    CHECK(!none.load((root / "bad.json").string()));
    CHECK(scan(root.string(), idx).size() == 6);   // the index file is not listed; bad.json is a log

    // trim SigMF: samples 250 .. 750 exactly
    SampleRange used;
    const std::string base = (root / "iq" / "cap1_trim").string();
    CHECK(trimSigmf((root / "iq" / "cap1.sigmf-meta").string(), base, 0.25, 0.75, &used, err));
    CHECK(used.first == 250 && used.end == 750);
    const std::string td = get(base + ".sigmf-data");
    CHECK(td.size() == 1000 && (unsigned char)td[0] == 50 && (unsigned char)td[1] == 51 && (unsigned char)td[998] == 49);   // sample 250 -> 250 % 100 = 50
    const std::string tm = get(base + ".sigmf-meta");
    CHECK(tm.find("core:sample_rate") != std::string::npos && tm.find("trimmed") != std::string::npos && tm.find("93900000") != std::string::npos);
    Index ni;
    auto l2 = scan(root.string(), ni);
    bool seen = false;
    for (auto& e : l2) if (e.name == "cap1_trim.sigmf-meta") seen = e.samples == 500 && e.seconds == 0.5;
    CHECK(seen);
    CHECK(!trimSigmf((root / "iq" / "cap1.sigmf-meta").string(), base + "x", 5, 6, nullptr, err));

    // trim raw
    CHECK(trimRawIq((root / "iq" / "cap1.sigmf-data").string(), (root / "raw_cut.cs8").string(), 2, {10, 20}, err) && get(root / "raw_cut.cs8").size() == 20);

    // trim WAV: frames 4000 .. 6000 at 8 kHz = 0.5 .. 0.75 s
    CHECK(trimWav((root / "audio" / "talk.wav").string(), (root / "cut.wav").string(), 0.5, 0.75, &used, err) && used.first == 4000 && used.end == 6000);
    WavInfo wi;
    CHECK(readWavInfo((root / "cut.wav").string(), wi, err) && wi.frames() == 2000 && wi.rate == 8000 && wi.bits == 16);
    const std::string cw = get(root / "cut.wav");
    CHECK((unsigned char)cw[44] == (4000 & 0xff) && (unsigned char)cw[45] == (4000 >> 8));   // the first frame is frame 4000
    put(root / "notwav.wav", "hello world, this is not a wav file");
    CHECK(!readWavInfo((root / "notwav.wav").string(), wi, err));

    // convert: cs8 -> cf32 -> cs8 keeps the values
    CHECK(convertIq((root / "iq" / "cap1.sigmf-data").string(), "cs8", (root / "f.cf32").string(), "cf32", err));
    CHECK(get(root / "f.cf32").size() == 8000);
    CHECK(convertIq((root / "f.cf32").string(), "cf32", (root / "back.cs8").string(), "cs8", err));
    CHECK(get(root / "back.cs8") == iq);
    CHECK(convertIq((root / "f.cf32").string(), "cf32", (root / "x.ci16").string(), "ci16", err) && get(root / "x.ci16").size() == 4000);
    CHECK(!convertIq((root / "f.cf32").string(), "bogus", (root / "y").string(), "cs8", err));

    // rename: both files of a SigMF move together; collisions are refused
    Entry capEntry;
    for (auto& e : scan(root.string(), ni)) if (e.name == "cap1.sigmf-meta") capEntry = e;
    std::string np;
    CHECK(renameEntry(capEntry, "renamed/..:x", np, err) == true);
    CHECK(fs::exists(root / "iq" / "renamed__..x.sigmf-meta") || fs::exists(root / "iq" / "renamed__.._x.sigmf-meta") || !np.empty());
    CHECK(np.find("sigmf-meta") != std::string::npos && fs::exists(np) && fs::exists(np.substr(0, np.size() - 4) + "data"));
    CHECK(!fs::exists(root / "iq" / "cap1.sigmf-data"));
    Entry rawEntry;
    for (auto& e : scan(root.string(), ni)) if (e.name == "raw_2.5Msps.cs8") rawEntry = e;
    CHECK(!renameEntry(rawEntry, "raw_2.5Msps", np, err));          // same name
    CHECK(!renameEntry(rawEntry, "  ", np, err));                    // empty
    put(root / "iq" / "taken.cs8", "z");
    CHECK(!renameEntry(rawEntry, "taken", np, err));                 // exists
    CHECK(renameEntry(rawEntry, "fresh_2.5Msps", np, err) && fs::exists(root / "iq" / "fresh_2.5Msps.cs8"));

    // trash and undo
    const std::string trash = (root / ".trash").string();
    std::string grp;
    const std::string victim = (root / "notes.log").string();
    CHECK(moveToTrash({victim, (root / "cut.wav").string()}, trash, 1000, grp, err));
    CHECK(!fs::exists(victim) && !fs::exists(root / "cut.wav") && listTrash(trash).size() == 2);
    put(root / "cut.wav", "new file with the old name");
    CHECK(!restoreFromTrash(trash, grp, err) && !fs::exists(victim));   // nothing moved when one name is taken
    fs::remove(root / "cut.wav");
    CHECK(restoreFromTrash(trash, grp, err) && fs::exists(victim) && fs::exists(root / "cut.wav") && listTrash(trash).empty());
    CHECK(!restoreFromTrash(trash, "nope", err));
    CHECK(moveToTrash({victim}, trash, 1000, grp, err));
    CHECK(purgeTrash(trash, 30, 1000 + 86400) == 0 && listTrash(trash).size() == 1);   // too young
    CHECK(purgeTrash(trash, 30, 1000 + 40 * 86400) == 5 && listTrash(trash).empty());   // "hello"
    CHECK(!moveToTrash({(root / "does_not_exist").string()}, trash, 1000, grp, err));
    // the trash is not listed as recordings
    moveToTrash({(root / "cut.wav").string()}, trash, 1000, grp, err);
    for (auto& e : scan(root.string(), ni)) CHECK(e.name != "cut.wav" && e.name != "journal.json");
    CHECK(folderBytes(root.string()) > 5000000);

    fs::remove_all(root);
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("recmgr: ok\n");
    return 0;
}
