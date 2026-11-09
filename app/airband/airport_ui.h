// The airport search of the airband panel: type an airport, pick it, and its frequencies become the channel list.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "dect2/airband/airband_rx.h"
#include "dect2/airports/airports.h"

struct App;

// The airport table in use: the one imported or downloaded by the user (settings folder) if there is one, else the built-in one.
const dect2::AirportDb& airportDb();

// The section: search box, "near me", the results, the selected airport's frequencies and the actions. apply() gets the channels and says
// whether they replace the list or are added to it; the section tunes the radio to the capture that holds most of them and offers one button
// per further capture. current is the list as it is now (to tell what an "Add" brings new).
void airportSection(App& a, const std::function<void(const std::vector<dect2::AirbandChannel>&, bool replace)>& apply);

// Radio tuned so that centreHz is the centre of the capture (the airband mode keeps the dial on the 25 kHz raster and the DC spike off the channels)
void airportTuneCentre(App& a, double centreHz);
// First start: the nearest airport (within 300 km) with airband channels loaded and the radio tuned to hold as many as fit; what is set to a sentence for the user. False: no position or none near.
bool airportLoadNearest(App& a, const std::function<void(const std::vector<dect2::AirbandChannel>&, bool replace)>& apply, std::string& what);

// "EGLL Tower" for a channel frequency that belongs to an airport near the station (or any airport when the position is not set and only one has it); "" when none is known
std::string airportLabelFor(double freqHz);

// Offline packs: the OurAirports airports.csv and airport-frequencies.csv are in place; converts them to the user's airport table (any thread).
bool airportImportPackFiles(const std::string& apFile, const std::string& frFile, std::string& msg);
