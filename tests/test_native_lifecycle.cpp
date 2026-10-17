// Shared native-source lifecycle: a rejected rate must never restart partially configured hardware.
#include "../core/src/native_common.h"
#include <iostream>
using namespace dect2;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c << '\n'; ++failures; } } while (0)

class RefusingSource final : public native::NativeSource {
public:
    bool selected = false, refuseRate = false;
    int releases = 0;
    std::atomic<int> streams{0};
    ~RefusingSource() override { stop(); }
    bool running() const { return run_.load(); }
protected:
    bool openDevice(const TuneSettings& s, std::string& err) override { selected = true; return configure(s, err, false); }
    bool configure(const TuneSettings& s, std::string& err, bool) override {
        requested_ = rate_ = s.sampleRate;   // emulate a vendor call that fails after changing some settings
        if (refuseRate) { err = "vendor refused the new rate"; return false; }
        return true;
    }
    void closeDevice() override { if (selected) { selected = false; ++releases; } }
    void streamLoop() override {
        ++streams;
        while (run_) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
};

static bool awaitStreams(const RefusingSource& s, int count) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (s.streams.load() < count && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return s.streams.load() == count;
}

int main() {
    RefusingSource source;
    IqRing ring(1024);
    TuneSettings t; t.sampleRate = 2e6;
    std::string err;
    CHECK(source.start(t, ring, err));
    CHECK(awaitStreams(source, 1));
    source.refuseRate = true;
    t.sampleRate = 4e6;
    CHECK(!source.retune(t, err));
    CHECK(err == "vendor refused the new rate");
    CHECK(!source.running() && !source.selected && source.releases == 1);
    source.stop();
    CHECK(source.streams.load() == 1);   // no stream may observe the rejected configuration
    CHECK(source.releases == 1);

    // A failure must not prevent a fresh selection, or a subsequent successful rate restart.
    source.refuseRate = false;
    CHECK(source.start(t, ring, err));
    CHECK(awaitStreams(source, 2));
    t.sampleRate = 8e6;
    CHECK(source.retune(t, err));
    CHECK(awaitStreams(source, 3));
    CHECK(source.selected && source.running() && source.sampleRate() == 8e6);
    source.stop(); source.stop();
    CHECK(!source.selected && !source.running() && source.releases == 2);
    std::cout << "native lifecycle: " << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
