// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DRM data services: packet reassembly -> MSC data groups -> MOT objects (slide show, broadcast website, EPG) and TPEG frames, see drm_data.h.
#include "dect2/drm/drm_data.h"
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_pad.h"
#include "dect2/drm/drm_proto.h"
#include <mutex>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace dect2 {

// The application names, the text helpers and the data group payload are drm_proto (Rust).
const char* drmAppName(int domain, int app) {
    static std::mutex mu;
    static std::set<std::string> names;   // a stable pointer for each distinct name
    const std::string s = drmp::appName(domain, app);
    std::lock_guard<std::mutex> lk(mu);
    return names.insert(s).first->c_str();
}

namespace {
const size_t kMaxObjects = 96;

std::string displayText(const std::vector<uint8_t>& b) { return drmp::displayText(b.data(), b.size()); }

bool looksText(const std::vector<uint8_t>& b) { return drmp::looksText(b.data(), b.size()); }

bool endsWith(const std::string& s, const char* e) {
    const std::string x(e);
    if (s.size() < x.size()) return false;
    for (size_t i = 0; i < x.size(); i++) if (std::tolower((unsigned char)s[s.size() - x.size() + i]) != x[i]) return false;
    return true;
}

uint64_t fnv(const std::vector<uint8_t>& b) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t c : b) h = (h ^ c) * 1099511628211ull;
    return h | 1;
}

// The payload of an MSC data group (EN 300 401 clause 5.3.3.1): after the headers, before the CRC; nullptr when the group is damaged
const uint8_t* groupPayload(const uint8_t* d, size_t n, size_t& len) {
    size_t off = 0;
    if (!drmp::groupPayload(d, n, off, len)) return nullptr;
    return d + off;
}
} // namespace

DrmDataService::DrmDataService() : mot_(24, 1u << 20) {
    mot_.setSink([this](const MotObject& m) { motObject(m); });
}

void DrmDataService::configure(const drm::SdcApp& a) {
    const int app = a.dabUserApp();
    const int key = (int)((a.shortId << 28) ^ (a.streamId << 24) ^ (a.packetId << 20) ^ ((a.packet ? 1 : 0) << 19) ^ ((a.dataUnit ? 1 : 0) << 18) ^ (a.packetLen << 10) ^ (a.domain << 8) ^ ((app + 1) & 0x7FF));
    if (key == key_) return;
    key_ = key;
    const uint64_t seq = view_.seq + 1;
    view_ = DrmDataView();
    view_.seq = seq;
    mot_.reset();
    demux_ = DrmPacketDemux();
    nextOrder_ = 1; slideHash_ = 0; fresh_.clear();
    DrmDataView& v = view_;
    v.shortId = a.shortId; v.domain = a.domain; v.userApp = app;
    v.appName = drmAppName(a.domain, app);
    v.packet = a.packet; v.dataUnit = a.dataUnit; v.streamId = a.streamId; v.packetId = a.packetId; v.packetLen = a.packetLen;
    v.journaline = a.domain == 0 && app == 0x44A;
    char b[160];
    if (!a.packet) {
        v.modeText = "stream mode data, not decoded";
    } else {
        snprintf(b, sizeof b, "packet mode, %s, packet id %d, %d byte packets", a.dataUnit ? "data units" : "single packets", a.packetId, a.packetLen);
        v.modeText = b;
        if (a.packetLen > 0) demux_.configure(a.packetLen, a.packetId);
    }
    const bool mot = a.domain == 0 && (app == 0x002 || app == 0x003 || app == 0x007);
    const bool tpeg = a.domain == 0 && app == 0x004;
    v.decoded = a.packet && a.dataUnit && a.packetLen > 0 && (mot || tpeg || v.journaline);
    if (a.packet && !a.dataUnit && (mot || tpeg)) v.modeText += " (single packets: not decoded)";
}

void DrmDataService::feed(const uint8_t* d, size_t n) {
    if (!view_.packet || !view_.dataUnit || view_.packetLen <= 0 || view_.journaline) return;
    const int app = view_.userApp;
    if (view_.domain != 0 || (app != 0x002 && app != 0x003 && app != 0x007 && app != 0x004)) return;
    demux_.feed(d, n, [this](const uint8_t* g, size_t m) { unit(g, m); });
    view_.seq++;
}

void DrmDataService::unit(const uint8_t* g, size_t n) {
    view_.unitBytes += n;
    if (view_.userApp == 0x004) {                                // TPEG: transport frames, a sync byte 0xFF, a 16 bit length, the frame
        size_t len = 0;
        const uint8_t* p = groupPayload(g, n, len);
        if (!p) { view_.mot.groupsBad++; return; }
        view_.mot.groups++;
        view_.tpegBytes += len;
        for (size_t i = 0; i + 3 <= len;) {
            if (p[i] != 0xFF) { i++; continue; }
            const size_t fl = ((size_t)p[i + 1] << 8) | p[i + 2];
            if (i + 3 + fl > len || fl == 0) { i++; continue; }
            view_.tpegFrames++;
            i += 3 + fl;
        }
        return;
    }
    mot_.dataGroup(g, (int)n);
}

void DrmDataService::motObject(const MotObject& m) {
    DrmDataView& v = view_;
    char nb[64];
    DrmObjectInfo o;
    o.tid = m.tid;
    snprintf(nb, sizeof nb, "object %u", (unsigned)m.tid);
    o.name = m.name.empty() ? nb : m.name;
    o.type = motContentTypeName(m.contentType, m.subtype);
    o.size = m.body.size();
    o.received = 1;
    const bool xml = m.body.size() > 5 && std::string((const char*)m.body.data(), 5) == "<?xml";
    o.text = m.contentType == 1 || (m.contentType != 2 && looksText(m.body)) ||
             endsWith(o.name, ".html") || endsWith(o.name, ".htm") || endsWith(o.name, ".txt") || endsWith(o.name, ".css") || endsWith(o.name, ".xml");
    if (o.text) o.content = displayText(m.body);
    if (v.userApp == 0x007) o.note = xml ? "XML in the clear" : m.body.empty() ? "" : "binary coded, not decoded";
    if (v.userApp == 0x002 || m.contentType == 2) {
        if (!m.category.empty()) o.note += (o.note.empty() ? "" : ", ") + std::string("category ") + m.category;
        if (m.hasTrigger) o.note += (o.note.empty() ? "" : ", ") + std::string("trigger ") + motTimeText(m.trigger);
    }
    // the same name again is the carousel round: count it, replace the content when it changed
    for (DrmObjectInfo& e : v.objects) {
        if (e.name == o.name && e.type == o.type) {
            e.received++; e.tid = o.tid;
            if (e.size != o.size || e.content != o.content) { e.size = o.size; e.content = o.content; e.note = o.note; e.order = nextOrder_++; }
            goto picture;
        }
    }
    o.order = nextOrder_++;
    if (v.objects.size() >= kMaxObjects) v.objects.erase(v.objects.begin());
    v.objects.push_back(std::move(o));
picture:
    if (m.contentType == 2 && (v.userApp == 0x002 || v.userApp == 0x003)) {
        const uint64_t h = fnv(m.body) ^ (uint64_t)m.slideId;
        if (v.userApp == 0x002) {
            if (h == slideHash_) return;                           // the same picture again under a new transport id
            DrmSlide s;
            if (!decodeSlideImage(m.body.data(), m.body.size(), m.subtype, s.w, s.h, s.rgba)) return;
            slideHash_ = h;
            s.seq = v.slide.seq + 1;
            s.name = m.name; s.caption = m.caption; s.category = m.category; s.clickUrl = m.clickUrl; s.altUrl = m.altUrl;
            s.categoryId = m.categoryId; s.slideId = m.slideId;
            s.type = motContentTypeName(m.contentType, m.subtype);
            s.bytes = m.body.size();
            s.trigger = m.hasTrigger ? motTimeText(m.trigger) : "";
            v.slide = s;
            if (fresh_.size() >= 16) fresh_.pop_front();
            fresh_.push_back(std::move(s));
        }
    }
}

bool DrmDataService::takeSlide(DrmSlide& out) {
    if (fresh_.empty()) return false;
    out = std::move(fresh_.front());
    fresh_.pop_front();
    return true;
}

void DrmDataService::fill(DrmDataView& v) const {
    v = view_;
    v.packets = demux_.packets; v.badPackets = demux_.badPackets; v.lostPackets = demux_.lostPackets; v.orphanPackets = demux_.orphanPackets;
    v.emptyPackets = demux_.emptyPackets; v.otherIdPackets = demux_.otherIdPackets; v.unitsOk = demux_.unitsOk; v.unitsDropped = demux_.unitsDropped;
    const MotStats& s = mot_.stats();
    if (view_.userApp != 0x004) v.mot = s;
    v.directory = mot_.directory();
    for (MotObject& m : v.directory) m.body.clear();
}

} // namespace dect2
