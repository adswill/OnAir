// A fake radio for the SoapySDR source test: a SoapySDR driver ("onairtest") that streams OnAir's synthetic DVB-T2 signal.
// Built as a plugin module; the test points SOAPY_SDR_PLUGIN_PATH at it.
#include "dect2/ring.h"
#include "dect2/source.h"
#include <SoapySDR/Device.hpp>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Time.hpp>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

using namespace dect2;

namespace {
constexpr double kRate = 64e6 / 7.0;   // the native rate of an 8 MHz channel: the only rate this "radio" offers

struct TestStream {
    IqRing ring{1u << 22};
    std::unique_ptr<IqSource> src;
    bool active = false;
};

class OnAirTestDevice : public SoapySDR::Device {
public:
    std::string getDriverKey() const override { return "onairtest"; }
    std::string getHardwareKey() const override { return "OnAir synthetic radio"; }
    size_t getNumChannels(const int dir) const override { return dir == SOAPY_SDR_RX ? 1 : 0; }
    std::vector<std::string> listAntennas(const int, const size_t) const override { return {"RX"}; }
    std::vector<std::string> listGains(const int, const size_t) const override { return {"TOTAL"}; }
    bool hasGainMode(const int, const size_t) const override { return true; }
    void setGainMode(const int, const size_t, const bool automatic) override { agc_ = automatic; }
    bool getGainMode(const int, const size_t) const override { return agc_; }
    void setGain(const int, const size_t, const double g) override { gain_ = std::min(60.0, std::max(0.0, g)); }
    double getGain(const int, const size_t) const override { return gain_; }
    SoapySDR::Range getGainRange(const int, const size_t) const override { return SoapySDR::Range(0, 60, 1); }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override { freq_ = f; }
    double getFrequency(const int, const size_t) const override { return freq_; }
    std::vector<std::string> listFrequencies(const int, const size_t) const override { return {"RF"}; }
    SoapySDR::RangeList getFrequencyRange(const int, const size_t) const override { return {SoapySDR::Range(50e6, 2e9)}; }
    void setSampleRate(const int, const size_t, const double r) override { rate_ = r; }
    double getSampleRate(const int, const size_t) const override { return rate_; }
    SoapySDR::RangeList getSampleRateRange(const int, const size_t) const override { return {SoapySDR::Range(kRate, kRate)}; }
    std::vector<std::string> getStreamFormats(const int, const size_t) const override { return {SOAPY_SDR_CF32}; }
    std::string getNativeStreamFormat(const int, const size_t, double& fullScale) const override { fullScale = 1.0; return SOAPY_SDR_CF32; }

    SoapySDR::Stream* setupStream(const int, const std::string& fmt, const std::vector<size_t>&, const SoapySDR::Kwargs&) override {
        if (fmt != SOAPY_SDR_CF32) throw std::runtime_error("only CF32");
        auto* s = new TestStream;
        return reinterpret_cast<SoapySDR::Stream*>(s);
    }
    void closeStream(SoapySDR::Stream* st) override { delete reinterpret_cast<TestStream*>(st); }
    size_t getStreamMTU(SoapySDR::Stream*) const override { return 1 << 15; }

    int activateStream(SoapySDR::Stream* st, const int, const long long, const size_t) override {
        auto* s = reinterpret_cast<TestStream*>(st);
        DeviceInfo d; d.kind = DeviceInfo::Synthetic;
        TuneSettings t;
        t.synth.snrDb = 30;
        t.synth.tx.s2field1 = 1;   // 8K
        t.synth.tx.giIdx = 3;      // 1/8
        const char* pace = getenv("ONAIR_TEST_PACE");
        t.synth.pace = pace ? atof(pace) : 1.0;
        s->src = makeSource(d);
        std::string err;
        if (!s->src->start(t, s->ring, err)) return SOAPY_SDR_STREAM_ERROR;
        s->active = true;
        return 0;
    }
    int deactivateStream(SoapySDR::Stream* st, const int, const long long) override {
        auto* s = reinterpret_cast<TestStream*>(st);
        if (s->src) { s->src->stop(); s->src.reset(); }
        s->active = false;
        return 0;
    }
    int readStream(SoapySDR::Stream* st, void* const* buffs, const size_t n, int& flags, long long& timeNs, const long timeoutUs) override {
        auto* s = reinterpret_cast<TestStream*>(st);
        flags = 0; timeNs = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (;;) {
            if (s->ring.available() > 0) return (int)s->ring.read(reinterpret_cast<cf32*>(buffs[0]), n);
            if (std::chrono::steady_clock::now() - t0 > std::chrono::microseconds(timeoutUs)) return SOAPY_SDR_TIMEOUT;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

private:
    double freq_ = 0, rate_ = kRate, gain_ = 0;
    bool agc_ = true;
};

SoapySDR::KwargsList findTest(const SoapySDR::Kwargs& args) {
    SoapySDR::KwargsList out;
    if (args.count("driver") && args.at("driver") != "onairtest") return out;
    SoapySDR::Kwargs k;
    k["driver"] = "onairtest";
    k["label"] = "OnAir synthetic radio";
    k["serial"] = "0001";
    out.push_back(k);
    return out;
}
SoapySDR::Device* makeTest(const SoapySDR::Kwargs&) { return new OnAirTestDevice; }

SoapySDR::Registry gRegistration("onairtest", &findTest, &makeTest, SOAPY_SDR_ABI_VERSION);
}
