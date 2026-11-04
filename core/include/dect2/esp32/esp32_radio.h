// The ESP32 radio as an OnAir device: an ESP32 board (optionally with an nRF24L01+ module on its SPI bus) on a serial port, speaking the framed protocol of
// firmware/esp32/onair_esp32/onair_wire.h. Receive: nRF24 packets, BLE advertising, IEEE 802.15.4 frames, WiFi (observation only: networks, clients, signal,
// channel load). Transmit: nRF24 packets and BLE advertising, only after setTxEnable(true) (the user's Arm). Never any WiFi transmit.
// Thread model: connect() starts a reader thread; every call may be made from the interface thread; snapshot() copies the lists.
#pragma once
#include "dect2/esp32/esp32_port.h"
#include "dect2/esp32/esp32_wire.h"
#include "dect2/nrf24/nrf24_tel.h"
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dect2 {
namespace esp32 {

struct DeviceInfo {
    bool connected = false;
    int protoVersion = 0, fwMajor = 0, fwMinor = 0, chip = 0, radios = 0;   // radios: wire::RadioBit mask
    std::string name, port;
};

struct Nrf24Params {            // all the module's settings (wire::kConfig, radio 1)
    int channel = 2;            // 0..125, 2400 + N MHz
    int rateKbps = 1000;        // 250, 1000, 2000
    int addrWidth = 5;          // 3..5
    uint8_t address[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7};
    int crcBits = 16;           // 0 (off), 8, 16
    bool autoAck = true;
    int retryDelay = 0, retryCount = 3;   // 0..15 (250 us steps), 0..15
    int payloadLen = 0;         // 0 = dynamic, 1..32 static
    int power = 3;              // 0..3 = -18, -12, -6, 0 dBm
    bool promiscuous = false;
};
struct BleParams { bool advertise = false; int channelMask = 7; bool activeScan = false; int power = 4; };
struct WifiParams { bool scan = false; int channel = 0; int hopMs = 250; };   // scan: active scan; otherwise passive listen; channel 0 = hop
struct Ieee154Params { int channel = 15; bool promiscuous = true; };

struct BleAdv { std::string addr; int rssi = 0, pduType = 0, channel = 0; std::string dataHex, name; uint64_t count = 0; int64_t lastSeen = 0; };
struct Ieee154Frame { int64_t utc = 0; int rssi = 0, channel = 0; std::string hex; };
struct WifiAp {
    std::string bssid, ssid, vendor;
    int channel = 0, rssi = -127, auth = 255;      // auth: wire code (0 open .. 7 enterprise, 255 unknown)
    uint64_t count = 0; int64_t firstSeen = 0, lastSeen = 0;
    std::deque<float> history;                     // RSSI of the last sightings (newest last)
    int clients = 0;
};
struct WifiClient {
    std::string mac, vendor, bssid;                // bssid empty: probing only
    int rssi = -127; uint64_t count = 0; int64_t lastSeen = 0;
    std::vector<std::string> probed;               // SSIDs it asked for (up to 8)
    bool randomised = false;                       // locally administered address (phones rotate these)
};
struct WifiSnapshot {
    std::vector<WifiAp> aps;                       // strongest first
    std::vector<WifiClient> clients;               // newest first
    uint64_t beacons = 0, probes = 0, deauths = 0, dataFrames = 0;
    int chanAps[15] = {0};                         // networks per channel 1..14 (index = channel)
    int chanBestDb[15] = {0};                      // strongest RSSI on the channel (dBm, 0 = none)
    int chanFrames[15] = {0};
};

struct Snapshot {
    DeviceInfo info;
    Nrf24Telemetry nrf;                            // frames (newest first) and addresses
    std::vector<BleAdv> ble;
    std::vector<Ieee154Frame> ieee;
    WifiSnapshot wifi;
    uint32_t devUptimeMs = 0, devFrames = 0, devBad = 0, hostBad = 0;
    std::vector<std::string> log;                  // device log lines and notes, newest last
};

std::string authName(int code);
std::string vendorOf(const std::string& mac);      // a short built-in OUI list (incomplete); "private address" for randomised MACs, "" when unknown
std::string macStr(const uint8_t* m);

class Radio {
public:
    Radio();
    ~Radio();
    // takes the port, does the HELLO handshake (a timeout or a wrong answer fails with err). Transmit is off after this.
    bool connect(std::unique_ptr<SerialPort> port, std::string& err);
    void disconnect();
    bool connected() const;
    // tries each candidate port (or only the given one): HELLO and wait for HELLO_ACK; true when a board answered and is connected
    bool autoConnect(std::string& err, const std::string& onlyPort = "", int baud = 115200);

    bool configure(const Nrf24Params& p, std::string& err);
    bool configure(const BleParams& p, std::string& err);
    bool configure(const WifiParams& p, std::string& err);
    bool configure(const Ieee154Params& p, std::string& err);
    bool start(int radio, std::string& err);       // wire::Radio
    bool stop(std::string& err);
    bool setTxEnable(bool on, std::string& err);   // the Arm button
    bool sendNrf24(const std::vector<uint8_t>& data, bool noAck, std::string& err);   // result: lastTxResult()
    bool sendBle(int advType, const uint8_t addr[6], const std::vector<uint8_t>& data, std::string& err);
    int lastTxResult() const;                      // 0 sent / acknowledged, 1 no acknowledge, 2 error, -1 none yet
    void clearResults();
    Snapshot snapshot() const;
    // the warning line shown with the transmit controls (policy: a warning, never a refusal)
    static const char* txWarning();
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

} // namespace esp32
} // namespace dect2
