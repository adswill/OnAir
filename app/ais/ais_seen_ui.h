// AIS vessel log over days and years (AIS-catcher statistics, ShipPlotter "seen" lists): ais_seen_ui.cpp
#pragma once
struct App;
void aisSeenTick(App& a);
void aisSeenView(App& a);
void aisArrivalsView(App& a);   // ais_arrivals_ui.cpp: the arrivals board of one port
