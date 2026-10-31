#include "vdl2_log.h"
#include "../state/results_export.h"
#include "dect2/common/time_fmt.h"
#include <cstdio>

namespace vdl2log {

std::string utc(int64_t wall) {
    if (wall <= 0) return "";
    return dect2::timefmt::format(wall, dect2::timefmt::Zone::Utc, dect2::timefmt::Style::DateClock, false);
}

const char* fmtExt(Fmt f) { return f == Fmt::Csv ? ".csv" : f == Fmt::Json ? ".json" : ".txt"; }

static std::string hex6(uint32_t a) { char b[8]; std::snprintf(b, sizeof b, "%06X", a & 0xFFFFFF); return b; }
static const char* kind(int t) { return t == 1 ? "aircraft" : t == 4 || t == 5 ? "ground station" : t == 7 ? "all stations" : "reserved"; }

size_t Log::feed(const dect2::Vdl2Telemetry& t) {
    size_t n = 0;
    uint64_t newest = last_;
    std::deque<const dect2::Vdl2Message*> fresh;
    for (const auto& m : t.messages) {   // newest first
        if (m.serial <= last_) break;
        fresh.push_front(&m);
        newest = std::max(newest, m.serial);
    }
    for (const auto* m : fresh) { rows_.push_back(*m); n++; }
    last_ = newest;
    while (rows_.size() > kMax) rows_.pop_front();
    return n;
}

std::string Log::render(Fmt f) const {
    if (f == Fmt::Text) {
        std::string o;
        for (const auto& m : rows_) {
            char b[256];
            std::snprintf(b, sizeof b, "%s UTC  %.3f MHz  %.0f dBFS  SNR %.0f dB  %s -> %s  %s", utc(m.wallTime).c_str(), m.freqHz / 1e6, m.levelDb, m.snrDb,
                          hex6(m.src).c_str(), hex6(m.dst).c_str(), m.frameName.c_str());
            o += b;
            if (!m.reg.empty()) o += "  reg " + m.reg;
            if (!m.flight.empty()) o += "  flight " + m.flight;
            o += "\n";
            if (!m.detail.empty()) { o += "  " + m.detail; if (o.back() != '\n') o += "\n"; }
            if (m.part) o += "  (one block of a longer ACARS message; the joined message follows)\n";
            if (m.acarsBlocks > 1) o += "  (joined from " + std::to_string(m.acarsBlocks) + " blocks" + (m.acarsIncomplete ? ", the end did not arrive" : "") + ")\n";
            if (m.acars && !m.acarsText.empty()) o += "  ACARS " + m.label + ": " + m.acarsText + "\n";
            o += "\n";
        }
        return o;
    }
    resexp::Table t;
    t.cols = {"utc", "freq_mhz", "level_dbfs", "snr_db", "from", "from_type", "to", "to_type", "frame", "protocol", "registration", "flight", "label", "summary", "acars_text", "blocks", "raw_block", "detail",
              "lat", "lon", "alt_ft"};
    for (const auto& m : rows_) {
        t.add({utc(m.wallTime), resexp::num(m.freqHz / 1e6, 4), resexp::num(m.levelDb, 1), resexp::num(m.snrDb, 1), hex6(m.src), kind(m.srcType), hex6(m.dst), kind(m.dstType),
               m.frameName, m.proto, m.reg, m.flight, m.label, m.summary, m.acarsText, m.acars ? std::to_string(m.acarsBlocks) : "", m.part ? "1" : "", m.detail,
               m.hasPos ? resexp::num(m.lat, 5) : "", m.hasPos ? resexp::num(m.lon, 5) : "", m.hasAlt ? std::to_string(m.altFt) : ""},
              (double)m.wallTime);
    }
    return resexp::render(t, f == Fmt::Csv ? resexp::Fmt::Csv : resexp::Fmt::Json);
}

}  // namespace vdl2log
