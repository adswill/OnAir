// Rational polyphase resampler (windowed sinc) for complex samples, used to bring any capture rate to the T2 native rate.
#pragma once
#include "ring.h"
#include <vector>

namespace dect2 {

class RationalResampler {
public:
    // Finds a small L/M approximating outRate/inRate. Returns false if no good ratio exists.
    bool configure(double inRate, double outRate, double* relError = nullptr);
    bool passthrough() const { return L_ == 1 && M_ == 1; }
    int L() const { return L_; }
    int M() const { return M_; }
    void reset();
    // Appends resampled samples to `out`.
    void process(const cf32* in, size_t n, std::vector<cf32>& out);

private:
    int L_ = 1, M_ = 1, H_ = 24, T_ = 48;
    std::vector<std::vector<float>> taps_; // per output phase (0..L-1), length T_
    std::vector<int> base_;                // input offset of the first output of each phase
    std::vector<int> nOfPhase_;            // n_p
    std::vector<float> re_, im_;           // pending input (split), includes history
    bool primed_ = false;
};

} // namespace dect2
