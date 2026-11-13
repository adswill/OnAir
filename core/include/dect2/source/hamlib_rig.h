// Hamlib CAT rig entries the user adds (serial port + model). IQ/audio uses a separate sound-card path when configured.
#pragma once
#include "source.h"
#include <string>
#include <vector>

namespace dect2 {

struct HamlibRig {
    int model = 0;              // Hamlib rig model number (rigctl -l), e.g. 3073 for IC-7300, 3079 for IC-R8600
    std::string path;           // serial device: /dev/cu.usbmodem…, COM3, …
    std::string label;          // optional display name (empty: built from model + path)
    std::string audioId;        // AudioIn device id for IQ/audio (empty: silence at 48 kHz, CAT only)
    int baud = 0;               // serial speed; 0 = the model's own default (ignored for a network rig)
    // the transmit profile of this rig (the Ham view): how PTT is keyed, the data mode, the sound card the TX audio goes to, split
    int pttType = 0;            // 0 CAT, 1 RTS, 2 DTR, 3 VOX (no PTT command: the rig keys itself from the audio)
    std::string pttPath;        // the port whose RTS / DTR line keys the rig ("" = the CAT port)
    std::string dataMode;       // the rig mode for the digital modes ("" = PKTUSB)
    std::string audioOutId;     // the sound card output that feeds the rig's audio input ("" = not paired yet)
    bool split = false;         // transmit through the TX VFO (split), the receive VFO stays
    bool operator==(const HamlibRig& o) const {
        return model == o.model && path == o.path && label == o.label && audioId == o.audioId && baud == o.baud && pttType == o.pttType &&
               pttPath == o.pttPath && dataMode == o.dataMode && audioOutId == o.audioOutId && split == o.split;
    }
};

// prefs "hamlibRigs": "model,path,label,audioId,baud,pttType,pttPath,dataMode,audioOutId,split;…" (older entries have four or five fields)
std::string hamlibRigsToString(const std::vector<HamlibRig>& v);
std::vector<HamlibRig> hamlibRigsFromString(const std::string& s);

void setHamlibRigs(const std::vector<HamlibRig>& v);
std::vector<HamlibRig> hamlibRigs();

// DeviceInfo::serial is "model:path" (path after the first colon)
// With a baud rate: "model@baud:path". A path with a colon is a network rig ("localhost:4532" with model 2, rigctld).
std::string hamlibRigSerial(int model, const std::string& path, int baud = 0);
bool parseHamlibSerial(const std::string& serial, int& model, std::string& path);
bool parseHamlibSerial(const std::string& serial, int& model, std::string& path, int& baud);

// The modes the "hamlib_mode" radio setting offers, in Hamlib's spelling (also what the interface maps a mode turned on the rig onto).
const std::vector<std::string>& hamlibModeChoices();

std::string hamlibRigDeviceName(const HamlibRig& r);
std::vector<DeviceInfo> hamlibRigDevices(const std::vector<HamlibRig>& v);

bool hamlibSupported();   // false when built without Hamlib
// The Hamlib library itself: always there when it was linked; the Windows packages load libhamlib-4.dll at run time (#495), and without it
// this is false with the reason (what to put where) in *why.
bool hamlibLibraryReady(std::string* why = nullptr);

// What the running Hamlib source last learned from the rig, for the interface: the dial and mode the operator turned on the rig.
// seq grows each time the RIG changed it (not when OnAir set it), so the interface applies each change once.
struct HamlibRigState {
    bool active = false;       // a Hamlib source is running
    bool connected = false;    // the CAT link answers; false while it is being re-opened
    double hz = 0;             // the rig's dial (0 = not read yet)
    std::string mode;          // Hamlib's mode name ("USB", "WFM", ...)
    uint64_t seq = 0;
    std::string status;        // "" when fine, else why the link is down (shown to the operator)
};
void setHamlibRigState(const HamlibRigState& s);
HamlibRigState hamlibRigState();

std::unique_ptr<IqSource> makeHamlibSource(const DeviceInfo& d);
std::unique_ptr<TxSink> makeHamlibTxSink(const DeviceInfo& d);

} // namespace dect2
