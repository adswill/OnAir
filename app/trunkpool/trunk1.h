// Single-radio trunking in the app (docs/TRUNK1-3.9.md): the "One radio" block that the trunking modes (P25, Motorola Type II, EDACS, MPT 1327)
// and the Multi-radio trunking window show: how many calls are followed at once from the one capture, which learned voice channels are outside
// it and the bandwidth they need, the radio's limit, and one button that retunes the centre (and the sample rate) to cover the control channel
// and the busiest voice channels. Defined in trunkpool_ui.cpp.
#pragma once
#include <string>

struct App;

namespace trunk1 {
int slots();                       // the voice slots (calls decoded at once), Advanced; default 4
bool record();                     // a WAV + JSON per call (default off)
std::string recordDir(const char* mode);   // recordings/trunk1/<mode>
void apply(App& a);                // pushes the slots and the recording to the running trunking receiver (every frame; cheap when nothing changed)
void section(App& a);              // the block (a collapsing header of its own)
}
