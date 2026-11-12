// NFC / RFID (13.56 MHz) in the engine: the demodulator of nfc_rx.h and the protocol layer of nfc_proto.h behind the engine's
// interface. Engine stdMode 150, active standard 149 (family 148, "NFC / RFID 13.56 MHz"). RECEIVE ONLY in Phase 1; nothing is
// transmitted and nothing is cloned. The active reader (Phase 2) lives in nfc_tx.h and only runs through the existing TX gate.
#pragma once
#include "dect2/nfc/nfc_proto.h"
#include "dect2/nfc/nfc_rx.h"
#include "dect2/engine/mode_tuning.h"
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {

// One line in the frame log.
struct NfcLogEntry {
    double timeSec = 0;
    bool fromReader = true;
    int proto = 0;           // 0 Type A, 1 Type B, 2 ISO 15693
    std::string hex;         // the frame bytes as hex
    std::string note;        // what the protocol layer called it
    bool crcChecked = false, crcOk = false;
    bool parityOk = true;
    float levelDb = -200;
};

// A tag the receiver has pieced together from an anticollision / select sequence (or an inventory answer).
struct NfcTag {
    std::string uid;         // hex, most significant byte first
    std::string type;        // "ISO 14443-A", "ISO 14443-A (ISO 14443-4)", "ISO 14443-B", "ISO 15693", ...
    uint16_t atqa = 0;
    uint8_t sak = 0;
    bool haveAtqa = false, haveSak = false;
    std::string ats;         // historical bytes as hex ("" when none)
    double firstSeen = 0, lastSeen = 0;
    int seenCount = 0;
};

// A decoded NDEF record for the panel.
struct NfcNdef {
    std::string uid;         // which tag it came from
    int tnf = 0;
    std::string type;        // "T" / "U" / ...
    std::string text;        // text records
    std::string uri;         // URI records (prefix expanded)
    std::string lang;
};

struct NfcTelemetry {
    uint64_t seq = 0;
    int state = 0;           // 0 no input, 1 field present (nothing decoded), 2 frames seen, 3 at least one tag identified
    double cfoHz = 0;        // not measured
    float snrDb = 0;
    float levelDb = -200;    // field (carrier) level, dBFS
    bool dataValid = false;  // at least one frame decoded
    uint64_t blocksOk = 0;   // frames decoded
    uint64_t blocksBad = 0;  // CRC / parity failures
    float subcarrierDb = -200;   // strongest load-modulation subcarrier seen, dBFS
    std::vector<NfcLogEntry> log;    // newest last, capped
    std::vector<NfcTag> tags;        // the tag table
    std::vector<NfcNdef> ndef;       // decoded NDEF records
    double inputRate = 0;
    double centerHz = 0;
};

// 4.0: the protocols that OnAir lists as modes of their own (docs/VIEWS-4.0.md): views onto this receiver. family N, active standard N + 1, engine standard N + 2, poll slot N.
struct NfcView {
    int family;
    const char* id;          // dect2cli --standard
    const char* name;        // the mode-list name
    int proto;               // NfcLogEntry::proto
};
const NfcView* nfcViews(int& count);
const NfcView* nfcViewForActive(int activeStd);
const NfcView* nfcViewForStd(int stdMode);
ModeTuning nfcViewTuning(int i);
#define DECT2_NFCVIEW_ACTIVE_CASES case 474: case 477: case 483:

class NfcReceiver {
public:
    NfcReceiver();
    ~NfcReceiver();

    void configure(double inputRateHz);
    void setSignalOffset(double hz);     // where the 13.56 MHz carrier sits relative to 0 Hz in the input (about -tuneOffsetHz)
    void setCenterHz(double hz);
    bool ready() const;
    void reset();
    void feed(const std::complex<float>* x, size_t n);
    bool telemetry(NfcTelemetry& out, uint64_t lastSeq);
    void setLogCallback(std::function<void(const std::string&)> cb);
    // 4.0: a view onto one protocol: bit k of the mask = NfcLogEntry::proto k (0 Type A, 1 Type B, 2 ISO 15693); only its frames, tags and NDEF records are reported (telemetry()), 0 = every protocol.
    void setProtoMask(uint32_t mask);

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

ModeTuning nfcTuning();

// One line for the top bar and the receiver tab
std::string nfcSummary(const NfcTelemetry& t);

}  // namespace dect2
