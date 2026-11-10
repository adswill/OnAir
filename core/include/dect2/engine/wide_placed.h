// Several modes at once (issue #384): signals the user places at fixed frequencies, each with its own mode, decoded from one capture by the WideSession.
// The list survives a retune: a signal that leaves the capture stops, and comes back when the capture holds it again. Pure logic, tests/engine/test_wide_placed.cpp.
#pragma once
#include "dect2/engine/wide_session.h"
#include <map>
#include <string>
#include <vector>

namespace dect2 {

struct Placed {
    double absHz = 0;
    double widthHz = 12500;
    WsKind kind = WsKind::Unknown;   // Unknown: identify it automatically
};

std::string placedToText(const std::vector<Placed>& v);
std::vector<Placed> placedFromText(const std::string& s);

// Inside the usable part of a capture (the outer 5 % of each side rolls off, the same reach the whole-span decoders have)?
bool placedInCapture(const Placed& p, double centerHz, double rateHz);

// Middle-click on the spectrum (issue #384): the press only counts as a click when the mouse did not move (a middle drag pans elsewhere).
bool placedIsTap(double dxPx, double dyPx, double slopPx = 4);
// The placed signal closest to absHz that lies within tolHz (or inside its own width), or -1.
int placedNearest(const std::vector<Placed>& v, double absHz, double tolHz);
// The Placed that a middle-click at absHz adds: rounded to 100 Hz, width kept at least 1 kHz.
Placed placedAtClick(double absHz, double widthHz, WsKind kind);

// Keeps a WideSession's manual entries equal to the placed signals inside the capture. Call whenever the list, the centre or the rate may have changed:
// nothing happens when none of them did. The entry ids of the running signals are in ids() (absHz -> id).
class PlacedSync {
public:
    void apply(WideSession& ws, const std::vector<Placed>& v, double centerHz, double rateHz);
    void clear(WideSession& ws);       // takes every placed entry away
    void forget() { ids_.clear(); lastText_.clear(); lastCenter_ = lastRate_ = 0; }   // the session was reset: its entries are gone
    const std::map<double, int>& ids() const { return ids_; }
private:
    std::map<double, int> ids_;
    std::string lastText_;
    double lastCenter_ = 0, lastRate_ = 0;
};

} // namespace dect2
