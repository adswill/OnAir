// The modes added after FM, seen together: one status line per telemetry (the command line prints it).
#pragma once
#include "mode_tuning.h"
#include "t2rx.h"
#include <string>

namespace dect2 {
std::string modeSummary(const RxTelemetry& t);   // "" when the telemetry is not from one of these modes
}
