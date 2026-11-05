// GK-2A LRIT / HRIT receiver (family 230 / 231). Receive only. Wraps the GOES/CGMS xRIT chain (goes_rx.h) with the KMA parameters:
// LRIT BPSK 128 ksym/s, HRIT QPSK 3 Msym/s, and the KMA file layer (JPEG 2000 pictures, DES with user-imported keys, GK-2A headers).
#pragma once
#include "dect2/gk2a/gk2a_tel.h"
#include "dect2/goes/goes_rx.h"
#include "dect2/engine/mode_tuning.h"
#include <functional>
#include <string>
#include <vector>

namespace dect2 {

class Gk2aReceiver {
public:
    explicit Gk2aReceiver(int link);      // 0 LRIT, 1 HRIT
    void configure(double inputRateHz);
    void setSignalOffset(double hz) { inner_.setSignalOffset(hz); }
    bool ready() const { return inner_.ready(); }
    void reset() { inner_.reset(); }
    void clearProducts() { inner_.clearProducts(); }
    void clearResults() { inner_.clearResults(); }
    void feed(const cf32* x, size_t n) { inner_.feed(x, n); }
    bool telemetry(Gk2aTelemetry& out, uint64_t lastSeq);
    std::string saveResults() const { return inner_.saveResults(); }
    bool loadResults(const std::string& blob) { return inner_.loadResults(blob); }
    void setLogCallback(std::function<void(const std::string&)> cb) { inner_.setLogCallback(std::move(cb)); }
    void setFileCallback(std::function<void(const std::vector<uint8_t>&, int, int)> cb) { inner_.setFileCallback(std::move(cb)); }
    void setCaduCallback(std::function<void(const uint8_t*)> cb) { inner_.setCaduCallback(std::move(cb)); }
    void importCadu(std::vector<uint8_t> d) { inner_.importCadu(std::move(d)); }
    // The KMA decryption keys (EncryptionKeyMessage.bin or "index:hexkey" lines); returns the keys read. Keys are never built in.
    int importKeys(const std::vector<uint8_t>& file);
    void setKeys(const std::vector<std::pair<int, std::array<uint8_t, 8>>>& keys);
    int keyCount() const { return keys_; }
    int link() const { return link_; }
    GoesReceiver& inner() { return inner_; }
    void setBandwidthOverride(double) {}

private:
    GoesReceiver inner_;
    int link_;
    int keys_ = 0;
    double rate_ = 0;
    bool running_ = true;
};

ModeTuning gk2aLritTuning();
ModeTuning gk2aHritTuning();

} // namespace dect2
