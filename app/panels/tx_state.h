// Shared state and entry points of the transmit panel: one TxUi, the request builder, the start / stop calls and the block drawers.
// Files: tx_ui.cpp (state, start / stop, the RX / TX switch), tx_payload_ui.cpp (the signal's own options), tx_panel.cpp (top bar, layout, shortcuts),
// tx_step_signal.cpp (the signal list), tx_step_settings.cpp (Basic), tx_params.cpp (the table of advanced parameters, presets), tx_step_send.cpp + tx_preview.cpp (preview).
#pragma once
#include "../app.h"
#include "dect2/tx/tx.h"
#include "dect2/tx/tx_iqfile.h"
#include <memory>
#include <atomic>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace txui {

using namespace dect2;

// One advanced parameter (tx_params.cpp has the table): where it sits in the panel, what it is called, its range and default, and where its value goes
enum class Sec { Modulation, Audio, Filters, Output, Timing };
enum class PType { Float, Int, Bool, Choice };
enum class Bind { Opt, ToneHz, Deviation, Preemph, Kind, Variant };   // Opt: TxRequest::opt[key]; the others are the request's own fields
struct PDef {
    const char* key = ""; Sec sec = Sec::Modulation; PType type = PType::Float; const char* label = ""; const char* tip = "";
    double def = 0, lo = 0, hi = 1; const char* fmt = "%.2f"; const char* when = nullptr; Bind bind = Bind::Opt;
    std::vector<const char*> choices; double base = 0;   // Choice: the value sent is base + the chosen index
};

struct TxUi {
    TxSession session;
    std::string modeId, radioKey;            // the transmitter's own mode and radio
    int family = -1;                         // mode-list family (same numbers as RX); -1: follow the receiver once
    char subId[32] = {};
    int dvbVariant = 1;                      // 1 DVB-T2, 2 DVB-T
    SynthConfig synth;                       // the mode's own signal options (the same fields as the receiver's built-in test signal)
    int bwIdx = 0;                           // DVB-T2 / DVB-T channel width: 8, 7, 6, 5 MHz
    double freqMhz = 0;                      // typed by the user, any value; pre-filled from the receiver when the mode is chosen
    bool autoSampleRate = true;
    double sampleRateMsps = 4;
    double offsetMhz = 0;                    // added only to the frequency sent to the radio; the typed frequency stays freqMhz
    float gainDb = 0;
    bool gainSet = false, ampOn = false, loop = true;
    int txCh = 0;                            // 0 = TX1. Shown when the radio has, or may have, more than one output
    int onceSeconds = 10;
    bool view = false;                       // TX view (never saved)
    bool claimed = false, askNotice = false;
    std::string error, radioName, modeName, fileNote;
    double freqSent = 0;
    int tickFrame = -1;
    char callsign[16] = {}, grid[8] = {}, dest[24] = {}, name[16] = {}, path[64] = {}, file[512] = {}, text[256] = {};
    char rdsRt[128] = {}, audioDevice[64] = {};
    char provider[64] = {}, network[64] = {}, svc[4][48] = {}, svcFile[4][512] = {}, svcCam[4][96] = {}, videoDevice[96] = {};
    int nsvc = 1, serviceId = 1, videoSrc = 0, svcSrc[4] = {};
    int kind = 0, baudIdx = 0, shiftIdx = 0, variant = 0, wpm = 20, dbm = 37, function = 3;
    unsigned address = 0;
    double toneHz = 1000, lat = 0, lon = 0;
    bool reverse = false, analogVoice = false, audioMute = false, prefsLoaded = false;
    int audioSrc = 2;                        // 0 mic, 1 file, 2 tone, 3 silence
    float micGain = 1.f;
    double deviationHz = 0, preemphUs = 50;
    int rdsPi = 0x4A21, slot = 1, colorCode = 1, talkgroup = 0;
    bool carrierHeld = false, carrierOnly = false;
    Waterfall wf;
    bool wfInit = false;
    float specFrac = 0.45f;
    std::vector<double> specX, specY;
    // the guided panel
    bool dryRun = false;                     // "Test (dry run, no RF)": the generator runs into a null sink; no radio is opened or claimed
    bool popped = false;                     // the panel is in a window of its own
    bool pendingStart = false, pendingCarrier = false;   // Start was pressed: the transmission starts on the next frame so "Preparing" is shown
    bool sessionDry = false;                 // the running session is a test
    double startedAt = 0;                    // ImGui time of the start (the elapsed time shown)
    double appliedMhz = 0; float appliedGain = 0;   // what the running radio has now (a retune is offered when the fields differ)
    char search[64] = {};                    // the signal list's search box
    std::string info;                        // an inline note under the header (not an error)
    struct Recent { std::string id; double mhz = 0; float gain = 0; };
    std::vector<Recent> recent;              // the last signals sent (newest first), saved in the settings
    bool devAutoStart = false;               // dev flag --txdry: start the test (no RF) at the first frame
    float meterPeak = 0, meterRms = 0;       // smoothed meters of what is shown
    // the advanced parameters of the chosen signal
    std::vector<PDef> params;                // its table
    std::string paramsKey, pvMode;           // what the table was built for; the signal the values belong to
    std::map<std::string, double> pv;        // the values by key
    char presetName[32] = {}, presetNew[32] = {};
    bool advOpen = false;                    // the Advanced block is open
    bool linkRx = false;                     // the transmit frequency follows the receiver's
    bool pttHold = false;                    // voice: transmit only while the PTT button is held
    bool pttDown = false;
    // the destination of the signal: 0 a radio, 1 an IQ file (no radio is opened or claimed, no radio-only notice applies)
    int destKind = 0;
    char ifPath[512] = {};                   // the folder (or a file name without extension) the IQ file goes to
    int ifFormat = 0;                        // IqFileFormat
    double ifRateHz = 48000;                 // the sample rate of the file: the signal is made (or resampled) at this rate
    bool sessionFile = false;                // the running session writes a file
    std::string ifPathUsed;                  // the data file of the running or last session
    std::shared_ptr<IqFileTxCounters> ifCounters;   // bytes and samples written, kept by the sink
    // KAAL: the identity beacon and the power written in it (callsign, grid, latitude and longitude, bio text are the shared fields above)
    bool kaalBeacon = true;
    double kaalPower = 10;
    int kaalBitrate = 16;                    // voice codec 12 / 16 / 24 / 32 kbit/s
    int kaalFrame = 3;                       // 3 = the v3 voice stream, 4 = frame version 4 (four slots, 20 kHz, docs/KAAL-NEXT.md)
    int kaalV4Profile = 2;                   // v4 voice profile: 1 = 8.0, 2 = 6.0, 3 = 4.8, 4 = 3.2 kbit/s
    int kaalV4Voice = 1;                     // v4: slots that carry the microphone (0 .. 4)
    bool kaalLoaded = false;
    std::string kaalSaved;
    // KAIT: the profile (0 balanced, 1 robust, 2 quality, 3 direct); the callsign and the picture file are the shared fields above
    int kaitProfile = 0;
    bool kaitLoaded = false;
    std::string kaitSaved;
    // KAVB: the profile (0 .. 3 = A .. D), the FEC group profile (0 .. 7), the length of the synthetic test stream, and a file whose bytes are the payload
    int kavbProfile = 0, kavbFec = 1, kavbSeconds = 5;
    char kavbFile[512] = {};
    bool kavbLoaded = false;
    std::string kavbSaved;
    // KAPP: the profile (0 .. 8), the passes of the one-way transmission, and a file to send instead of the text
    // KAWS: kind (0 chat, 1 short beacon, 2 full beacon), profile (0 .. 5; 6 and 7 are voice), passes, wait for the slot; callsign, grid, recipient (dest), text are the shared fields
    int kawsKind = 0, kawsProfile = 3, kawsPasses = 2, kawsEirp = 30;
    bool kawsAlign = true, kawsLoaded = false;
    std::string kawsSaved;
    int kappProfile = 2, kappPasses = 2;
    char kappFile[512] = {};
    bool kappLoaded = false;
    std::string kappSaved;
    // KAFT: the profile (0 .. 9), the width (0 1 MHz, 1 500 kHz, 2 200 kHz), the passes, and the file to send
    int kaftProfile = 2, kaftWidth = 2, kaftPasses = 2;
    char kaftFile[512] = {};
    bool kaftLoaded = false;
    std::string kaftSaved;
};

TxUi& ui();                                            // the one transmit state
extern const double kTxBw[4];                          // DVB channel widths: 8, 7, 6, 5 MHz
extern const ImVec4 kRed;
int familyOf(const TxModeSpec& m);                     // engine family of a transmit mode
const TxModeSpec* rxMode(const App& a);                // the transmit mode of the receiver's current selection
void ensureTxMode(App& a, TxUi& s);                    // first use: follow the receiver's mode once
void chooseMode(App& a, TxUi& s, const TxModeSpec& m); // a mode chosen for transmitting
std::vector<int> txRadios(const App& a);               // indices of the radios that can transmit
int rxHolder(const DeviceInfo& d);                     // the receiver holding a radio (0 none)
void release(TxUi& s);                                 // gives the claimed radio back
inline bool noRadio(const TxUi& s) { return s.dryRun || s.destKind == 1; }   // a test or an IQ file: no radio is opened or claimed
void loadPayloadPrefs(TxUi& s);                        // callsign, grid, text, offset, recent list from the settings
void saveStr(const char* key, const char* v);        // one text into the settings
void saveRecent(const TxUi& s);                        // writes the recent list
void pushRecent(TxUi& s, const std::string& id, double mhz, float gain);
TxRequest makeReq(const TxUi& s, const TxModeSpec& mode, bool carrier);
// the advanced parameters (tx_params.cpp)
const std::vector<PDef>& paramsFor(TxUi& s, const TxModeSpec& m);
void paramsEnter(TxUi& s, const TxModeSpec& m);        // the mode was chosen: defaults, then the values it had last time
void paramsApply(const TxUi& s, TxRequest& r);         // the values into the request
void advancedPanel(TxUi& s, const TxModeSpec& mode, bool running);
void audioSourceRow(TxUi& s, bool running);            // the audio input: microphone, WAV / MP3 file, tone, silence (tx_payload_ui.cpp)
bool isVoiceId(const std::string& id);
bool isFtxId(const std::string& id);
bool isKbdId(const std::string& id);
bool isTvId(const std::string& id);
void payloadPanel(TxUi& s, const TxModeSpec& mode, bool running);   // the signal's own options (tx_payload_ui.cpp)
const DeviceInfo* bindRadio(App& a, TxUi& s, int& conflict);        // the chosen transmit radio (nullptr: none) and the receiver that holds it
bool canStart(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode);   // everything needed to press Start is there
bool startTx(App& a, TxUi& s, const DeviceInfo* radio, const TxModeSpec* mode, bool carrier);
void stopTx(App& a, TxUi& s, const char* why);         // stops, gives the radio back, logs why
void destinationPanel(TxUi& s, const TxModeSpec& mode, bool running);   // Radio or IQ file, the folder, the format, the rate and what has been written (tx_ui.cpp)
void kavbPanel(TxUi& s, bool running);                // KAVB: callsign, file or test stream, profile A to D, FEC profile (kavb_ui.cpp)
void kawsPanel(TxUi& s, bool running);                // KAWS: callsign, recipient, message, kind, profile, passes, slot timing (app/kaws/kaws_ui.cpp)
void kappPanel(TxUi& s, bool running);                // KAPP: callsign, message or file, profile, passes (kapp_ui.cpp)
void kaftPanel(TxUi& s, bool running);                // KAFT: callsign, file, width, profile, passes (kaft_ui.cpp)
void kaalPanel(TxUi& s, bool running);                // KAAL: callsign, grid, position, power, bio text, beacon (kaal_tx_ui.cpp)
void kaitPanel(TxUi& s, bool running);                // KAIT: callsign, picture file, profile (app/kait/kait_ui.cpp)
std::string kaitPictureFile(const std::string& path); // the file the KAIT generator reads: PNG / PPM as they are, other pictures converted to a PPM
void txNoticePopup(TxUi& s);                           // the one-time notice before the first transmission

// the panel (tx_panel.cpp): the top bar is txTopBar (app.h); the body has the signal list, the Basic block with Advanced folded, and the preview
enum class Note { Info, Warn, Error };
void banner(Note kind, const char* text);              // an inline message box (no popup)
void stepSignal(App& a, TxUi& s, bool running, float listH);                                                         // tx_step_signal.cpp: the signal list
void stepSettings(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode, bool running);   // tx_step_settings.cpp: Basic + Advanced
TxIqStreamConfig txIqStreamFromPrefs();   // tx_step_settings.cpp: the "Send the IQ over TCP" setting (tx_iqstream.h)
void stepSend(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode, bool running);       // tx_step_send.cpp + tx_preview.cpp: the preview
void drawStatusHeader(App& a, TxUi& s, const DeviceInfo* radio, int conflict, const TxModeSpec* mode);              // tx_panel.cpp: state word and the big button

// the preview of what will be sent (tx_preview.cpp): IQ of the planned signal made on a worker thread, or the running session's tap
struct PreviewData { std::vector<cf32> iq; double rateHz = 0; std::string error; bool ready = false, busy = false; };
const PreviewData& previewFor(TxUi& s, const TxModeSpec& mode, const DeviceInfo* radio);
void previewDrop();                                    // forget it (the mode changed)

} // namespace txui
