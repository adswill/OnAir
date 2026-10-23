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
    case 14: return pollStd(rxSonde_, 14, t, seqs[7], &RxTelemetry::sonde);
    case 15: return pollStd(rxAis_, 15, t, seqs[8], &RxTelemetry::ais);
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
    case 60: return pollStd(rxMeteor_, 60, t, seqs[54], &RxTelemetry::meteor);
    case 61: return pollStd(rxGoes_, 61, t, seqs[55], &RxTelemetry::goes);
    case 62: return pollStd(rxCube_, 62, t, seqs[56], &RxTelemetry::cube);
    case 63: return pollStd(rxGs_, 63, t, seqs[57], &RxTelemetry::globalstar);
    case 64: return pollStd(rxVdes_, 64, t, seqs[58], &RxTelemetry::vdes);
    case 65: return pollStd(rxHrpt_, 65, t, seqs[59], &RxTelemetry::hrpt);
    case 66: return pollStd(rxZb_, 66, t, seqs[60], &RxTelemetry::zigbee);
    case 67: return pollStd(rxFlarm_, 67, t, seqs[61], &RxTelemetry::flarm);
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
    case 203: return pollStd(rxPskd_, 203, t, seqs[163], &RxTelemetry::pskd);   // PSK demodulator (slot 163)
    case 99: return pollStd(rxZw_, 99, t, seqs[90], &RxTelemetry::zwave);
    case 85: return pollStd(rxWs_, 85, t, seqs[79], &RxTelemetry::wisun);
    case 86: return pollStd(rxEcz_, 86, t, seqs[80], &RxTelemetry::eczas);
    case 102: return pollStd(rxFpv_, 102, t, seqs[96], &RxTelemetry::fpv);
    case 87: return pollStd(rxWmb_, 87, t, seqs[81], &RxTelemetry::wmbus);
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
    case 143: return pollStd(rxModeAc_, 143, t, seqs[120], &RxTelemetry::modeac);   // Mode A/C (slot 120)
    case 144: return pollStd(rxBt_, 144, t, seqs[121], &RxTelemetry::bletrk);   // BLE tracker detector (slot 121)
    case 155: return pollStd(rxIsdbs_, 155, t, seqs[154], &RxTelemetry::isdbs);   // ISDB-S (slot 154)
    case 145: return pollStd(rxSb_, 145, t, seqs[140], &RxTelemetry::satbeacon);   // TV sat beacon decoder (slot 140)
    case 130: return pollStd(rxLtr_, 130, t, seqs[126], &RxTelemetry::ltr);     // LTR control channel (slot 126)
    case 131: return pollStd(rxEdacs_, 131, t, seqs[127], &RxTelemetry::edacs); // EDACS control channel (slot 127)
    case 156: return pollStd(rxMarineVhf_, 156, t, seqs[142], &RxTelemetry::marinevhf);   // Marine VHF voice (slot 142)
    case 181: return pollStd(rxP25p2_, 181, t, seqs[181], &RxTelemetry::p25p2); // P25 Phase 2 (slot 181)
    default: return -1;
    }
}

}   // namespace dect2
