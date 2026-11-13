// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// DAB+ PAD: data stream element -> F-PAD / X-PAD -> MSC data groups -> MOT objects -> slide show picture.
#include "dect2/dab/dab_pad.h"
#include "dect2/dab/dab.h"
#include "dect2/dab/dab_proto.h"
#include "dect2/common/png.h"
#include "dect2/common/text_charset.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cctype>
#include <cstring>

namespace dect2 {

bool decodeSlideImage(const uint8_t* data, size_t n, int subtype, int& w, int& h, std::vector<uint8_t>& rgba) {
    if (!data || n < 8) return false;
    if (subtype == 3) return decodePng(data, n, w, h, rgba);
    const AVCodecID id = subtype == 1 ? AV_CODEC_ID_MJPEG : subtype == 0 ? AV_CODEC_ID_GIF : subtype == 2 ? AV_CODEC_ID_BMP : AV_CODEC_ID_NONE;
    const AVCodec* codec = id == AV_CODEC_ID_NONE ? nullptr : avcodec_find_decoder(id);
    if (!codec) return false;
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    bool ok = false;
    if (ctx && pkt && fr && avcodec_open2(ctx, codec, nullptr) >= 0 && av_new_packet(pkt, (int)n) >= 0) {
        std::memcpy(pkt->data, data, n);
        int r = avcodec_send_packet(ctx, pkt);
        if (r >= 0) { r = avcodec_receive_frame(ctx, fr); if (r == AVERROR(EAGAIN)) { avcodec_send_packet(ctx, nullptr); r = avcodec_receive_frame(ctx, fr); } }
        if (r >= 0 && fr->width > 0 && fr->height > 0 && fr->width <= 4096 && fr->height <= 4096) {
            SwsContext* sws = sws_getContext(fr->width, fr->height, (AVPixelFormat)fr->format, fr->width, fr->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws) {
                w = fr->width; h = fr->height;
                rgba.assign((size_t)w * (size_t)h * 4, 0);
                uint8_t* dst[1] = {rgba.data()};
                const int ls[1] = {w * 4};
                ok = sws_scale(sws, fr->data, fr->linesize, 0, h, dst, ls) == h;
                sws_freeContext(sws);
            }
        }
    }
    if (fr) av_frame_free(&fr);
    if (pkt) av_packet_free(&pkt);
    if (ctx) avcodec_free_context(&ctx);
    return ok;
}

const char* dabDlpTypeName(int t) {
    static const char* const n[] = {"Dummy", "Title", "Album", "Track number", "Artist", "Composition", "Movement", "Conductor", "Composer", "Band", "Comment",
        "Genre", "News", "Local news", "Stock market", "Sport", "Lottery", "Horoscope", "Daily diversion", "Health", "Event", "Scene", "Cinema", "TV",
        "Date and time", "Weather", "Traffic", "Alarm", "Advertisement", "URL", "Other info", "Station name (short)", "Station name (long)",
        "Programme now", "Programme next", "Programme part", "Host", "Editorial staff", "Programme frequency", "Programme homepage", "Programme sub-channel",
        "Hotline phone", "Studio phone", "Phone", "Studio SMS", "SMS", "Hotline e-mail", "Studio e-mail", "E-mail", "MMS", "Chat", "Chat centre",
        "Vote question", "Vote centre"};
    if (t >= 0 && t < (int)(sizeof n / sizeof *n)) return n[t];
    return t >= 56 && t <= 58 ? "Private" : "Reserved";
}

void DabPadDecoder::reset() {
    lastCont_ = 0; lastType_ = 0; group_.clear(); groupLen_ = 0; collecting_ = false;
    dls_.clear(); dlsOn_ = false; dlsToggle_ = -1; dlsParts_.clear(); dlsLast_ = -1; label_.clear();
    labelChars_.clear(); dlpRaw_.clear(); dlpToggle_ = -1; dlpRunning_ = false; dlp_.clear();
    mot_.reset();
    slide_ = DabSlide();
    spiLogos_.clear(); images_.clear(); imageOrder_.clear(); logos_.clear(); logoSeq_ = 0;
}

// The split of a PAD into F-PAD, X-PAD and application subfields is dab_proto (Rust); the decoder keeps what the subfields assemble into.
void DabPadDecoder::accessUnit(const uint8_t* au, int n) {
    dabp::PadBytes pb;
    if (!dabp::padFromAu(au, n < 0 ? 0 : (size_t)n, pb)) return;   // the data stream element (id_syn_ele 4) comes first when there is PAD
    pad(pb.xpad.data(), (int)pb.xpad.size(), pb.fpad);
}

void DabPadDecoder::mp2Frame(const uint8_t* f, int len) {
    dabp::PadBytes pb;
    if (!dabp::padFromMp2(f, len < 0 ? 0 : (size_t)len, lastCont_, pb)) return;
    pad(pb.xpad.data(), (int)pb.xpad.size(), pb.fpad);
}

void DabPadDecoder::pad(const uint8_t* xp, int xlen, const uint8_t fpad[2]) {
    for (const dabp::PadSub& s : dabp::padSplit(xp, xlen < 0 ? 0 : (size_t)xlen, fpad, lastType_)) subfield(s.type, xp + s.off, (int)s.len);
}

void DabPadDecoder::subfield(int type, const uint8_t* d, int n) {
    if (type == 1) {                                        // data group length indicator: 14 bit length, CRC-16
        int len = groupLen_;
        const int r = dabp::padDgLength(d, n < 0 ? 0 : (size_t)n, len);
        if (r == 0) return;
        if (r == 1) { groupLen_ = 0; collecting_ = false; return; }
        groupLen_ = len;
        collecting_ = false;
        return;
    }
    if (type == 2 || type == 3) {                           // dynamic label: segments of a 2 byte prefix, up to 16 characters and a CRC-16
        if (type == 2) { dls_.clear(); dlsOn_ = true; }
        if (!dlsOn_) return;
        dls_.insert(dls_.end(), d, d + n);
        if (dls_.size() >= 2) {
            const dabp::DlsStep st = dabp::dlsStep(dls_.data(), dls_.size());   // a segment (2 byte prefix, characters, CRC-16) or a command, complete yet?
            if (st.action == 2) { dlsOn_ = false; labelSegment(dls_.data(), (int)st.segLen); }
            else if (st.action == 1) dlsOn_ = false;
        }
        return;
    }
    if (type != 12 && type != 13) return;
    if (type == 12) { group_.clear(); collecting_ = groupLen_ > 0; }
    if (!collecting_) return;
    const size_t take = std::min((size_t)n, (size_t)groupLen_ - group_.size());
    group_.insert(group_.end(), d, d + take);
    if ((int)group_.size() >= groupLen_) {
        std::vector<uint8_t> g;
        g.swap(group_);
        collecting_ = false; groupLen_ = 0;
        dataGroup(g.data(), (int)g.size());
    }
}

// One dynamic label segment without its CRC: prefix = toggle, first, last, command flag, 4 bit field 1 (length - 1, or the command);
// second byte: field 2 (character set in the first segment, else the segment number in bits 6..4)
void DabPadDecoder::labelSegment(const uint8_t* d, int n) {
    const dabp::DlSegment sg = dabp::dlSegment(d, n < 0 ? 0 : (size_t)n);
    const int toggle = sg.toggle;
    const bool first = sg.first, last = sg.last, cmd = sg.cmd;
    if (cmd) {
        const int code = sg.code;
        if (code == 1) { label_.clear(); labelChars_.clear(); dlsParts_.clear(); dlsLast_ = -1; dlp_.clear(); dlpRaw_.clear(); }   // command 1: clear the display
        else if (code == 2) dlPlusCommand(toggle, d + 2, n - 2);
        return;
    }
    const int segNo = sg.segNo;
    if (toggle != dlsToggle_ || (first && !dlsParts_.empty() && dlsParts_.count(0) && dlsParts_[0] != std::vector<uint8_t>(d + 2, d + n))) {
        dlsParts_.clear(); dlsLast_ = -1; dlsToggle_ = toggle;     // a new label
    }
    if (first) dlsCharset_ = sg.charset;
    dlsParts_[segNo].assign(d + 2, d + n);
    if (last) dlsLast_ = segNo;
    if (dlsLast_ < 0 || (int)dlsParts_.size() != dlsLast_ + 1 || !dlsParts_.count(0)) return;
    std::vector<uint8_t> all;
    for (auto& kv : dlsParts_) { if (kv.first > dlsLast_) return; all.insert(all.end(), kv.second.begin(), kv.second.end()); }
    label_ = text::clean(text::dabCharset(dlsCharset_, all.data(), all.size()));
    // the characters one by one, for the DL Plus tags
    labelChars_.clear();
    for (size_t i = 0; i < all.size();) {
        size_t w = 1;
        if (dlsCharset_ == 6) w = 2;
        else if (dlsCharset_ == 15) { const uint8_t b = all[i]; w = b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : b >= 0xC0 ? 2 : 1; }
        w = std::min(w, all.size() - i);
        labelChars_.push_back(text::dabCharset(dlsCharset_, &all[i], w));
        i += w;
    }
    resolveDlPlus();
}

// DL Plus tags command (TS 102 980 clause 5.3): item toggle, item running, number of tags - 1, then per tag the content type, the start and the
// length - 1 in characters of the label, 7 bits each
void DabPadDecoder::dlPlusCommand(int toggle, const uint8_t* d, int n) {
    bool running = false;
    std::vector<dabp::DlpTag> tags;
    if (!dabp::dlPlusTags(d, n < 0 ? 0 : (size_t)n, running, tags)) return;
    dlpRaw_.clear();
    for (const dabp::DlpTag& t : tags) dlpRaw_.push_back({t.type, t.start, t.len});
    dlpRunning_ = running;
    dlpToggle_ = toggle;
    resolveDlPlus();
}

void DabPadDecoder::resolveDlPlus() {
    dlp_.clear();
    if (dlpToggle_ != dlsToggle_ || label_.empty()) return;
    for (const RawTag& t : dlpRaw_) {
        if (t.type == 0 || t.start + t.len > (int)labelChars_.size()) continue;
        std::string s;
        for (int i = t.start; i < t.start + t.len; i++) s += labelChars_[(size_t)i];
        s = text::clean(s);
        if (!s.empty()) dlp_.push_back({t.type, s});
    }
}

void DabPadDecoder::dataGroup(const uint8_t* d, int n) {
    if (n < 3) return;
    const bool crc = d[0] & 0x40;
    const int type = d[0] & 0x0F;
    if (sink_ && (type == 0 || type == 6)) sink_(d, (size_t)n);
    (void)crc;
    mot_.dataGroup(d, n);                                   // header, body and directory groups: header mode and directory mode
}

void DabPadDecoder::motObject(const MotObject& m) {
    const std::vector<uint8_t>& b = m.body;
    const int ctype = m.contentType, subtype = m.subtype;
    const std::string& name = m.name;
    const std::string& category = m.category;
    if (ctype != 2) {                                                   // not a picture: the SPI service information is XML
        if (motSink_) motSink_(m);
        if (b.size() < 20) return;
        const std::string xml((const char*)b.data(), b.size());
        if (xml.find("<service") != std::string::npos && xml.find("multimedia") != std::string::npos) spi(xml);
        return;
    }
    DabSlide s;
    if (!decodeSlideImage(b.data(), b.size(), subtype, s.w, s.h, s.rgba)) return;
    s.name = name;
    std::string cat = category;
    for (char& c : cat) c = (char)std::tolower((unsigned char)c);
    const bool catLogo = cat.find("logo") != std::string::npos;
    if (!name.empty()) {
        auto it = images_.find(name);
        if (it == images_.end()) {
            if (imageOrder_.size() >= 24) { images_.erase(imageOrder_.front()); imageOrder_.pop_front(); }
            imageOrder_.push_back(name);
        }
        images_[name] = s;
    }
    if (catLogo) {                                                      // a logo sent through the slide show: of the service that carries it
        bool same = false;
        for (DabLogo& l : logos_) if (l.sid == 0 && l.name == name) { same = l.rgba == s.rgba; l.w = s.w; l.h = s.h; l.rgba = s.rgba; }
        if (!same) {
            if (std::none_of(logos_.begin(), logos_.end(), [&](const DabLogo& l) { return l.sid == 0 && l.name == name; })) {
                DabLogo l; l.sid = 0; l.name = name; l.w = s.w; l.h = s.h; l.rgba = s.rgba; logos_.push_back(std::move(l));
            }
            logoSeq_++;
        }
        return;
    }
    if (isLogoName(name)) { resolveLogos(); return; }
    s.seq = slide_.seq + 1;
    slide_ = std::move(s);
}

bool DabPadDecoder::isLogoName(const std::string& name) const {
    if (name.empty()) return false;
    for (const SpiLogo& l : spiLogos_) if (l.url == name || (l.url.size() > name.size() && l.url.compare(l.url.size() - name.size(), name.size(), name) == 0 && l.url[l.url.size() - name.size() - 1] == '/')) return true;
    return false;
}

namespace {
// the value of attribute `key` in the tag text (between < and >), "" when absent
std::string attr(const std::string& tag, const char* key) {
    const std::string k = std::string(key) + "=";
    for (size_t p = tag.find(k); p != std::string::npos; p = tag.find(k, p + 1)) {
        if (p > 0 && !std::isspace((unsigned char)tag[p - 1])) continue;
        const size_t q = p + k.size();
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) continue;
        const size_t e = tag.find(tag[q], q + 1);
        if (e == std::string::npos) return "";
        return tag.substr(q + 1, e - q - 1);
    }
    return "";
}
// the 16 bit service id of a SPI bearer / service id such as "dab:e1.ce15.c224.0" or "e1.ce15.c224.0"; 0 when not parsable
uint32_t spiSid(std::string id) {
    if (id.compare(0, 4, "dab:") == 0) id.erase(0, 4);
    int seg = 0;
    size_t start = 0;
    for (size_t i = 0; i <= id.size(); i++) {
        if (i == id.size() || id[i] == '.') {
            if (seg == 2) {
                std::string h = id.substr(start, i - start);
                if (h.empty() || h.size() > 8) return 0;
                char* end = nullptr;
                const unsigned long v = std::strtoul(h.c_str(), &end, 16);
                return end && *end == 0 ? (uint32_t)v : 0;
            }
            seg++; start = i + 1;
        }
    }
    return 0;
}
}

// SPI service information (TS 102 818): per <service> the bearer / serviceID with the service id and the <multimedia type="logo_*" url width height>
void DabPadDecoder::spi(const std::string& xml) {
    std::vector<SpiLogo> found;
    size_t pos = 0;
    uint32_t sid = 0;
    bool inService = false;
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        const size_t e = xml.find('>', pos);
        if (e == std::string::npos) break;
        const std::string tag = xml.substr(pos + 1, e - pos - 1);
        pos = e + 1;
        if (tag.compare(0, 8, "service ") == 0 || tag == "service" || tag.compare(0, 8, "service>") == 0) { inService = true; sid = 0; }
        else if (tag.compare(0, 8, "/service") == 0 && (tag.size() == 8 || !std::isalnum((unsigned char)tag[8]))) { inService = false; sid = 0; }
        else if (inService && (tag.compare(0, 7, "bearer ") == 0 || tag.compare(0, 10, "serviceID ") == 0) && sid == 0) sid = spiSid(attr(tag, "id"));
        else if (inService && tag.compare(0, 11, "multimedia ") == 0) {
            const std::string type = attr(tag, "type"), url = attr(tag, "url");
            if (sid && !url.empty() && type.compare(0, 4, "logo") == 0)
                found.push_back({sid, type, url, std::atoi(attr(tag, "width").c_str()), std::atoi(attr(tag, "height").c_str())});
        }
    }
    if (found.empty()) return;
    for (const SpiLogo& f : found) {
        if (std::none_of(spiLogos_.begin(), spiLogos_.end(), [&](const SpiLogo& l) { return l.sid == f.sid && l.type == f.type && l.url == f.url; })) spiLogos_.push_back(f);
    }
    if (spiLogos_.size() > 400) spiLogos_.erase(spiLogos_.begin(), spiLogos_.end() - 400);
    resolveLogos();
}

// Matches the logo entries with the pictures heard: one picture per service, the colour square one first
void DabPadDecoder::resolveLogos() {
    auto rank = [](const std::string& t) {
        if (t == "logo_colour_square") return 0;
        if (t == "logo_unrestricted") return 1;
        if (t == "logo_colour_rectangle") return 2;
        return 3;
    };
    std::map<uint32_t, const SpiLogo*> best;
    std::map<uint32_t, const DabSlide*> img;
    for (const SpiLogo& l : spiLogos_) {
        const DabSlide* pic = nullptr;
        for (const auto& kv : images_) {
            const std::string& n = kv.first;
            if (l.url == n || (l.url.size() > n.size() && l.url.compare(l.url.size() - n.size(), n.size(), n) == 0 && l.url[l.url.size() - n.size() - 1] == '/')) { pic = &kv.second; break; }
        }
        if (!pic) continue;
        auto it = best.find(l.sid);
        if (it == best.end() || rank(l.type) < rank(it->second->type)) { best[l.sid] = &l; img[l.sid] = pic; }
    }
    bool changed = false;
    for (const auto& kv : best) {
        const DabSlide& pic = *img[kv.first];
        DabLogo* cur = nullptr;
        for (DabLogo& l : logos_) if (l.sid == kv.first) cur = &l;
        if (cur && cur->name == pic.name && cur->rgba == pic.rgba) continue;
        if (!cur) { logos_.emplace_back(); cur = &logos_.back(); cur->sid = kv.first; }
        cur->type = kv.second->type; cur->name = pic.name; cur->w = pic.w; cur->h = pic.h; cur->rgba = pic.rgba;
        changed = true;
    }
    if (changed) logoSeq_++;
}

} // namespace dect2
