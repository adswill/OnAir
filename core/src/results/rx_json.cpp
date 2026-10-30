#include "dect2/results/rx_json.h"
#include "dect2/results/sessions_json.h"
#include "dect2/source/gain_opt.h"
#include "dect2/net/remote_api.h"
#include <cstdio>

namespace dect2 {

namespace {
struct J {
    std::string s;
    bool first = true;
    void key(const char* k) { if (!first) s += ','; first = false; s += '"'; s += k; s += "\":"; }
    void num(const char* k, double v) { key(k); s += jsonNum(v); }
    void u64(const char* k, uint64_t v) { key(k); s += std::to_string(v); }
    void b(const char* k, bool v) { key(k); s += v ? "true" : "false"; }
    void str(const char* k, const std::string& v) { key(k); s += '"' + jsonEscape(v) + '"'; }
    void raw(const char* k, const std::string& v) { key(k); s += v; }
};
const char* const kStd[] = {"DVB-T2", "DVB-T", "ATSC", "DAB", "ATSC 3.0", "ISDB-T", "FM", "DVB-S/S2", "DTMB", "Analog TV"};
} // namespace

std::string rxTelemetryJson(const RxTelemetry& rx, const QualityReport* q) {
    J o;
    o.u64("seq", rx.seq);
    o.raw("sessions", sessionStatusJson());   // the counter a client watches; the list itself is /api/v1/sessions (paged)
    o.num("standard", rx.standard);
    if (rx.standard >= 0 && rx.standard < (int)(sizeof kStd / sizeof *kStd)) o.str("standardName", kStd[rx.standard]);
    o.num("state", rx.state);   // 0 searching, 1 converging, 2 locked
    double snr = 0;
    const char* what = nullptr;
    const bool has = rxSnrDb(rx, snr, &what);
    o.b("locked", has || rx.state == 2);
    o.raw("snrDb", has ? jsonNum(snr) : "null");
    o.raw("snrKind", has ? '"' + std::string(what) + '"' : "null");
    o.num("cfoHz", rx.cfoHz);
    o.u64("blocksOk", rx.blocksOk);
    o.u64("blocksBad", rx.blocksBad);
    if (q && q->valid) {
        J qj;
        qj.num("percent", q->percent); qj.num("snrDb", q->snrDb); qj.num("requiredDb", q->requiredDb); qj.num("marginDb", q->marginDb);
        qj.num("fecOk", q->fecOk); qj.str("label", q->label);
        o.raw("quality", "{" + qj.s + "}");
    }
    J d;
    switch (rx.standard) {
    case 0:
        d.b("l1preOk", rx.l1preOk); d.b("l1postOk", rx.l1postOk); d.num("fftSize", rx.fftN); d.num("guardSamples", rx.guard); d.num("sroPpm", rx.sroPpm);
        d.num("p2SnrDb", rx.p2SnrDb); d.num("dataSnrDb", rx.dataSnrDb); d.b("plpValid", rx.plpValid); d.num("plpId", rx.plpId);
        d.num("plpMerDb", rx.plpMerDb); d.num("plpPreBer", rx.plpPreBer); d.u64("plpFrames", rx.plpFrames); d.num("services", (double)rx.plpList.size());
        break;
    case 1:
        d.b("tpsOk", rx.dvbt.tpsOk); d.b("fecSync", rx.dvbt.fecSync); d.num("fftSize", rx.fftN); d.num("modulation", rx.dvbt.mod); d.num("codeRateHp", rx.dvbt.crHp);
        d.num("guard", rx.dvbt.guard); d.num("dataSnrDb", rx.dataSnrDb); d.u64("rsClean", rx.dvbt.rsClean); d.u64("rsCorrected", rx.dvbt.rsCorrected);
        d.u64("rsFailed", rx.dvbt.rsFailed); d.u64("packets", rx.dvbt.packets);
        break;
    case 2:
        d.b("pilot", rx.atsc.pilot); d.b("fieldSync", rx.atsc.fieldSync); d.b("tsOk", rx.atsc.tsOk); d.num("snrDb", rx.atsc.snrDb); d.num("dataSnrDb", rx.atsc.dataSnrDb);
        d.u64("rsClean", rx.atsc.rsClean); d.u64("rsCorrected", rx.atsc.rsCorrected); d.u64("rsFailed", rx.atsc.rsFailed);
        break;
    case 3:
        d.b("sync", rx.dab.sync); d.num("snrDb", rx.dab.snrDb); d.b("ensemble", rx.dab.ensemble); d.str("ensembleLabel", rx.dab.ensembleLabel);
        d.u64("fibOk", rx.dab.fibOk); d.u64("fibBad", rx.dab.fibBad);
        d.u64("syncLosses", rx.dab.syncLosses); d.num("clockPpm", rx.dab.sroPpm);
        d.u64("rsCorrected", rx.dab.audio.rsCorrected); d.u64("rsUncorrectable", rx.dab.audio.rsUncorrectable);
        d.u64("auOk", rx.dab.audio.auOk); d.u64("auBad", rx.dab.audio.auBad); d.u64("concealed", rx.dab.audio.concealedFrames);
        break;
    case 6:
        d.b("carrier", rx.fm.carrier); d.num("snrDb", rx.fm.snrDb); d.num("levelDbfs", rx.fm.levelDbfs); d.num("deviationKhz", rx.fm.devKhz);
        d.b("stereo", rx.fm.stereo); d.b("rdsSync", rx.fm.rdsSync); d.str("psName", rx.fm.psName); d.str("radioText", rx.fm.radioText);
        d.num("stereoPct", rx.fm.stereoPct); d.num("multipathPct", rx.fm.multipathPct); d.b("equaliser", rx.fm.equaliser);
        d.num("pi", rx.fm.piCode); d.str("pty", rx.fm.ptyText); d.num("rdsBlockOkPct", rx.fm.rdsBlockOkPct); d.u64("rdsGroups", rx.fm.rdsGroups);
        d.u64("rdsBlocksCorrected", rx.fm.rds.blocksCorrected); d.str("ptyn", rx.fm.rds.ptyn); d.str("ert", rx.fm.rds.ert);
        if (rx.fm.rds.ecc >= 0) d.num("ecc", rx.fm.rds.ecc);
        if (rx.fm.rds.clockUtc) { d.num("clockUtc", (double)rx.fm.rds.clockUtc); d.num("clockOffsetMin", rx.fm.rds.clockOffsetMin); }
        {
            std::string af;
            for (double f : rx.fm.rds.afMhz) { char b[16]; snprintf(b, sizeof b, "%s%.1f", af.empty() ? "" : ",", f); af += b; }
            d.str("afMhz", af);
            for (const auto& t : rx.fm.rds.rtPlus) { if (t.type == 1) d.str("rtPlusTitle", t.text); else if (t.type == 4) d.str("rtPlusArtist", t.text); }
        }
        break;
    case 7:
        d.num("symbolRate", rx.dvbs.symbolRate); d.num("snrDb", rx.dvbs.snrDb); d.num("merDb", rx.dvbs.merDb); d.b("dataValid", rx.dvbs.dataValid);
        break;
    case 8:
        d.num("snrDb", rx.dtmb.snrDb); d.num("merDb", rx.dtmb.merDb); d.num("carrierToNoiseDb", rx.dtmb.snrPnDb); d.b("tsLock", rx.dtmb.tsLock);
        break;
    case 41:
        d.num("symbolRate", rx.dvbc.symbolRate); d.num("qam", rx.dvbc.qam); d.num("merDb", rx.dvbc.merDb); d.num("levelDbfs", rx.dvbc.levelDbfs);
        break;
    case 82: {
        d.str("mode", "LBJ");
        std::string rows = "[";
        for (const auto& m : rx.lbj.messages) {
            J row;
            row.str("train", m.train); row.str("speedKmh", m.speedKmh); row.str("kilometer", m.kmMarker);
            row.str("locomotive", m.loco); row.str("route", m.route); row.str("direction", m.direction);
            row.b("paired", m.paired); row.b("timeSync", m.timeSync); row.str("networkTime", m.networkTime);
            row.raw("latitude", m.positionValid ? jsonNum(m.latitude) : "null"); row.raw("longitude", m.positionValid ? jsonNum(m.longitude) : "null");
            row.num("address", m.address); row.num("correctedBits", m.correctedBits); row.num("wallTime", double(m.wallTime));
            if (rows.size() > 1) rows += ','; rows += '{' + row.s + '}';
        }
        d.raw("messages", rows + ']'); break;
    }
    case horus::kActiveStandard: {   // the payloads heard and their last position
        d.str("mode", "Horus");
        std::string rows = "[";
        for (const auto& p : rx.horus.payloads) {
            J row;
            const auto& k = p.last;
            row.str("callsign", p.callsign); row.num("payloadId", p.payloadId); row.num("kind", k.kind); row.u64("seq", k.seq);
            row.num("latitude", k.lat); row.num("longitude", k.lon); row.num("altitudeM", k.altM);
            row.num("speedKmh", k.speedKmh); row.num("satellites", k.sats); row.num("temperatureC", k.tempC); row.num("batteryV", k.battV);
            row.num("snrDb", k.snrDb); row.num("offsetHz", k.offsetHz); row.u64("packets", p.packets); row.num("wallTime", double(k.wallTime));
            row.str("sentence", k.sentence);
            if (rows.size() > 1) rows += ',';
            rows += '{' + row.s + '}';
        }
        d.raw("payloads", rows + ']'); break;
    }
    case railway::kKlwActiveStandard:
    case railway::kTdcsActiveStandard: {
        const auto& t = rx.standard == railway::kKlwActiveStandard ? rx.klw : rx.tdcs;
        d.str("mode", rx.standard == railway::kKlwActiveStandard ? "KLW" : "TDCS");
        std::string rows = "[";
        for (const auto& m : t.messages) {
            J row;
            row.str("train", m.train); row.str("locomotive", m.locomotive); row.str("action", m.action);
            row.num("actionCode", m.actionCode); row.str("tailId", m.tailId); row.str("parameter", m.parameter); row.str("clockSync", m.clockSync);
            row.raw("pressureKpa", m.pressureKpa >= 0 ? std::to_string(m.pressureKpa) : "null");
            row.num("speedKmh", m.speedKmh); row.raw("kilometer", m.kilometerValid ? jsonNum(m.kilometer) : "null");
            row.b("testFrame", m.testFrame); row.b("increasingMileage", m.increasingMileage);
            row.num("weightTonnes", m.weightTonnes); row.num("length", m.length); row.num("carCount", m.carCount);
            row.num("station", m.station); row.num("track", m.track); row.num("actualTrack", m.actualTrack); row.num("driver", m.driver);
            row.num("correctedBits", m.correctedBits); row.num("wallTime", double(m.wallTime));
            if (rows.size() > 1) rows += ','; rows += '{' + row.s + '}';
        }
        d.raw("messages", rows + ']'); break;
    }
    default: break;
    }
    if (!d.s.empty()) o.raw("details", "{" + d.s + "}");
    return "{" + o.s + "}";
}

} // namespace dect2
