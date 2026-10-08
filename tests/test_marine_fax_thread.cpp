// Threads and speed: push() on one thread while status() / latestImage() and the setters run on others, and the
// real-time factor of push() at 12 kHz and 48 kHz audio.
#include "data/marine/fax/fax_util.h"
#include <atomic>
#include <chrono>
#include <thread>
using namespace faxt;

int main() {
    {
        Opts o; o.lines = 60; o.phasingS = 12; o.seconds = 170;     // three transmissions
        const auto a = makeAudio(o);
        FaxDecoder d;
        d.configure(o.rate);
        std::atomic<bool> done{false};
        std::atomic<int> bad{0}, images{0};
        std::thread reader([&] {
            uint64_t seq = 0;
            FaxImage im;
            while (!done) {
                if (d.latestImage(im, seq)) {
                    images++;
                    if (im.width < 0 || im.height < 0 || im.pix.size() != static_cast<size_t>(im.width) * im.height) bad++;
                }
                const FaxStatus s = d.status();
                if (s.state < 0 || s.state > 3 || s.lines < 0 || s.lines > 1500 || !(s.snrDb > -100 && s.snrDb < 100)) bad++;
            }
        });
        std::thread setter([&] {
            int k = 0;
            while (!done) {
                d.setSlantPpm((k++ % 7) - 3.0);
                d.setAutoSlant((k & 8) != 0);
                d.setMaxLines(1500);
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
        });
        feedAll(d, a, 1024);
        done = true;
        reader.join();
        setter.join();
        printf("threads: %d image updates seen, %d inconsistent snapshots\n", images.load(), bad.load());
        FCHECK(bad == 0, "inconsistent snapshots: %d", bad.load());
        FCHECK(images > 20, "image updates seen by the reader: %d", images.load());
        // reset() from another thread while pushing is also allowed.
        std::atomic<bool> stop{false};
        std::thread resetter([&] { while (!stop) { d.reset(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } });
        feedAll(d, a, 4096);
        stop = true;
        resetter.join();
    }
    for (double rate : {12000.0, 48000.0}) {
        Opts o; o.lines = 100; o.rate = rate; o.phasingS = 10;
        const auto a = makeAudio(o);
        FaxDecoder d;
        d.configure(rate);
        const auto t0 = std::chrono::steady_clock::now();
        feedAll(d, a, 4096);
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double rtf = a.size() / rate / el;
        printf("%.0f Hz audio: %.0f s decoded in %.2f s, %.0f times real time on one core\n", rate, a.size() / rate, el, rtf);
        FCHECK(rtf > (rate == 12000 ? 20 : 3), "real-time factor %.1f at %.0f Hz", rtf, rate);
    }
    if (fails) { printf("%d failures\n", fails); return 1; }
    printf("OK\n");
    return 0;
}
