#pragma once
#include <string>
#include "dect2/t2/t2rx.h"
namespace dect2 { struct SampleLoss; }
namespace onair {
// The telemetry the phone shows: cards (label, value), tables (columns + rows of text) and the one-line summary of the mode.
// The same names as the desktop's tables. One JSON object; `std` is RxTelemetry::standard of the active mode.
// loss: the engine's sample-loss counters (a card and a "linkNote" for the app when a live radio loses signal); cdrListen: the CDR service the
// bridge plays (-1 the first audio service, 0 none).
std::string resultsJson(const dect2::RxTelemetry& rx, bool running, const dect2::DabEnsemble* dabEnsemble = nullptr, const dect2::SampleLoss* loss = nullptr,
                        int cdrListen = -1);
}
