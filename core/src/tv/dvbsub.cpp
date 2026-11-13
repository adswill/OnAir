// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/dvbsub.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace dect2 {



DvbSubDecoder::Clut DvbSubDecoder::defaultClut() const {
    Clut c;
    dvbSubDefaultClut(c.e2, c.e4, c.e8);
    return c;
}

// ------------------------------------------------------------------ transport
void DvbSubDecoder::setStream(int pid, int comp, int anc) {
    std::lock_guard<std::mutex> lk(mu_);
    if (pid == pid_ && comp == comp_ && anc == anc_) return;
    pid_ = pid; comp_ = comp; anc_ = anc;
    pes_.clear(); inPes_ = false; cc_ = -1;
    havePage_ = false; pageRegs_.clear(); regions_.clear(); cluts_.clear(); objects_.clear(); queue_.clear(); shown_.reset();
    pageVersion_ = -1; dispW_ = 720; dispH_ = 576;
}
int DvbSubDecoder::pid() const { std::lock_guard<std::mutex> lk(mu_); return pid_; }
int DvbSubDecoder::compositionPage() const { std::lock_guard<std::mutex> lk(mu_); return comp_; }
uint64_t DvbSubDecoder::displays() const { std::lock_guard<std::mutex> lk(mu_); return seq_; }
void DvbSubDecoder::reset() {
    std::lock_guard<std::mutex> lk(mu_);
    pes_.clear(); inPes_ = false; cc_ = -1;
    havePage_ = false; pageRegs_.clear(); regions_.clear(); cluts_.clear(); objects_.clear(); queue_.clear(); shown_.reset();
    pageVersion_ = -1; dispW_ = 720; dispH_ = 576; seq_ = 0;
}

void DvbSubDecoder::feedTs(const uint8_t* p) {
    if (p[0] != 0x47) return;
    const int pid = ((p[1] & 0x1F) << 8) | p[2];
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (pid != pid_) return;
    }
    const bool pusi = (p[1] & 0x40) != 0;
    const int afc = (p[3] >> 4) & 3;
    int off = 4;
    if (afc & 2) off += 1 + p[4];
    if (!(afc & 1) || off >= 188) return;
    std::vector<uint8_t> done;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const int cc = p[3] & 15;
        if (cc_ >= 0 && ((cc_ + 1) & 15) != cc && !pusi) { pes_.clear(); inPes_ = false; }   // lost packets: drop the partial PES
        cc_ = cc;
        if (pusi) {
            if (inPes_ && !pes_.empty()) done.swap(pes_);   // a PES of unbounded length ends here
            pes_.clear();
            inPes_ = true;
            pesLen_ = 0;
        }
        if (inPes_) {
            pes_.insert(pes_.end(), p + off, p + 188);
            if (pes_.size() >= 6 && pesLen_ == 0 && pes_[0] == 0 && pes_[1] == 0 && pes_[2] == 1) {
                const size_t l = ((size_t)pes_[4] << 8) | pes_[5];
                if (l) pesLen_ = l + 6;
            }
            if (pesLen_ && pes_.size() >= pesLen_ && done.empty()) { pes_.resize(pesLen_); done.swap(pes_); inPes_ = false; pesLen_ = 0; }
        }
    }
    if (!done.empty()) feedPes(done.data(), done.size());
}

void DvbSubDecoder::feedPes(const uint8_t* pes, size_t n) {
    DvbSubPesInfo info;
    if (!dvbSubPesInfo(pes, n, info)) return;
    feedSegments(pes + info.payloadOff, n - info.payloadOff, info.hasPts ? info.pts : -1);   // skips data_identifier and subtitle_stream_id
}

// ------------------------------------------------------------------ segments
void DvbSubDecoder::feedSegments(const uint8_t* d, size_t n, double pts) {
    const std::vector<DvbSubSegment> segs = dvbSubSegments(d, n);
    std::lock_guard<std::mutex> lk(mu_);
    for (const DvbSubSegment& s : segs) {
        if (s.page == comp_ || s.page == anc_) {
            if (s.type == 0x80) endOfDisplaySet(pts);
            else segment(s.type, s.page, d + s.off, (int)s.len);
        }
    }
}

void DvbSubDecoder::segment(int type, int page, const uint8_t* d, int n) {
    (void)page;
    switch (type) {
    case 0x10: pageComposition(d, n); break;
    case 0x11: regionComposition(d, n); break;
    case 0x12: clutDefinition(d, n); break;
    case 0x13: objectData(d, n); break;
    case 0x14: displayDefinition(d, n); break;
    default: break;
    }
}

void DvbSubDecoder::pageComposition(const uint8_t* d, int n) {
    DvbSubPageInfo pi;
    if (!dvbSubParsePage(d, n, pi)) return;
    timeout_ = pi.timeout;
    const int ver = pi.version, state = pi.state;
    if (state == 0 && havePage_ && ver == pageVersion_) { /* the same version repeated: nothing new, but regions still listed */ }
    pageVersion_ = ver; pageState_ = state;
    if (state == 1 || state == 2) { regions_.clear(); cluts_.clear(); objects_.clear(); }   // acquisition point / mode change: everything starts afresh
    pageRegs_.clear();
    for (const DvbSubPageReg& r : pi.regions) pageRegs_.push_back({r.id, r.x, r.y});
    havePage_ = true;
}

void DvbSubDecoder::regionComposition(const uint8_t* d, int n) {
    DvbSubRegionInfo ri;
    if (!dvbSubParseRegion(d, n, ri)) return;
    const int id = ri.id, ver = ri.version;
    const bool fill = ri.fill;
    Region& r = regions_[id];
    const int w = ri.w, h = ri.h;
    if (r.version != ver || r.w != w || r.h != h || r.pix.empty()) {
        if (w <= 0 || h <= 0 || w > 4096 || h > 4096) { regions_.erase(id); return; }
        r.id = id; r.version = ver; r.w = w; r.h = h; r.fill = fill;
        r.depth = ri.depth;   // 1 = 2 bit, 2 = 4 bit, 3 = 8 bit
        r.clut = ri.clut; r.pix8 = ri.pix8; r.pix4 = ri.pix4; r.pix2 = ri.pix2;
        const int bg = r.depth == 2 ? r.pix2 : r.depth == 4 ? r.pix4 : r.pix8;
        r.pix.assign((size_t)w * h, fill ? (uint8_t)bg : (uint8_t)0);
        r.objs.clear();
        for (const DvbSubObjRef& oi : ri.objs) { Obj o; o.id = oi.id; o.type = oi.type; o.x = oi.x; o.y = oi.y; o.fg = oi.fg; o.bg = oi.bg; r.objs.push_back(o); }
        // objects already received (they may arrive before the region on a repeat) are drawn now
        for (const Obj& o : r.objs) { auto it = objects_.find(o.id); if (it != objects_.end()) drawObject(r, o, it->second); }
    }
}

void DvbSubDecoder::clutDefinition(const uint8_t* d, int n) {
    int id;
    std::vector<DvbSubClutEntry> entries;
    if (!dvbSubParseClut(d, n, id, entries)) return;
    auto it = cluts_.find(id);
    if (it == cluts_.end()) it = cluts_.emplace(id, defaultClut()).first;
    Clut& c = it->second;
    for (const DvbSubClutEntry& e : entries) {
        if ((e.flags & 0x80) && e.id < 4) c.e2[e.id] = e.rgba;
        if ((e.flags & 0x40) && e.id < 16) c.e4[e.id] = e.rgba;
        if ((e.flags & 0x20)) c.e8[e.id] = e.rgba;
    }
}

void DvbSubDecoder::displayDefinition(const uint8_t* d, int n) {
    int w, h;
    if (!dvbSubParseDisplay(d, n, w, h)) return;
    dispW_ = w;
    dispH_ = h;
}

void DvbSubDecoder::objectData(const uint8_t* d, int n) {
    DvbSubObjectInfo oi;
    if (!dvbSubParseObject(d, n, oi)) return;   // only bitmap objects; character-coded ones are not used by broadcasters
    const int id = oi.id;
    ObjData od;
    od.id = id;
    od.top.assign(d + oi.topOff, d + oi.topOff + oi.topLen);
    od.bottom.assign(d + oi.botOff, d + oi.botOff + oi.botLen);
    objects_[id] = od;
    for (auto& kv : regions_) for (const Obj& o : kv.second.objs) if (o.id == id) drawObject(kv.second, o, objects_[id]);
}


void DvbSubDecoder::drawObject(Region& r, const Obj& o, const ObjData& od) {
    if (o.type != 0) return;   // character-coded objects are not supported
    // a missing bottom field means the top field's lines are doubled
    const bool dbl = od.bottom.empty();
    dvbSubDecodeField(od.top.data(), od.top.size(), o.x, o.y, 2, dbl, r.w, r.h, r.depth, r.pix.data());
    if (!dbl) dvbSubDecodeField(od.bottom.data(), od.bottom.size(), o.x, o.y + 1, 2, false, r.w, r.h, r.depth, r.pix.data());
}

void DvbSubDecoder::endOfDisplaySet(double pts) {
    if (!havePage_) return;
    auto disp = std::make_shared<DvbSubDisplay>();
    disp->pts = pts;
    disp->timeout = timeout_;
    disp->dispW = dispW_; disp->dispH = dispH_;
    const Clut dflt = defaultClut();
    for (const PageReg& pr : pageRegs_) {
        auto it = regions_.find(pr.id);
        if (it == regions_.end()) continue;
        const Region& r = it->second;
        auto ci = cluts_.find(r.clut);
        const Clut& c = ci != cluts_.end() ? ci->second : dflt;
        DvbSubRegionImage im;
        im.x = pr.x; im.y = pr.y; im.w = r.w; im.h = r.h;
        im.rgba.resize((size_t)r.w * r.h * 4);
        dvbSubRegionRgba(r.pix.data(), r.pix.size(), r.depth, c.e2, c.e4, c.e8, im.rgba.data());
        disp->regions.push_back(std::move(im));
    }
    disp->seq = ++seq_;
    queue_.push_back(disp);
    while (queue_.size() > 24) queue_.pop_front();
}

std::shared_ptr<const DvbSubDisplay> DvbSubDecoder::displayAt(double clock) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::shared_ptr<DvbSubDisplay> best;
    for (const auto& d : queue_) if (d->pts < 0 || ptsDelta(d->pts, clock) <= 0.0) best = d;   // the newest one already due
    if (best && (best->regions.empty() || (best->pts >= 0 && best->timeout > 0 && ptsDelta(clock, best->pts) > best->timeout))) best = nullptr;
    if (best) {
        if (best != shown_) { shown_ = best; shownClock_ = clock; }
        return best;
    }
    if (shown_) {   // cleared or expired: keep what was on screen until it has been up for the minimum, measured on the picture clock
        const double up = ptsDelta(clock, shownClock_);
        if (up >= 0 && up < kMinShowSec) return shown_;
        shown_.reset();
    }
    return nullptr;
}

} // namespace dect2
