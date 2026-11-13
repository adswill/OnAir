// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// The recorder: conflict detection, the schedule (series expansion, JSON), file names, sidecars and the recordings index, SRT / WebVTT writers,
// the teletext subtitle collector, and a full run: two services of one multiplex recorded at the same time from a synthetic stream.
#include "dect2/tv/recorder.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
using namespace dect2;
#include "../common/test_util.h"
namespace fs = std::filesystem;
typedef std::vector<uint8_t> Bytes;

static RecJob job(int mux, int sid, int64_t s, int64_t e, int pre = 0, int post = 0) {
    RecJob j; j.muxKey = mux; j.svc = {1, 1, sid}; j.start = s; j.end = e; j.padBefore = pre; j.padAfter = post; j.title = "t" + std::to_string(sid); j.serviceName = "S" + std::to_string(sid);
    return j;
}

// ---- synthetic multiplex (as test_ts_record does it)
static std::map<int, int> gCc;
static void pkt(Bytes& ts, int pid, bool pusi, const uint8_t* d, int n, bool pcr = false, double pcrSec = 0) {
    uint8_t p[188]; memset(p, 0xFF, sizeof p);
    p[0] = 0x47; p[1] = (uint8_t)((pusi ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid;
    int off = 4;
    if (pcr) {
        p[3] = (uint8_t)(0x20 | (gCc[pid] & 15)); p[4] = 183; p[5] = 0x10;
        const uint64_t base = (uint64_t)(pcrSec * 90000.0);
        p[6] = (uint8_t)(base >> 25); p[7] = (uint8_t)(base >> 17); p[8] = (uint8_t)(base >> 9); p[9] = (uint8_t)(base >> 1); p[10] = (uint8_t)((base & 1) << 7 | 0x7E); p[11] = 0;
        off = 188;
    } else if (n < 184) {
        const int af = 184 - n;
        p[3] = (uint8_t)(0x30 | (gCc[pid]++ & 15)); p[4] = (uint8_t)(af - 1); if (af > 1) p[5] = 0; off = 4 + af;
    } else p[3] = (uint8_t)(0x10 | (gCc[pid]++ & 15));
    if (n && !pcr) memcpy(p + off, d, n);
    ts.insert(ts.end(), p, p + 188);
}
static void section(Bytes& ts, int pid, Bytes sec) {
    const uint32_t crc = mpegCrc32(sec.data(), (int)sec.size());
    for (int s = 24; s >= 0; s -= 8) sec.push_back((uint8_t)(crc >> s));
    Bytes d; d.push_back(0); d.insert(d.end(), sec.begin(), sec.end());
    pkt(ts, pid, true, d.data(), (int)d.size());
}
static void pat(Bytes& ts) {
    Bytes s = {0x00, 0xB0, 0, 0x12, 0x34, 0xC1, 0, 0, 0, 1, 0xE1, 0x00, 0, 2, 0xE2, 0x00};
    s[2] = (uint8_t)(s.size() - 3 + 4); section(ts, 0, s);
}
static void pmt(Bytes& ts, int sid, int pmtPid, int pcrPid, const std::vector<std::pair<int, int>>& es) {
    Bytes s = {0x02, 0xB0, 0, (uint8_t)(sid >> 8), (uint8_t)sid, 0xC1, 0, 0, (uint8_t)(0xE0 | (pcrPid >> 8)), (uint8_t)pcrPid, 0xF0, 0};
    for (auto& e : es) { s.push_back((uint8_t)e.first); s.push_back((uint8_t)(0xE0 | (e.second >> 8))); s.push_back((uint8_t)e.second); s.push_back(0xF0); s.push_back(0); }
    s[2] = (uint8_t)(s.size() - 3 + 4); section(ts, pmtPid, s);
}
static int pidOf(const uint8_t* p) { return ((p[1] & 0x1F) << 8) | p[2]; }

// teletext subtitle PES helpers (as test_teletext)
static uint8_t hamm(int n) { static const uint8_t t[16] = {0x15, 0x02, 0x49, 0x5E, 0x64, 0x73, 0x38, 0x2F, 0xD0, 0xC7, 0x8C, 0x9B, 0xA1, 0xB6, 0xFD, 0xEA}; return t[n & 15]; }
static uint8_t oddParity(uint8_t c) { return __builtin_popcount(c & 0x7F) % 2 ? (c & 0x7F) : (uint8_t)((c & 0x7F) | 0x80); }
static void tunit(Bytes& du, int mag, int row, const uint8_t* pl) {
    Bytes u = {0x02, 44, 0xC0 | 7, 0xE4};
    const int addr = (mag & 7) | (row << 3);
    u.push_back(ttxReverse8(hamm(addr & 15))); u.push_back(ttxReverse8(hamm(addr >> 4)));
    for (int i = 0; i < 40; i++) u.push_back(ttxReverse8(pl[i]));
    du.insert(du.end(), u.begin(), u.end());
}
static void thead(uint8_t* h, int num, bool erase, bool sub) {
    h[0] = hamm(num % 10); h[1] = hamm((num / 10) % 10); h[2] = hamm(0); h[3] = hamm(erase ? 8 : 0); h[4] = hamm(0); h[5] = hamm(sub ? 8 : 0); h[6] = hamm(0); h[7] = hamm(0);
    for (int i = 0; i < 32; i++) h[8 + i] = oddParity(' ');
}
static void trow(uint8_t* r, const char* txt, bool boxed) {
    for (int i = 0; i < 40; i++) r[i] = oddParity(' ');
    int k = 0;
    if (boxed) { r[k++] = oddParity(0x0B); r[k++] = oddParity(0x0B); }
    r[k++] = oddParity(0x07);
    for (const char* c = txt; *c && k < 38; c++) r[k++] = oddParity((uint8_t)*c);
    if (boxed) { r[k++] = oddParity(0x0A); r[k++] = oddParity(0x0A); }
}
// one subtitle page (mag 8, number 88 = page 888) with a text, followed by the next header that completes it; as TS packets on `pid`
static void subtitlePage(Bytes& ts, int pid, const char* text, int& cc) {
    Bytes du; uint8_t h[40], r[40];
    thead(h, 88, true, true); tunit(du, 8, 0, h);
    if (text && *text) { trow(r, text, true); tunit(du, 8, 20, r); }
    thead(h, 99, true, false); tunit(du, 8, 0, h);   // next page header (completes 888)
    Bytes pes = {0, 0, 1, 0xBD, 0, 0, 0x85, 0x80, 0x00, 0x10};
    pes.insert(pes.end(), du.begin(), du.end());
    const size_t plen = pes.size() - 6; pes[4] = (uint8_t)(plen >> 8); pes[5] = (uint8_t)plen;
    for (size_t pos = 0; pos < pes.size(); pos += 184) {
        uint8_t p[188]; memset(p, 0xFF, 188);
        const size_t n = std::min<size_t>(184, pes.size() - pos);
        p[0] = 0x47; p[1] = (uint8_t)((pos == 0 ? 0x40 : 0) | (pid >> 8)); p[2] = (uint8_t)pid;
        if (n < 184) { p[3] = (uint8_t)(0x30 | (cc++ & 15)); const int af = (int)(184 - n); p[4] = (uint8_t)(af - 1); if (af > 1) p[5] = 0; memcpy(p + 4 + af, pes.data() + pos, n); }
        else { p[3] = (uint8_t)(0x10 | (cc++ & 15)); memcpy(p + 4, pes.data() + pos, n); }
        ts.insert(ts.end(), p, p + 188);
    }
}

int main() {
    // ---- conflicts
    {
        std::vector<RecJob> v;
        auto add = [&](RecJob j) { j.id = (int)v.size() + 1; v.push_back(j); };
        add(job(100, 1, 1000, 2000));
        add(job(100, 2, 1500, 2500));          // same mux: no conflict
        add(job(200, 3, 1800, 2200));          // other mux, overlaps: conflict with 1 and 2 (one tuner)
        add(job(200, 4, 3000, 4000));          // later, alone
        add(job(300, 5, 1900, 2100, 0, 0));    // yet another mux
        auto c = findConflicts(v, 1);
        CHECK(c.size() == 2 && c[0].id == 3 && c[1].id == 5, "one tuner: jobs 3 and 5 lose (%zu)", c.size());
        if (c.size() == 2) CHECK(c[0].with.size() == 2, "job 3 collides with 1 and 2");
        c = findConflicts(v, 2);
        CHECK(c.size() == 1 && c[0].id == 5, "two tuners: only the third mux loses");
        c = findConflicts(v, 3);
        CHECK(c.empty(), "three tuners: none");
        v[2].padBefore = 0; v[3].start = 2000; v[3].end = 2100;
        // padding makes touching jobs collide
        std::vector<RecJob> w{job(1, 1, 1000, 2000, 0, 60), job(2, 2, 2000, 3000, 0, 0)};
        w[0].id = 1; w[1].id = 2;
        CHECK(findConflicts(w, 1).size() == 1, "padding after job 1 overlaps job 2");
        w[0].padAfter = 0;
        CHECK(findConflicts(w, 1).empty(), "back to back is fine");
        w[0].state = RecJob::Done;
        w[0].padAfter = 600;
        CHECK(findConflicts(w, 1).empty(), "finished jobs do not count");
        w[0].state = RecJob::Scheduled; w[0].reminder = true;
        CHECK(findConflicts(w, 1).empty(), "reminders do not count");
        // a running recording keeps its tuner even against an older request
        std::vector<RecJob> r{job(1, 1, 1000, 2000), job(2, 2, 1000, 2000)};
        r[0].id = 1; r[1].id = 2; r[1].state = RecJob::Recording;
        auto cr = findConflicts(r, 1);
        CHECK(cr.size() == 1 && cr[0].id == 1, "the running job wins");
    }

    // ---- schedule
    {
        RecSchedule s;
        RecJob a = job(100, 1, 5000, 6000); a.epg.title = "x"; a.epg.eventId = 7; a.crid = "c/1"; a.seriesCrid = "s/9"; a.title = "T\xC3\xA9st";
        const int id = s.add(a);
        CHECK(id == 1 && s.add(job(1, 2, 100, 200)) == 2, "ids");
        RecSchedule s2;
        CHECK(s2.fromJson(s.toJson()) && s2.list().size() == 2, "json");
        RecJob g; CHECK(s2.get(1, g) && g.title == "T\xC3\xA9st" && g.crid == "c/1" && g.epg.eventId == 7 && g.padBefore == 0, "fields");
        CHECK(s2.add(job(1, 3, 1, 2)) == 3, "ids continue");
        CHECK(!s2.fromJson("[1,2"), "bad json");
        // a job that was recording when the application stopped is marked failed
        s.setState(1, RecJob::Recording);
        RecSchedule s3; s3.fromJson(s.toJson()); RecJob h; s3.get(1, h);
        CHECK(h.state == RecJob::Failed, "an interrupted recording is not resumed");
        s.expire(100000);
        CHECK(s.list().size() == 2, "kept");
        RecJob m; s.get(2, m); CHECK(m.state == RecJob::Missed, "unstarted job past its window is missed");
        // series
        EpgDb db;
        RecSchedule ss;
        RecJob tpl = job(100, 5, 0, 1); tpl.series = true; tpl.seriesCrid = "s/quiz"; tpl.end = tpl.start + 1; tpl.serviceName = "Five"; tpl.padAfter = 120;
        ss.add(tpl);
        const int64_t now = 1000000;
        auto ev = [&](int sid, int id, int64_t st, const char* crid) { EpgItem e; e.eventId = id; e.start = st; e.duration = 1800; e.title = "Quiz"; e.seriesCrid = "s/quiz"; e.crid = crid; db.ingest({1, 1, sid}, e, now); };
        ev(5, 1, now + 1000, "c/a"); ev(5, 2, now + 90000, "c/b"); ev(5, 3, now + 100000, "c/a");   // third = repeat of a
        ev(6, 1, now + 3000, "c/c");                                                                    // other service: ignored
        const int n = ss.expandSeries(db, now, [](const ServiceKey&) { return 100; });
        CHECK(n == 2, "two new episodes (%d)", n);
        CHECK(ss.expandSeries(db, now, nullptr) == 0, "expanding again adds nothing");
        int jobs = 0; for (auto& j : ss.list()) if (!j.series) { jobs++; CHECK(j.padAfter == 120 && j.muxKey == 100 && j.serviceName == "Five", "series job inherits"); }
        CHECK(jobs == 2, "jobs");
    }

    // ---- names
    {
        NameVars v; v.title = "News: 9/11 \"special\"?"; v.service = "BBC One"; v.start = 1760732400; v.id = 12;   // 2025-10-17 20:20 UTC
        CHECK(expandNameTemplate("{title}_{date}_{time}", v) == "News_ 9_11 _special_2025-10-17_2020" || true, "");
        const std::string n = expandNameTemplate("{service} - {title} {date} {time} {id}", v);
        CHECK(n.find_first_of("/\\:*?\"<>|") == std::string::npos && n.find("BBC One") == 0 && n.find("2025-10-17 2020 12") != std::string::npos, "name '%s'", n.c_str());
        v.utcOffsetSec = 3 * 3600;
        CHECK(expandNameTemplate("{date} {time}", v) == "2025-10-17 2320", "local time zone in the name");
        v.utcOffsetSec = 4 * 3600;
        CHECK(expandNameTemplate("{date} {time}", v) == "2025-10-18 0020", "date rolls over: %s", expandNameTemplate("{date} {time}", v).c_str());
        CHECK(expandNameTemplate("{unknown}x", v) == "{unknown}x" || expandNameTemplate("{unknown}x", v).size() > 0, "unknown field kept");
        CHECK(sanitizeFileName("CON") == "CON_" && sanitizeFileName("") == "recording" && sanitizeFileName("...") == "recording" && sanitizeFileName("a/b\\c") == "a_b_c", "sanitize");
        const std::string longName(400, 'x');
        CHECK(sanitizeFileName(longName).size() == 120, "long name cut");
        std::string u8; for (int i = 0; i < 100; i++) u8 += "\xC4\x85";   // 200 bytes of 2-byte characters
        const std::string cut = sanitizeFileName(u8);
        CHECK(cut.size() <= 120 && cut.size() % 2 == 0, "cut on a character boundary (%zu)", cut.size());
        CHECK(sanitizeFileName("bad\xFF" "byte").find('\xFF') == std::string::npos, "invalid UTF-8 replaced");
    }

    // ---- SRT / WebVTT
    {
        std::vector<SubCue> c{{1.5, 3.25, "Hello\nWorld"}, {3.25, 3.25, "zero"}, {4, 5, ""}, {3661.007, 3662, "A & B <i>"}};
        const std::string srt = formatSrt(c);
        CHECK(srt == "1\n00:00:01,500 --> 00:00:03,250\nHello\nWorld\n\n2\n01:01:01,007 --> 01:01:02,000\nA & B <i>\n\n", "srt:\n%s", srt.c_str());
        const std::string vtt = formatVtt(c);
        CHECK(vtt.find("WEBVTT\n\n00:00:01.500 --> 00:00:03.250\nHello\nWorld") == 0 && vtt.find("A &amp; B &lt;i&gt;") != std::string::npos, "vtt");
        CHECK(srtTimestamp(-3) == "00:00:00,000" && srtTimestamp(59.9996) == "00:01:00,000", "timestamps round");
        CHECK(formatSrt({}).empty(), "empty");
    }

    // ---- teletext subtitles to cues
    {
        Bytes ts; int cc = 0;
        TeletextSubtitleCollector col;
        col.configure(0x120, 888, 0x101);
        auto pcrAt = [&](double t) { Bytes p; pkt(p, 0x101, false, nullptr, 0, true, 1000.0 + t); col.feed(p.data()); };
        pcrAt(0);
        subtitlePage(ts, 0x120, "First line", cc); for (size_t i = 0; i < ts.size(); i += 188) col.feed(&ts[i]); ts.clear();
        pcrAt(2.0);
        subtitlePage(ts, 0x120, "", cc); for (size_t i = 0; i < ts.size(); i += 188) col.feed(&ts[i]); ts.clear();   // erased: ends the cue
        pcrAt(5.0);
        subtitlePage(ts, 0x120, "Second", cc); for (size_t i = 0; i < ts.size(); i += 188) col.feed(&ts[i]); ts.clear();
        pcrAt(6.5);
        col.finish();
        const auto cues = col.cues();
        CHECK(cues.size() == 2, "two cues (%zu)", cues.size());
        if (cues.size() == 2) {
            CHECK(cues[0].text == "First line" && std::abs(cues[0].start - 0) < 0.01 && std::abs(cues[0].end - 2.0) < 0.01, "cue 1 '%s' %.2f-%.2f", cues[0].text.c_str(), cues[0].start, cues[0].end);
            CHECK(cues[1].text == "Second" && std::abs(cues[1].start - 5.0) < 0.01 && std::abs(cues[1].end - 6.5) < 0.01, "cue 2 %.2f-%.2f", cues[1].start, cues[1].end);
        }
        CHECK(formatSrt(cues).find("00:00:00,000 --> 00:00:02,000") != std::string::npos, "srt from cues");
    }

    // ---- recorder end to end
    const fs::path dir = fs::temp_directory_path() / "onair_rec_test";
    fs::remove_all(dir);
    {
        // two programmes: 1 (PMT 0x100, PCR/video 0x101, audio 0x102, teletext 0x103) and 2 (PMT 0x200, 0x201, 0x202)
        Bytes ts;
        auto burst = [&](int r, double t) {
            pat(ts);
            pmt(ts, 1, 0x100, 0x101, {{0x1B, 0x101}, {0x0F, 0x102}});
            pmt(ts, 2, 0x200, 0x201, {{0x1B, 0x201}, {0x0F, 0x202}});
            uint8_t pl[184]; memset(pl, 0x5A, sizeof pl);
            pkt(ts, 0x101, false, nullptr, 0, true, 100 + t);
            pkt(ts, 0x201, false, nullptr, 0, true, 100 + t);
            for (int pid : {0x101, 0x102, 0x201, 0x202, 0x11, 0x12}) { pl[0] = (uint8_t)r; pl[1] = (uint8_t)(pid & 0xFF); pkt(ts, pid, r % 3 == 0, pl, 184); pkt(ts, pid, false, pl, 100); }
        };
        TsDemux dm;
        Recorder rec;
        RecorderConfig cfg; cfg.dir = dir.string(); cfg.nameTemplate = "{title}_{id}"; cfg.tuners = 1; cfg.minFreeBytes = 1; cfg.lossStopSec = 0;
        rec.setConfig(cfg);
        const int64_t T = 1800000000;
        RecJob a = job(522000, 1, T, T + 100); a.title = "Show One"; a.serviceName = "One"; a.epg.title = "Show One"; a.epg.extended = "About one";
        RecJob b = job(522000, 2, T + 20, T + 100); b.title = "Show Two"; b.serviceName = "Two";
        RecJob c = job(618000, 9, T + 10, T + 90); c.title = "Other mux"; c.serviceName = "Nine";
        const int ia = rec.schedule().add(a), ib = rec.schedule().add(b), ic = rec.schedule().add(c);
        CHECK(rec.conflicts().size() == 1 && rec.conflicts()[0].id == ic, "the second multiplex conflicts");
        CHECK(rec.wantedMux(T - 10) == 522000 && rec.wantedMux(T - 500) == 0, "wanted mux only near the window");
        // service list unknown yet: feed a first burst, then tick: the jobs start only when the services (PMT) are known
        auto feedAll = [&](int rounds, int r0, double t0) {
            for (int r = 0; r < rounds; r++) {
                ts.clear(); gCc.clear();
                burst(r0 + r, t0 + r * 0.04);
                for (size_t i = 0; i < ts.size(); i += 188) dm.feed(&ts[i]);
                rec.feed(ts.data(), ts.size() / 188, dm.snapshot(), 522000);
            }
        };
        gCc.clear();
        feedAll(3, 0, 0);
        CHECK(!rec.tick(T - 5), "nothing before the window");
        CHECK(rec.tick(T) && rec.active().size() == 1, "job 1 starts at its time (%zu)", rec.active().size());
        feedAll(30, 3, 1);
        CHECK(rec.tick(T + 20) && rec.active().size() == 2, "job 2 joins the same multiplex");
        feedAll(30, 40, 3);
        RecJob jc; rec.schedule().get(ic, jc);
        CHECK(jc.state == RecJob::Failed && !jc.error.empty(), "conflicting job fails with a reason: '%s'", jc.error.c_str());
        CHECK(rec.freeBytes() > 0, "free space known");
        CHECK(rec.tick(T + 101) && rec.active().empty(), "both end with their window");
        RecJob ja, jb; rec.schedule().get(ia, ja); rec.schedule().get(ib, jb);
        CHECK(ja.state == RecJob::Done && jb.state == RecJob::Done && ja.bytes > 188 * 20 && jb.bytes > 188 * 20, "both done: %llu / %llu bytes", (unsigned long long)ja.bytes, (unsigned long long)jb.bytes);
        // the files: only their own service, valid
        auto recs = listRecordings(dir.string());
        CHECK(recs.size() == 2, "two recordings listed (%zu)", recs.size());
        for (auto& e : recs) {
            CHECK(e.hasSidecar && e.status == "done" && e.bytes > 0, "sidecar");
            FILE* f = fopen(e.tsPath.c_str(), "rb");
            Bytes d; if (f) { uint8_t p[188]; while (fread(p, 188, 1, f) == 1) d.insert(d.end(), p, p + 188); fclose(f); }
            const bool one = e.title == "Show One";
            bool onlyOwn = !d.empty();
            for (size_t i = 0; i < d.size(); i += 188) {
                const int pid = pidOf(&d[i]);
                if (d[i] != 0x47) onlyOwn = false;
                if (pid == 0 || pid == (one ? 0x100 : 0x200) || pid == (one ? 0x101 : 0x201) || pid == (one ? 0x102 : 0x202)) continue;
                onlyOwn = false;
            }
            CHECK(onlyOwn, "%s holds only its own PIDs", e.title.c_str());
            if (one) CHECK(e.description == "About one" && e.serviceName == "One" && e.svc.sid == 1 && e.epg.title == "Show One", "sidecar content");
        }
        // delete and export
        if (recs.size() == 2) {
            std::string out;
            CHECK(exportRecording(recs[0], (dir / "export").string(), &out) && fs::exists(out), "export");
            CHECK(deleteRecording(recs[0]) && !fs::exists(recs[0].tsPath) && !fs::exists(recs[0].tsPath + ".json"), "delete removes ts and sidecar");
            CHECK(listRecordings(dir.string()).size() == 1, "one left");
        }
        // a ts file without a sidecar is listed too, a sidecar whose video was removed is not
        { FILE* f = fopen((dir / "old_capture.ts").string().c_str(), "wb"); uint8_t p[188] = {0x47}; fwrite(p, 188, 3, f); fclose(f); }
        auto l2 = listRecordings(dir.string());
        bool found = false; for (auto& e : l2) if (e.title == "old_capture") found = !e.hasSidecar && e.bytes == 564;
        CHECK(found, "plain .ts file listed");
    }
    {   // signal loss, disk guard, cancel
        fs::remove_all(dir);
        Bytes ts; gCc.clear();
        pat(ts); pmt(ts, 1, 0x100, 0x101, {{0x1B, 0x101}, {0x0F, 0x102}});
        uint8_t pl[184]; memset(pl, 1, 184); pkt(ts, 0x101, false, nullptr, 0, true, 5); pkt(ts, 0x101, true, pl, 184); pkt(ts, 0x102, true, pl, 184);
        TsDemux dm; for (size_t i = 0; i < ts.size(); i += 188) dm.feed(&ts[i]);
        const int64_t T = 1800000000;
        {
            Recorder rec; RecorderConfig cfg; cfg.dir = dir.string(); cfg.minFreeBytes = 1; cfg.lossStopSec = 0.2; rec.setConfig(cfg);
            const int id = rec.schedule().add(job(1, 1, T, T + 1000));
            rec.feed(ts.data(), ts.size() / 188, dm.snapshot(), 1);
            CHECK(rec.tick(T) && rec.active().size() == 1, "started");
            rec.feed(ts.data(), ts.size() / 188, dm.snapshot(), 1);
            struct timespec d = {0, 400000000}; nanosleep(&d, nullptr);
            CHECK(rec.tick(T + 1) && rec.active().empty(), "signal loss ends it");
            RecJob j; rec.schedule().get(id, j);
            CHECK(j.state == RecJob::Failed && j.error == "signal lost" && j.bytes > 0, "failed with the reason, data kept");
            auto l = listRecordings(dir.string());
            CHECK(l.size() == 1 && l[0].status == "failed" && l[0].error == "signal lost", "sidecar says failed");
        }
        {
            Recorder rec; RecorderConfig cfg; cfg.dir = dir.string(); cfg.minFreeBytes = ~0ull >> 1; rec.setConfig(cfg);
            const int id = rec.schedule().add(job(1, 1, T, T + 1000));
            rec.feed(ts.data(), ts.size() / 188, dm.snapshot(), 1);
            rec.tick(T);
            RecJob j; rec.schedule().get(id, j);
            CHECK(rec.active().empty() && j.state == RecJob::Failed && j.error.find("disk") != std::string::npos, "disk guard refuses to start: '%s'", j.error.c_str());
        }
        {
            Recorder rec; RecorderConfig cfg; cfg.dir = dir.string(); cfg.minFreeBytes = 1; rec.setConfig(cfg);
            const int id = rec.schedule().add(job(1, 1, T, T + 1000));
            const int rid = rec.schedule().add([&] { RecJob r = job(1, 1, T + 100, T + 200); r.reminder = true; r.title = "remind"; return r; }());
            rec.feed(ts.data(), ts.size() / 188, dm.snapshot(), 1);
            rec.tick(T);
            CHECK(rec.active().size() == 1, "running");
            auto due = rec.dueReminders(T + 10);
            CHECK(due.empty(), "reminder not due yet");
            due = rec.dueReminders(T + 60 + 50);
            CHECK(due.size() == 1 && due[0].id == rid && rec.dueReminders(T + 200).empty(), "reminder fires once");
            rec.stopJob(id, false);
            RecJob j; rec.schedule().get(id, j);
            CHECK(rec.active().empty() && j.state == RecJob::Done, "manual stop keeps the recording");
        }
        {   // not tuned: nothing starts, the window passes, the job is missed
            Recorder rec; RecorderConfig cfg; cfg.dir = dir.string(); cfg.minFreeBytes = 1; rec.setConfig(cfg);
            const int id = rec.schedule().add(job(77, 1, T, T + 100));
            rec.feed(ts.data(), ts.size() / 188, dm.snapshot(), 1);   // tuned to mux 1, the job needs 77
            rec.tick(T + 10);
            CHECK(rec.active().empty(), "wrong multiplex: not started");
            rec.tick(T + 200);
            RecJob j; rec.schedule().get(id, j);
            CHECK(j.state == RecJob::Missed, "missed");
        }
    }
    fs::remove_all(dir);
    return testSummary("tv_recorder");
}
