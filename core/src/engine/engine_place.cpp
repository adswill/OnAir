// Placing and configuring one receiver inside a running engine (configureReceiver, muteReceiver, placeReceiver...) and the
// rules for which modes may run as the second receiver.
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

bool Engine::secondaryAllowed(int m) {
    switch (m) {
    case 11: case 13: case 15: case 16: case 17: case 18: case 19: case 20: case 22: case 25: case 26: case 27: case 28: case 29: case 30: case 31:
    case 32: case 33: case 35: case 36: case 37: case 38: case 39: case 40: case 45: case 47: case 48: case 51: case 52: case 57: case 60:
    case 61: case 63:
    // the narrow-channel modes added later (each has its configureReceiver / placeReceiver / pollMode case and a test signal that decodes beside another mode)
    case 65: case 67: case 68: case 71: case 72: case 73: case 74: case 76: case 77: case 78: case 79: case 80: case 81: case 82: case 83: case 84:
        return true;
    default:
        return false;
    }
}

bool Engine::secondaryAllowed(int m, int pri) { return secondaryAllowed(m) || (m == 8 && pri == 55); }

std::string Engine::secondaryCheck(int pri, int sec, double centreHz, double rateHz, double secHz, double chanHz) {
    const ModeTuning* st = modeTuning(sec);
    const ModeTuning* pt = modeTuning(pri);
    const std::string name = st ? st->name : "This mode";
    if (!secondaryAllowed(sec, pri) || !st) {
        const char* why;
        switch (sec) {
        case 8:
            why = "its transport stream and picture belong to the first mode, except beside QO-100"; break;
        case 14: case 34: case 41: case 43: case 44: case 50: case 53: case 54: case 59: case 69:
            why = "it tunes the radio, scans or takes the whole capture by itself"; break;
        case 7: case 12: case 23: case 24:
            why = "its station list and sound belong to the first mode"; break;
        case 21: case 46: case 55: case 56: case 58: case 64: case 70: case 75:
            why = "it needs the radio's whole band for itself"; break;
        default:
            why = "it makes a transport stream or a picture, which only the first mode can show"; break;
        }
        return name + " cannot run as a second mode: " + why;
    }
    if (pri == sec || activeStdFor(pri) == activeStdFor(sec)) return name + " is already the first mode";
    if (pri == 44 || pri == 50) return std::string("no second mode beside ") + (pt ? pt->name : "this mode") + ": it retunes the radio itself";
    char b[400];
    if (rateHz > 0 && rateHz < st->minSampleRate - 1) {
        snprintf(b, sizeof b, "%s needs at least %.2f Msps (the radio runs at %.2f Msps)", name.c_str(), st->minSampleRate / 1e6, rateHz / 1e6);
        return b;
    }
    // the channel (its width around its frequency) inside the part of the capture the filters keep
    const double off = std::fabs(secHz - centreHz);
    const double need = off + (chanHz > 0 ? chanHz : st->bandwidthMhz * 1e6) * 0.5;
    if (rateHz > 0 && need > 0.45 * rateHz) {
        snprintf(b, sizeof b, "%s at %.3f MHz lies %.3f MHz from the centre of the capture (%.3f MHz): it needs %.1f Msps or more, the radio runs at %.2f Msps. "
                 "Raise the sample rate or tune the first mode nearer", name.c_str(), secHz / 1e6, off / 1e6, centreHz / 1e6, need / 0.45 / 1e6, rateHz / 1e6);
        return b;
    }
    return "";
}

void Engine::setDecodeShiftHz(double hz) {
    if (std::fabs(hz - decodeShiftHz_.load()) < 0.5) return;
    decodeShiftHz_ = hz;
    decodeMix_.set(hz, rate_.load());   // after the offset resampler: at the receiver's rate
    resetReq_ = true;
}

double Engine::captureCentreHz() const {
    TuneSettings t;
    DeviceInfo::Kind k;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; k = lastDev_.kind; }
    if (k == DeviceInfo::File || k == DeviceInfo::Audio || k == DeviceInfo::Hamlib) return t.centerHz;   // dial frequency (sound card / Hamlib + audio IQ)
    const ModeTuning* mt = modeTuning(stdMode_.load());
    return t.centerHz + (mt && !noTuneOffset_.load() ? mt->tuneOffsetHz : 0);   // the radio's centre (with offset tuning the samples are shifted back to it)
}

// The receivers that can run as the second mode, set up for a sample rate (start() does it for all of them at the capture's)
void Engine::configureReceiver(int a, double r) {
    switch (a) {
    case 7: rxDvbs_.configure(r); break;   // the second mode beside QO-100 only
    case 10: rxDmr_.configure(r); break;
    case 12: rxAdsb_.configure(r); break;
    case 14: rxSonde_.configure(r); break;
    case 15: rxAis_.configure(r); break;
    case 16: rxMarine_.configure(r); break;
    case 17: rxAcars_.configure(r); break;
    case 18: rxInmc_.configure(r); break;
    case 19: rxAero_.configure(r); break;
    case 21: rxMesh_.configure(r); break;
    case 24: rxPager_.configure(r); break;
    case 25: rxPacket_.configure(r); break;
    case 26: rxHfdig_.configure(r); break;
    case 27: rxAirband_.configure(r); break;
    case 28: rxVdl2_.configure(r); break;
    case 29: rxIsm_.configure(r); break;
    case 30: rxNdb_.configure(r); break;
    case 122: rxDme_.configure(r); break;
    case 31: rxVor_.configure(r); break;
    case 32: rxM17_.configure(r); break;
    case 34: rxDstar_.configure(r); break;
    case 35: rxCw_.configure(r); break;
    case 36: rxHfdl_.configure(r); break;
    case 37: rxPsk_.configure(r); break;
    case 38: rxJs8_.configure(r); break;
    case 39: rxNxdn_.configure(r); break;
    case 44: rxUplink_.configure(r); break;
    case 46: rxDpmr_.configure(r); break;
    case 47: rxTetra_.configure(r); break;
    case 50: rxAle_.configure(r); break;
    case 51: rxP25_.configure(r); break;
    case 56: rxGeneral_.configure(r); for (auto& g : extraGeneral_) g.configure(r); break;
    case 59: rxTime_.configure(r); break;
    case 60: rxMeteor_.configure(r); break;
    case 62: rxCube_.configure(r); break;
    case 63: rxGs_.configure(r); break;
    case 64: rxVdes_.configure(r); break;
    case 65: rxHrpt_.configure(r); break;
    case 66: rxZb_.configure(r); break;
    case 67: rxFlarm_.configure(r); break;
    case 68: rxHfb_.configure(r); break;
    case 69: rxDji_.configure(r); break;
    case 70: rxNb_.configure(r); break;
    case 71: rxHl_.configure(r); break;
    case 73: rxCd_.configure(r); break;
    case 74: rxWb_.configure(r); break;
    case 75: rxDw_.configure(r); break;
    case 77: rxSame_.configure(r); break;
    case 78: rxAnt_.configure(r); break;
    case 79: rxDect_.configure(r); break;
    case 80: rxTetrapol_.configure(r); break;
    case 81: rxEl_.configure(r); break;
    case 82: rxLbj_.configure(r); break;
    case railway::kKlwActiveStandard: rxKlw_.configure(r); break;
    case railway::kTdcsActiveStandard: rxTdcs_.configure(r); break;
    case horus::kActiveStandard: rxHorus_.configure(r); break;
    case 121: rxDgps_.configure(r); break;
    case 127: rxEloran_.configure(r); break;
    case 149: rxNfc_.configure(r); break;
    case 128: rxMpt_.configure(r); break;
    case 129: rxFusion_.configure(r); break;
    case 141: rxArd_.configure(r); break;
    case 84: rxUat_.configure(r); break;
    case 83: rxVdl4_.configure(r); break;
    case 76: rxMt_.configure(r); break;
    case 72: rxLr_.configure(r); break;
    case 201: rxLw_.configure(r); break;
    case 202: rxLs_.configure(r); break;
    case 204: rxPac_.configure(r); break;
    case 203: rxPskd_.configure(r); break;
    case 85: rxWs_.configure(r); break;
    case 99: rxZw_.configure(r); break;
    case 86: rxEcz_.configure(r); break;
    case 102: rxFpv_.configure(r); break;
    case 87: rxWmb_.configure(r); break;
    case 90: rxTones_.configure(r); break;
    case 93: rxSar_.configure(r); break;
    case 96: rxMt63_.configure(r); break;
    case 114: rxFsq_.configure(r); break;
    case 117: rxIfkp_.configure(r); break;
    case 123: rxKaal_.configure(r); break;
    case 132: rxKait_.configure(r); break;
    case 134: rxKaws_.configure(r); break;
    case 125: rxKapp_.configure(r); break;
    case 124: rxKavb_.configure(r); break;
    case 126: rxKaft_.configure(r); break;
    case 108: rxCq_.configure(r); break;
    case 105: rxMs_.configure(r); break;
    case 142: rxGd_.configure(r); break;
    case 143: rxModeAc_.configure(r); break;
    case 144: rxBt_.configure(r); break;
    case 146: rxMilhf_.configure(r); break;
    case 151: rxSbas_.configure(r); break;
    case 152: break;
    case 153: rxArgos_.configure(r); break;
    case 154: rxSfx_.configure(r); break;
    case 145: rxSb_.configure(r); break;
    case 155: rxIsdbs_.configure(r); break;
    case 130: rxLtr_.configure(r); break;
    case 131: rxEdacs_.configure(r); break;
    case 156: rxMarineVhf_.configure(r); break;
    case 181: rxP25p2_.configure(r); break;
    default: break;
    }
}

void Engine::muteReceiver(int a, bool m) {
    if (WideModes::supports(a)) wide_.setMuted(m);
    switch (a) {
    case 10: rxDmr_.setMuted(m); break;
    case 22: rxHdr_.setMuted(m); break;
    case 26: rxHfdig_.setMuted(m); break;
    case 27: rxAirband_.setMuted(m); break;
    case 30: rxNdb_.setMuted(m); break;
    case 31: rxVor_.setMuted(m); break;
    case 32: rxM17_.setMuted(m); break;
    case 129: rxFusion_.setMuted(m); break;
    case 34: rxDstar_.setMuted(m); break;
    case 35: rxCw_.setMuted(m); break;
    case 37: rxPsk_.setMuted(m); break;
    case 38: rxJs8_.setMuted(m); break;
    case 39: rxNxdn_.setMuted(m); break;
    case 46: rxDpmr_.setMuted(m); break;
    case 50: rxAle_.setMuted(m); break;
    case 51: rxP25_.setMuted(m); break;
    case 77: rxSame_.setMuted(m); break;   // NOAA SAME: "Listen"
    case 56: rxGeneral_.setMuted(m); break;
    case 108: rxCq_.setMuted(m); break;
    case 123: rxKaal_.setMuted(m); break;
    case 156: rxMarineVhf_.setMuted(m); break;
    default: break;   // no sound
    }
}

void Engine::setSecondarySound(bool on) {
    secSound_ = on;
    std::lock_guard<std::mutex> ck(secCtlMu_);
    if (secTh_.joinable()) muteReceiver(activeStdFor(secStd_.load()), !on);
}

void Engine::setWideDecode(bool on, double spanHz) {
    wideDecode_ = on;
    rxCw_.setWide(on, spanHz);
    rxPsk_.setWide(on, spanHz);
    rxHfdig_.setWide(on, spanHz);
}

// Where the receiver of active standard a finds its channel: as start() does for the first mode, one receiver at a time
void Engine::placeReceiver(int a, double o, double centreHz, double dialHz, double chanHz) {
    switch (a) {
    case 14: rxSonde_.setSignalOffset(o); break;
    case 15: rxAis_.setSignalOffset(o); if (lastDev_.isRadio() && centreHz > 0) rxAis_.setCenterHz(centreHz); break;
    case 16: rxMarine_.setSignalOffset(o); break;
    case 17: rxAcars_.setSignalOffset(o); if (centreHz > 0) rxAcars_.setCenterHz(centreHz); break;
    case 18: rxInmc_.setSignalOffset(o); break;
    case 19: rxAero_.setSignalOffset(o); break;
    case 21: rxMesh_.setSignalOffset(o); break;
    case 24: rxPager_.setSignalOffset(o); break;
    case 25: rxPacket_.setSignalOffset(o); break;
    case 26: rxHfdig_.setSignalOffset(o); rxHfdig_.setDialHz(dialHz); break;
    case 27: rxAirband_.setSignalOffset(o); rxAirband_.setCenterHz(centreHz); break;
    case 28: rxVdl2_.setSignalOffset(o); rxVdl2_.setCenterHz(centreHz); break;
    case 29: rxIsm_.setSignalOffset(o); rxIsm_.setCenterHz(centreHz); break;
    case 30: rxNdb_.setSignalOffset(o); break;
    case 31: rxVor_.setSignalOffset(o); rxVor_.setCenterHz(centreHz); break;
    case 32: rxM17_.setSignalOffset(o); break;
    case 34: rxDstar_.setSignalOffset(o); break;
    case 35: rxCw_.setSignalOffset(o); break;
    case 36: rxHfdl_.setSignalOffset(o); rxHfdl_.setCenterHz(centreHz); break;
    case 37: rxPsk_.setSignalOffset(o); break;
    case 38: rxJs8_.setSignalOffset(o); break;
    case 39: rxNxdn_.setSignalOffset(o); break;
    case 46: rxDpmr_.setSignalOffset(o); rxDpmr_.setChannel(chanHz / 1e6); break;
    case 47: rxTetra_.setSignalOffset(o); rxTetra_.setCenterHz(centreHz); break;
    case 50: rxAle_.setSignalOffset(o); rxAle_.setDialHz(dialHz); break;
    case 51: rxP25_.setSignalOffset(o); break;
    case 56: rxGeneral_.setSignalOffset(o); for (auto& g : extraGeneral_) g.setSignalOffset(o); break;
    case 59: rxTime_.setSignalOffset(o); rxTime_.setDialHz(dialHz); break;
    case 60: rxMeteor_.setSignalOffset(o); break;
    case 62: rxCube_.setSignalOffset(o); rxCube_.setDialHz(dialHz); break;
    case 63: rxGs_.setSignalOffset(o); rxGs_.setDialHz(dialHz); break;
    case 64: rxVdes_.setSignalOffset(o); rxVdes_.setDialHz(dialHz); break;
    case 65: rxHrpt_.setSignalOffset(o); break;
    case 66: rxZb_.setSignalOffset(o); rxZb_.setCenterHz(centreHz); break;
    case 67: rxFlarm_.setSignalOffset(o); break;
    case 68: rxHfb_.setSignalOffset(o); rxHfb_.setCenterHz(centreHz); break;
    case 69: rxDji_.setSignalOffset(o); rxDji_.setCenterHz(centreHz); break;
    case 70: rxNb_.setSignalOffset(o); rxNb_.setCenterHz(centreHz); break;
    case 71: rxHl_.setSignalOffset(o); rxHl_.setCenterHz(centreHz); break;
    case 73: rxCd_.setSignalOffset(o); rxCd_.setCenterHz(centreHz); break;
    case 75: rxDw_.setSignalOffset(o); rxDw_.setCenterHz(centreHz); break;
    case 77: rxSame_.setSignalOffset(o); break;
    case 78: rxAnt_.setSignalOffset(o); rxAnt_.setCenterHz(centreHz); break;
    case 79: rxDect_.setSignalOffset(o); rxDect_.setCenterHz(centreHz); break;
    case 80: rxTetrapol_.setSignalOffset(o); rxTetrapol_.setCenterHz(centreHz); break;
    case 81: rxEl_.setSignalOffset(o); break;
    case 82: rxLbj_.setSignalOffset(o); rxLbj_.setCenterHz(centreHz); break;
    case railway::kKlwActiveStandard: rxKlw_.setSignalOffset(o); rxKlw_.setCenterHz(centreHz); break;
    case railway::kTdcsActiveStandard: rxTdcs_.setSignalOffset(o); rxTdcs_.setCenterHz(centreHz); break;
    case horus::kActiveStandard: rxHorus_.setSignalOffset(o); rxHorus_.setCenterHz(dialHz); break;
    case 121: rxDgps_.setSignalOffset(o); rxDgps_.setCenterHz(centreHz); break;
    case 127: rxEloran_.setSignalOffset(o); rxEloran_.setCenterHz(centreHz); break;
    case 149: rxNfc_.setSignalOffset(o); rxNfc_.setCenterHz(centreHz); break;
    case 128: rxMpt_.setSignalOffset(o); rxMpt_.setCenterHz(centreHz); break;
    case 129: rxFusion_.setSignalOffset(o); rxFusion_.setCenterHz(centreHz); break;
    case 141: rxArd_.setSignalOffset(o); break;
    case 84: rxUat_.setSignalOffset(o); break;
    case 83: rxVdl4_.setSignalOffset(o); rxVdl4_.setCenterHz(centreHz); break;
    case 76: rxMt_.setSignalOffset(o); break;
    case 72: rxLr_.setSignalOffset(o); break;
    case 201: rxLw_.setSignalOffset(o); rxLw_.setCenterHz(centreHz); break;
    case 202: rxLs_.setSignalOffset(o); rxLs_.setCenterHz(centreHz); break;
    case 204: rxPac_.setSignalOffset(o); break;
    case 203: rxPskd_.setSignalOffset(o); break;
    case 99: rxZw_.setSignalOffset(o); rxZw_.setCenterHz(centreHz); rxZw_.setDialHz(dialHz); break;
    case 85: rxWs_.setSignalOffset(o); rxWs_.setCenterHz(centreHz); rxWs_.setDialHz(dialHz); break;
    case 86: rxEcz_.setSignalOffset(o); break;
    case 102: rxFpv_.setSignalOffset(o); rxFpv_.setCenterHz(centreHz); break;
    case 87: rxWmb_.setSignalOffset(o); rxWmb_.setCenterHz(centreHz); break;
    case 90: rxTones_.setSignalOffset(o); break;
    case 93: rxSar_.setSignalOffset(o); break;
    case 96: rxMt63_.setSignalOffset(o); break;
    case 114: rxFsq_.setSignalOffset(o); break;
    case 117: rxIfkp_.setSignalOffset(o); break;
    case 108: rxCq_.setSignalOffset(o); break;
    case 105: rxMs_.setSignalOffset(o); rxMs_.setCenterHz(centreHz); break;
    case 142: rxGd_.setSignalOffset(o); rxGd_.setCenterHz(centreHz); break;
    case 143: rxModeAc_.setSignalOffset(o); break;
    case 144: rxBt_.setSignalOffset(o); rxBt_.setCenterHz(centreHz); break;
    case 146: rxMilhf_.setSignalOffset(o); rxMilhf_.setCenterHz(centreHz); break;
    case 151: rxSbas_.setSignalOffset(o); rxSbas_.setCenterHz(centreHz); break;
    case 153: rxArgos_.setSignalOffset(o); rxArgos_.setDialHz(dialHz); break;
    case 154: rxSfx_.setSignalOffset(o); rxSfx_.setDialHz(dialHz); break;
    case 145: rxSb_.setSignalOffset(o); rxSb_.setCenterHz(centreHz); break;
    case 155: rxIsdbs_.setSignalOffset(o); rxIsdbs_.setCenterHz(centreHz); break;
    case 130: rxLtr_.setSignalOffset(o); rxLtr_.setCenterHz(centreHz); break;
    case 131: rxEdacs_.setSignalOffset(o); rxEdacs_.setCenterHz(centreHz); break;
    case 156: rxMarineVhf_.setSignalOffset(o); rxMarineVhf_.setCenterHz(centreHz); rxMarineVhf_.setDialHz(dialHz); break;
    case 181: rxP25p2_.setSignalOffset(o); break;
    case 134: rxKaws_.setSignalOffset(o); break;
    default: break;   // DMR, ADS-B, Mode S uplink: the channel on the centre, nothing to place
    }
}

void Engine::placeReceiverAsPrimary(int a) {
    TuneSettings t;
    DeviceInfo::Kind k;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; k = lastDev_.kind; }
    const bool file = k == DeviceInfo::File;
    const bool noOff = file || k == DeviceInfo::Audio;
    const ModeTuning* mt = modeTuning(stdMode_.load());
    placeReceiver(a, mt && !noOff ? -mt->tuneOffsetHz : 0.0, file ? 0.0 : radioTune(t).centerHz, file ? 0.0 : t.centerHz, t.centerHz);
}

}   // namespace dect2
