// The frequency being listened to (the VFO), as everything that names "the" frequency should show it: the big readout, the status strip,
// the receiver tabs. The General receiver's VFO may sit off its dial (a.freqMhz); every other mode moves a.freqMhz itself (tuneDecode).
#pragma once
#include "../app.h"
#include "freq_model.h"

void tuneListen(App& a, double mhz);   // toolbar.cpp: a new listening frequency: the VFO inside the captured band, the radio only past its edge
inline double listenMhz(const App& a) { return isGeneralFamily(a.family) ? generalListenHz(a) / 1e6 : a.freqMhz; }
