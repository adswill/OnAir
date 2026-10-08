// DVB-T2 receiver front-end (phase 1): P1 detection and S1/S2 decoding, guard-interval detection,
// symbol synchronisation, CFO / timing tracking and raw OFDM cell extraction.
#pragma once
#include <string>
#include "ring.h"
#include "atsc_tel.h"
#include "dab_tel.h"
#include "fm_tel.h"
#include "dvbs_tel.h"
#include "dtmb_tel.h"
#include "atv_tel.h"
#include "dmr_tel.h"
#include "drm_tel.h"
#include "adsb_tel.h"
#include "gnss_tel.h"
#include "sonde_tel.h"
#include "ais_tel.h"
#include "marine_tel.h"
#include "acars_tel.h"
#include "inmc_tel.h"
#include "aero_tel.h"
#include "iridium_tel.h"
#include "mesh_tel.h"
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
    uint64_t seq = 0;
    int standard = 0;            // 0 DVB-T2, 1 DVB-T, 2 ATSC, 3 DAB, 4 ATSC 3.0 (details: Engine::atsc3Telemetry), 5 ISDB-T, 6 FM, 7 DVB-S/S2, 8 DTMB, 9 analog TV, 10 DMR, 11 DRM, 12 ADS-B, 13 GNSS, 14 radiosonde, 15 AIS, 16 marine, 17 ACARS, 18 Inmarsat-C, 19 Inmarsat Aero, 20 Iridium, 21 mesh
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
