// Engine::pollMode: reads the newest telemetry of one receiver into the shared RxTelemetry (the status bar, the results windows and the
// logs all read that). One switch case per mode; the modes that copy the common fields use pollStd(), the few that differ have their own
// pollNN() helper above the switch. The mode number is the engine standard (activeStd + 1).
#include "dect2/engine/engine.h"
#include "engine_internal.h"
#include "dect2/source/converter.h"
#include "dect2/net/share.h"
#include "dect2/engine/device_claims.h"
#include "dect2/hfdig/hfdig_ftx.h"
#include "dect2/hfdig/hfdig_kbd.h"
#include "dect2/engine/rate_choice.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/isdbt/isdbt.h"
#include "dect2/engine/modes.h"
#include "dect2/t2/t2rx.h"
#include <chrono>
#include "dect2/common/platform.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

namespace {

// The usual receiver: take its telemetry if a new one is out and copy the fields every mode shares (the mode's own struct goes to t.*field).
// Returns 1 when t was filled, 0 when there was nothing new.
template <class Tel, class Rx>
int pollStd(Rx& rx, int stdCode, RxTelemetry& t, uint64_t& seq, Tel RxTelemetry::* field) {
    Tel mt;
    if (!rx.telemetry(mt, seq)) return 0;
    seq = mt.seq;
    t.standard = stdCode;
    t.seq = mt.seq;
    t.state = mt.state;
    t.cfoHz = mt.cfoHz;
    t.dataValid = mt.dataValid;
    t.dataSnrDb = mt.snrDb;
    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
    t.rateOk = rx.ready();
    t.*field = std::move(mt);
    return 1;
}

// Spectrum explorer: no CFO, lock or block counts (mode 42)
template <class Rx>
int poll42(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        SpecxTelemetry mt;
        if (rx.telemetry(mt, seqs[36])) {
            seqs[36] = mt.seq;
            t.standard = 42;
            t.seq = mt.seq;
            t.state = mt.state;
            t.rateOk = rx.ready();
            // valid: the averaged spectrum is ready and a signal stands 10 dB over the floor (noise alone stays a few dB over it)
            t.dataValid = mt.state == 1 && mt.frames >= (uint64_t)std::max(1, mt.averaging) && mt.peakDb - mt.floorDb > 10;
            t.specx = std::move(mt);
            return 1;
        }
    return 0;
}

// Radio astronomy: no lock flags (mode 49)
template <class Rx>
int poll49(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        AstroTelemetry mt;
        if (rx.telemetry(mt, seqs[43])) {
            seqs[43] = mt.seq;
            t.standard = 49;
            t.seq = mt.seq;
            t.state = mt.state;
            t.cfoHz = 0;
            t.dataValid = (mt.frames > 0 || mt.completed > 0) && mt.powerDb > -150;   // integrating a signal (a measuring tool: no checked blocks)
            t.rateOk = rx.ready();
            t.astro = std::move(mt);
            return 1;
        }
    return 0;
}

// Spectrum analyser: CFO is zero, no SNR (mode 43)
template <class Rx>
int poll43(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        SpecanTelemetry mt;
        if (rx.telemetry(mt, seqs[37])) {
            seqs[37] = mt.seq;
            t.standard = 43;
            t.seq = mt.seq;
            t.state = mt.state;
            t.cfoHz = 0;
            t.dataValid = mt.dataValid;
            t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
            t.rateOk = rx.ready();
            t.specan = std::move(mt);
            return 1;
        }
    return 0;
}

// Ionosonde: no lock flags (mode 53)
template <class Rx>
int poll53(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        IonoTelemetry mt;
        if (rx.telemetry(mt, seqs[47])) {
            seqs[47] = mt.seq;
            t.standard = 53;
            t.seq = mt.seq;
            t.state = mt.state;
            t.cfoHz = 0;
            {   // a sounder's sweep was found (the chirp detector's hit count over its rate's threshold)
                int sw = 0;
                for (const auto& so : mt.sounders) sw += so.sweeps;
                t.dataValid = sw > 0;
            }
            t.rateOk = rx.ready();
            t.iono = std::move(mt);
            return 1;
        }
    return 0;
}

// QO-100 transponder view (mode 54)
template <class Rx>
int poll54(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        Qo100Telemetry mt;
        if (rx.telemetry(mt, seqs[48])) {
            seqs[48] = mt.seq;
            t.standard = 54;
            t.seq = mt.seq;
            t.state = mt.state;
            t.cfoHz = mt.corrValid ? mt.lnbErrHz : 0;
            t.rateOk = rx.ready();
            t.dataValid = mt.corrValid;   // the beacons are locked and the LNB error is measured
            t.qo100 = std::move(mt);
            return 1;
        }
    return 0;
}

// HF beacons (NCDXF): data valid means a beacon was heard (mode 68)
template <class Rx>
int poll68(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        HfbTelemetry mt;
        if (rx.telemetry(mt, seqs[62])) {
            seqs[62] = mt.seq;
            t.standard = 68;
            t.seq = mt.seq;
            t.state = mt.state;
            t.dataValid = mt.heardNow > 0;
            t.dataSnrDb = 0;
            t.rateOk = rx.ready();
            t.hfb = std::move(mt);
            return 1;
        }
    return 0;
}

// Wideband view: always rate-ok (mode 74)
template <class Rx>
int poll74(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        WbTelemetry mt;
        if (rx.telemetry(mt, seqs[68])) {
            seqs[68] = mt.seq;
            t.standard = 74;
            t.seq = mt.seq;
            t.state = mt.state;
            t.dataValid = mt.dataValid;
            t.rateOk = true;
            t.wb = std::move(mt);
            return 1;
        }
    return 0;
}

// Elektro-L: the LRIT/HRIT and GGAK links keep their own telemetry (mode 81)
template <class Rx>
int poll81(Rx& rx, RxTelemetry& t, uint64_t* seqs) {
        ElektroTelemetry mt;
        if (rx.telemetry(mt, seqs[75])) {
            const bool ggak = elektroIsGgak(mt.link);
            seqs[75] = ggak ? mt.ggak.seq : mt.hrit.seq;
            t.standard = 81;
            t.seq = ggak ? mt.ggak.seq : mt.hrit.seq;
            t.state = ggak ? mt.ggak.state : mt.hrit.state;
            t.cfoHz = ggak ? mt.ggak.cfoHz : mt.hrit.cfoHz;
            t.dataValid = ggak ? mt.ggak.dataValid : mt.hrit.dataValid;
            t.dataSnrDb = ggak ? mt.ggak.snrDb : mt.hrit.snrDb;
            t.blocksOk = ggak ? mt.ggak.blocksOk : mt.hrit.blocksOk;
            t.blocksBad = ggak ? mt.ggak.blocksBad : mt.hrit.blocksBad;
            t.rateOk = rx.ready();
            t.elektro = std::move(mt);
            return 1;
        }
    return 0;
}

}   // namespace

// The report of the receivers from DVB-S on (the first mode and the second, each with its own report numbers): 1 = a new one in t,
// 0 = none new, -1 = a is not one of them
static_assert(horus::kActiveStandard < Engine::kPollSeqSlots, "the Horus poll slot is its active standard");
static_assert(kHfuHdrFamily + 1 == 604 && kHfuAle3gFamily + 1 == 605 && kHfuLink11Family + 1 == 606 && kHfu4481Family + 1 == 607, "the HF military case labels in pollMode");   // literal labels: tools/gen_web_schema.py reads them
int Engine::pollMode(int a, RxTelemetry& t, uint64_t* seqs) {
    if (wideOn_.load(std::memory_order_relaxed) && WideModes::supports(a)) return wide_.telemetry(a, t, wideSeq_[a & 255]) ? 1 : 0;
    t.wide.clear();
    switch (a) {
    case 7: return pollStd(rxDvbs_, 7, t, seqs[0], &RxTelemetry::dvbs);
    case 8: return pollStd(rxDtmb_, 8, t, seqs[1], &RxTelemetry::dtmb);
    case 9: return pollStd(rxAtv_, 9, t, seqs[2], &RxTelemetry::atv);
    case 10: return pollStd(rxDmr_, 10, t, seqs[3], &RxTelemetry::dmr);
    case 11: return pollStd(rxDrm_, 11, t, seqs[4], &RxTelemetry::drm);
    case 12: return pollStd(rxAdsb_, 12, t, seqs[5], &RxTelemetry::adsb);
    case 13: return pollStd(rxGnss_, 13, t, seqs[6], &RxTelemetry::gnss);
    // 4.0: one entry per satellite system, the same receiver (gnss_rx.h: gnssViews)
    case 861: return pollStd(rxGnss_, 861, t, seqs[860], &RxTelemetry::gnss);   // GNSS system entry (family 860)
    case 864: return pollStd(rxGnss_, 864, t, seqs[863], &RxTelemetry::gnss);   // GNSS system entry (family 863)
    case 867: return pollStd(rxGnss_, 867, t, seqs[866], &RxTelemetry::gnss);   // GNSS system entry (family 866)
    case 873: return pollStd(rxGnss_, 873, t, seqs[872], &RxTelemetry::gnss);   // GNSS system entry (family 872)
    case 876: return pollStd(rxGnss_, 876, t, seqs[875], &RxTelemetry::gnss);   // GNSS system entry (family 875)
    case 14: return pollStd(rxSonde_, 14, t, seqs[7], &RxTelemetry::sonde);
    // 4.0: one entry per radiosonde type, the same receiver (sonde_rx.h: sondeViews)
    case 685: return pollStd(rxSonde_, 685, t, seqs[684], &RxTelemetry::sonde);   // radiosonde type entry (family 684)
    case 688: return pollStd(rxSonde_, 688, t, seqs[687], &RxTelemetry::sonde);   // radiosonde type entry (family 687)
    case 691: return pollStd(rxSonde_, 691, t, seqs[690], &RxTelemetry::sonde);   // radiosonde type entry (family 690)
    case 694: return pollStd(rxSonde_, 694, t, seqs[693], &RxTelemetry::sonde);   // radiosonde type entry (family 693)
    case 703: return pollStd(rxSonde_, 703, t, seqs[702], &RxTelemetry::sonde);   // radiosonde type entry (family 702)
    case 706: return pollStd(rxSonde_, 706, t, seqs[705], &RxTelemetry::sonde);   // radiosonde type entry (family 705)
    case 709: return pollStd(rxSonde_, 709, t, seqs[708], &RxTelemetry::sonde);   // radiosonde type entry (family 708)
    case 712: return pollStd(rxSonde_, 712, t, seqs[711], &RxTelemetry::sonde);   // radiosonde type entry (family 711)
    case 715: return pollStd(rxSonde_, 715, t, seqs[714], &RxTelemetry::sonde);   // radiosonde type entry (family 714)
    case 718: return pollStd(rxSonde_, 718, t, seqs[717], &RxTelemetry::sonde);   // radiosonde type entry (family 717)
    case 15: return pollStd(rxAis_, 15, t, seqs[8], &RxTelemetry::ais);
    // 4.0: one entry per kind of AIS station, the same receiver (ais_rx.h: aisViews)
    case 739: return pollStd(rxAis_, 739, t, seqs[738], &RxTelemetry::ais);   // AIS station-kind entry (family 738)
    case 808: return pollStd(rxAis_, 808, t, seqs[807], &RxTelemetry::ais);   // AIS station-kind entry (family 807)
    case 811: return pollStd(rxAis_, 811, t, seqs[810], &RxTelemetry::ais);   // AIS station-kind entry (family 810)
    case 814: return pollStd(rxAis_, 814, t, seqs[813], &RxTelemetry::ais);   // AIS station-kind entry (family 813)
    case 817: return pollStd(rxAis_, 817, t, seqs[816], &RxTelemetry::ais);   // AIS station-kind entry (family 816)
    case 858: return pollStd(rxAis_, 858, t, seqs[857], &RxTelemetry::ais);   // AIS station-kind entry (family 857)
    case 16: return pollStd(rxMarine_, 16, t, seqs[9], &RxTelemetry::marine);
    case 17: return pollStd(rxAcars_, 17, t, seqs[10], &RxTelemetry::acars);
    case 18: return pollStd(rxInmc_, 18, t, seqs[11], &RxTelemetry::inmc);
    case 19: return pollStd(rxAero_, 19, t, seqs[12], &RxTelemetry::aero);
    case 20: return pollStd(rxIridium_, 20, t, seqs[13], &RxTelemetry::iridium);
    case 21: return pollStd(rxMesh_, 21, t, seqs[14], &RxTelemetry::mesh);
    case 22: return pollStd(rxHdr_, 22, t, seqs[15], &RxTelemetry::hdr);
    case 23: return pollStd(rxCdr_, 23, t, seqs[16], &RxTelemetry::cdr);
    case 24: return pollStd(rxPager_, 24, t, seqs[17], &RxTelemetry::pager);
    case 25: return pollStd(rxPacket_, 25, t, seqs[18], &RxTelemetry::packet);
    case 26: return pollStd(rxHfdig_, 26, t, seqs[19], &RxTelemetry::hfdig);
    case 27: return pollStd(rxAirband_, 27, t, seqs[20], &RxTelemetry::airband);
    case 28: return pollStd(rxVdl2_, 28, t, seqs[21], &RxTelemetry::vdl2);
    case 29: return pollStd(rxIsm_, 29, t, seqs[22], &RxTelemetry::ism);
    case 30: return pollStd(rxNdb_, 30, t, seqs[23], &RxTelemetry::ndb);
    case 120: return pollStd(rxSitor_, 120, t, seqs[194], &RxTelemetry::sitor);   // SITOR (slot 194)
    case 122: return pollStd(rxDme_, 122, t, seqs[162], &RxTelemetry::dme);   // DME (slot 162: 110 is DGPS, docs/MODE-NUMBERS.md)
    case 31: return pollStd(rxVor_, 31, t, seqs[24], &RxTelemetry::vor);
    case 32: return pollStd(rxM17_, 32, t, seqs[25], &RxTelemetry::m17);
    case 33: return pollStd(rxCell_, 33, t, seqs[26], &RxTelemetry::cell);
    case 35: return pollStd(rxCw_, 35, t, seqs[28], &RxTelemetry::cw);
    case 39: return pollStd(rxNxdn_, 39, t, seqs[32], &RxTelemetry::nxdn);
    case 34: return pollStd(rxDstar_, 34, t, seqs[27], &RxTelemetry::dstar);
    case 36: return pollStd(rxHfdl_, 36, t, seqs[29], &RxTelemetry::hfdl);
    case 40: return pollStd(rxGsmr_, 40, t, seqs[34], &RxTelemetry::gsmr);
    case 37: return pollStd(rxPsk_, 37, t, seqs[30], &RxTelemetry::psk);
    case 42: return poll42(rxSpecx_, t, seqs);
    case 38: return pollStd(rxJs8_, 38, t, seqs[31], &RxTelemetry::js8);
    case 49: return poll49(rxAstro_, t, seqs);
    case 43: return poll43(rxSpecan_, t, seqs);
    case 48: return pollStd(rxMac_, 48, t, seqs[42], &RxTelemetry::mac);
    case 46: return pollStd(rxDpmr_, 46, t, seqs[40], &RxTelemetry::dpmr);
    case 41: return pollStd(rxDvbc_, 41, t, seqs[35], &RxTelemetry::dvbc);
    case 742: return pollStd(rxC2_, 742, t, seqs[742], &RxTelemetry::dvbc2);   // DVB-C2 (family 741)
    case 45: return pollStd(rxOrb_, 45, t, seqs[39], &RxTelemetry::orbcomm);
    case 47: return pollStd(rxTetra_, 47, t, seqs[41], &RxTelemetry::tetra);
    case 44: return pollStd(rxUplink_, 44, t, seqs[38], &RxTelemetry::uplink);
    case 50: return pollStd(rxAle_, 50, t, seqs[44], &RxTelemetry::ale);
    case 51: return pollStd(rxP25_, 51, t, seqs[45], &RxTelemetry::p25);
    case 52: return pollStd(rxBle_, 52, t, seqs[46], &RxTelemetry::ble);
    case 53: return poll53(rxIono_, t, seqs);
    case 54: return poll54(rxQo100_, t, seqs);
    case 55: return pollStd(rxWifi_, 55, t, seqs[49], &RxTelemetry::wifi);
    case 56: return pollStd(rxGeneral_, 56, t, seqs[50], &RxTelemetry::general);
    case 57: return pollStd(rxRid_, 57, t, seqs[51], &RxTelemetry::rid);
    case 58: return pollStd(rxNr_, 58, t, seqs[52], &RxTelemetry::nr);
    case 59: return pollStd(rxTime_, 59, t, seqs[53], &RxTelemetry::timesig);
    // 4.0: one entry per time station, the same receiver (time_rx.h: timeViews)
    case 608: return pollStd(rxTime_, 608, t, seqs[607], &RxTelemetry::timesig);   // time station entry (family 607)
    case 611: return pollStd(rxTime_, 611, t, seqs[610], &RxTelemetry::timesig);   // time station entry (family 610)
    case 614: return pollStd(rxTime_, 614, t, seqs[613], &RxTelemetry::timesig);   // time station entry (family 613)
    case 617: return pollStd(rxTime_, 617, t, seqs[616], &RxTelemetry::timesig);   // time station entry (family 616)
    case 629: return pollStd(rxTime_, 629, t, seqs[628], &RxTelemetry::timesig);   // time station entry (family 628)
    case 632: return pollStd(rxTime_, 632, t, seqs[631], &RxTelemetry::timesig);   // time station entry (family 631)
    case 635: return pollStd(rxTime_, 635, t, seqs[634], &RxTelemetry::timesig);   // time station entry (family 634)
    case 638: return pollStd(rxTime_, 638, t, seqs[637], &RxTelemetry::timesig);   // time station entry (family 637)
    case 643: return pollStd(rxTime_, 643, t, seqs[642], &RxTelemetry::timesig);   // time station entry (family 642)
    case 646: return pollStd(rxTime_, 646, t, seqs[645], &RxTelemetry::timesig);   // time station entry (family 645)
    case 649: return pollStd(rxTime_, 649, t, seqs[648], &RxTelemetry::timesig);   // time station entry (family 648)
    case 60: return pollStd(rxMeteor_, 60, t, seqs[54], &RxTelemetry::meteor);
    case 61: return pollStd(rxGoes_, 61, t, seqs[55], &RxTelemetry::goes);
    case 62: return pollStd(rxCube_, 62, t, seqs[56], &RxTelemetry::cube);
    // 3.9: one entry per framing, the same receiver with the frames of that framing only (cubesat_rx.h: cubesatViews)
    case 841: return pollStd(rxCube_, 841, t, seqs[841], &RxTelemetry::cube);   // AX100 Mode 5 (family 840)
    case 844: return pollStd(rxCube_, 844, t, seqs[844], &RxTelemetry::cube);   // AX100 Mode 6 (family 843)
    case 847: return pollStd(rxCube_, 847, t, seqs[847], &RxTelemetry::cube);   // USP (family 846)
    case 850: return pollStd(rxCube_, 850, t, seqs[850], &RxTelemetry::cube);   // NGHam (family 849)
    case 853: return pollStd(rxCube_, 853, t, seqs[853], &RxTelemetry::cube);   // U482C (family 852)
    case 856: return pollStd(rxCube_, 856, t, seqs[856], &RxTelemetry::cube);   // BEESAT / Mobitex (family 855)
    case 793: return pollStd(rxCube_, 793, t, seqs[792], &RxTelemetry::cube);   // framing entry (family 792)
    case 796: return pollStd(rxCube_, 796, t, seqs[795], &RxTelemetry::cube);   // framing entry (family 795)
    case 805: return pollStd(rxCube_, 805, t, seqs[804], &RxTelemetry::cube);   // framing entry (family 804)
    case 63: return pollStd(rxGs_, 63, t, seqs[57], &RxTelemetry::globalstar);
    case 64: return pollStd(rxVdes_, 64, t, seqs[58], &RxTelemetry::vdes);
    case 65: return pollStd(rxHrpt_, 65, t, seqs[59], &RxTelemetry::hrpt);
    case 66: return pollStd(rxZb_, 66, t, seqs[60], &RxTelemetry::zigbee);
    case 245: return pollStd(rxTh_, 245, t, seqs[204], &RxTelemetry::thread);   // Thread (slot 204)
    case 67: return pollStd(rxFlarm_, 67, t, seqs[61], &RxTelemetry::flarm);
    case 401: return pollStd(rxAvRoad_, 401, t, seqs[300], &RxTelemetry::avroad);
    case 402: return pollStd(rxAvRoad_, 402, t, seqs[301], &RxTelemetry::avroad);
    case 403: return pollStd(rxAvRoad_, 403, t, seqs[302], &RxTelemetry::avroad);
    case 404: return pollStd(rxAvRoad_, 404, t, seqs[303], &RxTelemetry::avroad);
    case 405: return pollStd(rxAvRoad_, 405, t, seqs[304], &RxTelemetry::avroad);
    case 406: return pollStd(rxAvRoad_, 406, t, seqs[305], &RxTelemetry::avroad);
    case 407: return pollStd(rxAvRoad_, 407, t, seqs[306], &RxTelemetry::avroad);
    case 408: return pollStd(rxAvRoad_, 408, t, seqs[307], &RxTelemetry::avroad);
    case 409: return pollStd(rxAvRoad_, 409, t, seqs[308], &RxTelemetry::avroad);
    case 410: return pollStd(rxAvRoad_, 410, t, seqs[309], &RxTelemetry::avroad);
    case 411: return pollStd(rxAvRoad_, 411, t, seqs[310], &RxTelemetry::avroad);
    case 412: return pollStd(rxAvRoad_, 412, t, seqs[311], &RxTelemetry::avroad);
    case 301: return pollStd(rxAdsl_, 301, t, seqs[260], &RxTelemetry::adsl);   // ADS-L (slot 260)
    case 68: return poll68(rxHfb_, t, seqs);
    case 69: return pollStd(rxDji_, 69, t, seqs[63], &RxTelemetry::dji);
    case 70: return pollStd(rxNb_, 70, t, seqs[64], &RxTelemetry::nbiot);
    case 71: return pollStd(rxHl_, 71, t, seqs[65], &RxTelemetry::halow);
    case 73: return pollStd(rxCd_, 73, t, seqs[67], &RxTelemetry::codar);
    case 74: return poll74(rxWb_, t, seqs);
    case 75: return pollStd(rxDw_, 75, t, seqs[69], &RxTelemetry::datawell);
    case 76: return pollStd(rxMt_, 76, t, seqs[70], &RxTelemetry::multitone);
    case 77: return pollStd(rxSame_, 77, t, seqs[71], &RxTelemetry::same);
    case 79: return pollStd(rxDect_, 79, t, seqs[73], &RxTelemetry::dect);
    case 80: return pollStd(rxTetrapol_, 80, t, seqs[74], &RxTelemetry::tetrapol);
    case 81: return poll81(rxEl_, t, seqs);
    case 151: return pollStd(rxSbas_, 151, t, seqs[150], &RxTelemetry::sbas);   // SBAS (slot 150)
    case 146: return pollStd(rxMilhf_, 146, t, seqs[135], &RxTelemetry::milhf);   // HF waveform ID (slot 135)
    case 153: return pollStd(rxArgos_, 153, t, seqs[143], &RxTelemetry::argos);   // Argos (slot 143)
    case 154: return pollStd(rxSfx_, 154, t, seqs[144], &RxTelemetry::sigfoxm);   // Sigfox (slot 144)
    case 121: return pollStd(rxDgps_, 121, t, seqs[110], &RxTelemetry::dgps);   // DGPS beacons (slot 110)
    case 127: return pollStd(rxEloran_, 127, t, seqs[111], &RxTelemetry::eloran);   // eLoran / Loran-C (slot 111)
    case 149: return pollStd(rxNfc_, 149, t, seqs[141], &RxTelemetry::nfc);   // NFC / RFID 13.56 MHz (slot 141)
    case 128: return pollStd(rxMpt_, 128, t, seqs[112], &RxTelemetry::mpt1327);   // MPT1327 trunking (slot 112)
    case 129: return pollStd(rxFusion_, 129, t, seqs[117], &RxTelemetry::fusion);   // Yaesu Fusion (slot 117)
    case 141: return pollStd(rxArd_, 141, t, seqs[118], &RxTelemetry::ardop);   // ARDOP (slot 118)
    case 82: return pollStd(rxLbj_, 82, t, seqs[76], &RxTelemetry::lbj);
    case railway::kKlwActiveStandard: return pollStd(rxKlw_, railway::kKlwActiveStandard, t, seqs[railway::kKlwActiveStandard], &RxTelemetry::klw);
    case railway::kTdcsActiveStandard: return pollStd(rxTdcs_, railway::kTdcsActiveStandard, t, seqs[railway::kTdcsActiveStandard], &RxTelemetry::tdcs);
    case horus::kActiveStandard: return pollStd(rxHorus_, horus::kActiveStandard, t, seqs[horus::kActiveStandard], &RxTelemetry::horus);   // Horus (slot 157)
    case 84: return pollStd(rxUat_, 84, t, seqs[78], &RxTelemetry::uat);
    case 83: return pollStd(rxVdl4_, 83, t, seqs[77], &RxTelemetry::vdl4);
    case 78: return pollStd(rxAnt_, 78, t, seqs[72], &RxTelemetry::ant);
    case 72: return pollStd(rxLr_, 72, t, seqs[66], &RxTelemetry::loraraw);
    case 201: return pollStd(rxLw_, 201, t, seqs[160], &RxTelemetry::lorawan);   // LoRaWAN all channels (slot 160)
    case 202: return pollStd(rxLs_, 202, t, seqs[161], &RxTelemetry::lorasat);   // LoRa satellites (slot 161)
    case 204: return pollStd(rxPac_, 204, t, seqs[164], &RxTelemetry::pactor);   // PACTOR-I monitor (slot 164)
    case 206: return pollStd(rxRoshf_, 206, t, seqs[182], &RxTelemetry::roshf);   // Roshf (slot 182)
    case 207: return pollStd(rxSsdv_, 207, t, seqs[183], &RxTelemetry::ssdv);   // Ssdv (slot 183)
    case 208: return pollStd(rxEbi_, 208, t, seqs[184], &RxTelemetry::ebicab);   // Ebicab (slot 184)
    case 209: return pollStd(rxInmsat_, 209, t, seqs[185], &RxTelemetry::inmsat);   // Inmsat (slot 185)
    case 231: return pollStd(rxGkL_, 231, t, seqs[189], &RxTelemetry::gk2a);   // GK-2A LRIT (slot 189)
    case 232: return pollStd(rxGkH_, 232, t, seqs[190], &RxTelemetry::gk2a);   // GK-2A HRIT (slot 190)
    case 213: return pollStd(rxSw_, 213, t, seqs[188], &RxTelemetry::shortwave);   // Shortwave (slot 188)
    case 233: return pollStd(rxFy_, 233, t, seqs[191], &RxTelemetry::fygeo);   // FengYun FY-4 LRIT, FY-4A or FY-4B (slot 191)
    case 234: return pollStd(rxFy_, 234, t, seqs[192], &RxTelemetry::fygeo);   // FengYun FY-4A HRIT-II/III (slot 192)
    case 235: return pollStd(rxFy_, 235, t, seqs[193], &RxTelemetry::fygeo);   // FengYun FY-2 S-VISSR (slot 193)
    case 242: return pollStd(rxSens_, 242, t, seqs[201], &RxTelemetry::sensors);   // Sub-GHz sensors (slot 201)
    // 4.0: one entry per sensor family, the same receiver (sensors_rx.h: sensorsViews)
    case 565: return pollStd(rxSens_, 565, t, seqs[564], &RxTelemetry::sensors);   // sensor family entry (family 564)
    case 568: return pollStd(rxSens_, 568, t, seqs[567], &RxTelemetry::sensors);   // sensor family entry (family 567)
    case 571: return pollStd(rxSens_, 571, t, seqs[570], &RxTelemetry::sensors);   // sensor family entry (family 570)
    case 574: return pollStd(rxSens_, 574, t, seqs[573], &RxTelemetry::sensors);   // sensor family entry (family 573)
    case 577: return pollStd(rxSens_, 577, t, seqs[576], &RxTelemetry::sensors);   // sensor family entry (family 576)
    case 580: return pollStd(rxSens_, 580, t, seqs[579], &RxTelemetry::sensors);   // sensor family entry (family 579)
    case 583: return pollStd(rxSens_, 583, t, seqs[582], &RxTelemetry::sensors);   // sensor family entry (family 582)
    case 586: return pollStd(rxSens_, 586, t, seqs[585], &RxTelemetry::sensors);   // sensor family entry (family 585)
    case 589: return pollStd(rxSens_, 589, t, seqs[588], &RxTelemetry::sensors);   // sensor family entry (family 588)
    case 592: return pollStd(rxSens_, 592, t, seqs[591], &RxTelemetry::sensors);   // sensor family entry (family 591)
    case 595: return pollStd(rxSens_, 595, t, seqs[594], &RxTelemetry::sensors);   // sensor family entry (family 594)
    case 664: return pollStd(rxSens_, 664, t, seqs[663], &RxTelemetry::sensors);   // sensor family entry (family 663)
    case 667: return pollStd(rxSens_, 667, t, seqs[666], &RxTelemetry::sensors);   // sensor family entry (family 666)
    case 670: return pollStd(rxSens_, 670, t, seqs[669], &RxTelemetry::sensors);   // sensor family entry (family 669)
    case 673: return pollStd(rxSens_, 673, t, seqs[672], &RxTelemetry::sensors);   // sensor family entry (family 672)
    case 676: return pollStd(rxSens_, 676, t, seqs[675], &RxTelemetry::sensors);   // sensor family entry (family 675)
    case 679: return pollStd(rxSens_, 679, t, seqs[678], &RxTelemetry::sensors);   // sensor family entry (family 678)
    case 682: return pollStd(rxSens_, 682, t, seqs[681], &RxTelemetry::sensors);   // sensor family entry (family 681)
    case 246: return pollStd(rxBgan_, 246, t, seqs[205], &RxTelemetry::bgan);   // Inmarsat BGAN (slot 205)
    case 244: return pollStd(rxSfer_, 244, t, seqs[203], &RxTelemetry::sferics);   // Sferics (slot 203)
    case 791: return pollStd(rxLtpos_, 791, t, seqs[790], &RxTelemetry::ltpos);   // Lightning position (slot 790)
    DECT2_X40_ACTIVE_CASES return pollStd(rxX40_, a, t, seqs[a - 1], &RxTelemetry::x40);   // 4.0 signal modes (slots 881 - 896)
    case 871: return pollStd(rxFbr_, 871, t, seqs[870], &RxTelemetry::fbr);   // Fleet Broadcast (slot 870)
    case 602: return pollStd(rxXlink_, 602, t, seqs[601], &RxTelemetry::xlink);   // Extra links, narrow-band members
    case 601: return pollStd(rxXlink_, 601, t, seqs[600], &RxTelemetry::xlink);   // Extra links (slot 600)
    case 621: return pollStd(rxCis_[0], a, t, seqs[621], &RxTelemetry::cishf);   // CIS HF modes (slots 621 - 627)
    case 622: return pollStd(rxCis_[1], a, t, seqs[622], &RxTelemetry::cishf);
    case 623: return pollStd(rxCis_[2], a, t, seqs[623], &RxTelemetry::cishf);
    case 624: return pollStd(rxCis_[3], a, t, seqs[624], &RxTelemetry::cishf);
    case 625: return pollStd(rxCis_[4], a, t, seqs[625], &RxTelemetry::cishf);
    case 626: return pollStd(rxCis_[5], a, t, seqs[626], &RxTelemetry::cishf);
    case 627: return pollStd(rxCis_[6], a, t, seqs[627], &RxTelemetry::cishf);
    case 801: return pollStd(rxW1a_[0], a, t, seqs[801], &RxTelemetry::w1a);   // W1A voiceband modes (slots 801 - 803)
    case 802: return pollStd(rxW1a_[1], a, t, seqs[802], &RxTelemetry::w1a);
    case 803: return pollStd(rxW1a_[2], a, t, seqs[803], &RxTelemetry::w1a);
    case 361: return pollStd(rxRc_, 361, t, seqs[360], &RxTelemetry::rclink);   // RC and mesh links (slot 360)
    case 211: return pollStd(rxNrf_, 211, t, seqs[186], &RxTelemetry::nrf24);   // nRF24L01 (slot 186)
    case 481: return pollStd(rxUmts_, 481, t, seqs[440], &RxTelemetry::umts);   // UMTS cells (slot 440)
    case 661: return pollStd(rxCdma_, 661, t, seqs[660], &RxTelemetry::cdma);   // CDMA2000 cells (slot 660)
    case 662: return pollStd(rxTds_, 662, t, seqs[661], &RxTelemetry::tdscdma);   // TD-SCDMA cells (slot 661)
    case 641: return pollStd(rxVdl3_, 641, t, seqs[640], &RxTelemetry::vdl3);   // VDL Mode 3 (slot 640)
    case 881: return pollStd(rxCp16_, 881, t, seqs[880], &RxTelemetry::cp16);   // CP16 (slot 880)
    case 241: return pollStd(rxSelcal_, 241, t, seqs[200], &RxTelemetry::selcal);   // SELCAL (slot 200)
    case 212: return pollStd(rxAtis_, 212, t, seqs[187], &RxTelemetry::atis);   // ATIS inland waterways (slot 187)
    case 291: return pollStd(rxSd3_[0], 291, t, seqs[290], &RxTelemetry::sd3);
    case 292: return pollStd(rxSd3_[1], 292, t, seqs[291], &RxTelemetry::sd3);
    case 293: return pollStd(rxSd3_[2], 293, t, seqs[292], &RxTelemetry::sd3);
    case 294: return pollStd(rxSd3_[3], 294, t, seqs[293], &RxTelemetry::sd3);
    case 295: return pollStd(rxSd3_[4], 295, t, seqs[294], &RxTelemetry::sd3);
    case 296: return pollStd(rxSd3_[5], 296, t, seqs[295], &RxTelemetry::sd3);
    case 297: return pollStd(rxSd3_[6], 297, t, seqs[296], &RxTelemetry::sd3);
    case 298: return pollStd(rxSd3_[7], 298, t, seqs[297], &RxTelemetry::sd3);
    case 299: return pollStd(rxSd3_[8], 299, t, seqs[298], &RxTelemetry::sd3);
    case 300: return pollStd(rxSd3_[9], 300, t, seqs[299], &RxTelemetry::sd3);
    case 511: return pollStd(rxSd3_[10], 511, t, seqs[510], &RxTelemetry::sd3);
    case 461: return pollStd(rxHs_[0], 461, t, seqs[461], &RxTelemetry::hamsci);   // QRSS (slot 461)
    case 462: return pollStd(rxHs_[1], 462, t, seqs[462], &RxTelemetry::hamsci);   // DFCW (slot 462)
    case 463: return pollStd(rxHs_[2], 463, t, seqs[463], &RxTelemetry::hamsci);   // FSK441 (slot 463)
    case 464: return pollStd(rxHs_[3], 464, t, seqs[464], &RxTelemetry::hamsci);   // ISCAT (slot 464)
    case 465: return pollStd(rxHs_[4], 465, t, seqs[465], &RxTelemetry::hamsci);   // PI4 (slot 465)
    case 466: return pollStd(rxHs_[5], 466, t, seqs[466], &RxTelemetry::hamsci);   // Throb (slot 466)
    case 501: return pollStd(rxHs_[6], 501, t, seqs[501], &RxTelemetry::hamsci);   // SuperSID (slot 501)
    case 502: return pollStd(rxHs_[7], 502, t, seqs[502], &RxTelemetry::hamsci);   // riometer (slot 502)
    case 503: return pollStd(rxHs_[8], 503, t, seqs[503], &RxTelemetry::hamsci);   // pulsar (slot 503)
    case 751: return pollStd(rxHs_[9], 751, t, seqs[751], &RxTelemetry::hamsci);   // JT6M (slot 751)
    case 821: return pollStd(rxHs_[10], 821, t, seqs[821], &RxTelemetry::hamsci);   // event monitor, kind 10 (slot 821)
    case 824: return pollStd(rxHs_[11], 824, t, seqs[824], &RxTelemetry::hamsci);   // event monitor, kind 11 (slot 824)
    case 827: return pollStd(rxHs_[12], 827, t, seqs[827], &RxTelemetry::hamsci);   // event monitor, kind 12 (slot 827)
    case 830: return pollStd(rxHs_[13], 830, t, seqs[830], &RxTelemetry::hamsci);   // event monitor, kind 13 (slot 830)
    case 833: return pollStd(rxHs_[14], 833, t, seqs[833], &RxTelemetry::hamsci);   // event monitor, kind 14 (slot 833)
    case 836: return pollStd(rxHs_[15], 836, t, seqs[836], &RxTelemetry::hamsci);   // event monitor, kind 15 (slot 836)
    case 531: return pollStd(rxXb_[0], 531, t, seqs[530], &RxTelemetry::xband);
    case 532: return pollStd(rxXb_[1], 532, t, seqs[531], &RxTelemetry::xband);
    case 533: return pollStd(rxXb_[2], 533, t, seqs[532], &RxTelemetry::xband);
    case 534: return pollStd(rxXb_[3], 534, t, seqs[533], &RxTelemetry::xband);
    case 535: return pollStd(rxXb_[4], 535, t, seqs[534], &RxTelemetry::xband);
    case 536: return pollStd(rxXb_[5], 536, t, seqs[535], &RxTelemetry::xband);
    case 537: return pollStd(rxXb_[6], 537, t, seqs[536], &RxTelemetry::xband);
    case 538: return pollStd(rxXb_[7], 538, t, seqs[537], &RxTelemetry::xband);
    case 539: return pollStd(rxXb_[8], 539, t, seqs[538], &RxTelemetry::xband);
    case 540: return pollStd(rxXb_[9], 540, t, seqs[539], &RxTelemetry::xband);
    case 541: return pollStd(rxXb_[10], 541, t, seqs[540], &RxTelemetry::xband);
    case 542: return pollStd(rxXb_[11], 542, t, seqs[541], &RxTelemetry::xband);
    case 543: return pollStd(rxXb_[12], 543, t, seqs[542], &RxTelemetry::xband);
    case 544: return pollStd(rxXb_[13], 544, t, seqs[543], &RxTelemetry::xband);
    case 545: return pollStd(rxXb_[14], 545, t, seqs[544], &RxTelemetry::xband);
    case 546: return pollStd(rxXb_[15], 546, t, seqs[545], &RxTelemetry::xband);
    case 547: return pollStd(rxXb_[16], 547, t, seqs[546], &RxTelemetry::xband);
    case 548: return pollStd(rxXb_[17], 548, t, seqs[547], &RxTelemetry::xband);
    case 549: return pollStd(rxXb_[18], 549, t, seqs[548], &RxTelemetry::xband);
    case 550: return pollStd(rxXb_[19], 550, t, seqs[549], &RxTelemetry::xband);
    case 261: return pollStd(rxWx_[0], 261, t, seqs[220], &RxTelemetry::wxsat);   // 0
    case 262: return pollStd(rxWx_[1], 262, t, seqs[221], &RxTelemetry::wxsat);   // 1
    case 263: return pollStd(rxWx_[2], 263, t, seqs[222], &RxTelemetry::wxsat);   // 2
    case 264: return pollStd(rxWx_[3], 264, t, seqs[223], &RxTelemetry::wxsat);   // 3
    case 265: return pollStd(rxWx_[4], 265, t, seqs[224], &RxTelemetry::wxsat);   // 4
    case 266: return pollStd(rxWx_[5], 266, t, seqs[225], &RxTelemetry::wxsat);   // 5
    case 267: return pollStd(rxWx_[6], 267, t, seqs[226], &RxTelemetry::wxsat);   // 6
    case 268: return pollStd(rxWx_[7], 268, t, seqs[227], &RxTelemetry::wxsat);   // 7
    case 269: return pollStd(rxWx_[8], 269, t, seqs[228], &RxTelemetry::wxsat);   // 8
    case 270: return pollStd(rxWx_[9], 270, t, seqs[229], &RxTelemetry::wxsat);   // 9
    case 271: return pollStd(rxWx_[10], 271, t, seqs[230], &RxTelemetry::wxsat);   // 10
    case 272: return pollStd(rxWx_[11], 272, t, seqs[231], &RxTelemetry::wxsat);   // 11
    case 236: return pollStd(rxFy3a_, 236, t, seqs[195], &RxTelemetry::fy3a);   // FY-3 AHRPT (slot 195)
    case 237: return pollStd(rxFy3m_, 237, t, seqs[196], &RxTelemetry::fy3m);   // FY-3 MPT (slot 196)
    case 431: case 432: case 433: case 434: case 435: case 436: case 437: case 438: case 439: case 440: return pollStd(rxHfu_[a - 431], a, t, hfuSeq_[a - 431], &RxTelemetry::hfutil);   // HF utility modes
    case 604: case 605: case 606: case 607:   /* kHfuHdrFamily + 1 .. kHfu4481Family + 1 */ return pollStd(rxHfm_[a - kHfuExtraActiveFirst], a, t, hfmSeq_[a - kHfuExtraActiveFirst], &RxTelemetry::hfutil);   // STANAG 4539 (603), 3G-ALE (604), Link 11 (605), STANAG 4481 (606)
    case 238: return pollStd(rxPh2_, 238, t, seqs[197], &RxTelemetry::pactorhf);   // PACTOR-II (slot 197)
    case 239: return pollStd(rxPh3_, 239, t, seqs[198], &RxTelemetry::pactorhf);   // PACTOR-III (slot 198)
    case 240: return pollStd(rxPh4_, 240, t, seqs[199], &RxTelemetry::pactorhf);   // PACTOR-IV (slot 199)
    case 205: return pollStd(rxEsg_, 205, t, seqs[165], &RxTelemetry::esubghz);   // Flipper Zero Sub-GHz chat (slot 165)
    case 203: return pollStd(rxPskd_, 203, t, seqs[163], &RxTelemetry::pskd);   // PSK demodulator (slot 163)
    case 99: return pollStd(rxZw_, 99, t, seqs[90], &RxTelemetry::zwave);
    case 85: return pollStd(rxWs_, 85, t, seqs[79], &RxTelemetry::wisun);
    case 86: return pollStd(rxEcz_, 86, t, seqs[80], &RxTelemetry::eczas);
    case 102: return pollStd(rxFpv_, 102, t, seqs[96], &RxTelemetry::fpv);
    case 87: return pollStd(rxWmb_, 87, t, seqs[81], &RxTelemetry::wmbus);
    // 4.0: one entry per wireless M-Bus mode, the same receiver (wmbus_live.h: wmbusViews)
    case 721: return pollStd(rxWmb_, 721, t, seqs[720], &RxTelemetry::wmbus);   // wireless M-Bus mode entry (family 720)
    case 724: return pollStd(rxWmb_, 724, t, seqs[723], &RxTelemetry::wmbus);   // wireless M-Bus mode entry (family 723)
    case 727: return pollStd(rxWmb_, 727, t, seqs[726], &RxTelemetry::wmbus);   // wireless M-Bus mode entry (family 726)
    case 733: return pollStd(rxWmb_, 733, t, seqs[732], &RxTelemetry::wmbus);   // wireless M-Bus mode entry (family 732)
    case 736: return pollStd(rxWmb_, 736, t, seqs[735], &RxTelemetry::wmbus);   // wireless M-Bus mode entry (family 735)
    case 90: return pollStd(rxTones_, 90, t, seqs[85], &RxTelemetry::tones);
    case 93: return pollStd(rxSar_, 93, t, seqs[87], &RxTelemetry::sarsat);
    case 96: return pollStd(rxMt63_, 96, t, seqs[88], &RxTelemetry::mt63);
    case 114: return pollStd(rxFsq_, 114, t, seqs[108], &RxTelemetry::fsq);
    case 117: return pollStd(rxIfkp_, 117, t, seqs[109], &RxTelemetry::ifkp);
    case 123: return pollStd(rxKaal_, 123, t, seqs[116], &RxTelemetry::kaal);
    case 132: return pollStd(rxKait_, 132, t, seqs[132], &RxTelemetry::kait);   // KAIT picture bursts (slot 132)
    case 124: return pollStd(rxKavb_, 124, t, seqs[133], &RxTelemetry::kavb);   // KAVB (slot 133)
    case 134: return pollStd(rxKaws_, 134, t, seqs[136], &RxTelemetry::kaws);   // KAWS HF weak-signal chat and beacons (slot 136)
    case 125: return pollStd(rxKapp_, 125, t, seqs[130], &RxTelemetry::kapp);   // KAPP (slot 130)
    case 126: return pollStd(rxKaft_, 126, t, seqs[131], &RxTelemetry::kaft);   // KAFT (slot 131)
    case 108: return pollStd(rxCq_, 108, t, seqs[104], &RxTelemetry::cquam);
    case 105: return pollStd(rxMs_, 105, t, seqs[101], &RxTelemetry::mscatter);
    case 142: return pollStd(rxGd_, 142, t, seqs[119], &RxTelemetry::goesdcs);
    case 701: return pollStd(rxHrdcp_, 701, t, seqs[700], &RxTelemetry::hrdcp);   // Meteosat HRDCP (slot 700)
    case 143: return pollStd(rxModeAc_, 143, t, seqs[120], &RxTelemetry::modeac);   // Mode A/C (slot 120)
    case 144: return pollStd(rxBt_, 144, t, seqs[121], &RxTelemetry::bletrk);   // BLE tracker detector (slot 121)
    case 155: return pollStd(rxIsdbs_, 155, t, seqs[154], &RxTelemetry::isdbs);   // ISDB-S (slot 154)
    case 747: return pollStd(rxDvbsh_, 747, t, seqs[747], &RxTelemetry::dvbsh);   // DVB-SH (slot 747)
    case 551: return pollStd(rxSatd_, 551, t, seqs[551], &RxTelemetry::satdetect);
    case 552: return pollStd(rxSatd_, 552, t, seqs[552], &RxTelemetry::satdetect);
    case 553: return pollStd(rxSatd_, 553, t, seqs[553], &RxTelemetry::satdetect);
    case 554: return pollStd(rxSatd_, 554, t, seqs[554], &RxTelemetry::satdetect);
    case 555: return pollStd(rxSatd_, 555, t, seqs[555], &RxTelemetry::satdetect);
    case 556: return pollStd(rxSatd_, 556, t, seqs[556], &RxTelemetry::satdetect);
    case 557: return pollStd(rxSatd_, 557, t, seqs[557], &RxTelemetry::satdetect);
    case 558: return pollStd(rxSatd_, 558, t, seqs[558], &RxTelemetry::satdetect);
    case 559: return pollStd(rxSatd_, 559, t, seqs[559], &RxTelemetry::satdetect);
    case 560: return pollStd(rxSatd_, 560, t, seqs[560], &RxTelemetry::satdetect);
    case 561: return pollStd(rxSatd_, 561, t, seqs[561], &RxTelemetry::satdetect);
    case 563: return pollStd(rxSatd_, 563, t, seqs[563], &RxTelemetry::satdetect);
    case 562: return pollStd(rxNavic_, 562, t, seqs[562], &RxTelemetry::navic);
    case 731: {   // ISDB-TSB: the ISDB-T receiver fills the common telemetry itself
        if (!rxTsb_.telemetry(t, seqs[731])) return 0;
        seqs[731] = t.seq;
        logIsdbtEvents(t);
        return 1;
    }
    case 145: return pollStd(rxSb_, 145, t, seqs[140], &RxTelemetry::satbeacon);   // TV sat beacon decoder (slot 140)
    case 130: return pollStd(rxLtr_, 130, t, seqs[126], &RxTelemetry::ltr);     // LTR control channel (slot 126)
    case 131: return pollStd(rxEdacs_, 131, t, seqs[127], &RxTelemetry::edacs); // EDACS control channel (slot 127)
    case 321: return pollStd(rxSmartnet_, 321, t, seqs[321], &RxTelemetry::smartnet); // Motorola Type II (slot 321)
    case 322: return pollStd(rxTetraDmo_, 322, t, seqs[322], &RxTelemetry::tetradmo); // TETRA Direct Mode (slot 322)
    case 323: return pollStd(rxOpenSky_, 323, t, seqs[232], &RxTelemetry::opensky);   // OpenSky identifier (slot 232)
    case 156: return pollStd(rxMarineVhf_, 156, t, seqs[142], &RxTelemetry::marinevhf);   // Marine VHF voice (slot 142)
    case 341: case 342: case 343: case 344: case 345: case 346: case 347: return pollStd(rxChv_, a, t, seqs[218], &RxTelemetry::chanvoice);   // channelised voice (slot 218, one receiver for all seven)
    case 181: return pollStd(rxP25p2_, 181, t, seqs[181], &RxTelemetry::p25p2); // P25 Phase 2 (slot 181)
    case 381: return pollStd(rxBtc_, 381, t, seqs[380], &RxTelemetry::btc);     // Bluetooth Classic discovery (slot 380)
    default: return -1;
    }
}

}   // namespace dect2
