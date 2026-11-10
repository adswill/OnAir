// The KAAL transmitter's resampler at output rates other than 48 kHz: when the fractional position rounded up to the next input sample, the
// kernel read one sample past the end of its buffer (ASan container-overflow under fuzz_engine_modes, in the test-signal generator). Every
// rate below is run for a few seconds of speech in odd-sized chunks; each must give the expected number of samples. The overflow itself
// shows under DECT2_SANITIZE=ON.
#include "dect2/kaal/kaal_tx.h"
#include <cmath>
#include <vector>
#include "../common/test_util.h"
using namespace dect2;

int main() {
    for (double rate : {2e6, 1.2e6, 999999.0, 250e3, 96000.0, 44100.0, 48001.0}) for (int kbit : {0, 16}) {
        kaal::VoiceTx tx;
        tx.setBitrate(kbit);
        tx.configure(rate);
        std::vector<int16_t> pcm(16000 * 3);
        for (size_t i = 0; i < pcm.size(); i++) pcm[i] = (int16_t)(8000 * std::sin(0.07 * (double)i) * std::sin(0.0011 * (double)i));
        std::vector<std::complex<float>> out(1 << 16);
        size_t got = 0;
        for (size_t i = 0; i < pcm.size();) {
            const size_t n = std::min<size_t>(pcm.size() - i, 1 + (i * 7919) % 1777);
            tx.pushAudio(pcm.data() + i, n);
            i += n;
            while (size_t k = tx.popIq(out.data(), out.size())) got += k;
        }
        tx.flush();
        while (size_t k = tx.popIq(out.data(), out.size())) got += k;
        const double secs = got / rate;
        CHECK(secs > 2.5 && secs < 4.5, "rate %.0f, %d kbit/s: %.2f s of IQ for 3 s of speech", rate, kbit, secs);
    }
    return testSummary("kaal_tx_resample");
}
