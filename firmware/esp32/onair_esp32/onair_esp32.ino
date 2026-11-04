// OnAir ESP32 radio firmware (protocol version 1, see onair_wire.h and docs/ESP32-NRF24-DESIGN.md).
// Licence: GPL-3.0-or-later. Written from the public nRF24L01+ datasheet and the ESP-IDF / Arduino-ESP32 APIs; no third-party library code is copied.
// NOT TESTED ON HARDWARE: the protocol logic is tested in OnAir's loopback test (onair_core.h); this file, the hardware layer, has never run on a chip.
// Build: Arduino IDE (board "ESP32 Dev Module" / S3 / C3 / C6, Arduino-ESP32 core 3.x) or PlatformIO (platformio.ini next to this folder).
#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <BLEDevice.h>
#include <BLEAdvertising.h>
#include <BLEScan.h>
#include "onair_core.h"

// nRF24L01+ wiring (change to suit). Put 10 uF across the module's supply.
static const int PIN_SCK = 18, PIN_MISO = 19, PIN_MOSI = 23, PIN_CSN = 5, PIN_CE = 4;

// ---- a small queue of received packets (radio + kRx* payload), filled by callbacks, emptied by pollRx
struct QItem { uint8_t radio; uint8_t len; uint8_t d[200]; };
static QItem q[24];
static volatile uint8_t qh = 0, qt = 0;
static portMUX_TYPE qmux = portMUX_INITIALIZER_UNLOCKED;
static void qpush(uint8_t radio, const uint8_t* d, size_t n) {
    if (n > sizeof(QItem::d)) return;
    portENTER_CRITICAL(&qmux);
    const uint8_t nx = (qh + 1) % 24;
    if (nx != qt) { q[qh].radio = radio; q[qh].len = (uint8_t)n; memcpy(q[qh].d, d, n); qh = nx; }
    portEXIT_CRITICAL(&qmux);
}
static void p32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

// ---- nRF24L01+ (register map and SPI commands from the Nordic datasheet)
namespace nrf {
enum { CONFIG = 0x00, EN_AA = 0x01, EN_RXADDR = 0x02, SETUP_AW = 0x03, SETUP_RETR = 0x04, RF_CH = 0x05, RF_SETUP = 0x06, STATUS = 0x07,
       RX_ADDR_P0 = 0x0A, TX_ADDR = 0x10, RX_PW_P0 = 0x11, FIFO_STATUS = 0x17, DYNPD = 0x1C, FEATURE = 0x1D };
enum { R_REG = 0x00, W_REG = 0x20, R_RX_PL_WID = 0x60, R_RX_PAYLOAD = 0x61, W_TX_PAYLOAD = 0xA0, W_TX_PAYLOAD_NOACK = 0xB0, FLUSH_TX = 0xE1, FLUSH_RX = 0xE2, NOP = 0xFF };
static SPISettings spiSet(4000000, MSBFIRST, SPI_MODE0);
static uint8_t cmd(uint8_t c, const uint8_t* out, uint8_t* in, size_t n) {
    SPI.beginTransaction(spiSet); digitalWrite(PIN_CSN, LOW);
    const uint8_t st = SPI.transfer(c);
    for (size_t i = 0; i < n; i++) { const uint8_t v = SPI.transfer(out ? out[i] : NOP); if (in) in[i] = v; }
    digitalWrite(PIN_CSN, HIGH); SPI.endTransaction();
    return st;
}
static void wr(uint8_t r, uint8_t v) { cmd(W_REG | r, &v, nullptr, 1); }
static uint8_t rd(uint8_t r) { uint8_t v = 0; cmd(R_REG | r, nullptr, &v, 1); return v; }
static bool present() { wr(SETUP_AW, 0x03); return rd(SETUP_AW) == 0x03; }
static uint8_t gCrc = 2, gRate = 1, gWidth = 5, gAddr[5] = {0xE7, 0xE7, 0xE7, 0xE7, 0xE7}, gPlen = 0, gPromisc = 0, gAck = 1, gCh = 2, gRetr = 0x03, gPower = 3;
static bool rxOn = false;
static void apply(bool tx) {
    digitalWrite(PIN_CE, LOW);
    const uint8_t crcbits = gCrc == 0 ? 0 : gCrc == 1 ? 0x08 : 0x0C;     // EN_CRC, CRCO
    const uint8_t dr = gRate == 0 ? 0x20 : gRate == 2 ? 0x08 : 0x00;     // RF_DR_LOW / RF_DR_HIGH
    wr(CONFIG, 0x02 | crcbits | (tx ? 0 : 0x01));                        // PWR_UP, PRIM_RX
    wr(RF_CH, gCh); wr(RF_SETUP, dr | (gPower << 1));
    wr(FEATURE, 0x07); wr(EN_RXADDR, 0x01);                              // EN_DPL, EN_ACK_PAY, EN_DYN_ACK
    uint8_t addr[5];
    if (gPromisc && !tx) { wr(SETUP_AW, 0x00); addr[0] = 0x55; addr[1] = 0xAA; wr(CONFIG, 0x02 | 0x01); wr(EN_AA, 0); wr(RX_PW_P0, 32); wr(DYNPD, 0); cmd(W_REG | RX_ADDR_P0, addr, nullptr, 2); }
    else {
        wr(SETUP_AW, gWidth - 2);
        for (int i = 0; i < gWidth; i++) addr[i] = gAddr[i];
        cmd(W_REG | RX_ADDR_P0, addr, nullptr, gWidth); cmd(W_REG | TX_ADDR, addr, nullptr, gWidth);
        wr(EN_AA, gAck ? 1 : 0); wr(SETUP_RETR, gRetr);
        if (gPlen == 0) { wr(DYNPD, 1); } else { wr(DYNPD, 0); wr(RX_PW_P0, gPlen); }
    }
    cmd(FLUSH_RX, nullptr, nullptr, 0); cmd(FLUSH_TX, nullptr, nullptr, 0); wr(STATUS, 0x70);
}
} // namespace nrf

// ---- WiFi: promiscuous listener and active scan (observation only)
static uint8_t wifiMode = 0, wifiChan = 0; static uint16_t wifiHop = 250; static uint32_t wifiLast = 0, wifiCh = 1; static bool wifiOn = false;
static uint8_t authCode(wifi_auth_mode_t a) {
    switch (a) { case WIFI_AUTH_OPEN: return 0; case WIFI_AUTH_WEP: return 1; case WIFI_AUTH_WPA_PSK: return 2; case WIFI_AUTH_WPA2_PSK: return 3;
        case WIFI_AUTH_WPA_WPA2_PSK: return 4; case WIFI_AUTH_WPA3_PSK: return 5; case WIFI_AUTH_WPA2_WPA3_PSK: return 6;
        case WIFI_AUTH_WPA2_ENTERPRISE: return 7; default: return 255; }
}
static void wifiRx(void* buf, wifi_promiscuous_pkt_type_t type) {
    const wifi_promiscuous_pkt_t* pk = (const wifi_promiscuous_pkt_t*)buf;
    const uint8_t* f = pk->payload;
    const int len = pk->rx_ctrl.sig_len;
    if (len < 24) return;
    const uint8_t fc = f[0], ftype = (fc >> 2) & 3, sub = fc >> 4;
    uint8_t kind = 255, bssid[6], mac[6], auth = 255, ssidLen = 0; uint16_t reason = 0; char ssid[33] = {0};
    memcpy(mac, f + 10, 6); memcpy(bssid, f + 16, 6);
    if (ftype == 0) {
        kind = sub == 8 ? 0 : sub == 4 ? 1 : sub == 5 ? 2 : sub == 12 ? 3 : sub == 10 ? 4 : 255;
        if (kind == 3 || kind == 4) { if (len >= 26) reason = f[24] | (f[25] << 8); }
        if (kind <= 2) {   // tagged parameters after the fixed part (beacon / probe response 36, probe request 24)
            int o = kind == 1 ? 24 : 36;
            bool rsn = false, wpa = false, sae = false, psk = false, ent = false;
            while (o + 2 <= len - 4) {
                const int id = f[o], l = f[o + 1];
                if (o + 2 + l > len) break;
                if (id == 0 && l <= 32) { ssidLen = l; memcpy(ssid, f + o + 2, l); }
                if (id == 48 && l >= 8) {   // RSN: AKM suite type 8 = SAE (WPA3), 1 = 802.1X (enterprise), 2 = PSK
                    rsn = true;
                    const uint8_t* r = f + o + 2; const int pc = r[6] | (r[7] << 8); const int ao = 8 + pc * 4;
                    if (ao + 2 <= l) { const int ac = r[ao] | (r[ao + 1] << 8);
                        for (int i = 0; i < ac && ao + 2 + i * 4 + 4 <= l; i++) { const uint8_t t = r[ao + 2 + i * 4 + 3]; if (t == 8) sae = true; else if (t == 1) ent = true; else if (t == 2) psk = true; } }
                }
                if (id == 221 && l >= 5 && f[o + 2] == 0x00 && f[o + 3] == 0x50 && f[o + 4] == 0xF2 && f[o + 5] == 1) wpa = true;
                o += 2 + l;
            }
            if (kind == 0 || kind == 2) {
                const bool priv = f[34] & 0x10;   // capability: privacy
                auth = ent ? 7 : (rsn && sae && psk) ? 6 : (rsn && sae) ? 5 : rsn ? (wpa ? 4 : 3) : wpa ? 2 : priv ? 1 : 0;
            }
        }
        }
    } else if (ftype == 2) {   // data: the client is the transmitter when it goes to the access point (ToDS)
        kind = 5;
        if (fc & 0x01 && !(fc & 0x02)) { memcpy(bssid, f + 4, 6); memcpy(mac, f + 10, 6); } else if (fc & 0x02 && !(fc & 0x01)) { memcpy(bssid, f + 10, 6); memcpy(mac, f + 4, 6); } else return;
    }
    if (kind == 255) return;
    uint8_t d[4 + 1 + 1 + 1 + 6 + 6 + 2 + 1 + 1 + 32]; size_t n = 0;
    p32(d, millis()); n = 4; d[n++] = (uint8_t)pk->rx_ctrl.rssi; d[n++] = pk->rx_ctrl.channel; d[n++] = kind;
    memcpy(d + n, mac, 6); n += 6; memcpy(d + n, bssid, 6); n += 6; d[n++] = reason; d[n++] = reason >> 8; d[n++] = auth; d[n++] = ssidLen; memcpy(d + n, ssid, ssidLen); n += ssidLen;
    qpush(onair::wire::kRadioWifi, d, n);
}

// ---- BLE advertising scan and advertise (Arduino-ESP32 BLE library)
static BLEScan* bleScan = nullptr; static bool bleAdv = false, bleActive = false;
class AdvCb : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice d) override {
        uint8_t o[4 + 1 + 1 + 1 + 6 + 1 + 62]; size_t n = 0;
        p32(o, millis()); n = 4; o[n++] = (uint8_t)d.getRSSI(); o[n++] = 37; o[n++] = d.getAddressType() ? 1 : 0;
        const uint8_t* a = *d.getAddress().getNative(); for (int i = 0; i < 6; i++) o[n++] = a[5 - i];
        const int pl = d.getPayloadLength() > 62 ? 62 : d.getPayloadLength(); o[n++] = pl; memcpy(o + n, d.getPayload(), pl); n += pl;
        qpush(onair::wire::kRadioBle, o, n);
    }
};
static AdvCb advCb;

// ---- the hardware layer for the dispatcher
class EspHal : public onair::Hal {
public:
    bool nrfOk = false;
    uint32_t millis() override { return ::millis(); }
    void serialWrite(const uint8_t* p, size_t n) override { Serial.write(p, n); }
    uint8_t chip() override {
#if CONFIG_IDF_TARGET_ESP32S3
        return onair::wire::kChipEsp32S3;
#elif CONFIG_IDF_TARGET_ESP32C3
        return onair::wire::kChipEsp32C3;
#elif CONFIG_IDF_TARGET_ESP32C6
        return onair::wire::kChipEsp32C6;
#elif CONFIG_IDF_TARGET_ESP32H2
        return onair::wire::kChipEsp32H2;
#elif CONFIG_IDF_TARGET_ESP32S2
        return onair::wire::kChipEsp32S2;
#else
        return onair::wire::kChipEsp32;
#endif
    }
    uint8_t radios() override {
        uint8_t m = 0;
        if (nrfOk) m |= onair::wire::kBitNrf24;
#if !CONFIG_IDF_TARGET_ESP32S2 && !CONFIG_IDF_TARGET_ESP32H2
        m |= onair::wire::kBitWifi;
#endif
#if !CONFIG_IDF_TARGET_ESP32S2
        m |= onair::wire::kBitBle;
#endif
        return m;   // 802.15.4 (C6, H2) is not wired up in this version
    }
    const char* name() override { return "OnAir ESP32 radio"; }
    uint8_t configure(uint8_t radio, const uint8_t* p, size_t n) override {
        using namespace onair::wire;
        if (radio == kRadioNrf24) {
            nrf::gCh = p[0]; nrf::gRate = p[1]; nrf::gWidth = p[2]; memcpy(nrf::gAddr, p + 3, 5); nrf::gCrc = p[8]; nrf::gAck = p[9];
            nrf::gRetr = (p[10] << 4) | p[11]; nrf::gPlen = p[12]; nrf::gPower = p[13]; nrf::gPromisc = p[14];
            nrf::apply(false); nrf::rxOn = false; return kOk;
        }
        if (radio == kRadioWifi && n >= 4) { wifiMode = p[0]; wifiChan = p[1]; wifiHop = p[2] | (p[3] << 8); return kOk; }
        if (radio == kRadioBle && n >= 4) { bleAdv = p[0] == 1; bleActive = p[2] != 0; return kOk; }
        return kUnsupported;
    }
    uint8_t start(uint8_t radio) override {
        using namespace onair::wire;
        if (radio == kRadioNrf24) { nrf::apply(false); digitalWrite(PIN_CE, HIGH); nrf::rxOn = true; return kOk; }
        if (radio == kRadioWifi) {
            WiFi.mode(WIFI_STA); WiFi.disconnect();
            if (wifiMode == 0) { esp_wifi_set_promiscuous_rx_cb(&wifiRx); esp_wifi_set_promiscuous(true); wifiOn = true; }
            else { WiFi.scanNetworks(true, true); wifiOn = true; }
            return kOk;
        }
        if (radio == kRadioBle) {
            if (!BLEDevice::getInitialized()) BLEDevice::init("OnAir");
            bleScan = BLEDevice::getScan(); bleScan->setAdvertisedDeviceCallbacks(&advCb, true); bleScan->setActiveScan(bleActive);
            bleScan->start(0, nullptr, false); return kOk;
        }
        return kUnsupported;
    }
    void stopAll() override {
        digitalWrite(PIN_CE, LOW); nrf::rxOn = false;
        if (wifiOn) { esp_wifi_set_promiscuous(false); WiFi.scanDelete(); wifiOn = false; }
        if (bleScan) bleScan->stop();
        if (bleAdv && BLEDevice::getInitialized()) BLEDevice::getAdvertising()->stop();
    }
    uint8_t transmit(uint8_t radio, const uint8_t* p, size_t n, uint8_t& res) override {
        using namespace onair::wire;
        if (radio == kRadioNrf24) {
            digitalWrite(PIN_CE, LOW); nrf::apply(true);
            nrf::cmd((p[0] & 1) ? nrf::W_TX_PAYLOAD_NOACK : nrf::W_TX_PAYLOAD, p + 2, nullptr, p[1]);
            digitalWrite(PIN_CE, HIGH); delayMicroseconds(20); digitalWrite(PIN_CE, LOW);
            const uint32_t t0 = ::millis(); res = 2;
            while (::millis() - t0 < 100) { const uint8_t st = nrf::rd(nrf::STATUS); if (st & 0x20) { res = 0; break; } if (st & 0x10) { res = 1; break; } }
            nrf::wr(nrf::STATUS, 0x70);
            if (nrf::rxOn) { nrf::apply(false); digitalWrite(PIN_CE, HIGH); }
            return kOk;
        }
        if (radio == kRadioBle && n >= 9) {
            if (!BLEDevice::getInitialized()) BLEDevice::init("OnAir");
            BLEAdvertising* adv = BLEDevice::getAdvertising();
            BLEAdvertisementData ad; ad.addData(String((const char*)(p + 8), p[7])); adv->setAdvertisementData(ad);
            adv->setAdvertisementType(p[0] == 3 ? ADV_TYPE_NONCONN_IND : p[0] == 2 ? ADV_TYPE_SCAN_IND : ADV_TYPE_IND);
            adv->start(); res = 0; return kOk;
        }
        return kUnsupported;
    }
    bool pollRx(uint8_t& radio, uint8_t* out, size_t& len, size_t cap) override {
        using namespace onair::wire;
        if (nrf::rxOn && !(nrf::rd(nrf::FIFO_STATUS) & 0x01)) {   // RX FIFO not empty
            uint8_t w = nrf::gPromisc ? 32 : (nrf::gPlen ? nrf::gPlen : 0), buf[32];
            if (!nrf::gPromisc && nrf::gPlen == 0) { nrf::cmd(nrf::R_RX_PL_WID, nullptr, &w, 1); if (w > 32) { nrf::cmd(nrf::FLUSH_RX, nullptr, nullptr, 0); return false; } }
            nrf::cmd(nrf::R_RX_PAYLOAD, nullptr, buf, w); nrf::wr(nrf::STATUS, 0x40);
            uint8_t* o = out; p32(o, ::millis());
            o[4] = nrf::gPromisc ? 1 : 0; o[5] = nrf::gRate; o[6] = nrf::gCh; o[7] = nrf::gPromisc ? 0 : nrf::gWidth; o[8] = 0; o[9] = 0; o[10] = nrf::gCrc; o[11] = w;
            size_t k = 12; if (!nrf::gPromisc) { memcpy(o + k, nrf::gAddr, nrf::gWidth); k += nrf::gWidth; }
            memcpy(o + k, buf, w); len = k + w; radio = kRadioNrf24; return true;
        }
        if (wifiOn && wifiMode == 0 && wifiChan == 0 && ::millis() - wifiLast >= wifiHop) { wifiLast = ::millis(); wifiCh = wifiCh % 13 + 1; esp_wifi_set_channel(wifiCh, WIFI_SECOND_CHAN_NONE); }
        else if (wifiOn && wifiMode == 0 && wifiChan && wifiLast == 0) { wifiLast = 1; esp_wifi_set_channel(wifiChan, WIFI_SECOND_CHAN_NONE); }
        if (wifiOn && wifiMode == 1) {
            const int c = WiFi.scanComplete();
            if (c >= 0) {
                static int idx = 0;
                if (idx < c) {
                    uint8_t* o = out; p32(o, ::millis()); o[4] = (uint8_t)WiFi.RSSI(idx); o[5] = WiFi.channel(idx); o[6] = 0;
                    const uint8_t* b = WiFi.BSSID(idx); memcpy(o + 7, b, 6); memcpy(o + 13, b, 6); o[19] = o[20] = 0; o[21] = authCode(WiFi.encryptionType(idx));
                    String s = WiFi.SSID(idx); const size_t sl = s.length() > 32 ? 32 : s.length(); o[22] = sl; memcpy(o + 23, s.c_str(), sl); len = 23 + sl;
                    radio = kRadioWifi; idx++; return true;
                }
                idx = 0; WiFi.scanDelete(); WiFi.scanNetworks(true, true);
            }
        }
        if (qt != qh) {
            portENTER_CRITICAL(&qmux); radio = q[qt].radio; len = q[qt].len; memcpy(out, q[qt].d, len); qt = (qt + 1) % 24; portEXIT_CRITICAL(&qmux);
            return true;
        }
        (void)cap; return false;
    }
};

static EspHal hal;
static onair::Core core(hal);

void setup() {
    Serial.begin(115200);
    pinMode(PIN_CSN, OUTPUT); pinMode(PIN_CE, OUTPUT); digitalWrite(PIN_CSN, HIGH); digitalWrite(PIN_CE, LOW);
    SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN);
    hal.nrfOk = nrf::present();
}

void loop() {
    while (Serial.available()) core.onByte((uint8_t)Serial.read());
    core.poll();
}
