// The records board: personal best distances of every mode (core: dect2/common/records.h), kept in <data folder>/stats/records.txt.
#pragma once
#include <string>
// a position heard (distance from the receiver); band "" except for HF modes. A new record shows a short note.
void recordsOffer(const char* category, const std::string& band, double km, const std::string& id, const std::string& detail);
// distance from the receiver position; -1 when it is not set
double recordsDistKm(double lat, double lon);
void recordsPanel();   // the board (ADS-B and AIS History)
