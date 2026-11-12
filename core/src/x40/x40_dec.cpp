// The decoder factory and the tuning of each 4.0 signal mode.
#include "x40_int.h"

namespace dect2 {

std::unique_ptr<x40::Decoder> x40::makeDecoder(int v) {
    switch (v) {
    case RcPpm: return makeRcPpm();
    case Uvb76: return makeUvb76();
    case Graves: return makeGraves();
    case Jove: return makeJove();
    case Beacon: return makeBeacon();
    case Em4100: return makeEm4100();
    case UhfRfid: return makeUhfRfid();
    case Qi: return makeQi();
    case FmMon: return makeFmMon();
    case AmMon: return makeAmMon();
    case PlNoise: return makePlNoise();
    case Radar: return makeRadar();
    case GnssJam: return makeGnssJam();
    case Passband: return makePassband();
    case VlfMsk: return makeVlfMsk();
    case Occupancy: return makeOccupancy();
    case Evm: return makeEvm();
    case FreqAcc: return makeFreqAcc();
    case PttLog: return makePttLog();
    case Intermod: return makeIntermod();
    case Spur: return makeSpur();
    case DopplerLog: return makeDopplerLog();
    case HopAnalyser: return makeHopAnalyser();
    case Birdie: return makeBirdie();
    case Burst: return makeBurst();
    case Fading: return makeFading();
    case InputHealth: return makeInputHealth();
    case PhaseNoise: return makePhaseNoise();
    case SsbTest: return makeSsbTest();
    case Sinad: return makeSinad();
    case FskAnalyser: return makeFskAnalyser();
    case YFactor: return makeYFactor();
    case CwKeying: return makeCwKeying();
    case NoiseFloor: return makeNoiseFloor();
    case NfmDev: return makeNfmDev();
    case Afsk: return makeAfsk();
    case OokPulse: return makeOokPulse();
    case TxStability: return makeTxStability();
    case OccBw: return makeOccBw();
    case Papr: return makePapr();
    case SpecKurt: return makeSpecKurt();
    case Glitch: return makeGlitch();
    case Marker: return makeMarker();
    case FskModem: return makeFskModem();
    case SsrInt: return makeSsrInt();
    case Dcr: return makeDcr();
    case Xpa: return makeXpa();
    case Xpa2: return makeXpa2();
    case Crowd36: return makeCrowd36();
    case F01: return makeF01();
    case F06: return makeF06();
    case Gw: return makeGw();
    default: return nullptr;
    }
}

ModeTuning x40Tuning(int v) {
    using namespace x40;
    ModeTuning t;
    t.stdMode = familyOfVariant(v) + 2;
    struct T { const char* id; double minMhz, maxMhz, defMhz, rate, minRate, bwMhz; bool carrier; };
    static const T k[kNumVariants] = {
        {"rcppm", 26, 75, 72.350, 1000000, 200000, 0.04, false},
        {"uvb76", 4, 5, 4.625, 1000000, 24000, 0.012, true},
        {"graves", 143, 143.1, 143.050, 250000, 8000, 0.004, true},
        {"jove", 19, 22, 20.100, 1000000, 24000, 0.012, false},
        {"beacon", 28, 450, 144.400, 250000, 8000, 0.004, true},
        {"em4100", 0.1, 0.15, 0.125, 250000, 24000, 0.01, true},
        {"uhfrfid", 860, 960, 865.700, 2000000, 400000, 0.6, true},
        {"qi", 0.1, 0.21, 0.140, 250000, 24000, 0.012, true},
        {"fmmon", 64, 110, 100.000, 250000, 200000, 0.2, true},
        {"ammon", 0.148, 30, 1.000, 250000, 24000, 0.012, true},
        {"plnoise", 0.1, 1000, 7.100, 250000, 24000, 0.02, false},
        {"radar", 100, 12000, 9410.000, 2000000, 1000000, 1.5, false},
        {"gnssjam", 1100, 1700, 1575.420, 4000000, 2000000, 2.0, false},
        {"passband", 28, 450, 145.900, 96000, 48000, 0.048, false},
        {"vlfmsk", 0.01, 0.03, 0.0240, 48000, 8000, 0.003, true},
        {"occupancy", 0.1, 6000, 145.000, 2000000, 500000, 2.0, false},
        {"evm", 0.1, 6000, 435.000, 250000, 96000, 0.09, true},
        {"freqacc", 0.1, 6000, 10.000, 48000, 16000, 0.006, true},
        {"pttlog", 0.1, 6000, 146.520, 48000, 16000, 0.012, false},
        {"intermod", 0.1, 6000, 145.000, 2000000, 500000, 2.0, false},
        {"spur", 0.1, 6000, 145.000, 2000000, 500000, 2.0, false},
        {"dopplerlog", 100, 3000, 435.000, 48000, 24000, 0.03, true},
        {"hop", 0.1, 6000, 433.900, 2000000, 1000000, 2.0, false},
        {"birdie", 0.1, 6000, 145.000, 2000000, 500000, 2.0, false},
        {"burst", 0.1, 6000, 433.920, 2000000, 1000000, 2.0, false},
        {"fading", 0.1, 6000, 144.400, 48000, 8000, 0.006, true},
        {"inputhealth", 0.1, 6000, 145.000, 2000000, 100000, 2.0, false},
        {"phasenoise", 0.1, 6000, 10.000, 250000, 50000, 0.1, true},
        {"ssbtest", 0.1, 500, 14.200, 48000, 12000, 0.004, false},
        {"sinad", 26, 6000, 146.520, 48000, 24000, 0.016, true},
        {"fskan", 0.1, 6000, 433.920, 250000, 48000, 0.1, false},
        {"yfactor", 0.1, 6000, 145.000, 2000000, 250000, 0.2, false},
        {"cwkeying", 0.1, 500, 7.030, 48000, 12000, 0.004, true},
        {"noisefloor", 0.1, 6000, 145.000, 2000000, 500000, 2.0, false},
        {"nfmdev", 26, 6000, 146.520, 48000, 24000, 0.016, true},
        {"afsk", 26, 6000, 144.390, 48000, 24000, 0.016, true},
        {"ookpulse", 26, 2000, 433.920, 250000, 100000, 0.2, false},
        {"txstability", 26, 6000, 146.520, 48000, 24000, 0.016, true},
        {"occbw", 0.1, 6000, 145.000, 1000000, 100000, 0.5, false},
        {"papr", 0.1, 6000, 145.000, 1000000, 100000, 0.5, false},
        {"speckurt", 0.1, 6000, 145.000, 2000000, 250000, 2.0, false},
        {"glitch", 0.1, 6000, 145.000, 2000000, 100000, 2.0, false},
        {"marker", 74.5, 75.5, 75.0, 48000, 24000, 0.012, true},
        {"fskmodem", 0.1, 6000, 7.85, 48000, 24000, 0.004, false},
        {"ssrint", 1000, 1100, 1030.0, 4000000, 2000000, 2.0, false},
        {"dcr", 150, 470, 351.2875, 250000, 48000, 0.0125, false},
        {"xpa", 1.6, 30, 8.000, 48000, 8000, 0.004, false},        // HF, upper sideband: the dial frequency
        {"xpa2", 1.6, 30, 8.000, 48000, 8000, 0.004, false},
        {"crowd36", 1.6, 30, 10.000, 48000, 8000, 0.004, false},
        {"f01", 1.6, 30, 9.000, 48000, 8000, 0.004, false},
        {"f06", 1.6, 30, 6.000, 48000, 8000, 0.004, false},
        {"gwfsk", 1.6, 30, 8.400, 48000, 8000, 0.004, false},
    };
    if (v < 0 || v >= kNumVariants) return t;
    const T& e = k[v];
    t.id = e.id; t.name = variantName(v);
    t.minMhz = e.minMhz; t.maxMhz = e.maxMhz; t.defMhz = e.defMhz;
    t.sampleRate = e.rate; t.minSampleRate = e.minRate; t.bandwidthMhz = e.bwMhz;
    t.basebandHz = 0; t.tuneOffsetHz = 0; t.carrierAtCentre = e.carrier;
    return t;
}

} // namespace dect2
