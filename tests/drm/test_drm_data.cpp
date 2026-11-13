// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM data services end to end: the generator's multiplex (audio service 0 and a packet mode data service 1 with MOT objects, TPEG frames or EPG files, segmented,
// with lost, damaged and foreign packets) through the OFDM chain into the receiver's data service view: slide show pictures with their parameters,
// broadcast website objects as text, EPG and TPEG identified, the packet counters, and the application names in the service table.
#include "dect2/drm/drm_gen.h"
#include "dect2/drm/drm_rx.h"
#include "dect2/dab/dab.h"
#include <atomic>
#include <cstdio>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>
using namespace dect2;

static std::atomic<int> fails{0};
static std::mutex outMu;
#define CHECK(c, ...) do { if (!(c)) { std::lock_guard<std::mutex> lk(outMu); printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct Run {
    DrmDataView view;                       // the last view
    std::map<std::string, DrmSlide> slides; // every picture that came, by name
    std::vector<DrmService> services;
    DrmTelemetry tel;
    double secs = 0;
    bool done = false;
    int newSlides = 0;
};

// Runs the transmitter into the receiver until `until` is true for the view (or maxSecs of signal), looking at the data service after every chunk.
static Run run(DrmTxConfig cfg, double maxSecs, const std::function<bool(const DrmDataView&)>& until, double extraSecs = 0) {
    Run r;
    cfg.dataPacketId = 1;
    DrmTransmitter tx(cfg, nullptr);
    if (!tx.ok()) { CHECK(false, "transmitter configuration rejected"); return r; }
    DrmReceiver rx;
    rx.setSilent(true);
    rx.configure(tx.sampleRate());
    std::vector<cf32> buf;
    uint64_t lastSlide = 0;
    double t = 0, doneAt = -1;
    while (t < maxSecs) {
        buf.clear();
        tx.superFrame(buf);
        for (size_t at = 0; at < buf.size(); at += 4096) {
            const size_t n = std::min<size_t>(4096, buf.size() - at);
            rx.feed(buf.data() + at, n);
            t += (double)n / tx.sampleRate();
            int sid = 0;
            DrmSlide fresh;
            while (rx.takeSlide(sid, fresh)) { CHECK(sid == 1, "slide from service %d", sid); r.slides[fresh.name] = fresh; r.newSlides++; }
            if (rx.dataView(1, r.view)) {
                if (r.view.slide.seq != lastSlide && r.view.slide.seq > 0) lastSlide = r.view.slide.seq;
                if (doneAt < 0 && until(r.view)) { doneAt = t; r.done = true; }
            }
        }
        if (doneAt >= 0 && t >= doneAt + extraSecs) break;
    }
    r.secs = t;
    rx.dataView(1, r.view);
    DrmTelemetry tel;
    uint64_t seq = 0;
    if (rx.telemetry(tel, seq)) { r.tel = tel; r.services = tel.services; }
    return r;
}

static std::vector<uint8_t> text(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static DrmTxConfig base(int app, std::shared_ptr<DrmDataSource> src) {
    DrmTxConfig c;
    c.mode = 1;              // robustness mode B, 10 kHz, 64-QAM
    c.textMessage = true;
    c.dataApp = app;
    c.dataSource = std::move(src);
    c.dataPackets = 12;
    c.dataPacketLen = 45;
    return c;
}

static const DrmService* dataService(const Run& r) {
    for (const DrmService& s : r.services) if (!s.audio) return &s;
    return nullptr;
}

static void checkServiceTable(const char* what, const Run& r, const char* app) {
    const DrmService* d = dataService(r);
    CHECK(d != nullptr, "%s: no data service in the service table (%zu services)", what, r.services.size());
    if (!d) return;
    CHECK(d->appName == app, "%s: application name '%s' (expected '%s')", what, d->appName.c_str(), app);
    CHECK(d->appMode.find("packet mode, data units, packet id 1") == 0, "%s: mode text '%s'", what, d->appMode.c_str());
    CHECK(d->appDecoded && d->label == "OnAir Data", "%s: decoded %d label '%s'", what, (int)d->appDecoded, d->label.c_str());
    bool audio = false;
    for (const DrmService& s : r.services) audio |= s.audio && s.label == "OnAir DRM";
    CHECK(audio, "%s: the audio service is missing", what);
    CHECK(r.tel.blocksOk > 0 && r.tel.blocksOk >= 3 * r.tel.blocksBad, "%s: audio blocks %llu ok, %llu bad", what, (unsigned long long)r.tel.blocksOk, (unsigned long long)r.tel.blocksBad);
}

// ---------------------------------------------------------------- slide show

static std::vector<DrmMotObject> slides() {
    std::vector<DrmMotObject> v;
    for (int i = 1; i <= 3; i++) {
        DrmMotObject o;
        o.name = "slide" + std::to_string(i) + ".png";
        o.contentType = 2; o.subtype = 3;
        o.body = drmGenPng(32, 24, i);
        o.caption = std::string("Caption of slide ") + std::to_string(i) + " \xC3\xA9";
        o.category = i == 2 ? "Weather" : "News";
        o.categoryId = i; o.slideId = 10 + i;
        o.clickUrl = "http://example.org/" + std::to_string(i);
        o.hasTrigger = i != 3;                     // slide 1: now, slide 2: a date, slide 3: no trigger time
        o.trigMjd = i == 2 ? 61319 : 0; o.trigHour = 12; o.trigMinute = 30;
        v.push_back(o);
    }
    return v;
}

static void checkSlides(const char* what, const Run& r) {
    CHECK(r.slides.size() == 3, "%s: %zu pictures received", what, r.slides.size());
    for (int i = 1; i <= 3; i++) {
        const std::string n = "slide" + std::to_string(i) + ".png";
        auto it = r.slides.find(n);
        if (it == r.slides.end()) { CHECK(false, "%s: %s missing", what, n.c_str()); continue; }
        const DrmSlide& s = it->second;
        std::vector<uint8_t> px;
        drmGenPngPixels(32, 24, i, px);
        CHECK(s.w == 32 && s.h == 24 && s.rgba == px, "%s: %s pixels differ (%dx%d)", what, n.c_str(), s.w, s.h);
        CHECK(s.caption == std::string("Caption of slide ") + std::to_string(i) + " \xC3\xA9", "%s: %s caption '%s'", what, n.c_str(), s.caption.c_str());
        CHECK(s.category == (i == 2 ? "Weather" : "News") && s.categoryId == i && s.slideId == 10 + i, "%s: %s category '%s' %d/%d", what, n.c_str(), s.category.c_str(), s.categoryId, s.slideId);
        CHECK(s.clickUrl == "http://example.org/" + std::to_string(i), "%s: %s url '%s'", what, n.c_str(), s.clickUrl.c_str());
        CHECK(s.trigger == (i == 1 ? "now" : i == 2 ? "2026-10-06 12:30" : ""), "%s: %s trigger '%s'", what, n.c_str(), s.trigger.c_str());
        CHECK(s.type == "image/PNG" && s.bytes > 100, "%s: %s type '%s' %zu bytes", what, n.c_str(), s.type.c_str(), s.bytes);
    }
}

static void testSlideshowLossy() {
    DrmTxConfig c = base(0x002, makeDrmMotSource(slides(), 60, false));
    c.dataLossEvery = 89; c.dataBadCrcEvery = 131; c.dataOtherIdEvery = 53;
    const Run r = run(c, 110, [](const DrmDataView& v) { return v.slide.seq >= 3 && v.objects.size() >= 3; }, 2.0);
    CHECK(r.done, "slide show (lossy): not complete after %.0f s (pictures %zu)", r.secs, r.slides.size());
    checkSlides("slide show (lossy)", r);
    const DrmDataView& v = r.view;
    CHECK(v.appName == "MOT slide show" && v.userApp == 2 && v.decoded && v.packet && v.dataUnit && v.packetLen == 45 && v.packetId == 1, "slide show: view '%s' app %d", v.appName.c_str(), v.userApp);
    CHECK(v.lostPackets > 0 && v.unitsDropped > 0, "lost packets %llu, units dropped %llu", (unsigned long long)v.lostPackets, (unsigned long long)v.unitsDropped);
    CHECK(v.badPackets > 0, "bad CRC packets %llu", (unsigned long long)v.badPackets);
    CHECK(v.otherIdPackets > 0 && v.emptyPackets == 0, "other id %llu empty %llu", (unsigned long long)v.otherIdPackets, (unsigned long long)v.emptyPackets);
    CHECK(v.unitsOk > 50 && v.packets > v.unitsOk, "units %llu packets %llu", (unsigned long long)v.unitsOk, (unsigned long long)v.packets);
    CHECK(v.mot.objects >= 3 && v.mot.headerSegments >= 3 && v.mot.bodySegments > 30, "MOT: %llu objects, %llu header / %llu body segments", (unsigned long long)v.mot.objects, (unsigned long long)v.mot.headerSegments, (unsigned long long)v.mot.bodySegments);
    CHECK(v.objects.size() == 3, "object list: %zu", v.objects.size());
    checkServiceTable("slide show (lossy)", r, "MOT slide show");
    CHECK(r.tel.textMessage == "OnAir DRM test signal", "text message '%s' next to the data service", r.tel.textMessage.c_str());
    CHECK(r.newSlides == 3 && v.slide.seq == 3, "pictures taken: %d, sequence %llu", r.newSlides, (unsigned long long)v.slide.seq);
    printf("slide show lossy: %.0f s of signal, packets %llu (lost %llu, bad CRC %llu, other id %llu), units ok %llu dropped %llu, repeats %llu\n", r.secs, (unsigned long long)v.packets,
           (unsigned long long)v.lostPackets, (unsigned long long)v.badPackets, (unsigned long long)v.otherIdPackets, (unsigned long long)v.unitsOk, (unsigned long long)v.unitsDropped, (unsigned long long)v.mot.repeats);
}

static void testSlideshowDirectory() {
    DrmTxConfig c = base(0x002, makeDrmMotSource(slides(), 70, true));
    const Run r = run(c, 80, [](const DrmDataView& v) { return v.slide.seq >= 3; }, 1.0);
    CHECK(r.done, "slide show (directory mode): not complete (pictures %zu)", r.slides.size());
    checkSlides("slide show (directory mode)", r);
    CHECK(r.view.mot.directories >= 1 && r.view.directory.size() == 3 && r.view.directory[0].name == "slide1.png", "directory: %llu, %zu entries", (unsigned long long)r.view.mot.directories, r.view.directory.size());
    CHECK(r.view.lostPackets == 0 && r.view.badPackets == 0 && r.view.unitsDropped == 0, "clean channel: lost %llu bad %llu dropped %llu", (unsigned long long)r.view.lostPackets, (unsigned long long)r.view.badPackets, (unsigned long long)r.view.unitsDropped);
}

// ---------------------------------------------------------------- broadcast website

static void testWebsite(bool directory) {
    std::vector<DrmMotObject> v;
    DrmMotObject a;
    a.name = "index.html"; a.contentType = 1; a.subtype = 2;
    a.body = text("<html><head><title>OnAir</title></head><body><h1>Hello</h1><p>Page one with an <a href=\"about.html\">about</a> link and <script>alert(1)</script>.</p></body></html>");
    DrmMotObject b;
    b.name = "about.html"; b.contentType = 1; b.subtype = 2;
    b.body = text(std::string("<html><body>") + std::string(300, 'x') + " \xE9t\xE9</body></html>");   // 8 bit text that is not UTF-8
    DrmMotObject cs;
    cs.name = "style.css"; cs.contentType = 0; cs.subtype = 0;
    cs.body = text("body { color: #123456; }\nh1 { margin: 0; }\n");
    DrmMotObject p;
    p.name = "logo.png"; p.contentType = 2; p.subtype = 3; p.body = drmGenPng(24, 16, 5);
    v = {a, b, cs, p};
    DrmTxConfig c = base(0x003, makeDrmMotSource(v, 80, directory));
    if (!directory) { c.dataLossEvery = 97; c.dataOtherIdEvery = 61; }
    const char* what = directory ? "website (directory)" : "website (lossy)";
    const Run r = run(c, 90, [](const DrmDataView& w) { return w.objects.size() >= 4; }, 1.0);
    CHECK(r.done, "%s: only %zu objects", what, r.view.objects.size());
    const DrmDataView& w = r.view;
    CHECK(w.appName == "MOT broadcast website" && w.userApp == 3 && w.decoded, "%s: '%s'", what, w.appName.c_str());
    std::map<std::string, const DrmObjectInfo*> by;
    for (const DrmObjectInfo& o : w.objects) by[o.name] = &o;
    for (const DrmMotObject& m : v) {
        auto it = by.find(m.name);
        if (it == by.end()) { CHECK(false, "%s: %s not listed", what, m.name.c_str()); continue; }
        const DrmObjectInfo& o = *it->second;
        CHECK(o.size == m.body.size(), "%s: %s size %zu (sent %zu)", what, m.name.c_str(), o.size, m.body.size());
        if (m.name == "logo.png") CHECK(!o.text && o.type == "image/PNG", "%s: logo type '%s' text %d", what, o.type.c_str(), (int)o.text);
        else CHECK(o.text, "%s: %s not shown as text", what, m.name.c_str());
    }
    if (by.count("index.html")) {
        const DrmObjectInfo& o = *by["index.html"];
        CHECK(o.type == "text/HTML" && o.content == std::string((const char*)a.body.data(), a.body.size()), "%s: index.html content '%s' type '%s'", what, o.content.c_str(), o.type.c_str());
    }
    if (by.count("about.html")) CHECK(by["about.html"]->content.find("\xC3\xA9t\xC3\xA9") != std::string::npos, "%s: Latin-1 text not turned into UTF-8", what);
    if (by.count("style.css")) CHECK(by["style.css"]->content.find("color: #123456") != std::string::npos, "%s: css content", what);
    CHECK(w.slide.seq == 0, "%s: a website must not fill the slide show", what);
    if (directory) CHECK(w.mot.directories >= 1 && w.directory.size() == 4, "%s: directory %zu", what, w.directory.size());
    else CHECK(w.lostPackets > 0 && w.otherIdPackets > 0, "%s: lost %llu other %llu", what, (unsigned long long)w.lostPackets, (unsigned long long)w.otherIdPackets);
    checkServiceTable(what, r, "MOT broadcast website");
}

// ---------------------------------------------------------------- EPG and TPEG

class TpegSource : public DrmDataSource {
public:
    std::vector<uint8_t> nextUnit() override {
        std::vector<uint8_t> g = {0x40, 0x00};                    // data group type 0 (general data) with a CRC, no segment and no session field
        for (int f = 0; f < 2; f++) {
            const int len = 20 + 10 * f;
            g.push_back(0xFF); g.push_back(0x00); g.push_back((uint8_t)len);       // TPEG transport frame: sync, length
            for (int i = 0; i < len; i++) g.push_back((uint8_t)(n_ * 3 + i));
        }
        const uint16_t c = dab::crc16(g.data(), (int)g.size());
        g.push_back((uint8_t)(c >> 8)); g.push_back((uint8_t)c);
        n_++;
        return g;
    }
private:
    int n_ = 0;
};

static void testTpeg() {
    DrmTxConfig c = base(0x004, std::make_shared<TpegSource>());
    c.dataLossEvery = 41;
    const Run r = run(c, 60, [](const DrmDataView& v) { return v.tpegFrames >= 20; }, 0.5);
    CHECK(r.done, "TPEG: %llu frames", (unsigned long long)r.view.tpegFrames);
    const DrmDataView& v = r.view;
    CHECK(v.appName == "TPEG" && v.userApp == 4 && v.decoded, "TPEG name '%s'", v.appName.c_str());
    CHECK(v.mot.groups >= 10 && v.tpegBytes >= v.tpegFrames * 20 && v.tpegFrames >= 2 * v.mot.groups - 2 && v.tpegFrames <= 2 * v.mot.groups, "TPEG: %llu groups, %llu frames, %llu bytes", (unsigned long long)v.mot.groups, (unsigned long long)v.tpegFrames, (unsigned long long)v.tpegBytes);
    CHECK(v.objects.empty() && v.slide.seq == 0, "TPEG makes no objects");
    checkServiceTable("TPEG", r, "TPEG");
}

static void testEpg() {
    std::vector<DrmMotObject> v;
    DrmMotObject a;
    a.name = "20261007_E1C0A1.xml"; a.contentType = 0; a.subtype = 0;
    a.body = text("<?xml version=\"1.0\" encoding=\"UTF-8\"?><epg xmlns=\"http://www.worlddab.org/schemas/epgSchedule/14\"><schedule><programme shortId=\"1\"/></schedule></epg>");
    DrmMotObject b;
    b.name = "SI_E1C0A1.bin"; b.contentType = 0; b.subtype = 0;
    for (int i = 0; i < 150; i++) b.body.push_back((uint8_t)(i * 5 + 1));   // binary coded: listed only
    v = {a, b};
    DrmTxConfig c = base(0x007, makeDrmMotSource(v, 64, false));
    const Run r = run(c, 60, [](const DrmDataView& w) { return w.objects.size() >= 2; }, 0.5);
    CHECK(r.done, "EPG: %zu objects", r.view.objects.size());
    const DrmDataView& w = r.view;
    CHECK(w.appName == "EPG" && w.userApp == 7 && w.decoded, "EPG name '%s'", w.appName.c_str());
    bool xml = false, bin = false;
    for (const DrmObjectInfo& o : w.objects) {
        if (o.name == "20261007_E1C0A1.xml") xml = o.note == "XML in the clear" && o.text && o.size == a.body.size() && o.content.find("<programme shortId=\"1\"/>") != std::string::npos;
        if (o.name == "SI_E1C0A1.bin") bin = o.note == "binary coded, not decoded" && !o.text && o.size == 150;
    }
    CHECK(xml && bin, "EPG objects: xml %d binary %d", (int)xml, (int)bin);
    checkServiceTable("EPG", r, "EPG");
}

// single packets (not data units) are named, not decoded
static void testSinglePackets() {
    DrmTxConfig c = base(0x002, makeDrmMotSource(slides(), 60, false));
    c.dataUnitMode = false;
    const Run r = run(c, 25, [](const DrmDataView& v) { return v.packetLen == 45 && v.shortId == 1 && v.seq > 3; }, 3.0);
    const DrmDataView& v = r.view;
    CHECK(v.appName == "MOT slide show" && !v.decoded && v.packets == 0 && v.slide.seq == 0 && v.objects.empty(), "single packets: '%s' decoded %d packets %llu", v.appName.c_str(), (int)v.decoded, (unsigned long long)v.packets);
    CHECK(v.modeText.find("single packets: not decoded") != std::string::npos, "mode text '%s'", v.modeText.c_str());
}

int main() {
    std::vector<std::future<void>> jobs;
    jobs.push_back(std::async(std::launch::async, testSlideshowLossy));
    jobs.push_back(std::async(std::launch::async, testSlideshowDirectory));
    jobs.push_back(std::async(std::launch::async, [] { testWebsite(false); }));
    jobs.push_back(std::async(std::launch::async, [] { testWebsite(true); }));
    jobs.push_back(std::async(std::launch::async, testTpeg));
    jobs.push_back(std::async(std::launch::async, testEpg));
    jobs.push_back(std::async(std::launch::async, testSinglePackets));
    for (auto& j : jobs) j.get();
    if (fails) { printf("%d failures\n", (int)fails.load()); return 1; }
    printf("drm data ok\n");
    return 0;
}
