// ISM sensors: the protocol layer. A burst arrives as a train of pulses (OOK: carrier on / off; FSK: the upper / lower tone) with their
// lengths in microseconds; every device protocol slices it its own way (PWM, PPM, Manchester, NRZ) into rows of bits and checks them.
//
// The formats follow rtl_433 (GPL-2.0, https://github.com/merbanan/rtl_433, src/devices/*.c, and its pulse_slicer.c / bitbuffer.c for the
// slicing): every decoder in ism_proto.cpp names the rtl_433 file it was written from. Only what those files implement is decoded; a
// reading leaves this layer only when its CRC, checksum or (for the few protocols without one) the repetition and constant-bit checks
// that rtl_433 uses have passed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dect2 {

struct IsmPulse {
    float pulseUs = 0;           // mark: carrier on (OOK) or the upper tone (FSK)
    float gapUs = 0;             // the space after it (the last one of a burst: long)
};

struct IsmTrain {
    bool fsk = false;
    std::vector<IsmPulse> p;
};

struct IsmField {
    std::string name, unit;      // "Temperature", "C"
    double value = 0;
    int decimals = 1;
    std::string text;            // instead of the number when not empty (a tri-state code, a manufacturer)
};

struct IsmReading {
    std::string model;           // "Bresser-6in1", rtl_433's model names
    std::string kind;            // Weather, TPMS, Remote, Energy, Meter, Sensor
    std::string id;              // as the device broadcasts it (hex or decimal, as rtl_433 prints it)
    int channel = -1;            // -1 = the device has none
    std::string channelName;     // instead of the number when the device names it (Acurite: A, B, C)
    int battery = -1;            // 1 ok, 0 low, -1 not sent
    std::string mic;             // what validated it: CRC, CHECKSUM, PARITY, REPEAT (several identical rows; the protocol has no check)
    std::vector<IsmField> fields;
    void add(const char* name, double v, const char* unit, int dec = 1) { fields.push_back({name, unit, v, dec, ""}); }
    void addText(const char* name, const std::string& t) { IsmField f; f.name = name; f.text = t; fields.push_back(f); }
};

// Every protocol that matches the train (normally none or one). Never throws; bad input gives no reading.
std::vector<IsmReading> ismDecode(const IsmTrain& t);

// The protocol list, for the UI's "supported devices" and the tests
struct IsmProtoInfo { const char* name; const char* file; const char* kind; bool fsk; };
const std::vector<IsmProtoInfo>& ismProtocols();

// The line code of an unknown train, as rtl_433's analyzer would guess it: "PWM", "PPM", "Manchester", "NRZ" or "?", with the short and
// long widths (us) it found.
struct IsmGuess { std::string code; float shortUs = 0, longUs = 0, gapUs = 0; int bits = 0; };
IsmGuess ismClassify(const IsmTrain& t);

// The check functions of rtl_433's bit_util.c, also used by the test signal generator
uint8_t ismCrc8(const uint8_t* m, unsigned n, uint8_t poly, uint8_t init);
uint8_t ismCrc4(const uint8_t* m, unsigned n, uint8_t poly, uint8_t init);   // bit_util.c crc4
uint16_t ismCrc16(const uint8_t* m, unsigned n, uint16_t poly, uint16_t init);
uint8_t ismLfsrDigest8(const uint8_t* m, unsigned n, uint8_t gen, uint8_t key);
uint8_t ismLfsrDigest8Reflect(const uint8_t* m, int n, uint8_t gen, uint8_t key);
uint16_t ismLfsrDigest16(const uint8_t* m, unsigned n, uint16_t gen, uint16_t key);

} // namespace dect2
