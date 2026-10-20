// DVB-T2 receiver front-end (phase 1): P1 detection and S1/S2 decoding, guard-interval detection,
// symbol synchronisation, CFO / timing tracking and raw OFDM cell extraction.
#pragma once
#include "dect2/engine/wide_bank.h"
#include "dect2/channels/tvchan.h"
#include <string>
#include "dect2/common/ring.h"
#include "dect2/atsc/atsc_tel.h"
#include "dect2/dab/dab_tel.h"
#include "dect2/fm/fm_tel.h"
#include "dect2/dvbs/dvbs_tel.h"
#include "dect2/dtmb/dtmb_tel.h"
#include "dect2/atv/atv_tel.h"
#include "dect2/dmr/dmr_tel.h"
#include "dect2/drm/drm_tel.h"
#include "dect2/adsb/adsb_tel.h"
#include "dect2/gnss/gnss_tel.h"
#include "dect2/sonde/sonde_tel.h"
#include "dect2/ais/ais_tel.h"
#include "dect2/marine/marine_tel.h"
#include "dect2/acars/acars_tel.h"
#include "dect2/inmc/inmc_tel.h"
#include "dect2/aero/aero_tel.h"
#include "dect2/iridium/iridium_tel.h"
#include "dect2/mesh/mesh_tel.h"
#include "dect2/hdr/hdr_tel.h"
#include "dect2/cdr/cdr_tel.h"
#include "dect2/pager/pager_tel.h"
#include "dect2/packet/packet_tel.h"
#include "dect2/hfdig/hfdig_tel.h"
#include "dect2/airband/airband_tel.h"
#include "dect2/vdl2/vdl2_tel.h"
#include "dect2/vdl4/vdl4_tel.h"
#include "dect2/ism/ism_tel.h"
#include "dect2/vor/vor_tel.h"
#include "dect2/ndb/ndb_tel.h"
#include "dect2/dme/dme_tel.h"
#include "dect2/m17/m17_tel.h"
#include "dect2/hfdl/hfdl_tel.h"
#include "dect2/cell/cell_tel.h"
#include "dect2/cw/cw_tel.h"
#include "dect2/dstar/dstar_tel.h"
#include "dect2/nxdn/nxdn_tel.h"
#include "dect2/dpmr/dpmr_tel.h"
#include "dect2/tetra/tetra_tel.h"
#include "dect2/tetrapol/tetrapol_tel.h"
#include "dect2/mac/mac_tel.h"
#include "dect2/gsmr/gsmr_tel.h"
#include "dect2/lbj/lbj_tel.h"
#include "dect2/railway/railway_tel.h"
#include "dect2/dgps/dgps_engine.h"
#include "dect2/eloran/eloran_engine.h"
#include "dect2/mpt1327/mpt1327_engine.h"
#include "dect2/fusion/fusion_rx.h"
#include "dect2/ltr/ltr_tel.h"
#include "dect2/edacs/edacs_tel.h"
#include "dect2/milhf/milhf_engine.h"
#include "dect2/dvbc/dvbc_tel.h"
#include "dect2/orbcomm/orbcomm_tel.h"
#include "dect2/uplink/uplink_tel.h"
#include "dect2/ale/ale_tel.h"
#include "dect2/p25/p25_tel.h"
#include "dect2/ble/ble_tel.h"
#include "dect2/iono/iono_tel.h"
#include "dect2/psk/psk_tel.h"
#include "dect2/js8/js8_tel.h"
#include "dect2/specx/specx_tel.h"
#include "dect2/specan/specan_tel.h"
#include "dect2/astro/astro_tel.h"
#include "dect2/qo100/qo100_tel.h"
#include "dect2/wifi/wifi_tel.h"
#include "dect2/general/general_tel.h"
#include "dect2/time/time_tel.h"
#include "dect2/cubesat/cubesat_tel.h"
#include "dect2/globalstar/globalstar_tel.h"
#include "dect2/vdes/vdes_tel.h"
#include "dect2/zigbee/zigbee_tel.h"
#include "dect2/flarm/flarm_tel.h"
#include "dect2/hfbeacon/hfbeacon_tel.h"
#include "dect2/dji/dji_tel.h"
#include "dect2/nbiot/nbiot_tel.h"
#include "dect2/halow/halow_tel.h"
#include "dect2/codar/codar_tel.h"
#include "dect2/wb/wb_tel.h"
#include "dect2/datawell/datawell_tel.h"
#include "dect2/same/same_tel.h"
#include "dect2/ant/ant_tel.h"
#include "dect2/dect/dect_tel.h"
#include "dect2/multitone/multitone_tel.h"
#include "dect2/loraraw/loraraw_tel.h"
#include "dect2/wisun/wisun_tel.h"
#include "dect2/zwave/zwave_tel.h"
#include "dect2/eczas/eczas_tel.h"
#include "dect2/fpv/fpv_tel.h"
#include "dect2/wmbus/wmbus_tel.h"
#include "dect2/tones/tones_tel.h"
#include "dect2/sarsat/sarsat_tel.h"
#include "dect2/mt63/mt63_tel.h"
#include "dect2/fsq/fsq_tel.h"
#include "dect2/ifkp/ifkp_tel.h"
#include "dect2/kaal/kaal_tel.h"
#include "dect2/kait/kait_tel.h"
#include "dect2/kapp/kapp_tel.h"
#include "dect2/kavb/kavb_tel.h"
#include "dect2/kaws/kaws_tel.h"
#include "dect2/kaft/kaft_tel.h"
#include "dect2/cquam/cquam_tel.h"
#include "dect2/modeac/modeac_tel.h"
#include "dect2/mscatter/mscatter_tel.h"
#include "dect2/ardop/ardop_tel.h"
#include "dect2/goesdcs/goesdcs_tel.h"
#include "dect2/bletrk/bletrk_tel.h"
#include "dect2/satbeacon/satbeacon_tel.h"
#include "dect2/rid/rid_tel.h"
#include "dect2/meteor/meteor_tel.h"
#include "dect2/nr/nr_tel.h"
#include "dect2/goes/goes_tel.h"
#include "dect2/elektro/elektro_tel.h"
#include "dect2/uat/uat_tel.h"
#include "dect2/hrpt/hrpt_tel.h"
#include "t2.h"
#include "t2l1.h"
#include "t2plp.h"
#include <functional>
#include <cstdint>
#include <memory>
#include <vector>

namespace dect2 {
std::string t2rxProfile();   // time per stage of the DVB-T2 receiver thread, for the log


struct P1Info {
    bool valid = false;
    int s1 = -1;
    int s2field1 = -1;
    bool mixed = false;
    int fftN = 0;
    double cfoHz = 0;     // from the P1 correlation + comb search
    float conf = 0;       // 0..1 correlation of the decoded S1/S2 sequences
    float metric = 0;     // C-A-B correlation peak, 0..1
    double timingFrac = 0; // sub-sample refinement applied to the position
    uint64_t pos = 0;      // absolute native-rate sample index of P1 start
};

struct RxTelemetry {
    std::vector<WideCarrier> wide;   // whole-span decoding: the carriers table (empty otherwise)
    std::vector<TvChannelRow> tvChannels;   // multi-channel TV (DVB-T / DVB-T2): the channels of the span (empty otherwise)
    AtscTelemetry atsc;          // valid when standard == 2
    DabTelemetry dab;            // valid when standard == 3
    FmTelemetry fm;              // valid when standard == 6
    DvbsTelemetry dvbs;    // valid when standard == 7
    DtmbTelemetry dtmb;    // valid when standard == 8
    AtvTelemetry atv;     // valid when standard == 9
    DmrTelemetry dmr;     // valid when standard == 10
    DrmTelemetry drm;     // valid when standard == 11
    AdsbTelemetry adsb;    // valid when standard == 12
    GnssTelemetry gnss;      // valid when standard == 13
    SondeTelemetry sonde;      // valid when standard == 14
    AisTelemetry ais;      // valid when standard == 15
    MarineTelemetry marine;      // valid when standard == 16
    AcarsTelemetry acars;      // valid when standard == 17
    InmcTelemetry inmc;      // valid when standard == 18
    AeroTelemetry aero;      // valid when standard == 19
    IridiumTelemetry iridium;      // valid when standard == 20
    MeshTelemetry mesh;      // valid when standard == 21
    HdrTelemetry hdr;        // valid when standard == 22
    CdrTelemetry cdr;        // valid when standard == 23
    PagerTelemetry pager;    // valid when standard == 24
    PacketTelemetry packet;  // valid when standard == 25
    HfdigTelemetry hfdig;    // valid when standard == 26
    AirbandTelemetry airband;   // valid when standard == 27
    Vdl2Telemetry vdl2;         // valid when standard == 28
    Vdl4Telemetry vdl4;         // valid when standard == 83
    uint64_t seq = 0;
    int standard = 0;            // 0 DVB-T2, 1 DVB-T, 2 ATSC, 3 DAB, 4 ATSC 3.0 (details: Engine::atsc3Telemetry), 5 ISDB-T, 6 FM, 7 DVB-S/S2, 8 DTMB, 9 analog TV, 10 DMR, 11 DRM, 12 ADS-B, 13 GNSS, 14 radiosonde, 15 AIS, 16 marine, 17 ACARS, 18 Inmarsat-C, 19 Inmarsat Aero, 20 Iridium, 21 mesh, 22 HD Radio, 23 CDR, 24 pagers, 25 APRS / packet, 26 HF digital, 27 airband, 28 VDL Mode 2, 29 ISM sensors, 31 VOR / ILS, 32 M17, 34 D-STAR, 35 CW, 36 HFDL, 37 PSK31, 38 JS8Call, 41 DVB-C, 45 Orbcomm, 47 TETRA, 39 NXDN, 39 NXDN, 50 HF ALE, 48 MAC, 51 P25
    IsmTelemetry ism;           // valid when standard == 29
    VorTelemetry vor;           // valid when standard == 31
    NdbTelemetry ndb;           // valid when standard == 30 (NDB, engine standard 31)
    HfdlTelemetry hfdl;         // valid when standard == 36
    M17Telemetry m17;           // valid when standard == 32
    CellTelemetry cell;         // valid when standard == 33
    CwTelemetry cw;             // valid when standard == 35
    SpecxTelemetry specx;  // valid when standard == 42
    Js8Telemetry js8;      // valid when standard == 38
    AstroTelemetry astro;     // valid when standard == 49 (radio astronomy)
    SpecanTelemetry specan;   // valid when standard == 43 (spectrum analyser)
    OrbcommTelemetry orbcomm;   // valid when standard == 45
    DvbcTelemetry dvbc;    // valid when standard == 41
    UplinkTelemetry uplink;   // valid when standard == 44 (Mode S uplink)
    AleTelemetry ale;         // valid when standard == 50 (HF ALE)
    P25Telemetry p25;         // valid when standard == 51 (P25 Phase 1)
    BleTelemetry ble;         // valid when standard == 52 (Bluetooth LE advertising)
    IonoTelemetry iono;       // valid when standard == 53 (ionospheric sounders)
    Qo100Telemetry qo100;     // valid when standard == 54 (QO-100)
    WifiTelemetry wifi;       // valid when standard == 55 (Wi-Fi networks)
    GeneralTelemetry general; // valid when standard == 56 (general receiver)
    TimeTelemetry timesig;    // valid when standard == 59 (time signals)
    FlarmTelemetry flarm;     // valid when standard == 67 (FLARM)
    DjiTelemetry dji;         // valid when standard == 69 (DJI drones)
    CubeTelemetry cube;       // valid when standard == 62 (CubeSat telemetry)
    GlobalstarTelemetry globalstar;   // valid when standard == 63 (Globalstar activity monitor)
    VdesTelemetry vdes;       // valid when standard == 64 (VDES)
    NbiotTelemetry nbiot;     // valid when standard == 70 (NB-IoT cells)
    ZigbeeTelemetry zigbee;   // valid when standard == 66 (Zigbee / 802.15.4)
    LoraRawTelemetry loraraw; // valid when standard == 72 (LoRa raw)
    ZwaveTelemetry zwave;     // valid when standard == 99 (Z-Wave)
    WisunTelemetry wisun;     // valid when standard == 85 (Wi-SUN FAN)
    FpvTelemetry fpv;         // valid when standard == 102 (analog FPV video)
    WmbusTelemetry wmbus;     // valid when standard == 87 (Wireless M-Bus)
    TonesTelemetry tones;     // valid when standard == 90 (tone decoders: CTCSS, DCS, DTMF, selcall, SELCAL)
    SarsatTelemetry sarsat;   // valid when standard == 93 (COSPAS-SARSAT 406 MHz beacons)
    Mt63Telemetry mt63;       // valid when standard == 96 (MT63)
    FsqTelemetry fsq;         // valid when standard == 114 (FSQ)
    IfkpTelemetry ifkp;       // valid when standard == 117 (IFKP)
    KaalTelemetry kaal;       // valid when standard == 123 (KAAL, engine standard 124)
    KaitTelemetry kait;       // valid when standard == 132 (KAIT, engine standard 133)
    KavbTelemetry kavb;       // valid when standard == 124 (KAVB, engine standard 125)
    KawsTelemetry kaws;       // valid when standard == 134 (KAWS, engine standard 135)
    KappTelemetry kapp;       // valid when standard == 125 (KAPP, engine standard 126)
    KaftTelemetry kaft;       // valid when standard == 126 (KAFT, engine standard 127)
    CquamTelemetry cquam;     // valid when standard == 108 (AM stereo, C-QUAM)
    ArdopTelemetry ardop;     // valid when standard == 141 (ARDOP, family 140)
    ModeAcTelemetry modeac;   // valid when standard == 143 (Mode A/C, family 142)
    EczasTelemetry eczas;     // valid when standard == 86 (e-Czas, 225 kHz Poland)
    MsTelemetry mscatter;     // valid when standard == 105 (meteor scatter)
    DmeTelemetry dme;         // valid when standard == 123 (DME, engine standard 123)
    DcsTelemetry goesdcs;     // valid when standard == 142 (GOES DCS, family 141)
    BletrkTelemetry bletrk;   // valid when standard == 144 (BLE tracker detector, family 143)
    SbTelemetry satbeacon;    // valid when standard == 145 (satellite beacon)
    HfbTelemetry hfb;         // valid when standard == 68 (HF beacons)
    WbTelemetry wb;           // valid when standard == 74 (Signal workbench)
    DatawellTelemetry datawell;   // valid when standard == 75 (Datawell buoy HF link)
    CodarTelemetry codar;     // valid when standard == 73 (CODAR / HF ocean radar)
    SameTelemetry same;       // valid when standard == 77 (NOAA weather radio SAME)
    AntTelemetry ant;         // valid when standard == 78 (ANT+ sensors)
    DectTelemetry dect;       // valid when standard == 79 (DECT cordless phones)
    TetrapolTelemetry tetrapol;   // valid when standard == 80 (TETRAPOL)
    MultitoneTelemetry multitone;   // valid when standard == 76 (Multitone paging)
    HalowTelemetry halow;     // valid when standard == 71 (Wi-Fi HaLow / 802.11ah)
    RidTelemetry rid;         // valid when standard == 57 (Drone Remote ID)
    MeteorTelemetry meteor;   // valid when standard == 60 (Meteor-M LRPT)
    NrTelemetry nr;           // valid when standard == 58 (5G NR cells)
    GoesTelemetry goes;       // valid when standard == 61 (GOES HRIT)
    ElektroTelemetry elektro; // valid when standard == 81 (Elektro-L)
    UatTelemetry uat;         // valid when standard == 84 (UAT 978 MHz)
    HrptTelemetry hrpt;       // valid when standard == 65 (HRPT / AHRPT)
    PskTelemetry psk;      // valid when standard == 37
    DstarTelemetry dstar;       // valid when standard == 34
    NxdnTelemetry nxdn;         // valid when standard == 39
    DpmrTelemetry dpmr;         // valid when standard == 46
    TetraTelemetry tetra;       // valid when standard == 47
    MacTelemetry mac;           // valid when standard == 48
    GsmrTelemetry gsmr;         // valid when standard == 40
    RailwayTelemetry klw, tdcs; // active standards 147 and 148
    LbjTelemetry lbj;           // valid when standard == 82 (LBJ, engine standard 83)
    ElLoranTelemetry eloran;    // valid when standard == 127 (eLoran / Loran-C, family 126)
    MptTelemetry mpt1327;       // valid when standard == 128 (MPT1327 trunking, family 127)
    FusionTelemetry fusion;     // valid when standard == 129 (Yaesu Fusion, family 128, stdMode 130)
    LtrTelemetry ltr;           // valid when standard == 130 (LTR control channel, engine standard 131)
    EdacsTelemetry edacs;       // valid when standard == 131 (EDACS control channel, engine standard 132)
    MilhfTelemetry milhf;       // valid when standard == 146 (HF waveform ID, engine standard 147)
    DgpsTelemetry dgps;         // valid when standard == 122 (DGPS beacon, engine standard 122)
    struct Dvbt {                // DVB-T only: TPS parameters and the channel decoder's statistics
        bool tpsOk = false, fecSync = false;
        int mode = 0, guard = 0, mod = 0, hier = 0, crHp = 0, crLp = 0, cellId = 0, frameIdx = 0, punctPhase = 0;
        uint64_t packets = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0;
        double viterbiMargin = 0, secSinceTps = 0;
        int symbolIdx = 0;
    } dvbt;
    struct Isdbt {               // ISDB-T only: the TMCC parameters and the statistics of the three layers
        bool tmccOk = false;
        int mode = 0, guard = 0, symbolIdx = -1, intShift = 0, switching = 15;
        int bwMhz = 6;           // channel width the receiver works with (6, 7, 8 MHz)
        bool partial = false, emergency = false;
        double secSinceTmcc = 0;
        struct Lay { int segments = 0, mod = 0, rate = 0, ti = 0; uint64_t packets = 0, rsClean = 0, rsCorrected = 0, rsFailed = 0; double viterbi = 0; bool synced = false; } layer[3];
    } isdbt;
    bool rateOk = true;
    bool decimating = false;
    double inputRate = 0, nativeRate = 0;
    int state = 0; // 0 searching P1, 1 waiting for GI, 2 locked
    P1Info p1;
    uint64_t p1Count = 0;
    uint64_t p1Evaluated = 0;     // start positions the exact P1 metric was computed for (the windowed search and the cheap first stage keep it a small part of the samples)
    uint64_t p1Rescans = 0;       // times the windowed P1 search found nothing where the frame cadence put it and searched everything again
    double secSinceP1 = 0;
    double frameMs = 0;
    int symbolsPerFrame = 0;
    double sroPpm = 0;
    float giScore[kNumGi] = {};
    int giIdx = -1;
    float giMargin = 0;
    double cfoHz = 0;
    float cpCorr = 0;
    float cpSnrDb = 0;
    float timingErr = 0;
    uint64_t symbols = 0;
    int fftN = 0, guard = 0, carriers = 0;
    std::vector<float> p1Trace;   // decimated C-A-B metric, newest last (kTraceDecim samples per point)
    std::vector<cf32> p1Const;    // differentially decoded P1 carriers (BPSK ~ +-1)
    std::vector<cf32> cells;      // inter-symbol differential cells (diagnostic), subsampled
    std::vector<cf32> rawCells;   // raw FFT cells of the latest symbol, subsampled

    // ---- P2 / channel (phase 2)
    bool chValid = false;
    bool extCarriers = false;     // extended carrier mode detected
    int chCarriers = 0;           // K_total of the estimate
    std::vector<float> chMagDb;   // |H| per carrier, dB (decimated to <= 4096 points)
    std::vector<float> chPhase;   // arg(H) per carrier, radians (same decimation)
    int chDecim = 1;
    std::vector<float> irDb;      // power-delay profile (dB rel. peak) from irTauMin, 1 sample per entry
    int irTauMin = 0;
    std::vector<float> snrDb;     // pilot-derived SNR per pilot-grid point
    int snrStep = 3;              // carriers between points
    float p2SnrDb = 0;
    std::vector<cf32> eqCells;    // equalised P2 data cells (subsampled)

    // ---- L1 signalling
    bool l1preOk = false, l1postOk = false;
    L1Pre l1pre;
    L1Post l1post;
    uint64_t l1preGood = 0, l1preBad = 0, l1postGood = 0, l1postBad = 0;
    int l1Iters = 0;

    // ---- data symbols (after L1-pre gives the pilot pattern)
    bool dataValid = false;
    int dataPp = 0, dataDx = 0, dataDy = 0;
    float dataSnrDb = 0;
    std::vector<cf32> eqData;     // equalised data cells of the last frame (subsampled)
    std::vector<float> dataSnrCarrier; // per pilot-grid point
    uint64_t dataFrames = 0;

    // ---- PLP decoding (FEC)
    bool plpValid = false;       // a PLP was selected and submitted at least once
    int plpId = 0;
    PlpFec plpFec;
    int plpBlocks = 0;           // FEC blocks in the last decoded frame
    uint64_t plpFrames = 0, plpFramesDropped = 0;
    uint64_t blocksOk = 0, blocksBad = 0, headersOk = 0;
    double plpMerDb = 0, plpPreBer = 0, plpIters = 0, plpDecodeMs = 0;
    bool plpOnGpu = false, gpuAvailable = false;
    int computeMode = 2;         // 0 CPU, 1 GPU, 2 auto
    uint64_t plpBchCorrected = 0;
    std::vector<cf32> plpConst;
    std::vector<uint8_t> plpConstErr;
    std::vector<uint16_t> plpConstTx;   // transmitted point label for each plpConst cell
    uint64_t plpConstSeq = 0;           // increases whenever plpConst is replaced
    int plpHeaderUpl = 0, plpHeaderDfl = 0, plpHeaderSyncd = 0;
    int plpSkipped = 0;          // frames skipped (unsupported TI type, missing cells, ...)
    struct PlpInfo { int id = 0, type = 0, payloadType = 0, mod = 0, cod = 0, rotation = 0, fecType = 0, tiType = 0, tiLength = 0, blocks = 0; bool supported = true; };
    std::vector<PlpInfo> plpList;          // every PLP signalled in L1-post
    int plpSelectedId = -1;                // PLP being decoded
    std::vector<std::string> unsupported;  // reasons why (part of) this multiplex cannot be decoded, e.g. "MISO transmission"
    std::vector<std::vector<uint8_t>> blockMap; // per decoded frame (oldest first, last 90): 1 = FEC block decoded, 0 = failed
};

constexpr int kTraceDecim = 512;

class T2Receiver {
public:
    T2Receiver();
    ~T2Receiver();
    void configure(double inputRateHz, double bandwidthMhz);
    void reset();
    void feed(const cf32* x, size_t n);
    // `skippedSamples` input samples were thrown away between the last feed() and the next one (the receiver could not keep up): drop the
    // frame in flight and carry the frame timeline across the gap, so the next P1 is found where it is expected.
    void markGap(size_t skippedSamples);
    // For live radios: the resampler runs on its own thread, a little ahead of the rest of the receiver, and feed() returns without waiting
    // for the samples to be processed (they are, in order, by a later call). Off (the default) feed() is synchronous: file sources and
    // tests see everything processed when it returns.
    void setPipelined(bool on);
    bool telemetry(RxTelemetry& out, uint64_t lastSeq);
    // Called (on the receiver thread) for every decoded PLP frame, in order.
    // LDPC compute backend: 0 = CPU, 1 = GPU, 2 = auto (GPU once the CPU can't keep up)
    void setComputeMode(int m);
    // Which PLP to decode: its PLP_ID, or -1 for automatic (the first data PLP).
    void selectPlp(int id);
    void setPlpCallback(std::function<void(const PlpResult&)> cb);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace dect2
