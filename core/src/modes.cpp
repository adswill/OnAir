// The modes added after FM, seen together: tuning table, test signals, status lines.
#include "dect2/dvbs_rx.h"
#include "dect2/dvbs_gen.h"
#include "dect2/dtmb_rx.h"
#include "dect2/dtmb_gen.h"
#include "dect2/atv_rx.h"
#include "dect2/atv_gen.h"
#include "dect2/dmr_rx.h"
#include "dect2/dmr_gen.h"
#include "dect2/drm_rx.h"
#include "dect2/drm_gen.h"
#include "dect2/adsb_rx.h"
#include "dect2/adsb_gen.h"
#include "dect2/modes.h"
#include <vector>

namespace dect2 {

static const std::vector<ModeTuning>& table() {
    static const std::vector<ModeTuning> t = {dvbsTuning(), dtmbTuning(), atvTuning(), dmrTuning(), drmTuning(), adsbTuning()};
    return t;
}

const ModeTuning* modeTuning(int stdMode) {
    for (const auto& m : table()) if (m.stdMode == stdMode) return &m;
    return nullptr;
}

const ModeTuning* modeTuningById(const std::string& id) {
    for (const auto& m : table()) if (id == m.id) return &m;
    return nullptr;
}

std::unique_ptr<ModeSynth> makeModeSynth(int stdMode, const SynthConfig& cfg, double sampleRate) {
    switch (stdMode) {
    case 8: return makeDvbsSynth(cfg, sampleRate);
    case 9: return makeDtmbSynth(cfg, sampleRate);
    case 10: return makeAtvSynth(cfg, sampleRate);
    case 11: return makeDmrSynth(cfg, sampleRate);
    case 12: return makeDrmSynth(cfg, sampleRate);
    case 13: return makeAdsbSynth(cfg, sampleRate);
    default: return nullptr;
    }
}

std::string modeSummary(const RxTelemetry& t) {
    switch (t.standard) {
    case 7: return dvbsSummary(t.dvbs);
    case 8: return dtmbSummary(t.dtmb);
    case 9: return atvSummary(t.atv);
    case 10: return dmrSummary(t.dmr);
    case 11: return drmSummary(t.drm);
    case 12: return adsbSummary(t.adsb);
    default: return "";
    }
}

} // namespace dect2
