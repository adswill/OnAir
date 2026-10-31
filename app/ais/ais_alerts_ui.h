// AIS: the station filter (class and ship-type group), the CPA and geofence alert settings and the list of alerts (ais_alerts_ui.cpp), called from ais_ui.cpp.
#pragma once
#include "dect2/ais/ais_alerts.h"
struct App;
const dect2::AisFilter& aisUiFilter();            // the table / map filter the user set (the default shows everything)
bool aisFilterBar(App& a);                        // a "Filter" button with its pop-up; true when the filter changed
void aisAlertsPanel(App& a);                      // the Alerts view of the AIS tab
void aisAlertsTick(App& a);                       // every frame while AIS is selected: pushes a changed alert setting to the receiver
void aisAlertsLoadPrefs(App& a);                  // at start: loads the saved settings and gives them to the receiver (alerts also run with the AIS tab closed)
