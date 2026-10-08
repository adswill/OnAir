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
#include <cmath>
#include <map>
#include <memory>
#include <string>
#include <thread>

using namespace dect2;

namespace {
constexpr double kRate = 64e6 / 7.0;

// what the driver was asked to do, for the test (the same process) to read: environment variables ONAIR_TEST_*
void note(const std::string& k, const std::string& v) {
    const std::string name = "ONAIR_TEST_" + k;
#ifdef _WIN32
    _putenv_s(name.c_str(), v.c_str());
#else
    setenv(name.c_str(), v.c_str(), 1);
#endif
}   // the native rate of an 8 MHz channel: the only rate this "radio" offers

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
    // two inputs; the one picked is put in ONAIR_TEST_ANTENNA, which the test (the same process) reads
    std::vector<std::string> listAntennas(const int, const size_t) const override { return {"RX1", "RX2"}; }
    void setAntenna(const int, const size_t, const std::string& name) override {
        antenna_ = name;
#ifdef _WIN32
        _putenv_s("ONAIR_TEST_ANTENNA", name.c_str());
#else
        setenv("ONAIR_TEST_ANTENNA", name.c_str(), 1);
#endif
    }
    std::string getAntenna(const int, const size_t) const override { return antenna_; }
    std::vector<std::string> listGains(const int, const size_t) const override { return {"TOTAL"}; }
    bool hasGainMode(const int, const size_t) const override { return true; }
    void setGainMode(const int, const size_t, const bool automatic) override { agc_ = automatic; }
    bool getGainMode(const int, const size_t) const override { return agc_; }
    void setGain(const int, const size_t, const double g) override { gain_ = std::min(60.0, std::max(0.0, g)); }
    double getGain(const int, const size_t) const override { return gain_; }
    SoapySDR::Range getGainRange(const int, const size_t) const override { return SoapySDR::Range(0, 60, 1); }
    void setFrequency(const int, const size_t, const double f, const SoapySDR::Kwargs&) override { freq_ = f; note("FREQ", std::to_string((long long)std::llround(f))); }
    double getFrequency(const int, const size_t) const override { return freq_; }
    // a frequency correction ("CORR", what SoapySDR's hasFrequencyCorrection looks for) and driver settings, as SoapyRTLSDR has them
    std::vector<std::string> listFrequencies(const int, const size_t) const override { return {"RF", "CORR"}; }
    void setFrequency(const int, const size_t, const std::string& name, const double f, const SoapySDR::Kwargs&) override {
        if (name == "CORR") { corr_ = f; note("PPM", std::to_string(f)); } else { freq_ = f; note("FREQ", std::to_string((long long)std::llround(f))); }
    }
    double getFrequency(const int, const size_t, const std::string& name) const override { return name == "CORR" ? corr_ : freq_; }
    SoapySDR::ArgInfoList getSettingInfo() const override {
        SoapySDR::ArgInfo ds;
        ds.key = "direct_samp"; ds.value = "0"; ds.name = "Direct Sampling"; ds.description = "RTL-SDR Direct Sampling Mode";
        ds.type = SoapySDR::ArgInfo::STRING; ds.options = {"0", "1", "2"}; ds.optionNames = {"Off", "I-ADC", "Q-ADC"};
        SoapySDR::ArgInfo tm;
        tm.key = "testmode"; tm.value = "true"; tm.name = "Test Mode"; tm.type = SoapySDR::ArgInfo::BOOL;   // the info says true, the radio has it off
        SoapySDR::ArgInfo bt;
        bt.key = "biastee"; bt.value = "false"; bt.name = "Bias Tee"; bt.type = SoapySDR::ArgInfo::BOOL;
        SoapySDR::ArgInfo txt;
        txt.key = "label"; txt.value = ""; txt.type = SoapySDR::ArgInfo::STRING;   // free text: not offered
        return {ds, tm, bt, txt};
    }
    std::string readSetting(const std::string& key) const override {
        auto it = settings_.find(key);
        return it == settings_.end() ? std::string() : it->second;
    }
    void writeSetting(const std::string& key, const std::string& value) override { settings_[key] = value; note("SET_" + key, value); }
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
    double freq_ = 0, rate_ = kRate, gain_ = 0, corr_ = 0;
    std::map<std::string, std::string> settings_ = {{"direct_samp", "0"}, {"testmode", "false"}, {"biastee", "false"}, {"label", ""}};
    bool agc_ = true;
    std::string antenna_ = "RX1";
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

// A radio whose gains are gain reductions, as SoapySDRPlay3 has them (IFGR 20-59 dB, RFGR the LNA state 0-9; more is less), with
// SoapySDR's own overall gain (the elements filled in order from their minimum)
class OnAirTestGrDevice : public OnAirTestDevice {
public:
    std::string getDriverKey() const override { return "onairtestgr"; }
    std::vector<std::string> listGains(const int, const size_t) const override { return {"IFGR", "RFGR"}; }
    SoapySDR::Range getGainRange(const int, const size_t, const std::string& name) const override { return name == "IFGR" ? SoapySDR::Range(20, 59) : SoapySDR::Range(0, 9); }
    void setGain(const int, const size_t, const std::string& name, const double g) override { (name == "IFGR" ? ifgr_ : rfgr_) = g; note(name, std::to_string((int)std::lround(g))); }
    double getGain(const int, const size_t, const std::string& name) const override { return name == "IFGR" ? ifgr_ : rfgr_; }
    void setGain(const int d, const size_t c, const double g) override { SoapySDR::Device::setGain(d, c, g); }
    double getGain(const int d, const size_t c) const override { return SoapySDR::Device::getGain(d, c); }
    SoapySDR::Range getGainRange(const int d, const size_t c) const override { return SoapySDR::Device::getGainRange(d, c); }
private:
    double ifgr_ = 40, rfgr_ = 0;
};
SoapySDR::KwargsList findTestGr(const SoapySDR::Kwargs& args) {
    SoapySDR::KwargsList out;
    if (args.count("driver") && args.at("driver") != "onairtestgr") return out;
    SoapySDR::Kwargs k;
    k["driver"] = "onairtestgr";
    k["label"] = "OnAir gain-reduction radio";
    k["serial"] = "0002";
    out.push_back(k);
    return out;
}
SoapySDR::Device* makeTestGr(const SoapySDR::Kwargs&) { return new OnAirTestGrDevice; }
SoapySDR::Registry gRegistrationGr("onairtestgr", &findTestGr, &makeTestGr, SOAPY_SDR_ABI_VERSION);
}
