// Sub-GHz sensors telemetry, shared by the receiver and the UI.
// The decoders come from rtl_433 (third_party/rtl_433, GPL-2.0-or-later); docs/SENSORS-3.7.md says which ones are supported and how they were tested.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dect2 {

constexpr size_t kSensorsDevicesMax = 500;
constexpr size_t kSensorsUnknownMax = 60;
constexpr size_t kSensorsHistoryMax = 600;      // points kept per measured value of a sensor
constexpr size_t kSensorsRecentMax = 200;       // the message log

// One value a decoder reported: a number (temperature_C, humidity, ...) or text (model, id, ...)
struct SensorField {
    std::string key;
    std::string text;       // always filled (numbers as text, 3 decimals at most)
    double num = 0;
    bool numeric = false;
};

struct SensorHistPoint { int64_t utc = 0; double v = 0; };
struct SensorSeries { std::string key; std::vector<SensorHistPoint> pts; };

// what the receiver learned about a sensor (sensors_health.h)
struct SensorHealth {
    double intervalSec = 0;      // 0 until learned (at least 3 transmissions)
    int gaps = 0;
    int64_t lastTx = 0;
    bool silent = false, outlier = false, collision = false, batteryFalling = false;
    std::string detail;          // the field that looked wrong
};

struct SensorDevice {
    std::string key;                 // model|id|channel
    std::string model, id, channel;
    std::string kind;                // weather, tpms, meter, remote, security, other
    std::vector<SensorField> latest; // the values of the last message, in the order the decoder gave them
    std::vector<SensorSeries> history;   // numeric values over time
    uint64_t count = 0;              // messages (the radios repeat each one 2 to 8 times)
    int64_t firstSeen = 0, lastSeen = 0;
    float rssiDb = -200, snrDb = 0;
    int batteryOk = -1;              // -1 unknown, 0 low, 1 ok
    std::string json;                // the last message as JSON (rtl_433 style, for export)
    SensorHealth health;
};

// A pulse train that no decoder took: what the unknown signal analyser shows
struct SensorUnknown {
    int64_t utc = 0;
    bool fsk = false;
    int pulses = 0;
    double durationMs = 0;
    float rssiDb = 0, snrDb = 0;
    double freqMHz = 0;
    std::string guess;               // modulation guess in words
    std::string flexSpec;            // a flex decoder definition for this signal (editable by the user)
    std::string bits;                // the bit rows as 0/1 text, rows separated by newlines
    std::string hex;                 // the same rows in hex
    std::string widths;              // pulse and gap widths in microseconds, as a table
    std::vector<float> pulseUs, gapUs;
    double shortUs = 0, longUs = 0, gapLimitUs = 0, resetUs = 0;
    std::string modName;             // PWM, PPM, Manchester, PCM
};

struct SensorsTelemetry {
    uint64_t seq = 0;
    int state = 0;                   // 0 nothing, 1 pulses seen, 2 sensors decoded
    float snrDb = 0, levelDb = -200;
    double cfoHz = 0;
    double timeSec = 0, inputRate = 0, workRate = 0;
    bool dataValid = false;
    uint64_t blocksOk = 0;           // decoded messages
    uint64_t blocksBad = 0;          // packages that no decoder took
    uint64_t packages = 0, ookPackages = 0, fskPackages = 0;
    unsigned protocols = 0;          // decoders running
    unsigned customProtocols = 0;
    std::vector<SensorDevice> devices;     // most recently heard first
    std::vector<SensorUnknown> unknown;    // newest first
    std::vector<std::string> recent;       // JSON lines, newest first
};

inline std::string sensorsSummary(const SensorsTelemetry& t) {
    char b[160];
    if (!t.devices.empty()) { std::snprintf(b, sizeof b, "Sensors: %zu heard, %llu messages", t.devices.size(), (unsigned long long)t.blocksOk); return b; }
    if (t.packages) { std::snprintf(b, sizeof b, "Sensors: %llu pulse packages, no sensor decoded", (unsigned long long)t.packages); return b; }
    return "Sensors: no signal";
}

} // namespace dect2
