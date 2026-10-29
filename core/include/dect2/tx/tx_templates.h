// Ready-made texts for the message of the Digital video generator's "Test generator": the same as the TX studio's Templates row
// (app/panels/tx_studio.cpp), duplicated there for now until the generator is integrated with the TX studio.
#pragma once
#include <string>

namespace dect2 {

enum class TxTemplate { CqCall, Beacon, TestPattern, Ryry };
constexpr int kTxTemplateCount = 4;

// An empty callsign is N0CALL, an empty grid KO24
inline std::string txTemplateText(TxTemplate t, const std::string& callsign, const std::string& grid) {
    const std::string call = callsign.empty() ? "N0CALL" : callsign, loc = grid.empty() ? "KO24" : grid;
    switch (t) {
    case TxTemplate::CqCall: return "CQ CQ CQ DE " + call + " " + call + " K";
    case TxTemplate::Beacon: return "VVV DE " + call + " " + loc;
    case TxTemplate::TestPattern: return "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789";
    case TxTemplate::Ryry: return "RYRYRYRYRYRYRYRYRYRYRYRYRYRYRYRY";
    }
    return std::string();
}

} // namespace dect2
