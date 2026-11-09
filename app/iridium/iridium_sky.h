// Iridium sky plot and satellite identification from Doppler (core/include/dect2/iridium/iridium_doppler.h).
#pragma once
#include "app.h"
void iridiumSkyTick(App& a);                 // feeds the ring-alert bursts to the identifier (a live radio only)
void iridiumSkyView(App& a, float W, float H);
