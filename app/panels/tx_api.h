// The transmit panel's pieces for other screens (the Ham station, a mode's own screen): choose a signal, fill its payload, and draw the same blocks the
// Transmit panel draws (the state line with ARM / TRANSMIT / STOP, What to send, Advanced, the preview). They all act on the one transmitter (txui::ui()),
// so a transmission started here is the one the Transmit panel shows, and STOP anywhere stops it. Nothing here starts a transmission by itself.
#pragma once
#include "../app.h"
#include "dect2/tx/tx_params.h"
#include <string>

namespace txapi {

enum class TxState { Idle, Armed, Preparing, OnAir, Test, Done };

bool selectMode(App& a, const std::string& modeId);                 // the transmit mode by its id (tx.h txModes); false: no such mode
std::string modeId();                                               // the chosen one ("" none)
void setText(dect2::TxPField field, const std::string& v);          // a payload text: Callsign, Grid, Text, Dest, Path, File, Name, Network
std::string text(dect2::TxPField field);
bool setParam(const std::string& key, double value);                // a number row of the chosen mode's table (tx_params.h); false: not a row of it
double param(const std::string& key, double def = 0);
void setFrequencyMhz(double mhz);                                   // the transmit frequency (unlinks it from the receiver)
double frequencyMhz();

TxState state();
bool arm(App& a);                                                   // step 1 (a radio as destination); false when nothing can be sent yet
bool transmit(App& a);                                              // step 2: only when armed (or a test / IQ file); the panel opens the radio next frame
void stop(App& a, const char* why = "by the user");                 // stops, cancels a schedule, disarms

void drawFlow(App& a);                                              // the state line and ARM / TRANSMIT / STOP
void drawWhatToSend(App& a);                                        // the Simple rows of the chosen mode
void drawAdvanced(App& a);                                          // the Advanced rows in their groups (and presets)
void drawPreview(App& a);                                           // spectrum, levels, occupied bandwidth, the log

} // namespace txapi
