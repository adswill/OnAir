// The program-wide insight hub (docs/SMART5-*-3.9.md): fed on the decoders' threads through the automation engine's analyser hook
// (APRS, pager, session and msg events) and by the FT8 view; the interface thread calls tick() about once a minute, which also sends
// the findings on as automation events "insight.<kind>" (rules, notifications, integrations). The talkgroup heatmap is kept in a file.
#pragma once
#include "dect2/automation/engine.h"
#include "dect2/insight/hub.h"
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace dect2::insight {

class Live {
public:
    // turns the analysers on or off (they see nothing until this is called with true)
    void enable(bool on) {
        if (on == on_) return;
        on_ = on;
        namespace au = dect2::automation;
        if (on) au::global().setAnalyser([this](const au::Event& e) { std::lock_guard<std::mutex> lk(mu_); hub_.onEvent(e); },
                                         au::familyBit(au::FAprs) | au::familyBit(au::FPager) | au::familyBit(au::FSession) | au::familyBit(au::FMsg) |
                                             au::familyBit(au::FAdsb) | au::familyBit(au::FAis));   // ADS-B and AIS: field.h
        else au::global().setAnalyser(nullptr, 0);
    }
    bool enabled() const { return on_; }
    void setStation(const std::string& call, const std::string& grid) { std::lock_guard<std::mutex> lk(mu_); hub_.setStation(call, grid); }
    void setTzOffset(int sec) { std::lock_guard<std::mutex> lk(mu_); hub_.setTzOffset(sec); }
    void onFtx(double t, const std::string& band, const std::string& msg, float snr) {
        if (!on_) return;
        std::lock_guard<std::mutex> lk(mu_);
        hub_.onFtx(t, band, msg, snr);
    }
    // a finding from the interface thread's own analysers (app/smart5): goes out with the next tick, also when the decode analysers are off
    void post(const Insight& i) { std::lock_guard<std::mutex> lk(mu_); hub_.post(i); }
    // the interface thread, about once a minute: the findings, also sent on as automation events
    std::vector<Insight> tick(double now, const std::vector<double>& tunedHz) {
        std::vector<Insight> v;
        {
            std::lock_guard<std::mutex> lk(mu_);
            hub_.tick(now, tunedHz);
            v = hub_.take();
        }
        for (const auto& i : v) dect2::automation::emit(toEvent(i));
        return v;
    }
    // read the analysers under the lock (the interface's tables)
    void with(const std::function<void(Hub&)>& f) { std::lock_guard<std::mutex> lk(mu_); f(hub_); }

    bool loadHeatmap(const std::string& path) {
        std::ifstream in(path);
        if (!in) return false;
        std::stringstream ss;
        ss << in.rdbuf();
        std::lock_guard<std::mutex> lk(mu_);
        return hub_.heatmap().load(ss.str());
    }
    bool saveHeatmap(const std::string& path) {
        std::string text;
        { std::lock_guard<std::mutex> lk(mu_); text = hub_.heatmap().save(); }
        std::ofstream out(path, std::ios::trunc);
        out << text;
        return (bool)out;
    }

private:
    std::mutex mu_;
    Hub hub_;
    bool on_ = false;
};

inline Live& live() { static Live l; return l; }

// the amateur band of a dial frequency ("20m"), "" outside them
inline const char* hamBand(double hz) {
    struct B { double lo, hi; const char* n; };
    static const B b[] = {{1.8e6, 2.0e6, "160m"}, {3.5e6, 4.0e6, "80m"}, {5.3e6, 5.45e6, "60m"}, {7.0e6, 7.3e6, "40m"}, {10.1e6, 10.15e6, "30m"},
                          {14.0e6, 14.35e6, "20m"}, {18.068e6, 18.168e6, "17m"}, {21.0e6, 21.45e6, "15m"}, {24.89e6, 24.99e6, "12m"},
                          {28.0e6, 29.7e6, "10m"}, {50e6, 54e6, "6m"}, {70e6, 70.5e6, "4m"}, {144e6, 148e6, "2m"}, {430e6, 440e6, "70cm"}};
    for (const auto& x : b) if (hz >= x.lo && hz < x.hi) return x.n;
    return "";
}

}  // namespace dect2::insight
