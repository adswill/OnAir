#include "dect2/esp32/esp32_radio.h"
#include "dect2/nrf24/nrf24_proto.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace dect2 {
namespace esp32 {

namespace w = onair::wire;

std::string macStr(const uint8_t* m) {
    char b[24];
    std::snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
    return b;
}

std::string authName(int c) {
    switch (c) {
    case 0: return "open"; case 1: return "WEP"; case 2: return "WPA"; case 3: return "WPA2"; case 4: return "WPA/WPA2"; case 5: return "WPA3";
    case 6: return "WPA2/WPA3"; case 7: return "enterprise"; default: return "?";
    }
}

// a few well-known IEEE OUI assignments (public registry); far from complete
std::string vendorOf(const std::string& mac) {
    if (mac.size() < 8) return "";
    unsigned a = 0;
    if (std::sscanf(mac.c_str(), "%x", &a) == 1 && (a & 0x02)) return "private address";
    struct O { const char* p; const char* v; };
    static const O t[] = {{"00:03:93", "Apple"}, {"00:00:0C", "Cisco"}, {"00:02:B3", "Intel"}, {"00:00:F0", "Samsung"}, {"00:09:5B", "Netgear"},
        {"00:05:5D", "D-Link"}, {"00:06:25", "Linksys"}, {"00:E0:FC", "Huawei"}, {"24:0A:C4", "Espressif"}, {"30:AE:A4", "Espressif"}, {"B8:27:EB", "Raspberry Pi"},
        {"DC:A6:32", "Raspberry Pi"}, {"14:CC:20", "TP-Link"}, {"00:27:22", "Ubiquiti"}, {"00:50:F2", "Microsoft"}, {"00:06:5B", "Dell"}};
    for (const auto& o : t) if (mac.compare(0, 8, o.p) == 0) return o.v;
    return "";
}

const char* Radio::txWarning() {
    return "Transmitting needs a licence or an allowed band where you are; the Arm switch is your decision, nothing here blocks it.";
}

namespace {
int64_t nowUtc() { return (int64_t)std::time(nullptr); }
std::string hexOf(const uint8_t* p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%02X", p[i]); s += b; }
    return s;
}
// the local name of a BLE advertisement (AD type 8 or 9), if there is one
std::string bleName(const uint8_t* d, size_t n) {
    for (size_t i = 0; i + 1 < n;) {
        const size_t l = d[i];
        if (l == 0 || i + 1 + l > n) break;
        if (d[i + 1] == 8 || d[i + 1] == 9) return std::string((const char*)d + i + 2, l - 1);
        i += 1 + l;
    }
    return "";
}
} // namespace

struct Radio::Impl {
    std::unique_ptr<SerialPort> port;
    std::thread th;
    std::atomic<bool> run{false};
    mutable std::mutex mu;            // the lists
    std::mutex wmu;                   // writes, seq
    std::mutex amu; std::condition_variable acv;   // acks
    uint8_t seq = 0;
    struct Reply { bool have = false; uint8_t type = 0, status = 0; std::vector<uint8_t> data; } reply;
    uint8_t waitSeq = 0; uint8_t waitType = 0;
    std::atomic<int> txResult{-1};
    Snapshot s;
    w::Parser parser;
    Nrf24Params nrfCfg;
    std::map<std::string, size_t> devIdx;

    bool sendFrame(uint8_t type, const uint8_t* p, size_t n, uint8_t& sq) {
        std::lock_guard<std::mutex> l(wmu);
        sq = (uint8_t)(seq == 255 ? 1 : seq + 1); seq = sq;
        uint8_t f[w::kMaxFrame];
        const size_t m = w::encode(type, sq, p, n, f);
        return m && port && port->write(f, m);
    }
    // a request: wait for the reply frame of the matching seq (an ACK, or HELLO_ACK for the hello)
    bool request(uint8_t type, const std::vector<uint8_t>& payload, uint8_t expect, Reply& out, std::string& err, int timeoutMs = 1500) {
        uint8_t sq;
        {
            std::lock_guard<std::mutex> l(amu);
            reply = Reply(); waitType = expect; waitSeq = 0;
        }
        std::unique_lock<std::mutex> lk(amu);
        {   // set the expected seq before the frame can be answered
            std::lock_guard<std::mutex> l(wmu);
            sq = (uint8_t)(seq == 255 ? 1 : seq + 1); seq = sq; waitSeq = sq;
            uint8_t f[w::kMaxFrame];
            const size_t m = w::encode(type, sq, payload.data(), payload.size(), f);
            if (!m || !port || !port->write(f, m)) { err = "write to the serial port failed"; return false; }
        }
        if (!acv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return reply.have; })) { err = "no answer from the device"; return false; }
        out = reply;
        return true;
    }
    bool ackOk(uint8_t type, const std::vector<uint8_t>& payload, std::string& err) {
        if (!run) { err = "not connected"; return false; }
        Reply r;
        if (!request(type, payload, w::kAck, r, err)) return false;
        if (r.status != w::kOk) {
            static const char* names[] = {"ok", "not supported by this board", "bad parameters", "hardware failure", "transmit is not enabled (press Arm)", "busy", "protocol version not supported"};
            err = r.status < 7 ? names[r.status] : "error";
            return false;
        }
        return true;
    }
    void note(const std::string& t) { s.log.push_back(t); if (s.log.size() > 100) s.log.erase(s.log.begin()); }

    void readerLoop() {
        uint8_t buf[256];
        while (run) {
            const int n = port->read(buf, sizeof buf, 100);
            if (n < 0) { std::lock_guard<std::mutex> l(mu); s.info.connected = false; note("serial port closed"); run = false; break; }
            for (int i = 0; i < n; i++) if (parser.feed(buf[i])) onFrame(parser.type(), parser.seq(), parser.payload(), parser.length());
            std::lock_guard<std::mutex> l(mu);
            s.hostBad = parser.badFrames();
        }
    }

    void onFrame(uint8_t type, uint8_t sq, const uint8_t* p, size_t n) {
        if (type == w::kHelloAck || type == w::kAck) {
            std::lock_guard<std::mutex> l(amu);
            if (type == waitType && sq == waitSeq) {
                reply.have = true; reply.type = type; reply.data.assign(p, p + n);
                reply.status = (type == w::kAck && n >= 2) ? p[1] : 0;
                acv.notify_all();
            }
            return;
        }
        std::lock_guard<std::mutex> l(mu);
        switch (type) {
        case w::kRxNrf24: rxNrf(p, n); break;
        case w::kRxBle: rxBle(p, n); break;
        case w::kRxIeee802154: rxIeee(p, n); break;
        case w::kRxWifi: rxWifi(p, n); break;
        case w::kTxDone: if (n >= 2) txResult = p[1]; break;
        case w::kLog: note(std::string((const char*)p, n)); break;
        case w::kStats: if (n >= 12) { s.devUptimeMs = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); s.devFrames = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24); s.devBad = p[8] | (p[9] << 8) | (p[10] << 16) | ((uint32_t)p[11] << 24); } break;
        default: break;
        }
    }

    void addNrfFrame(const nrf24::Frame& f, int rateKbps, bool weak) {
        Nrf24Telemetry& t = s.nrf;
        Nrf24Frame o;
        o.utc = nowUtc(); o.timeSec = (double)s.devUptimeMs / 1e3; o.rateKbps = rateKbps; o.address = nrf24::hex(f.address);
        o.addrLen = (int)f.address.size(); o.payloadLen = (int)f.payload.size(); o.pid = f.pid; o.crcBits = f.crcBits; o.noAck = f.noAck;
        o.payloadHex = nrf24::hex(f.payload); o.text = nrf24::printable(f.payload); o.confirmed = !weak;
        if (!t.frames.empty() && t.frames.front().address == o.address && t.frames.front().payloadHex == o.payloadHex && t.frames.front().pid == o.pid) { t.frames.front().repeats++; }
        else { t.frames.insert(t.frames.begin(), o); if (t.frames.size() > kNrf24FramesMax) t.frames.pop_back(); }
        t.blocksOk++; t.seq++; t.dataValid = true; t.state = 2;
        (f.crcBits == 8 ? t.crc8Frames : t.crc16Frames)++;
        (rateKbps == 250 ? t.frames250 : rateKbps == 2000 ? t.frames2M : t.frames1M)++;
        bool found = false;
        for (auto& d : t.devices) if (d.address == o.address) { d.count++; d.lastSeen = o.utc; d.lastPayloadLen = o.payloadLen; found = true; std::rotate(t.devices.begin(), t.devices.begin() + (&d - &t.devices[0]), t.devices.begin() + (&d - &t.devices[0]) + 1); break; }
        if (!found) {
            Nrf24Device d; d.address = o.address; d.addrLen = o.addrLen; d.rateKbps = rateKbps; d.crcBits = o.crcBits; d.count = 1; d.firstSeen = d.lastSeen = o.utc; d.lastPayloadLen = o.payloadLen;
            t.devices.insert(t.devices.begin(), d); if (t.devices.size() > kNrf24DevicesMax) t.devices.pop_back();
        }
    }

    void rxNrf(const uint8_t* p, size_t n) {
        if (n < w::kRxNrf24Head) return;
        const bool raw = p[4] & 1;
        const int rate = p[5] == 0 ? 250 : p[5] == 2 ? 2000 : 1000;
        const int aLen = p[7], len = p[11];
        if (n < w::kRxNrf24Head + (size_t)aLen + (size_t)len) return;
        const uint8_t* a = p + w::kRxNrf24Head; const uint8_t* d = a + aLen;
        if (!raw) {
            nrf24::Frame f;
            f.address.assign(a, a + aLen); f.pid = p[8]; f.noAck = p[9] != 0; f.crcBits = p[10]; f.payload.assign(d, d + len);
            addNrfFrame(f, rate, false);
            return;
        }
        // promiscuous: the chip matched 0xAA55 and gave the bytes that follow; put an alternating lead in front and let the sniffer find the frame
        std::vector<uint8_t> bits;
        const int lead = (len > 0 && (d[0] & 0x80)) ? 0xAA : 0x55;   // the preamble alternates into the first address bit
        for (int i = 7; i >= 0; i--) bits.push_back((lead >> i) & 1);
        for (int i = 0; i < len; i++) for (int b = 7; b >= 0; b--) bits.push_back((d[i] >> b) & 1);
        bool any = false;
        for (const auto& c : nrf24::sniff(bits)) { addNrfFrame(c.frame, rate, c.weak); any = true; }
        if (!any) s.nrf.blocksBad++;
        s.nrf.bursts++;
    }

    void rxBle(const uint8_t* p, size_t n) {
        if (n < 4 + 1 + 1 + 1 + 6 + 1) return;
        const size_t len = p[13];
        if (n < 14 + len) return;
        BleAdv a; a.rssi = (int8_t)p[4]; a.channel = p[5]; a.pduType = p[6]; a.addr = macStr(p + 7); a.dataHex = hexOf(p + 14, len); a.name = bleName(p + 14, len);
        a.lastSeen = nowUtc(); a.count = 1;
        for (auto& b : s.ble) if (b.addr == a.addr) { a.count = b.count + 1; if (a.name.empty()) a.name = b.name; b = a; return; }
        s.ble.insert(s.ble.begin(), a); if (s.ble.size() > 300) s.ble.pop_back();
    }

    void rxIeee(const uint8_t* p, size_t n) {
        if (n < 7) return;
        const size_t len = p[6];
        if (n < 7 + len) return;
        Ieee154Frame f; f.utc = nowUtc(); f.rssi = (int8_t)p[4]; f.channel = p[5]; f.hex = hexOf(p + 7, len);
        s.ieee.insert(s.ieee.begin(), f); if (s.ieee.size() > 300) s.ieee.pop_back();
    }

    void rxWifi(const uint8_t* p, size_t n) {
        if (n < 23) return;
        const int rssi = (int8_t)p[4], ch = p[5], kind = p[6];
        const std::string mac = macStr(p + 7), bssid = macStr(p + 13);
        const int reason = p[19] | (p[20] << 8), auth = p[21], sl = p[22];
        if (n < 23 + (size_t)sl) return;
        const std::string ssid((const char*)p + 23, sl);
        WifiSnapshot& W = s.wifi;
        const int64_t now = nowUtc();
        if (ch >= 1 && ch <= 14) {
            W.chanFrames[ch]++;
            if (rssi > -127 && (W.chanBestDb[ch] == 0 || rssi > W.chanBestDb[ch])) W.chanBestDb[ch] = rssi;
        }
        auto findAp = [&](const std::string& b) -> WifiAp* { for (auto& a : W.aps) if (a.bssid == b) return &a; return nullptr; };
        if (kind == 0 || kind == 2) {
            (kind == 0 ? W.beacons : W.probes)++;
            WifiAp* a = findAp(bssid);
            if (!a) { W.aps.push_back(WifiAp()); a = &W.aps.back(); a->bssid = bssid; a->vendor = vendorOf(bssid); a->firstSeen = now; if (ch >= 1 && ch <= 14) W.chanAps[ch]++; }
            else if (a->channel != ch && ch >= 1 && ch <= 14) { if (a->channel >= 1 && a->channel <= 14 && W.chanAps[a->channel] > 0) W.chanAps[a->channel]--; W.chanAps[ch]++; }
            if (!ssid.empty()) a->ssid = ssid;
            a->channel = ch; a->rssi = rssi; if (auth != 255) a->auth = auth; a->count++; a->lastSeen = now;
            a->history.push_back((float)rssi); if (a->history.size() > 240) a->history.pop_front();
            std::stable_sort(W.aps.begin(), W.aps.end(), [](const WifiAp& x, const WifiAp& y) { return x.rssi > y.rssi; });
        } else if (kind == 1 || kind == 5) {
            if (kind == 1) W.probes++; else W.dataFrames++;
            WifiClient* c = nullptr;
            for (auto& x : W.clients) if (x.mac == mac) { c = &x; break; }
            if (!c) { W.clients.insert(W.clients.begin(), WifiClient()); c = &W.clients.front(); c->mac = mac; c->vendor = vendorOf(mac); c->randomised = c->vendor == "private address"; }
            c->rssi = rssi; c->count++; c->lastSeen = now;
            if (kind == 5) { if (c->bssid.empty()) { if (WifiAp* a = findAp(bssid)) a->clients++; } c->bssid = bssid; }
            if (kind == 1 && !ssid.empty() && c->probed.size() < 8 && std::find(c->probed.begin(), c->probed.end(), ssid) == c->probed.end()) c->probed.push_back(ssid);
            if (W.clients.size() > 500) W.clients.pop_back();
        } else if (kind == 3 || kind == 4) {
            W.deauths++;
            char b[96]; std::snprintf(b, sizeof b, "%s from %s (reason %d, channel %d)", kind == 3 ? "deauthentication" : "disassociation", mac.c_str(), reason, ch);
            note(b);
        }
    }
};

Radio::Radio() : p_(new Impl) {}
Radio::~Radio() { disconnect(); }

bool Radio::connected() const { std::lock_guard<std::mutex> l(p_->mu); return p_->s.info.connected; }

bool Radio::connect(std::unique_ptr<SerialPort> port, std::string& err) {
    disconnect();
    Impl& I = *p_;
    I.port = std::move(port);
    I.run = true;
    I.parser.reset();
    I.th = std::thread([this] { p_->readerLoop(); });
    Impl::Reply r;
    // a board that has just reset may need a moment: try the hello twice
    bool ok = false;
    for (int i = 0; i < 2 && !ok; i++) ok = I.request(w::kHello, {w::kProtoVersion}, w::kHelloAck, r, err, 1200);
    if (!ok || r.data.size() < 6) { if (ok) err = "unexpected answer"; I.run = false; if (I.th.joinable()) I.th.join(); I.port.reset(); return false; }
    if (r.data[0] != w::kProtoVersion) { err = "the firmware speaks protocol version " + std::to_string(r.data[0]) + ", this OnAir needs " + std::to_string(w::kProtoVersion); I.run = false; if (I.th.joinable()) I.th.join(); I.port.reset(); return false; }
    std::lock_guard<std::mutex> l(I.mu);
    DeviceInfo& d = I.s.info;
    d.connected = true; d.protoVersion = r.data[0]; d.fwMajor = r.data[1]; d.fwMinor = r.data[2]; d.chip = r.data[3]; d.radios = r.data[4];
    const size_t nl = std::min<size_t>(r.data[5], r.data.size() - 6);
    d.name.assign((const char*)r.data.data() + 6, nl);
    d.port = I.port->name();
    I.note("connected: " + d.name);
    return true;
}

void Radio::disconnect() {
    Impl& I = *p_;
    if (I.run && I.port) { uint8_t sq; I.sendFrame(w::kStop, nullptr, 0, sq); }
    I.run = false;
    if (I.th.joinable()) I.th.join();
    if (I.port) I.port->close();
    I.port.reset();
    std::lock_guard<std::mutex> l(I.mu);
    I.s.info.connected = false;
}

bool Radio::autoConnect(std::string& err, const std::string& onlyPort, int baud) {
    std::vector<std::string> ports;
    if (!onlyPort.empty()) ports.push_back(onlyPort); else ports = candidatePorts();
    if (ports.empty()) { err = "no USB serial port found (is the board plugged in? a USB-serial driver may be needed)"; return false; }
    std::string last;
    for (const auto& pth : ports) {
        std::string e;
        auto sp = openSerial(pth, baud, e);
        if (!sp) { last = e; continue; }
        if (connect(std::move(sp), e)) return true;
        last = pth + ": " + e;
    }
    err = "no ESP32 radio answered (" + last + ")";
    return false;
}

static void put(std::vector<uint8_t>& v, int x) { v.push_back((uint8_t)x); }

bool Radio::configure(const Nrf24Params& c, std::string& err) {
    std::vector<uint8_t> v;
    put(v, w::kRadioNrf24); put(v, c.channel); put(v, c.rateKbps == 250 ? 0 : c.rateKbps == 2000 ? 2 : 1); put(v, c.addrWidth);
    for (int i = 0; i < 5; i++) put(v, c.address[i]);
    put(v, c.crcBits == 0 ? 0 : c.crcBits == 8 ? 1 : 2); put(v, c.autoAck); put(v, c.retryDelay); put(v, c.retryCount); put(v, c.payloadLen); put(v, c.power); put(v, c.promiscuous);
    if (!p_->ackOk(w::kConfig, v, err)) return false;
    std::lock_guard<std::mutex> l(p_->mu);
    p_->nrfCfg = c;
    return true;
}
bool Radio::configure(const BleParams& c, std::string& err) {
    return p_->ackOk(w::kConfig, {w::kRadioBle, (uint8_t)c.advertise, (uint8_t)c.channelMask, (uint8_t)c.activeScan, (uint8_t)c.power}, err);
}
bool Radio::configure(const WifiParams& c, std::string& err) {
    return p_->ackOk(w::kConfig, {w::kRadioWifi, (uint8_t)c.scan, (uint8_t)c.channel, (uint8_t)(c.hopMs & 0xFF), (uint8_t)(c.hopMs >> 8)}, err);
}
bool Radio::configure(const Ieee154Params& c, std::string& err) {
    return p_->ackOk(w::kConfig, {w::kRadioIeee802154, (uint8_t)c.channel, (uint8_t)c.promiscuous}, err);
}
bool Radio::start(int radio, std::string& err) { return p_->ackOk(w::kStart, {(uint8_t)radio}, err); }
bool Radio::stop(std::string& err) { return p_->ackOk(w::kStop, {}, err); }
bool Radio::setTxEnable(bool on, std::string& err) { return p_->ackOk(w::kTxEnable, {(uint8_t)on}, err); }

bool Radio::sendNrf24(const std::vector<uint8_t>& d, bool noAck, std::string& err) {
    if (d.size() > 32) { err = "an nRF24 payload is at most 32 bytes"; return false; }
    std::vector<uint8_t> v = {w::kRadioNrf24, (uint8_t)noAck, (uint8_t)d.size()};
    v.insert(v.end(), d.begin(), d.end());
    p_->txResult = -1;
    return p_->ackOk(w::kTx, v, err);
}
bool Radio::sendBle(int advType, const uint8_t addr[6], const std::vector<uint8_t>& d, std::string& err) {
    if (d.size() > 31) { err = "a BLE advertisement carries at most 31 bytes"; return false; }
    std::vector<uint8_t> v = {w::kRadioBle, (uint8_t)advType};
    v.insert(v.end(), addr, addr + 6);
    v.push_back((uint8_t)d.size());
    v.insert(v.end(), d.begin(), d.end());
    p_->txResult = -1;
    return p_->ackOk(w::kTx, v, err);
}
int Radio::lastTxResult() const { return p_->txResult; }
void Radio::clearResults() {
    std::lock_guard<std::mutex> l(p_->mu);
    DeviceInfo info = p_->s.info;
    std::vector<std::string> lg = p_->s.log;
    p_->s = Snapshot();
    p_->s.info = info; p_->s.log = lg;
}
Snapshot Radio::snapshot() const { std::lock_guard<std::mutex> l(p_->mu); return p_->s; }

} // namespace esp32
} // namespace dect2
