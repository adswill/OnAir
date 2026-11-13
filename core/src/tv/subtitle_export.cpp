// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

#include "dect2/tv/subtitle_export.h"
#include "dect2/tv/ts_record.h"
#include "dect2/common/text_charset.h"
#include "dect2/common/u8file.h"
#include <cmath>
#include <cstdio>
#include "dect2/common/diag.h"

namespace dect2 {

static std::string stamp(double sec, char sep) {
    if (!(sec > 0)) sec = 0;
    const long long ms = (long long)std::llround(sec * 1000.0);
    char b[32];
    snprintf(b, sizeof b, "%02lld:%02lld:%02lld%c%03lld", ms / 3600000, ms / 60000 % 60, ms / 1000 % 60, sep, ms % 1000);
    return b;
}
std::string srtTimestamp(double s) { return stamp(s, ','); }
std::string vttTimestamp(double s) { return stamp(s, '.'); }

std::string formatSrt(const std::vector<SubCue>& cues) {
    std::string o;
    int n = 0;
    for (const auto& c : cues) {
        if (c.text.empty() || !(c.end > c.start)) continue;
        o += std::to_string(++n) + "\n" + srtTimestamp(c.start) + " --> " + srtTimestamp(c.end) + "\n" + c.text + "\n\n";
    }
    return o;
}

std::string formatVtt(const std::vector<SubCue>& cues) {
    std::string o = "WEBVTT\n\n";
    for (const auto& c : cues) {
        if (c.text.empty() || !(c.end > c.start)) continue;
        std::string t;
        for (char ch : c.text) { if (ch == '&') t += "&amp;"; else if (ch == '<') t += "&lt;"; else if (ch == '>') t += "&gt;"; else t += ch; }
        o += vttTimestamp(c.start) + " --> " + vttTimestamp(c.end) + "\n" + t + "\n\n";
    }
    return o;
}

bool writeTextFile(const std::string& path, const std::string& text) {
    FILE* f = u8fopen(path, "wb");
    if (!f) { DIAG_DEBUG(919964, "The file %s could not be created for writing, so it is not saved.", path.c_str()); return false; }
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

std::string ttxSubtitleText(const TtxPage& p) {
    TtxGrid g;
    ttxRender(p, g);
    std::string out;
    for (int r = 1; r < 24; r++) {
        std::string line;
        for (int c = 0; c < 40; c++) {
            const TtxCell& cell = g[r][c];
            if (!cell.boxed || cell.mosaic || cell.conceal) { line += ' '; continue; }
            text::appendUtf8(line, cell.ch < 0x20 ? ' ' : cell.ch);
        }
        size_t e = line.find_last_not_of(' ');
        if (e == std::string::npos) continue;
        line.resize(e + 1);
        size_t b = line.find_first_not_of(' ');
        line.erase(0, b);
        if (!out.empty()) out += '\n';
        out += line;
    }
    return out;
}

void TeletextSubtitleCollector::configure(int ttxPid, int page, int pcrPid) {
    pid_ = ttxPid; page_ = page; pcrPid_ = pcrPid;
    dec_.setPid(ttxPid);
    dec_.reset();
    cues_.clear(); shown_.clear(); t_ = 0; firstPcr_ = -1; lastPcr_ = 0; wrap_ = 0; lastVersion_ = ~0ull; shownAt_ = 0;
}

void TeletextSubtitleCollector::feed(const uint8_t* pkt) {
    if (pid_ < 0 || pkt[0] != 0x47) return;
    const int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
    if (pid == pcrPid_) {
        double s;
        if (tsPcrSeconds(pkt, s)) {
            if (firstPcr_ < 0) { firstPcr_ = s; lastPcr_ = s; }
            else if (s < lastPcr_ - 1000.0) wrap_ += 95443.7176;                       // the 33 bit PCR base wrapped (26.5 hours)
            else if (s < lastPcr_ - 0.5 || s > lastPcr_ + 10.0) { firstPcr_ += (s - lastPcr_) - 0.04; }   // a jump in the stream: the timeline goes on
            lastPcr_ = s;
            t_ = s - firstPcr_ + wrap_;
        }
        return;
    }
    if (pid != pid_) return;
    dec_.feedTs(pkt);
    TtxPage pg;
    if (!dec_.page(page_, pg)) return;
    if (pg.version == lastVersion_) return;
    lastVersion_ = pg.version;
    const std::string txt = ttxSubtitleText(pg);
    if (txt == shown_) return;
    if (!shown_.empty() && t_ > shownAt_) cues_.push_back({shownAt_, t_, shown_});
    shown_ = txt;
    shownAt_ = t_;
}

void TeletextSubtitleCollector::finish() {
    if (!shown_.empty() && t_ > shownAt_) cues_.push_back({shownAt_, t_, shown_});
    shown_.clear();
}

std::vector<SubCue> TeletextSubtitleCollector::cues() const { return cues_; }

} // namespace dect2
