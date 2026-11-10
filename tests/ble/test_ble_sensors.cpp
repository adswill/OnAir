// BLE sensor payloads in the clear: BTHome v2, pvvx ATC custom format, Ruuvi RAWv2. Vectors from bthome.io, the pvvx README and the docs.ruuvi.com test vector. Simulated data only.
#include "dect2/ble/ble_proto.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace dect2::ble;
#include "../common/test_util.h"
#undef CHECK
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static std::string svc(uint16_t uuid, std::vector<uint8_t> d) {
    std::vector<uint8_t> ad;
    ad.push_back((uint8_t)(d.size() + 3)); ad.push_back(0x16); ad.push_back((uint8_t)uuid); ad.push_back((uint8_t)(uuid >> 8));
    ad.insert(ad.end(), d.begin(), d.end());
    Adv o; parseAd(ad.data(), ad.size(), o);
    return o.beacon;
}
static std::string mfr(uint16_t co, std::vector<uint8_t> d) {
    std::vector<uint8_t> ad;
    ad.push_back((uint8_t)(d.size() + 3)); ad.push_back(0xFF); ad.push_back((uint8_t)co); ad.push_back((uint8_t)(co >> 8));
    ad.insert(ad.end(), d.begin(), d.end());
    Adv o; parseAd(ad.data(), ad.size(), o);
    return o.beacon;
}
static bool has(const std::string& s, const char* t) { bool ok = s.find(t) != std::string::npos; if (!ok) printf("in: %s\n", s.c_str()); return ok; }

int main() {
    // bthome.io example: temperature 25.00 C (0x02) and humidity 50.55 %
    auto b = svc(0xFCD2, {0x40, 0x02, 0xC4, 0x09, 0x03, 0xBF, 0x13});
    CHECK(has(b, "temperature 25 C") && has(b, "humidity 50.55 %"));
    // battery, signed sint8 temperature -5, unknown object stops the parse
    b = svc(0xFCD2, {0x40, 0x01, 0x64, 0x57, 0xFB, 0x77, 0x01});
    CHECK(has(b, "battery 100 %") && has(b, "temperature -5 C"));
    CHECK(has(svc(0xFCD2, {0x41, 0x02, 0xC4}), "encrypted"));
    CHECK(svc(0xFCD2, {0x20, 0x02}).empty() || !has(svc(0xFCD2, {0x20, 0x02}), "temperature"));   // not version 2
    // pvvx: 25.50 C, 60.00 %, 3000 mV, 88 %
    b = svc(0x181A, {1,2,3,4,5,6, 0xF6,0x09, 0x70,0x17, 0xB8,0x0B, 88, 7, 0});
    CHECK(has(b, "25.50 C") && has(b, "60.00 %") && has(b, "3000 mV"));
    // Ruuvi data format 5 test vector: 24.3 C, 53.49 %, 1000.44 hPa, 2.977 V
    b = mfr(0x0499, {0x05, 0x12, 0xFC, 0x53, 0x94, 0xC3, 0x7C, 0x00, 0x04, 0xFF, 0xFC, 0x04, 0x0C, 0xAC, 0x36, 0x42, 0x00, 0xCD, 0xCB, 0xB8, 0x33, 0x4C, 0x88, 0x4F});
    CHECK(has(b, "24.30 C") && has(b, "53.49 %") && has(b, "1000.44 hPa"));
    return testSummary("ble_sensors");
}
