// Transmit: the list of modes OnAir can send (see tx.h for the reasoning), their generators, and the TxSession that feeds a radio.
#include "dect2/common/thread_name.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_shape.h"
#include "dect2/common/timecompat.h"
#include "dect2/atsc/atsc_gen.h"
#include "dect2/engine/device_claims.h"
#include "dect2/tv/demo_ts.h"
#include "dect2/dtmb/dtmb_gen.h"
#include "dect2/dvbc/dvbc_rx.h"
#include "dect2/dvbs/dvbs_gen.h"
#include "dect2/dvbt/dvbt.h"
#include "dect2/dvbt/dvbt_gen.h"
#include "dect2/dsp/exact_resampler.h"
#include "dect2/net/map_guard.h"
#include "dect2/dsp/dsp_compat.h"
#include "dect2/dsp/fftutil.h"
#include "dect2/isdbt/isdbt.h"
#include "dect2/hfdig/hfdig_freedv.h"
#include "dect2/hfdig/hfdig_gen.h"
#include "dect2/isdbt/isdbt_gen.h"
#include "dect2/m17/m17_codec2.h"
#include "dect2/kaal/kaal_txgen.h"
#include "dect2/kait/kait_txgen.h"
#include "dect2/kapp/kapp_txgen.h"
#include "dect2/kavb/kavb_txgen.h"
#include "dect2/kaws/kaws_txgen.h"
#include "dect2/esubghz/esubghz_gen.h"
#include "dect2/kaft/kaft_txgen.h"
#include "dect2/nfc/nfc_tx.h"
#include "dect2/engine/mode_tuning.h"
#include "dect2/t2/t2.h"
#include "dect2/t2/t2gen.h"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <vector>
#include "dect2/common/mathconst.h"
#ifndef _WIN32
#include <unistd.h>
#endif

namespace dect2 {

// ---------------------------------------------------------------- the list

namespace {
constexpr const char* kTrip = "tests/tx/test_tx.cpp";
constexpr const char* kUnv = "offered from the mode generator; engine round-trip not run in test_tx (unverified)";
constexpr const char* kOpt = "the mode's SynthConfig (see the mode's *_gen.h)";

TxModeSpec mk(const char* id, const char* name, int std, const char* gen, const char* opt, const char* trip,
              int variant = 0, double nom = 0, bool fixed = false, double minr = -1) {
    TxModeSpec m;
    m.id = id; m.name = name; m.stdMode = std; m.variant = variant;
    const ModeTuning* mt = modeTuning(std);
    m.nominalRateHz = nom > 0 ? nom : (mt ? mt->sampleRate : 2e6);
    m.fixedRate = fixed;
    m.minRateHz = minr >= 0 ? minr : (mt ? mt->minSampleRate : 0);
    m.generator = gen; m.options = opt; m.roundTrip = trip;
    return m;
}

std::vector<TxModeSpec> makeTxTable() {
    // First eight: the original verified TV / ADS-B list. After that: every other generator-capable mode that is not on the deny-list.
    return {
        mk("dvbt2", "DVB-T2", 0, "T2Generator, real PLP with the test transport stream", "FFT size, guard interval, pilot pattern, channel width", kTrip, 1, 64e6 / 7, true, 0),
        mk("dvbt", "DVB-T", 0, "dvbt::Generator, demo programme", "mode 2K/8K, guard, modulation, code rate, channel width", kTrip, 2, 64e6 / 7, true, 0),
        mk("atsc", "ATSC 1.0 (8-VSB)", 3, "atsc::Generator, demo programme", "none", kTrip, 0, 8e6, false, 6.5e6),
        mk("dab", "DAB / DAB+", 4, "makeDabSynth, transmission mode I with real audio", "none", kTrip, 0, 2.048e6, false, 2e6),
        mk("isdbt", "ISDB-T", 6, "ISDB-T layers A and B with the demo programme", "channel width 6/7/8 MHz", kTrip, 0, 8e6, false, 6e6),
        mk("dvbs", "DVB-S / S2 / S2X", 8, "makeDvbsSynth (dvbs_tx.h chain), demo programme", "standard, modulation, code rate, roll-off, symbol rate ...", kTrip),
        mk("dtmb", "DTMB", 9, "makeDtmbSynth (dtmb_tx.h chain), demo programme", "header, modulation, code rate, interleaver, channel width", kTrip),
        mk("adsb", "ADS-B / Mode S", 13, "makeAdsbSynth, Mode S frames of simulated aircraft", "number of aircraft, message mix, message rate", kTrip),
        mk("atsc3", "ATSC 3.0", 5, "makeAtsc3Synth, in-memory test programme", kOpt, kUnv, 0, 8e6, false, 6.5e6),
        mk("fm", "FM broadcast", 7, "FmGenerator, stereo multiplex with RDS", "tone Hz, WAV, station name, radio text", kTrip, 0, 2e6, false, 500e3),
        mk("atv", "Analog TV", 10, "makeAtvSynth, test card and sound carrier", kOpt, kUnv),
        mk("dmr", "DMR", 11, "makeDmrSynth", kOpt, kTrip),
        mk("drm", "DRM", 12, "makeDrmSynth", kOpt, kUnv),
        mk("sonde", "Radiosonde", 15, "makeSondeSynth", kOpt, kUnv),
        mk("ais", "AIS", 16, "makeAisSynth", kOpt, kTrip),
        mk("marine", "Marine (NAVTEX / DSC / weather fax)", 17, "makeMarineSynth", kOpt, kUnv),
        mk("acars", "ACARS", 18, "makeAcarsSynth", kOpt, kUnv),
        mk("inmc", "Inmarsat-C", 19, "makeInmcSynth", kOpt, kUnv),
        mk("aero", "Inmarsat Aero", 20, "makeAeroSynth", kOpt, kUnv),
        mk("iridium", "Iridium", 21, "makeIridiumSynth", kOpt, kUnv),
        mk("mesh", "Mesh (LoRa)", 22, "makeMeshSynth", kOpt, kUnv),
        mk("hdr", "HD Radio", 23, "makeHdrSynth", kOpt, kUnv),
        mk("cdr", "CDR", 24, "makeCdrSynth", kOpt, kUnv),
        mk("pager", "Pagers (POCSAG / FLEX)", 25, "makePagerSynth, FM on a 25 kHz channel", "mix / POCSAG / FLEX, speed, polarity (pager_gen.h)", kTrip),
        mk("packet", "APRS / Packet", 26, "makePacketSynth", kOpt, kTrip),
        mk("rtty", "RTTY", 27, "makeRttyTxAudio / makeHfdigSynth", "text, baud, shift, USB/LSB", kTrip, 0),
        mk("sstv", "SSTV", 27, "makeSstvTx, PPM or test picture with callsign stamp", "image file, callsign, SSTV mode", kUnv, 1),
        mk("freedv", "FreeDV", 27, "makeFreedvTestAudio", kOpt, kUnv, 2),
        mk("ft8", "FT8", 27, "makeFtxTxAudio", "callsign, grid, message kind", kTrip, 3),
        mk("ft4", "FT4", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 4),
        mk("wspr", "WSPR", 27, "makeFtxTxAudio", "callsign, grid, power dBm", kUnv, 5),
        mk("ft2", "FT2", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 6),
        mk("hamdrm", "HamDRM", 27, "makeHamdrmTx, file bytes and callsign", "file, callsign", kUnv, 7),
        mk("jt65", "JT65", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 21),
        mk("jt9", "JT9", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 22),
        mk("q65", "Q65", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 23),
        mk("fst4", "FST4", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 24),
        mk("msk144", "MSK144", 27, "makeFtxTxAudio", "callsign, grid, message kind", kUnv, 25),
        mk("olivia", "Olivia", 27, "makeKbdTx", "text, tones, bandwidth", kUnv, 26),
        mk("contestia", "Contestia", 27, "makeKbdTx", "text", kUnv, 27),
        mk("mfsk", "MFSK", 27, "makeKbdTx", "text", kUnv, 28),
        mk("thor", "THOR", 27, "makeKbdTx", "text", kUnv, 29),
        mk("dominoex", "DominoEX", 27, "makeKbdTx", "text", kUnv, 30),
        mk("hell", "Hellschreiber", 27, "makeKbdTx", "text", kUnv, 31),
        mk("airband", "Airband AM", 28, "makeAirbandSynth, AM voice channels", "tone Hz or WAV file", kUnv),
        mk("vdl2", "VDL Mode 2", 29, "makeVdl2Synth", kOpt, kUnv),
        mk("vdl4", "VDL Mode 4", 84, "makeVdl4Synth", kOpt, kUnv),
        mk("uat", "UAT 978", 85, "makeUatSynth", kOpt, kUnv),
        mk("wisun", "Wi-SUN", 86, "makeWisunSynth", kOpt, kUnv),
        mk("eczas", "e-Czas 225 kHz", 87, "makeEczasSynth", kOpt, kUnv),
        mk("fpv", "FPV video (analog)", 103, "makeFpvSynth", kOpt, kUnv),
        mk("wmbus", "Wireless M-Bus", 88, "makeWmbusSynth", kOpt, kUnv),
        mk("mt63", "MT63", 97, "makeMt63Synth", kOpt, kUnv),
        mk("fsq", "FSQ", 115, "makeFsqSynth", kOpt, kUnv),
        mk("ifkp", "IFKP", 118, "makeIfkpSynth", kOpt, kUnv),
        mk("cquam", "AM stereo (C-QUAM)", 109, "makeCquamSynth", kOpt, kUnv),
        mk("ardop", "ARDOP", 142, "makeArdopSynth", kOpt, kUnv),
        mk("zwave", "Z-Wave", 100, "makeZwaveSynth", kOpt, kUnv),
        mk("mscatter", "Meteor scatter", 106, "makeMscatterSynth", kOpt, kUnv),
        mk("goesdcs", "GOES DCS", 143, "makeGoesDcsSynth", kOpt, kUnv),
        mk("modeac", "Mode A/C", 144, "makeModeAcSynth", kOpt, kUnv),
        mk("bletracker", "BLE tracker detector", 145, "makeBletrkSynth", kOpt, kUnv),
        mk("satbeacon", "TV sat beacon decoder", 146, "makeSatbeaconSynth", kOpt, kUnv),
        mk("nfc", "NFC / RFID reader (13.56 MHz)", 150, "makeNfcReaderTxGenerator, 100% ASK Miller reader frames on a continuous carrier", "none (REQA, anticollision, SELECT, READ, GET_VERSION; 13.553-13.567 MHz ISM band only)", kUnv, 0, 4e6, false, 2.4e6),
        mk("vorils", "VOR / ILS", 32, "makeVorSynth", kOpt, kUnv),
        mk("m17", "M17", 33, "makeM17Synth", kOpt, kUnv),
        mk("dstar", "D-STAR", 35, "makeDstarSynth", kOpt, kUnv),
        mk("cw", "CW", 36, "makeCwSynth", kOpt, kTrip),
        mk("hfdl", "HFDL", 37, "makeHfdlSynth", kOpt, kUnv),
        mk("psk", "PSK31", 38, "makePskSynth", kOpt, kUnv),
        mk("js8", "JS8Call", 39, "makeJs8Synth", kOpt, kUnv),
        mk("nxdn", "NXDN", 40, "makeNxdnSynth", kOpt, kUnv),
        mk("dvbc", "DVB-C", 42, "makeDvbcSynth", kOpt, kUnv),
        mk("uplink", "Mode S uplink", 45, "makeUplinkSynth", kOpt, kUnv),
        mk("orbcomm", "Orbcomm", 46, "makeOrbcommSynth", kOpt, kUnv),
        mk("dpmr", "dPMR", 47, "makeDpmrSynth", kOpt, kUnv),
        mk("tetra", "TETRA", 48, "makeTetraSynth", kOpt, kUnv),
        mk("mac", "MAC", 49, "makeMacSynth", kOpt, kUnv),
        mk("ale", "HF ALE", 51, "makeAleSynth", kOpt, kUnv),
        mk("p25", "P25", 52, "makeP25Synth", kOpt, kUnv),
        mk("qo100", "QO-100", 55, "makeQo100Synth", kOpt, kUnv),
        mk("meteor", "Meteor-M LRPT", 61, "makeMeteorSynth", kOpt, kUnv),
        mk("goes", "GOES HRIT", 62, "makeGoesSynth", kOpt, kUnv),
        mk("cubesat", "CubeSat telemetry", 63, "makeCubesatSynth", kOpt, kUnv),
        mk("globalstar", "Globalstar", 64, "makeGlobalstarSynth", kOpt, kUnv),
        mk("vdes", "VDES", 65, "makeVdesSynth", kOpt, kUnv),
        mk("hrpt", "HRPT / AHRPT", 66, "makeHrptSynth", kOpt, kUnv),
        mk("zigbee", "Zigbee / 802.15.4", 67, "makeZigbeeSynth", kOpt, kUnv),
        mk("flarm", "FLARM", 68, "makeFlarmSynth", kOpt, kUnv),
        mk("hfbeacon", "HF beacons", 69, "makeHfbeaconSynth", kOpt, kUnv),
        mk("dji", "DJI drones", 70, "makeDjiSynth", kOpt, kUnv),
        mk("loraraw", "LoRa (raw)", 73, "makeLoraRawSynth", kOpt, kUnv),
        mk("same", "NOAA Weather Radio SAME", 78, "makeSameSynth", kOpt, kUnv),
        mk("dect", "DECT cordless phones", 80, "makeDectSynth", kOpt, kUnv),
        mk("gnss", "GNSS", 14, "makeGnssSynth", kOpt, kUnv),
        mk("ism", "ISM sensors", 30, "makeIsmSynth", kOpt, kUnv),
        mk("cell", "Cellular (LTE)", 34, "makeCellSynth", kOpt, kUnv),
        mk("gsmr", "GSM-R", 41, "makeGsmrSynth", kOpt, kUnv),
        mk("specx", "Explorer", 43, "makeSpecxSynth", kOpt, kUnv),
        mk("specan", "Spectrum analyser", 44, "makeSpecanSynth", kOpt, kUnv),
        mk("astro", "Radio astronomy", 50, "makeAstroSynth", kOpt, kUnv),
        mk("ble", "Bluetooth LE", 53, "makeBleSynth", kOpt, kUnv),
        mk("iono", "Ionospheric sounders", 54, "makeIonoSynth", kOpt, kUnv),
        mk("wifi", "Wi-Fi", 56, "makeWifiSynth", kOpt, kUnv),
        mk("general", "General receiver", 57, "makeGeneralSynth", kOpt, kUnv),
        mk("rid", "Drone Remote ID", 58, "makeRidSynth", kOpt, kUnv),
        mk("nr", "5G NR", 59, "makeNrSynth", kOpt, kUnv),
        mk("timesig", "Time signals", 60, "makeTimeSynth", kOpt, kUnv),
        mk("nbiot", "NB-IoT", 71, "makeNbiotSynth", kOpt, kUnv),
        mk("halow", "Wi-Fi HaLow", 72, "makeHalowSynth", kOpt, kUnv),
        mk("codar", "CODAR", 74, "makeCodarSynth", kOpt, kUnv),
        mk("datawell", "Datawell", 76, "makeDatawellSynth", kOpt, kUnv),
        mk("multitone", "Multitone paging", 77, "makeMultitoneSynth", kOpt, kUnv),
        mk("ant", "ANT+", 79, "makeAntSynth", kOpt, kUnv),
        mk("tetrapol", "TETRAPOL", 81, "makeTetrapolSynth", kOpt, kUnv),
        mk("ndb", "NDB beacons", 31, "makeNdbSynth", kOpt, kUnv),
        mk("elektro", "Elektro-L", 82, "makeElektroSynth", kOpt, kUnv),
        mk("lbj", "LBJ", 83, "makeLbjSynth", kOpt, kUnv),
        mk("kait", "KAIT", 133, "makeKaitTxGenerator (kait::planBurst: wavelet image stream in layers, 10 s burst, sweeps, pilots)", "callsign, a PNG or PPM picture (empty: the demo picture), profile 0 balanced / 1 robust / 2 quality / 3 direct", kUnv, 0, 632000, false, 400000),
        mk("kaal", "KAAL", 124, "makeKaalTxGenerator (kaal::VoiceTx: 16 kbit/s voice codec, 16-QAM LDPC, beacon slice)", "callsign, grid, position, power, bio text, beacon on or off; the voice from a microphone or a WAV file", kUnv, 0, 48000, false, 48000),
        mk("kavb", "KAVB", 125, "makeKavbTxGenerator (kavb::Transmitter: 16-QAM / 64-QAM LDPC frames with the V sweep, container packets in FEC groups)", "callsign, a file of bytes or the synthetic test stream, profile A to D (700 / 1050 / 1400 ksym/s), FEC profile, test stream seconds", kUnv, 0, 2.8e6, false, 1.12e6),
        mk("kapp", "KAPP", 126, "makeKappTxGenerator (kapp::buildBurst: BPSK to 1024-QAM LDPC bursts of selective-repeat blocks, sent one-way and repeated)", "callsign, message text or a file, profile 0 to 8, passes", kUnv, 0, 250000, false, 125000),
        mk("kaft", "KAFT", 127, "makeKaftTxGenerator (kaft::buildBurst: OPEN, data frames of LDPC blocks with CRC-32, FINISH; one-way and repeated)", "callsign, file, width 200 kHz / 500 kHz / 1 MHz, profile 0 to 9, passes", kUnv, 0, 2.4e6, false, 250000),
        mk("kaws", "KAWS", 135, "makeKawsTxGenerator (kaws::buildBurst: HF weak-signal chat and beacon bursts of LDPC blocks in UTC slots, as upper-sideband audio IQ, sent one-way and repeated)", "callsign, grid, recipient, message text, kind chat / short beacon / full beacon, profile 0 to 5, passes", kUnv, 0, 12000, false, 12000),
        mk("esubghz", "Flipper Zero Sub-GHz chat", 206, "makeEsubghzTxGenerator (CC1101 GFSK 9.99 kbaud, packets of 60 octets; the line is sent once)", "the text of one plain chat line; encrypted lines are sent from the mode's Chat screen", "tests/esubghz/test_esubghz_rx.cpp (generator into the receiver)", 0, 2e6, false, 200e3),
    };
}
}

const std::vector<TxModeSpec>& txModes() {
    static const std::vector<TxModeSpec> t = makeTxTable();
    return t;
}

const TxModeSpec* txModeById(const std::string& id) {
    for (const auto& m : txModes()) if (id == m.id) return &m;
    return nullptr;
}

std::unique_ptr<ModeSynth> makeTxPayloadSynth(const TxModeSpec& mode, const TxRequest& r, const SynthConfig& clean, double rateHz);

const TxModeSpec* txModeFor(int stdMode, int dvbVariant) {
    if (stdMode == 1) return txModeById("dvbt2");
    if (stdMode == 2) return txModeById("dvbt");
    for (const auto& m : txModes()) {
        if (m.stdMode != stdMode) continue;
        if (stdMode == 0 && m.variant != (dvbVariant == 2 ? 2 : 1)) continue;
        return &m;
    }
    return nullptr;
}

std::string txRefusalReason(int stdMode) {
    // the only reason left: there is no generator for the mode (nothing else is refused: no mode, frequency or power limit)
    if (txModeFor(stdMode, 1)) return {};
    return "this mode has no transmit generator";
}

int txEngineStd(int family) {
    if (family >= 6) return family + 2;
    if (family == 1) return 3;
    if (family == 2) return 4;
    if (family == 3) return 5;
    if (family == 4) return 6;
    if (family == 5) return 7;
    return 0;
}

const TxModeSpec* txModeForList(int family, const char* subId, int dvbVariant) {
    if (subId && *subId) {
        if (const TxModeSpec* m = txModeById(subId)) return m;
        const std::string s = subId;
        if (s == "pocsag" || s == "flex") return txModeById("pager");
        if (s == "navtex" || s == "dsc" || s == "wefax") return txModeById("marine");
        if (s == "vor" || s == "ils") return txModeById("vorils");
        if (s == "lorawan" || s == "meshtastic" || s == "meshcore" || s == "loraaprs" || s == "meshcom") return txModeById("mesh");
    }
    if (family == 0) return txModeFor(0, dvbVariant == 2 ? 2 : 1);
    return txModeFor(txEngineStd(family), dvbVariant);
}

TxPayloadKind txPayloadKind(const std::string& id) {
    if (id == "dvbt2" || id == "dvbt" || id == "atsc" || id == "atsc3" || id == "isdbt" || id == "dvbs" || id == "dtmb" || id == "dvbc")
        return TxPayloadKind::TvFile;
    if (id == "atv") return TxPayloadKind::AnalogTv;
    if (id == "dab" || id == "drm" || id == "hdr" || id == "cdr") return TxPayloadKind::BroadcastAudio;
    if (id == "ft8" || id == "ft4" || id == "wspr" || id == "ft2" || id == "jt65" || id == "jt9" || id == "q65" || id == "fst4" || id == "msk144")
        return TxPayloadKind::Ftx;
    if (id == "rtty") return TxPayloadKind::Rtty;
    if (id == "olivia" || id == "contestia" || id == "mfsk" || id == "thor" || id == "dominoex" || id == "hell") return TxPayloadKind::Keyboard;
    if (id == "cw") return TxPayloadKind::Cw;
    if (id == "psk") return TxPayloadKind::Psk;
    if (id == "js8") return TxPayloadKind::Js8;
    if (id == "hamdrm") return TxPayloadKind::Hamdrm;
    if (id == "sstv") return TxPayloadKind::Sstv;
    if (id == "freedv") return TxPayloadKind::Freedv;
    if (id == "pager") return TxPayloadKind::Pager;
    if (id == "packet") return TxPayloadKind::Packet;
    if (id == "ais") return TxPayloadKind::Ais;
    if (id == "adsb") return TxPayloadKind::AdsB;
    if (id == "fm") return TxPayloadKind::VoiceFm;
    if (id == "airband") return TxPayloadKind::VoiceAir;
    if (id == "m17" || id == "dmr" || id == "p25" || id == "nxdn" || id == "dpmr" || id == "dstar" || id == "tetra") return TxPayloadKind::VoiceDigital;
    if (id == "qo100") return TxPayloadKind::Qo100;
    if (id == "marine") return TxPayloadKind::Marine;
    if (id == "acars" || id == "vdl2" || id == "hfdl") return TxPayloadKind::AcarsFamily;
    if (id == "sonde") return TxPayloadKind::Sonde;
    if (id == "same") return TxPayloadKind::Same;
    if (id == "ale") return TxPayloadKind::Ale;
    if (id == "vorils") return TxPayloadKind::VorIls;
    if (id == "loraraw") return TxPayloadKind::LoraRaw;
    if (id == "dect") return TxPayloadKind::Dect;
    if (id == "cubesat") return TxPayloadKind::CubeSat;
    if (id == "meteor" || id == "goes" || id == "hrpt") return TxPayloadKind::WeatherScene;
    if (id == "inmc" || id == "aero" || id == "iridium" || id == "orbcomm" || id == "globalstar") return TxPayloadKind::SatScene;
    if (id == "flarm") return TxPayloadKind::Flarm;
    if (id == "zigbee") return TxPayloadKind::Zigbee;
    if (id == "dji") return TxPayloadKind::Dji;
    if (id == "hfbeacon") return TxPayloadKind::HfBeacon;
    if (id == "vdes") return TxPayloadKind::Vdes;
    if (id == "uplink") return TxPayloadKind::Uplink;
    if (id == "mesh") return TxPayloadKind::Mesh;
    if (id == "mac") return TxPayloadKind::Mac;
    if (id == "kaal") return TxPayloadKind::Kaal;
    if (id == "kait") return TxPayloadKind::Kait;
    if (id == "kavb") return TxPayloadKind::Kavb;
    if (id == "kapp") return TxPayloadKind::Kapp;
    if (id == "kaft") return TxPayloadKind::Kaft;
    if (id == "kaws") return TxPayloadKind::Kaws;
    if (id == "esubghz") return TxPayloadKind::Keyboard;   // one chat line of text
    if (id == "vdl4" || id == "uat" || id == "wisun" || id == "zwave") return TxPayloadKind::SatScene;   // demo-traffic generators, no custom message
    return TxPayloadKind::SatScene;   // never a generic callsign dump; unknown ids are treated as a test scene
}

bool txCanEncodeVoice(const TxModeSpec& m) {
    const std::string id = m.id ? m.id : "";
    if (id == "m17") return m17Codec2Available();
    if (id == "freedv") return hfdigFreedvLibrary();
    return false;
}

std::string txVocoderNote(const TxModeSpec& m) {
    const std::string id = m.id ? m.id : "";
    if (id == "m17") {
        return m17Codec2Available()
            ? "Live voice is encoded with Codec 2 (libcodec2)."
            : "libcodec2 is not loaded: live encode is off. Destination callsigns still go on the air; voice bits are the known test pattern. A 1 kHz tone, silence or a WAV/MP3 can still be sent as analog NFM.";
    }
    if (id == "freedv") {
        return hfdigFreedvLibrary()
            ? "Live voice is encoded with Codec 2 (FreeDV)."
            : "libcodec2 is not loaded: live encode is off. A 1 kHz tone, silence or a WAV/MP3 can still be sent.";
    }
    if (id == "dmr" || id == "p25" || id == "nxdn" || id == "dpmr" || id == "dstar")
        return "This vocoder cannot encode: OnAir loads mbelib for decode only (AMBE/IMBE). Destination ID / talkgroup / colour code still go on the air; voice bits are the test pattern. A 1 kHz tone, silence or a WAV/MP3 can be sent as analog NFM instead.";
    if (id == "tetra")
        return "TETRA ACELP is not encoded here. Colour code and IDs are shown; the downlink test pattern is sent. A 1 kHz tone, silence or a WAV/MP3 can be sent as analog NFM instead.";
    return {};
}

bool txModeFinite(const TxModeSpec& m) { return m.id && (std::string(m.id) == "kapp" || std::string(m.id) == "kaft" || std::string(m.id) == "kavb" || std::string(m.id) == "kaws" || std::string(m.id) == "esubghz"); }

bool txModeNeedsAudio(const TxModeSpec& m, const TxRequest& r) {
    if (r.carrierTune) return false;
    const std::string id = m.id ? m.id : "";
    if (id == "kaal") return true;   // the speech comes from the audio input
    if (r.analogVoice) return true;
    if (id == "qo100" && r.variant == 1) return false;
    if (id == "fm" || id == "airband" || id == "qo100") return true;
    if (id == "atv") return r.audioSrc == 0 || r.audioSrc == 1;
    if ((id == "m17" || id == "freedv") && txCanEncodeVoice(m)) return true;
    return false;
}

bool txClaimRadio(const DeviceInfo& d, std::string& err) {
    txReleaseRadio();
    if (d.txFullDuplex) return true;   // receiving and transmitting on the same radio is allowed: no claim
    int holder = 0;
    if (claimDevice(d, kTxClaimOwner, &holder)) return true;
    err = d.name + " is in use by " + deviceHolderText(holder) + ", and it cannot receive and transmit at the same time. Choose another radio for transmitting, or stop " +
          deviceHolderText(holder) + " first.";
    return false;
}
void txReleaseRadio() { releaseDevice(kTxClaimOwner); }

// ---------------------------------------------------------------- generators

namespace {

// a stream of samples built up block by block from a frame generator
class FrameGen : public TxGenerator {
public:
    FrameGen(double rate, std::function<void(std::vector<cf32>&)> next, float gain) : rate_(rate), next_(std::move(next)), gain_(gain) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        while (pending_.size() - pos_ < n) {
            frame_.clear();
            next_(frame_);
            pending_.insert(pending_.end(), frame_.begin(), frame_.end());
        }
        for (size_t i = 0; i < n; i++) out[i] = pending_[pos_ + i] * gain_;
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    double rate_;
    std::function<void(std::vector<cf32>&)> next_;
    float gain_;
    std::vector<cf32> pending_, frame_;
    size_t pos_ = 0;
};

class AtscGen : public TxGenerator {
public:
    AtscGen(double rate, std::function<void(uint8_t*)> ts) : rate_(rate) {
        atsc::ChannelConfig cc;   // snrDb 99: no noise worth the name
        cc.snrDb = 200;
        g_ = std::make_unique<atsc::Generator>(ts ? std::move(ts) : demoTsSource(19.39e6, DemoTsIdentity()), cc, rate);
    }
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override {
        tmp_.clear();
        g_->generate(n, tmp_);
        const float g = 0.22f / 6.7f;   // the 8-VSB signal has an rms of about 6.7 units
        for (size_t i = 0; i < n; i++) out[i] = tmp_[i] * g;
    }
private:
    double rate_;
    std::unique_ptr<atsc::Generator> g_;
    std::vector<cf32> tmp_;
};

// ModeSynth modes are already at the rate asked for
class SynthGen : public TxGenerator {
public:
    SynthGen(std::unique_ptr<ModeSynth> s, double rate) : s_(std::move(s)), rate_(rate) {}
    double sampleRate() const override { return rate_; }
    void generate(cf32* out, size_t n) override { s_->generate(out, n); }
private:
    std::unique_ptr<ModeSynth> s_;
    double rate_;
};

// a generator at its own rate, played at another one
class ResampledGen : public TxGenerator {
public:
    ResampledGen(std::unique_ptr<TxGenerator> in, double outRate) : in_(std::move(in)), out_(outRate) {}
    bool configure() { return rs_.configure(in_->sampleRate(), out_); }
    double sampleRate() const override { return out_; }
    void generate(cf32* out, size_t n) override {
        if (in_buf_.empty()) in_buf_.resize(1 << 14);
        while (pending_.size() - pos_ < n) {
            in_->generate(in_buf_.data(), in_buf_.size());
            rs_.process(in_buf_.data(), in_buf_.size(), pending_);
        }
        std::copy(pending_.begin() + (long)pos_, pending_.begin() + (long)(pos_ + n), out);
        pos_ += n;
        if (pos_ > (1u << 20)) { pending_.erase(pending_.begin(), pending_.begin() + (long)pos_); pos_ = 0; }
    }
private:
    std::unique_ptr<TxGenerator> in_;
    double out_;
    ExactResampler rs_;
    std::vector<cf32> pending_, in_buf_;
    size_t pos_ = 0;
};

// Switches between the payload and an unmodulated carrier (amplitude 1.0). The change is a 512-sample raised-cosine cross-fade, not a step, so that
// pressing "Carrier tune" does not splatter. While the payload is on (the normal case) the samples pass through untouched.
class CarrierGate : public TxGenerator {
public:
    CarrierGate(std::unique_ptr<TxGenerator> inner, std::atomic<bool>* flag, double rate)
        : inner_(std::move(inner)), flag_(flag), rate_(rate) {}
    double sampleRate() const override { return inner_ ? inner_->sampleRate() : rate_; }
    bool finished() const override { return inner_ && inner_->finished() && !carrier_; }   // a finite transmission ends the session; a carrier tune keeps it
    void generate(cf32* out, size_t n) override {
        const bool want = flag_ && flag_->load(std::memory_order_relaxed);
        if (want == carrier_ && fade_ >= kFade) {
            if (carrier_) { for (size_t i = 0; i < n; i++) out[i] = cf32(1.f, 0.f); return; }
            if (inner_) inner_->generate(out, n);
            else for (size_t i = 0; i < n; i++) out[i] = cf32(1.f, 0.f);
            return;
        }
        if (want != carrier_) { carrier_ = want; fade_ = 0; }
        if (inner_) inner_->generate(out, n);   // the payload keeps running while it is faded out, so it resumes where it would be
        else for (size_t i = 0; i < n; i++) out[i] = cf32(1.f, 0.f);
        for (size_t i = 0; i < n; i++) {
            float w = 1.f;   // weight of the carrier
            if (fade_ < kFade) { const float t = (float)fade_ / (float)kFade; w = 0.5f - 0.5f * std::cos(t * 3.14159265f); fade_++; }
            const float wc = carrier_ ? w : 1.f - w;
            out[i] = out[i] * (1.f - wc) + cf32(wc, 0.f);
        }
    }
private:
    static constexpr int kFade = 512;
    std::unique_ptr<TxGenerator> inner_;
    std::atomic<bool>* flag_;
    double rate_;
    bool carrier_ = false;
    int fade_ = kFade;
};

std::function<void(uint8_t*)> loopingTsFile(const std::string& path, int stride = 188, int syncOff = 0) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    auto hold = std::shared_ptr<FILE>(f, fclose);
    return [hold, stride, syncOff](uint8_t* pkt) {
        std::vector<uint8_t> raw((size_t)stride);
        for (;;) {
            if (fread(raw.data(), 1, (size_t)stride, hold.get()) == (size_t)stride) {
                if (raw[(size_t)syncOff] == 0x47) { memcpy(pkt, raw.data() + syncOff, 188); return; }
                fseek(hold.get(), -(stride - 1), SEEK_CUR);
                continue;
            }
            fseek(hold.get(), 0, SEEK_SET);
            if (fread(raw.data(), 1, (size_t)stride, hold.get()) != (size_t)stride) { memset(pkt, 0, 188); pkt[0] = 0x47; return; }
            if (raw[(size_t)syncOff] == 0x47) { memcpy(pkt, raw.data() + syncOff, 188); return; }
        }
    };
}

bool looksLikeMpegTs(const std::string& path, int& stride, int& syncOff) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t b[192 * 4] = {};
    const size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    auto hits = [&](int st, int off) {
        int ok = 0, need = 0;
        for (size_t i = (size_t)off; i + 188 <= n; i += (size_t)st) { need++; if (b[i] == 0x47) ok++; }
        return need >= 3 && ok == need;
    };
    if (hits(188, 0)) { stride = 188; syncOff = 0; return true; }
    if (hits(192, 4)) { stride = 192; syncOff = 4; return true; }
    return false;
}

} // namespace

static bool remuxToMpegTs(const std::string& in, const std::string& out, std::string& err) {
    AVFormatContext* ic = nullptr;
    if (avformat_open_input(&ic, in.c_str(), nullptr, nullptr) < 0) { err = "could not open the file"; return false; }
    if (avformat_find_stream_info(ic, nullptr) < 0) { avformat_close_input(&ic); err = "could not read the file"; return false; }
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mpegts", out.c_str()) < 0 || !oc) {
        avformat_close_input(&ic); err = "MPEG-TS muxer is not in this FFmpeg build"; return false;
    }
    std::vector<int> map(ic->nb_streams, -1);
    int nOut = 0;
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        AVStream* is = ic->streams[i];
        const AVMediaType t = is->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) continue;
        AVStream* os = avformat_new_stream(oc, nullptr);
        if (!os || avcodec_parameters_copy(os->codecpar, is->codecpar) < 0) continue;
        os->codecpar->codec_tag = 0;
        map[i] = nOut++;
    }
    if (nOut == 0) {
        avformat_free_context(oc); avformat_close_input(&ic);
        err = "the file has no video or audio stream to remux"; return false;
    }
    if (!(oc->oformat->flags & AVFMT_NOFILE) && avio_open(&oc->pb, out.c_str(), AVIO_FLAG_WRITE) < 0) {
        avformat_free_context(oc); avformat_close_input(&ic);
        err = "could not write the remuxed transport stream"; return false;
    }
    if (avformat_write_header(oc, nullptr) < 0) {
        avio_closep(&oc->pb); avformat_free_context(oc); avformat_close_input(&ic);
        err = "could not write an MPEG-TS header (the codecs may need a full encode)"; return false;
    }
    AVPacket* pkt = av_packet_alloc();
    int rc = 0;
    while ((rc = av_read_frame(ic, pkt)) >= 0) {
        const int oi = pkt->stream_index >= 0 && pkt->stream_index < (int)map.size() ? map[(size_t)pkt->stream_index] : -1;
        if (oi < 0) { av_packet_unref(pkt); continue; }
        AVStream* is = ic->streams[pkt->stream_index];
        AVStream* os = oc->streams[oi];
        pkt->stream_index = oi;
        av_packet_rescale_ts(pkt, is->time_base, os->time_base);
        pkt->pos = -1;
        if (av_interleaved_write_frame(oc, pkt) < 0) { av_packet_unref(pkt); rc = -1; break; }
        av_packet_unref(pkt);
    }
    av_write_trailer(oc);
    av_packet_free(&pkt);
    avio_closep(&oc->pb);
    avformat_free_context(oc);
    avformat_close_input(&ic);
    if (rc < 0 && rc != AVERROR_EOF) { err = "remux stopped before the end of the file"; return false; }
    return true;
}

bool txOpenMpegTs(const std::string& path, std::function<void(uint8_t*)>& outTs, std::string& note) {
    outTs = {};
    if (path.empty()) { note = "empty: the built-in demo programme"; return false; }
    int stride = 188, syncOff = 0;
    if (looksLikeMpegTs(path, stride, syncOff)) {
        outTs = loopingTsFile(path, stride, syncOff);
        note = "this file is used as a transport stream";
        return (bool)outTs;
    }
    char tmp[512];
#ifdef _WIN32
    snprintf(tmp, sizeof tmp, "%s\\onair_tx_remux.ts", getenv("TEMP") ? getenv("TEMP") : ".");
#else
    snprintf(tmp, sizeof tmp, "/tmp/onair_tx_remux_%d.ts", (int)getpid());
#endif
    std::string err;
    if (!remuxToMpegTs(path, tmp, err)) { note = err; return false; }
    outTs = loopingTsFile(tmp, 188, 0);
    note = "remuxed to MPEG-TS for the transmitter";
    return (bool)outTs;
}

void txSpectrumDb(const cf32* x, size_t n, int nfft, std::vector<float>& db) {
    db.assign((size_t)nfft, -120.f);
    if (!x || n == 0 || nfft < 8) return;
    int log2n = 0;
    while ((1 << log2n) < nfft) log2n++;
    nfft = 1 << log2n;
    db.assign((size_t)nfft, -120.f);
    std::vector<float> re((size_t)nfft), im((size_t)nfft);
    const size_t use = std::min(n, (size_t)nfft);
    for (int i = 0; i < nfft; i++) {
        const float w = i < (int)use ? (float)(0.5 - 0.5 * std::cos(2 * dect2::math::kPi * i / (double)use)) : 0.f;
        const cf32 s = i < (int)use ? x[i] : cf32(0.f, 0.f);
        re[(size_t)i] = s.real() * w;
        im[(size_t)i] = s.imag() * w;
    }
    fftSplit(re.data(), im.data(), log2n, false);
    float peak = 1e-20f;
    for (int i = 0; i < nfft; i++) {
        const float p = re[(size_t)i] * re[(size_t)i] + im[(size_t)i] * im[(size_t)i];
        peak = std::max(peak, p);
        db[(size_t)i] = 10.f * std::log10(std::max(p, 1e-20f) / (float)nfft);
    }
    (void)peak;
}

static SynthConfig cleanSynth(const SynthConfig& c, int stdMode) {
    SynthConfig s = c;
    s.snrDb = 200;            // no noise, no carrier or clock offset, no echo: a transmitter makes the clean signal
    s.cfoHz = 0;
    s.sroPpm = 0; s.echoDb = 0;
    s.gainModel = false;
    s.pace = 1.0;
    s.mode = stdMode;
    return s;
}

double txModeTuneOffsetHz(const TxModeSpec& m) {
    switch (m.stdMode) {   // their own generators: the frequency plan is theirs (KAAL, KAVB, KAPP, KAFT, KAWS, KAIT, NFC, Flipper Sub-GHz)
    case 124: case 125: case 126: case 127: case 135: case 133: case 150: case 206: return 0;
    default: break;
    }
    const ModeTuning* mt = modeTuning(m.stdMode);
    return mt ? mt->tuneOffsetHz : 0;
}

std::unique_ptr<TxGenerator> makeTxGenerator(const TxModeSpec& m, const TxRequest& r, double rateHz, std::string& err) {
    if (m.stdMode == 125) return makeKavbTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // KAVB: its own video-container transmitter at the radio's rate
    if (m.stdMode == 126) return makeKappTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // KAPP: its own burst transmitter at the radio's rate
    if (m.stdMode == 127) return makeKaftTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // KAFT: its own file transmitter at the radio's rate
    if (m.stdMode == 124) return makeKaalTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // KAAL: its own transmitter chain at the radio's rate
    if (m.stdMode == 135) return makeKawsTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // KAWS: HF chat / beacon bursts as upper-sideband IQ at the radio's rate
    if (m.stdMode == 206) return makeEsubghzTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // Flipper Zero Sub-GHz chat: one message, then it ends
    if (m.stdMode == 133) return makeKaitTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // KAIT: a picture burst at the radio's rate
    if (m.stdMode == 150) return makeNfcReaderTxGenerator(r, rateHz > 0 ? rateHz : m.nominalRateHz, err);   // NFC active reader: 100% ASK Miller on a continuous 13.56 MHz carrier (Phase 2, gated)
    const SynthConfig s = cleanSynth(r.synth, m.stdMode);
    std::unique_ptr<TxGenerator> g;
    double native = 0;
    std::function<void(uint8_t*)> fileTs;
    std::string fileNote;
    const bool wantTs = m.stdMode == 0 || m.stdMode == 3 || m.stdMode == 6 || m.stdMode == 8 || m.stdMode == 9 || m.stdMode == 42;
    if (wantTs && !r.file.empty()) {
        if (!txOpenMpegTs(r.file, fileTs, fileNote)) {
            if (fileNote.empty()) fileNote = "could not use this file as a transport stream; the demo programme is sent";
        }
    } else if (wantTs) fileNote = r.services.empty() && r.name.empty() ? "empty: the built-in demo programme" : "built-in test card with your service names";
    const_cast<TxRequest&>(r).fileNote = fileNote;
    auto ident = [](const TxRequest& r) {
        DemoTsIdentity id;
        if (!r.provider.empty()) id.provider = r.provider;
        if (!r.network.empty()) id.network = r.network;
        id.serviceId = r.serviceId > 0 ? r.serviceId : 1;
        id.programmes = r.mux;
        id.services = r.services;
        if (id.services.empty() && !r.name.empty()) id.services.push_back(r.name);
        return id;
    };
    auto namedTs = [&](std::function<void(uint8_t*)> file, double br) {
        if (file) return file;
        return demoTsSource(br > 1e6 ? br : 20e6, ident(r));
    };
    if (m.stdMode == 0) {
        const double bw = r.bandwidthMhz > 0 ? r.bandwidthMhz : 8;
        native = nativeRateHz(bw);
        if (m.variant == 2) {
            dvbt::Params tp;
            tp.mode = s.dvbtMode; tp.guard = s.dvbtGuard; tp.mod = s.dvbtMod; tp.crHp = tp.crLp = s.dvbtRate;
            auto gen = std::make_shared<dvbt::Generator>(tp, namedTs(fileTs, dvbt::netBitrate(tp, native)));
            g = std::make_unique<FrameGen>(native, [gen](std::vector<cf32>& f) { gen->nextSymbol(f); }, 0.25f);
        } else {
            TxParams tp = s.tx;
            tp.payload = true;
            tp.tsSource = namedTs(fileTs, 24e6);
            auto gen = std::make_shared<T2Generator>(tp);
            g = std::make_unique<FrameGen>(native, [gen](std::vector<cf32>& f) { gen->nextFrame(f); }, 0.25f);
        }
    } else if (m.stdMode == 3) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        if (rr < 6.5e6) { err = "ATSC needs at least 6.5 Msps"; return nullptr; }
        g = std::make_unique<AtscGen>(rr, namedTs(fileTs, 19.39e6));
        native = rr;
    } else if (m.stdMode == 8) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        auto ms = makeDvbsSynth(s, rr, namedTs(fileTs, dvbsNetBitrate(dvbs::dvbsConfigFromSynth(s, rr).tx)));
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 9) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        auto ms = makeDtmbSynth(s, rr, namedTs(fileTs, 20e6));
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 42) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        auto ms = makeDvbcSynth(s, rr, namedTs(fileTs, 20e6));
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 55 && r.variant == 1) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        std::function<void(uint8_t*)> ts;
        std::string note;
        if (!r.file.empty()) txOpenMpegTs(r.file, ts, note);
        const_cast<TxRequest&>(r).fileNote = note;
        auto ms = makeDvbsSynth(s, rr, namedTs(ts, dvbsNetBitrate(dvbs::dvbsConfigFromSynth(s, rr).tx)));
        if (!ms) ms = makeModeSynth(m.stdMode, s, rr);
        if (!ms) { err = "QO-100 DATV could not start"; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        native = rr;
    } else if (m.stdMode == 6) {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        isdbt::Params p;
        p.mode = 3; p.guard = isdbt::kGi8; p.partial = true;
        p.layer[0].segments = 1; p.layer[0].mod = isdbt::kQpsk; p.layer[0].rate = isdbt::kR23; p.layer[0].ti = 2;
        p.layer[1].segments = 12; p.layer[1].mod = isdbt::k64Qam; p.layer[1].rate = isdbt::kR34; p.layer[1].ti = 1;
        p.bwMhz = s.isdbtBwMhz == 7 || s.isdbtBwMhz == 8 ? s.isdbtBwMhz : 6;
        auto src = isdbt::singleLayerSource(1, namedTs(fileTs, isdbt::layerBitrate(p, 1) * 0.9));
        auto gen = std::make_shared<isdbt::Generator>(p, src, 1);
        const double nativeIsdb = isdbt::sampleRateFor(p.bwMhz);
        g = std::make_unique<FrameGen>(nativeIsdb, [gen](std::vector<cf32>& f) { gen->nextFrame(f); }, 0.22f);
        native = nativeIsdb;
        if (rr > 0 && std::fabs(rr - native) > 0.5) {
            auto rg = std::make_unique<ResampledGen>(std::move(g), rr);
            if (!rg->configure()) { char b[160]; snprintf(b, sizeof b, "%s cannot be resampled from %.3f to %.3f Msps", m.name, native / 1e6, rr / 1e6); err = b; return nullptr; }
            return rg;
        }
        return g;
    } else {
        const double rr = rateHz > 0 ? rateHz : m.nominalRateHz;
        if (m.minRateHz > 0 && rr < m.minRateHz) { char b[160]; snprintf(b, sizeof b, "%s needs at least %.3g Msps; the radio runs %.3g Msps", m.name, m.minRateHz / 1e6, rr / 1e6); err = b; return nullptr; }
        auto ms = makeTxPayloadSynth(m, r, s, rr);
        if (!ms && m.stdMode == 27 && txPayloadKind(m.id) == TxPayloadKind::Ftx && !txFtxMessage(r).empty()) {
            err = std::string("the message \"") + txFtxMessage(r) + "\" cannot be sent as " + m.name +
                  ": a standard callsign is a prefix, one digit and at most three letters (N0ABC, LY2XYZ), a grid has four characters (KO24), free text is at most 13 characters";
            return nullptr;
        }
        if (!ms && m.stdMode == 27) {   // the HF digital family's demo: the mode chosen (its variant), not whatever modeOpt[0] another mode left (FT8 sent RTTY)
            SynthConfig hs = s;
            hs.modeOpt[0] = m.variant;
            if (auto audio = makeHfdigTestAudio(m.variant, hs))   // a band of several stations: at most 0.5, so that their sum stays under the limiter's knee
                ms = makeHfdigSynthFromAudio(std::move(audio), hs, rr, false, 0.5f);
            if (!ms) ms = makeModeSynth(m.stdMode, hs, rr);
        }
        if (!ms) ms = makeModeSynth(m.stdMode, s, rr);
        if (!ms) { char b[200]; snprintf(b, sizeof b, "the %s generator cannot make a signal at %.3f Msps with these options", m.name, rr / 1e6); err = b; return nullptr; }
        g = std::make_unique<SynthGen>(std::move(ms), rr);
        if (const double ramp = txBurstRampSec(m.id); ramp > 0) g = std::make_unique<TxBurstShaper>(std::move(g), ramp);   // hard-keyed bursts get ramps (tx_shape.h)
        native = rr;
    }
    if (rateHz > 0 && std::fabs(rateHz - native) > 0.5) {
        auto rg = std::make_unique<ResampledGen>(std::move(g), rateHz);
        if (!rg->configure()) { char b[160]; snprintf(b, sizeof b, "%s cannot be resampled from %.3f to %.3f Msps", m.name, native / 1e6, rateHz / 1e6); err = b; return nullptr; }
        return rg;
    }
    return g;
}

double txRequestRate(const TxModeSpec& m, const TxRequest& r, const TxRateLimits& lim) {
    if (r.tune.sampleRate != 0) return r.tune.sampleRate;
    double rate = m.stdMode == 0 ? nativeRateHz(r.bandwidthMhz > 0 ? r.bandwidthMhz : 8) : m.nominalRateHz;
    if (m.stdMode == 6) rate = std::max(rate, (r.synth.isdbtBwMhz >= 7 ? r.synth.isdbtBwMhz : 6) * 1e6 * 1.34);
    if (lim.minHz > 0 && rate < lim.minHz) rate = lim.minHz;
    if (lim.maxHz > 0 && rate > lim.maxHz) rate = lim.maxHz;
    return rate;
}

// ---------------------------------------------------------------- the session


// ---------------------------------------------------------------- level helpers

float txSoftLimit(cf32* x, size_t n, size_t* limited) {
    constexpr float kKnee = 0.9f, kRoom = 1.f - kKnee;
    float peak2 = 0;
    size_t lim = 0;
    for (size_t i = 0; i < n; i++) {
        const float re = x[i].real(), im = x[i].imag();
        if (!std::isfinite(re) || !std::isfinite(im)) { x[i] = cf32(0.f, 0.f); continue; }   // a NaN would reach the DAC as an undefined byte
        const float m2 = re * re + im * im;
        if (m2 > peak2) peak2 = m2;
        if (m2 <= kKnee * kKnee) continue;
        const float m = std::sqrt(m2);
        const float k = (kKnee + kRoom * std::tanh((m - kKnee) / kRoom)) / m;
        x[i] = cf32(re * k, im * k);
        lim++;
    }
    if (limited) *limited = lim;
    return std::sqrt(peak2);
}

TxLevel txMeasure(const cf32* x, size_t n) {
    TxLevel l;
    if (!x || !n) return l;
    double acc = 0;
    float pk = 0;
    for (size_t i = 0; i < n; i++) {
        const float m2 = x[i].real() * x[i].real() + x[i].imag() * x[i].imag();
        acc += m2;
        if (m2 > pk) pk = m2;
    }
    l.peak = std::sqrt(pk);
    l.rms = (float)std::sqrt(acc / (double)n);
    return l;
}

double txOccupiedBandwidthHz(const std::vector<float>& db, double rateHz) {
    const size_t n = db.size();
    if (n < 8 || rateHz <= 0) return 0;
    std::vector<double> p(n);
    double tot = 0;
    for (size_t i = 0; i < n; i++) { p[i] = std::pow(10.0, db[i] / 10.0); tot += p[i]; }
    if (tot <= 0) return 0;
    // 99%: drop 0.5% of the power from each edge of the span (DC at bin 0: bins n/2.. are negative frequencies)
    std::vector<double> q(n);   // in frequency order, lowest first
    for (size_t i = 0; i < n; i++) q[i] = p[(i + n / 2) % n];
    double lo = 0, hi = 0;
    size_t a = 0, b = n - 1;
    while (a < b && lo + q[a] <= tot * 0.005) lo += q[a++];
    while (b > a && hi + q[b] <= tot * 0.005) hi += q[b--];
    return (double)(b - a + 1) / (double)n * rateHz;
}

std::string txRangeInfo(const DeviceInfo& d, double hz) {
    if (d.maxFreqHz <= d.minFreqHz || hz <= 0) return "";
    if (hz >= d.minFreqHz && hz <= d.maxFreqHz) return "";
    char b[256];
    snprintf(b, sizeof b, "%s is specified for %.3f to %.3f MHz; %.6f MHz is outside that. It is sent anyway: the radio decides what comes out.", d.name.c_str(), d.minFreqHz / 1e6, d.maxFreqHz / 1e6, hz / 1e6);
    return b;
}

std::string txRadioLabel(const DeviceInfo& d) {
    std::string n = !d.txName.empty() ? d.txName : d.name;
    for (const char* suf : {" (native, experimental)", " (native)"}) {
        const std::string t = suf;
        if (n.size() > t.size() && n.compare(n.size() - t.size(), t.size(), suf) == 0) n.erase(n.size() - t.size());
    }
    return n;
}

std::string txUnavailableReason(const DeviceInfo& d) {
    if (d.canTransmit) return "";
    const std::string n = txRadioLabel(d);
    switch (d.kind) {
    case DeviceInfo::Synthetic: return "the test signal is not a radio";
    case DeviceInfo::File: return "a recording is not a radio";
    case DeviceInfo::Audio: return "a sound card is not a radio";
    case DeviceInfo::Hamlib: return n + " is controlled through CAT only: OnAir has no transmit path for it";
    default: break;
    }
    const std::string& b = d.board;
    if (b == "rtlsdr" || b == "rtltcp") return "RTL-SDR dongles can only receive";
    if (b == "airspy" || b == "airspyhf") return n + " cannot transmit: Airspy radios are receive-only";
    if (b == "sdrplay") return n + " cannot transmit: SDRplay radios are receive-only";
    if (b == "usrp") return n + " cannot transmit in this build: the installed UHD has no transmit functions (UHD 3.15 or newer is needed)";
    if (b == "bladerf" || b == "lime") return n + " cannot transmit through its native driver here: it is receive-only in OnAir; install its SoapySDR module to transmit";
    if (d.kind == DeviceInfo::HackRF) return n + " cannot transmit in this build: no transmit path for this driver";
    return n + " cannot transmit in this build: it is receive-only in OnAir (no transmit channel was found)";
}

std::vector<int> txRadioChoices(const std::vector<DeviceInfo>& devs) {
    std::vector<int> v;
    std::vector<std::string> seen;
    for (int i = 0; i < (int)devs.size(); i++) {
        const DeviceInfo& d = devs[(size_t)i];
        if (!d.isRadio() || !d.canTransmit) continue;
        if (!d.txName.empty()) {   // several receive entries of one device: listed once
            const std::string key = d.txName + "|" + d.nativeArgs.substr(0, d.nativeArgs.find('#'));
            if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
            seen.push_back(key);
        }
        v.push_back(i);
    }
    return v;
}

std::vector<int> txReceiveOnlyRadios(const std::vector<DeviceInfo>& devs) {
    std::vector<int> v;
    std::vector<std::string> seen;
    for (int i = 0; i < (int)devs.size(); i++) {
        const DeviceInfo& d = devs[(size_t)i];
        if (!d.isRadio() || d.canTransmit) continue;
        const std::string key = txRadioLabel(d);   // the RX inputs of one radio are one radio
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        v.push_back(i);
    }
    return v;
}

namespace {
// takes samples at the pace of the rate it was opened with and sends them nowhere
class NullTxSink : public TxSink {
public:
    bool open(const TxTune& t, std::string&) override { rate_ = t.sampleRate > 0 ? t.sampleRate : 1e6; next_ = std::chrono::steady_clock::now(); open_ = true; return true; }
    bool write(const cf32*, size_t n, std::string&) override {
        const auto now = std::chrono::steady_clock::now();
        if (next_ < now - std::chrono::milliseconds(200)) next_ = now;   // do not "catch up" after a stall
        next_ += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>((double)n / rate_));
        std::this_thread::sleep_until(next_);
        return true;
    }
    bool retune(const TxTune&, std::string&) override { return true; }
    void close() override { open_ = false; }
    double sampleRate() const override { return rate_; }
    std::string name() const override { return "test (no RF)"; }
private:
    double rate_ = 1e6;
    bool open_ = false;
    std::chrono::steady_clock::time_point next_;
};
}
std::unique_ptr<TxSink> makeNullTxSink() { return std::make_unique<NullTxSink>(); }

// ---------------------------------------------------------------- the session

namespace {
constexpr size_t kBlock = 1 << 15;
std::string mhzText(double hz) { char b[48]; snprintf(b, sizeof b, "%.6f MHz", hz / 1e6); return b; }
std::string manualRateProblem(const TxModeSpec& m, const TxRequest& r, const TxRateLimits& lim) {
    const double rate = r.tune.sampleRate;
    if (rate == 0) return {};
    if (!std::isfinite(rate) || rate <= 0) return "TX sample rate must be a finite positive number, or 0 for automatic";
    char b[256];
    if ((lim.minHz > 0 && rate < lim.minHz) || (lim.maxHz > 0 && rate > lim.maxHz)) {
        snprintf(b, sizeof b, "requested TX rate %.6g Msps is outside the radio's range (minimum %.6g, maximum %.6g Msps; 0 means unknown)", rate / 1e6, lim.minHz / 1e6, lim.maxHz / 1e6);
        return b;
    }
    double minimum = m.minRateHz;
    if (m.stdMode == 0) minimum = std::max(minimum, (r.bandwidthMhz > 0 ? r.bandwidthMhz : 8) * 1e6);
    if (m.stdMode == 6) minimum = std::max(minimum, (r.synth.isdbtBwMhz >= 7 ? r.synth.isdbtBwMhz : 6) * 1e6);
    if (!r.carrierTune && rate < minimum) {
        snprintf(b, sizeof b, "%s needs at least %.6g Msps; requested TX rate is %.6g Msps", m.name, minimum / 1e6, rate / 1e6);
        return b;
    }
    return {};
}
}

void TxSession::note(const std::string& s) {
    std::function<void(const std::string&)> cb;
    std::string line;
    {
        std::lock_guard<std::mutex> lk(mu_);
        const time_t now = time(nullptr);
        char b[32];
        struct tm tmv;
        dect2::localTime(now, &tmv);
        strftime(b, sizeof b, "%H:%M:%S ", &tmv);
        line = std::string(b) + s;
        log_.push_back(line);
        if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + 100);
        cb = cb_;
    }
    if (cb) cb(line);
}

void TxSession::fail(const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (failed_) return;
        error_ = why;
        failed_ = true;
    }
    note("TRANSMISSION STOPPED: " + why);
}

// closes the radio once (the sender thread does it when a pass ends, stop() does it otherwise)
void TxSession::closeSink() {
    if (sink_ && !sinkClosed_.exchange(true)) {
        sink_->close();
        if (rfOn_.exchange(false)) dect2::mapguard::txEnd();   // the map guard holds uploads for 10 minutes from now
    }
}

bool TxSession::start(std::unique_ptr<TxSink> sink, const TxRequest& r, const TxRateLimits& lim, std::string& err) {
    stop();
    std::lock_guard<std::mutex> ctl(ctl_);
    if (!sink) { err = "this radio has no transmit path in OnAir"; return false; }
    const TxModeSpec* mode = txModeById(r.modeId);
    if (!mode) { err = "this mode cannot be transmitted by OnAir"; return false; }
    dry_ = sink->name() == "test (no RF)";
    sink_ = std::move(sink);
    req_ = r;
    carrierTune_ = r.carrierTune;
    audio_.reset();
    video_.reset();
    stop_ = false; genDone_ = false; failed_ = false; finished_ = false; sent_ = 0; under_ = 0; limited_ = 0; madeTotal_ = 0; error_.clear();
    senderDone_ = false; sinkClosed_ = false; peak_ = 0; rms_ = 0; audioPeak_ = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.clear(); pool_.clear();
        tap_.clear(); tapPos_ = 0; tapHave_ = 0;
        log_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(rtMu_);
        rtPending_ = false; rtDone_ = false;
    }
    const std::string rateProblem = manualRateProblem(*mode, r, lim);
    if (!rateProblem.empty()) { err = rateProblem; note("could not start: " + err); sink_.reset(); return false; }
    TxTune t = r.tune;
    t.sampleRate = txRequestRate(*mode, r, lim);
    if (sink_->takesIq()) t.offsetHz += txModeTuneOffsetHz(*mode);   // the signal sits tuneOffsetHz below the radio's centre: the dial frequency (#351)
    if (!sink_->open(t, err)) { note("could not start: " + err); sink_.reset(); return false; }
    rate_ = sink_->sampleRate() > 0 ? sink_->sampleRate() : t.sampleRate;
    iq_.reset();
    if (const TxIqStreamConfig ic = txIqStreamConfig(); ic.enabled) {   // the IQ on TCP (also in a dry run); a problem here is a warning, never a refusal
        auto st = std::make_unique<TxIqStream>();
        std::string ie;
        if (st->start(ic, rate_, r.tune.centerHz, r.modeId, ie)) { iq_ = std::move(st); note("IQ stream on TCP port " + std::to_string(iq_->port()) + " (" + txIqFormatName(ic.format) + ")" + (ie.empty() ? "" : "; " + ie)); }
        else note("warning: the IQ stream on TCP port " + std::to_string(ic.port) + " could not start: " + ie);
    }
    // Allow small hardware rounding, but never silently replace a manual Fs with
    // a different clock (for example the faster rate of an already-running RX).
    if (r.tune.sampleRate != 0 && std::fabs(rate_ - t.sampleRate) > std::max(1.0, t.sampleRate * 1e-4)) {
        char b[256];
        snprintf(b, sizeof b, "the radio runs at %.6g Msps, not the requested TX rate %.6g Msps; select a supported rate (if RX shares this radio, use its rate)", rate_ / 1e6, t.sampleRate / 1e6);
        err = b; note("could not start: " + err); sink_->close(); sink_.reset(); return false;
    }
    if (txModeNeedsAudio(*mode, req_) && !req_.carrierTune) {
        audio_ = std::make_unique<AudioIn>();
        AudioIn::Kind k = AudioIn::Tone;
        if (req_.audioSrc == 0) k = AudioIn::Mic;
        else if (req_.audioSrc == 1) k = AudioIn::File;
        else if (req_.audioSrc == 3) k = AudioIn::Silence;
        std::string aerr;
        const std::string arg = k == AudioIn::Mic ? req_.audioDevice : req_.file;
        if (k == AudioIn::File && arg.empty()) aerr = "no audio file was chosen";
        if (!aerr.empty() || !audio_->start(k, 48000, arg, req_.toneHz > 0 ? req_.toneHz : 1000, aerr)) {
            if (k == AudioIn::Mic) { err = aerr; note("could not start: " + err); closeSink(); sink_.reset(); audio_.reset(); return false; }
            if (k == AudioIn::File) note("audio file not used: " + aerr + "; a 1 kHz tone is sent instead");
            audio_->start(AudioIn::Tone, 48000, {}, req_.toneHz > 0 ? req_.toneHz : 1000, aerr);
        }
        audio_->setMuted(req_.audioMute);
        audio_->setGain(req_.micGain > 0 ? req_.micGain : 1.f);
        if (audio_->kind() == AudioIn::File && sink_->name() == "IQ file" && audio_->fileSeconds() > 0) {   // into a file: the audio file is sent once, then the file is closed
            req_.loop = false;
            req_.onceSeconds = audio_->fileSeconds() + 1.5;
            note("IQ file: the audio file is sent once");
        }
        req_.pullAudio = [this](float* st, int n) { return audio_ ? audio_->read(st, n) : 0; };
    }
    if (!req_.carrierTune && mode->stdMode == 10 && req_.videoSrc > 0) {
        video_ = std::make_unique<VideoIn>();
        std::string verr;
        const VideoIn::Kind vk = req_.videoSrc == 2 ? VideoIn::Camera : VideoIn::File;
        const std::string varg = vk == VideoIn::Camera ? req_.videoDevice : req_.file;
        if (!video_->start(vk, varg, verr)) {
            if (vk == VideoIn::Camera) { err = verr; note("could not start: " + err); closeSink(); sink_.reset(); audio_.reset(); video_.reset(); return false; }
            video_.reset();
            note("video file not used: " + verr + "; the test card is sent instead");
        } else {
            req_.pullVideo = [this](int w, int h, float* y, float* u, float* v) { return video_ && video_->copyYuv(w, h, y, u, v); };
        }
    }
    try {
        gen_ = makeTxGenerator(*mode, req_, rate_, err);
    } catch (const std::exception& e) {
        gen_.reset();
        err = std::string("the generator failed to start: ") + e.what();
    }
    if (!gen_ && req_.carrierTune) { err.clear(); gen_.reset(); }
    if (!gen_ && !req_.carrierTune) {
        note("could not start: " + err);
        closeSink(); sink_.reset();
        if (audio_) { audio_->stop(); audio_.reset(); }
        if (video_) { video_->stop(); video_.reset(); }
        return false;
    }
    gen_ = std::make_unique<CarrierGate>(std::move(gen_), &carrierTune_, rate_);
    maxQ_ = std::max<size_t>(8, (size_t)(rate_ * 0.25 / kBlock));
    running_ = true;
    started_ = true;
    if (!dry_ && sink_->name() != "IQ file") {   // a real transmission: the map guard holds the uploads and remembers what was sent (net/map_guard.h)
        rfOn_ = true;
        dect2::mapguard::txBegin();
        for (const auto& id : dect2::mapguard::identitiesOf(req_.callsign, req_.dest, req_.name, req_.address, req_.text)) dect2::mapguard::noteSent(id);
    }
    char b[400];
    snprintf(b, sizeof b, "%s %s on %s, %.3f Msps, gain %.1f dB%s, %s", dry_ ? "TEST (no RF)" : "TRANSMITTING", mode->name, mhzText(r.tune.centerHz).c_str(), rate_ / 1e6, r.tune.gainDb, r.tune.ampOn ? " + amplifier" : "",
             r.loop ? "until stopped" : (std::to_string(r.onceSeconds).substr(0, std::to_string(r.onceSeconds).find('.') + 2) + " s, one pass").c_str());
    note(b);
    if (req_.carrierTune) note("CARRIER TUNE (unmodulated, for adjusting the transmission — not a message)");
    gen_t_ = dect2::namedThread("onair-txgen", [this] { generatorLoop(); });
    send_t_ = dect2::namedThread("onair-txsend", [this] { senderLoop(); });
    return true;
}

// the output stage every mode shares (out.* keys): a trim in dB, a raised-cosine fade-in over the first samples so that the key-up does not
// click (kTxDefaultRampMs when the key is not set), and an I / Q swap that mirrors the spectrum
void txOutputStage(const TxRequest& r, cf32* x, size_t n, uint64_t firstSample, double rateHz) {
    const float trim = (float)std::pow(10.0, r.o("out.trimDb", 0) / 20.0);
    const uint64_t ramp = (uint64_t)std::max(0.0, r.o("out.rampMs", kTxDefaultRampMs) * 1e-3 * rateHz);
    const bool swapIq = r.o("out.swapIq", 0) > 0.5;
    if (trim == 1.f && (!ramp || firstSample >= ramp) && !swapIq) return;
    for (size_t i = 0; i < n; i++) {
        float g = trim;
        if (firstSample + i < ramp) g *= 0.5f - 0.5f * (float)std::cos(math::kPi * (double)(firstSample + i) / (double)ramp);
        const cf32 v = x[i] * g;
        x[i] = swapIq ? cf32(v.imag(), v.real()) : v;
    }
}

void TxSession::generatorLoop() {
    const uint64_t limit = req_.loop ? ~0ull : (uint64_t)(std::max(0.1, req_.onceSeconds) * rate_);
    uint64_t made = 0;
    try {
        while (!stop_ && !senderDone_ && made < limit) {
            const size_t n = (size_t)std::min<uint64_t>(kBlock, limit - made);
            std::vector<cf32> blk;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!pool_.empty()) { blk = std::move(pool_.back()); pool_.pop_back(); }
            }
            blk.resize(n);
            gen_->generate(blk.data(), n);
            txOutputStage(req_, blk.data(), n, made, rate_);
            if (!req_.loop) {   // one pass: the same raised-cosine fade at the end, so that the key-down does not click either
                const uint64_t fade = (uint64_t)(std::max(0.0, req_.o("out.rampMs", kTxDefaultRampMs)) * 1e-3 * rate_);
                for (size_t i = 0; i < n; i++) {
                    const uint64_t left = limit - (made + i);   // samples still to come, this one included (>= 1)
                    if (left <= fade) blk[i] *= 0.5f - 0.5f * (float)std::cos(math::kPi * (double)(left - 1) / (double)fade);
                }
            }
            size_t lim = 0;
            txSoftLimit(blk.data(), n, &lim);   // never above full scale, never NaN, at the last step before the radio
            const TxLevel lv = txMeasure(blk.data(), n);
            peak_ = std::max(lv.peak, peak_.load() * 0.97f);
            rms_ = rms_.load() * 0.7f + lv.rms * 0.3f;
            limited_ += lim;
            madeTotal_ += n;
            if (audio_) audioPeak_ = audio_->peak();
            tapWrite(blk.data(), n);
            made += n;
            std::unique_lock<std::mutex> lk(mu_);
            cvIn_.wait(lk, [&] { return stop_ || senderDone_ || q_.size() < maxQ_; });
            if (stop_ || senderDone_) break;
            q_.push_back(std::move(blk));
            cvOut_.notify_one();
            if (gen_->finished()) break;   // a finite transmission (KAPP, KAFT) has made its last sample
        }
    } catch (const std::exception& e) {
        fail(std::string("the generator stopped: ") + e.what());
    } catch (...) {
        fail("the generator stopped unexpectedly");
    }
    {
        std::lock_guard<std::mutex> lk(mu_);
        genDone_ = true;
    }
    cvOut_.notify_all();
}

void TxSession::senderLoop() {
    const bool rt = sink_->realtime();
    // the radio is written in pieces of about 40 ms so that Stop and a retune are heard quickly, whatever the rate
    const size_t piece = std::max<size_t>(1024, std::min<size_t>(kBlock, (size_t)(rate_ * 0.04)));
    {   // fill the queue before the first sample goes out, so that the start is not an underrun: about 150 ms of signal (at least 4 blocks, at most half the queue)
        const size_t need = std::min<size_t>(maxQ_ / 2 + 1, std::max<size_t>(4, (size_t)std::ceil(rate_ * 0.15 / kBlock)));
        std::unique_lock<std::mutex> lk(mu_);
        cvOut_.wait_for(lk, std::chrono::seconds(5), [&] { return stop_ || genDone_ || q_.size() >= need; });
    }
    uint64_t lastReported = 0;
    auto lastReport = std::chrono::steady_clock::now();
    bool ok = true;
    auto applyRetune = [&] {
        TxTune t;
        {
            std::lock_guard<std::mutex> lk(rtMu_);
            if (!rtPending_) return;
            t = rtTune_;
        }
        std::string e;
        const bool r = sink_->retune(t, e);
        {
            std::lock_guard<std::mutex> lk(rtMu_);
            rtPending_ = false; rtDone_ = true; rtOk_ = r; rtErr_ = e;
        }
        rtCv_.notify_all();
    };
    cf32 last(0.f, 0.f);   // the last sample the radio was given
    while (!stop_ && ok) {
        applyRetune();
        std::vector<cf32> blk;
        bool underrun = false;
        {
            std::unique_lock<std::mutex> lk(mu_);
            for (;;) {
                if (stop_) break;
                if (!q_.empty()) { blk = std::move(q_.front()); q_.pop_front(); cvIn_.notify_one(); break; }
                if (genDone_) break;
                if (rt) { underrun = true; break; }   // the radio runs on: what it gets when nothing is ready is silence, counted
                cvOut_.wait_for(lk, std::chrono::milliseconds(5));
            }
        }
        if (stop_) break;
        if (blk.empty() && !underrun) break;   // the generator is done and the queue is empty: one pass finished
        if (underrun) { blk.assign(kBlock / 8, cf32(0.f, 0.f)); under_++; }
        for (size_t off = 0; off < blk.size() && !stop_; off += piece) {
            const size_t m = std::min(piece, blk.size() - off);
            std::string e;
            if (!sink_->write(blk.data() + off, m, e)) { fail(e); ok = false; break; }
            if (iq_) iq_->push(blk.data() + off, m);
            if (!underrun) sent_ += m;
            last = blk[off + m - 1];
            applyRetune();
        }
        if (!underrun && !blk.empty()) {   // give the block back for the generator to refill (no allocation per block)
            std::lock_guard<std::mutex> lk(mu_);
            if (pool_.size() < 4) pool_.push_back(std::move(blk));
        }
        const auto now = std::chrono::steady_clock::now();
        if (under_ != lastReported && now - lastReport > std::chrono::seconds(2)) {
            note("the generator fell behind: " + std::to_string(under_.load() - lastReported) + " gap(s) of silence sent");
            lastReported = under_;
            lastReport = now;
        }
    }
    if (stop_ && ok && rt && std::norm(last) > 0) {   // Stop on a radio: a 2 ms raised-cosine fade from the last sample, not a step (stop() returns after it)
        std::vector<cf32> tail((size_t)std::max(16.0, std::min(8192.0, rate_ * 0.002)));
        for (size_t i = 0; i < tail.size(); i++) tail[i] = last * (0.5f + 0.5f * (float)std::cos(math::kPi * (double)(i + 1) / (double)tail.size()));
        std::string e;
        sink_->write(tail.data(), tail.size(), e);
    }
    if (!stop_) {   // finished or failed on its own: the transmitter goes off now
        closeSink();
        if (!failed_) {
            char b[160];
            snprintf(b, sizeof b, "transmission finished: %.1f s sent", (double)sent_ / rate_);
            note(b);
            std::lock_guard<std::mutex> lk(mu_);
            finished_ = true;
        }
        running_ = false;
    }
    senderDone_ = true;
    {   // a retune waiting for this thread is answered
        std::lock_guard<std::mutex> lk(rtMu_);
        if (rtPending_) { rtPending_ = false; rtDone_ = true; rtOk_ = false; rtErr_ = "the transmission has ended"; }
    }
    rtCv_.notify_all();
    cvIn_.notify_all();
}

bool TxSession::retune(const TxTune& t, std::string& err) {
    std::lock_guard<std::mutex> ctl(ctl_);
    if (!started_ || !running_ || senderDone_) { err = "nothing is being sent"; return false; }
    std::unique_lock<std::mutex> lk(rtMu_);
    rtTune_ = t; rtTune_.sampleRate = rate_;
    if (const TxModeSpec* md = txModeById(req_.modeId); md && sink_ && sink_->takesIq()) rtTune_.offsetHz += txModeTuneOffsetHz(*md);   // as at start (#351)
    rtPending_ = true; rtDone_ = false;
    if (!rtCv_.wait_for(lk, std::chrono::seconds(3), [&] { return rtDone_; })) { rtPending_ = false; err = "the radio did not answer the change in time"; return false; }
    err = rtErr_;
    if (rtOk_) {
        char b[160];
        snprintf(b, sizeof b, "retuned: %s, gain %.1f dB", mhzText(t.centerHz).c_str(), t.gainDb);
        lk.unlock();
        req_.tune = t;
        note(b);
    }
    return rtOk_;
}

void TxSession::stop() {
    std::lock_guard<std::mutex> ctl(ctl_);
    if (!started_) { sink_.reset(); return; }
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cvIn_.notify_all();
    cvOut_.notify_all();
    rtCv_.notify_all();
    if (gen_t_.joinable()) gen_t_.join();
    if (send_t_.joinable()) send_t_.join();
    const bool wasRunning = running_.exchange(false);
    closeSink();     // no sample goes out after this
    if (wasRunning && !failed_) {
        char b[160];
        snprintf(b, sizeof b, "transmission stopped: %.1f s sent, %llu gap(s) of silence", rate_ > 0 ? (double)sent_ / rate_ : 0.0, (unsigned long long)under_.load());
        note(b);
    }
    sink_.reset();
    iq_.reset();
    gen_.reset();
    if (audio_) { audio_->stop(); audio_.reset(); }
    if (video_) { video_->stop(); video_.reset(); }
    carrierTune_ = false;
    started_ = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        tap_.clear(); tap_.shrink_to_fit(); tapPos_ = 0; tapHave_ = 0;
        q_.clear(); pool_.clear(); pool_.shrink_to_fit();
    }
}

void TxSession::tapWrite(const cf32* x, size_t n) {
    if (!x || !n || rate_ <= 0) return;
    const size_t cap = (size_t)std::max<double>(n, std::min(rate_ * 0.5, 1e6));
    std::lock_guard<std::mutex> lk(mu_);
    if (tap_.size() != cap) { tap_.assign(cap, cf32(0.f, 0.f)); tapPos_ = 0; tapHave_ = 0; }
    if (n >= cap) { x += n - cap; n = cap; }   // only the newest cap samples can matter
    const size_t first = std::min(n, cap - tapPos_);
    std::memcpy(tap_.data() + tapPos_, x, first * sizeof(cf32));
    if (n > first) std::memcpy(tap_.data(), x + first, (n - first) * sizeof(cf32));
    tapPos_ = (tapPos_ + n) % cap;
    tapHave_ = std::min(cap, tapHave_ + n);
}

void TxSession::copyTap(std::vector<cf32>& out, double& rateHz) const {
    std::lock_guard<std::mutex> lk(mu_);
    rateHz = rate_;
    if (!tapHave_ || tap_.empty()) { out.clear(); return; }
    out.resize(tapHave_);
    const size_t cap = tap_.size();
    const size_t start = tapHave_ < cap ? 0 : tapPos_;
    const size_t first = std::min(tapHave_, cap - start);
    std::memcpy(out.data(), tap_.data() + start, first * sizeof(cf32));
    if (tapHave_ > first) std::memcpy(out.data() + first, tap_.data(), (tapHave_ - first) * sizeof(cf32));
}

void TxSession::setCarrierTune(bool on) {
    const bool was = carrierTune_.exchange(on);
    if (on && !was) note("CARRIER TUNE (unmodulated, for adjusting the transmission — not a message)");
    else if (!on && was) note("carrier tune off: payload resumed");
}

TxSession::Stats TxSession::stats() const {
    Stats s;
    s.samples = sent_;
    s.underruns = under_;
    s.rateHz = rate_;
    s.seconds = rate_ > 0 ? (double)sent_ / rate_ : 0;
    s.peak = peak_;
    s.rms = rms_;
    s.audioPeak = audioPeak_;
    const uint64_t made = madeTotal_;
    s.limitedPct = made ? 100.0 * (double)limited_.load() / (double)made : 0;
    std::lock_guard<std::mutex> lk(mu_);
    s.queued = q_.size();
    s.queueMax = maxQ_;
    s.failed = failed_;
    s.finished = finished_;
    s.error = error_;
    s.carrierTune = carrierTune_.load();
    s.fileNote = req_.fileNote;
    return s;
}

std::vector<std::string> TxSession::log() const {
    std::lock_guard<std::mutex> lk(mu_);
    return log_;
}

} // namespace dect2
