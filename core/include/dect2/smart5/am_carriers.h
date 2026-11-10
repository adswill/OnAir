// AM split-carrier (het) analyser (3.9, docs/SMART5-broadcast-3.9.md item 4). Medium-wave DXers tell stations on one channel
// apart by their exact carrier offsets: each transmitter sits a fraction of a hertz to some hertz off the nominal frequency.
// From complex samples centred on the channel this finds every carrier within +/- spanHz to about 0.05 Hz (Hann window,
// averaged long FFTs, parabolic interpolation on the dB spectrum), its level against the strongest, its drift over the
// analyses, and the het tone between it and the strongest. A user carrier book names the carriers it recognises.
#pragma once
#include <complex>
#include <cstddef>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

struct AmBookEntry { double offsetHz = 0; double tolHz = 0.3; std::string name; };

struct AmCarrier {
    double offsetHz = 0;      // from the channel centre
    double levelDb = 0;       // against the strongest carrier (0 for it)
    double snrDb = 0;         // above the noise floor of the analysis
    double driftHzPerMin = 0; // from the earlier analyses (0 until two)
    double hetHz = 0;         // |offset - strongest offset| (0 for the strongest)
    int seen = 0;             // analyses it was found in
    std::string name;         // from the carrier book
};

struct AmCarrierParams { double spanHz = 600; double resolutionHz = 0.2; int averages = 4; double minSnrDb = 10; };

class AmCarrierAnalyser {
public:
    void configure(double rateHz, const AmCarrierParams& p = {}, double nowSec = 0);
    void push(const std::complex<float>* x, size_t n, double nowSec);
    void setBook(const std::vector<AmBookEntry>& b) { book_ = b; }
    bool fresh() { bool f = fresh_; fresh_ = false; return f; }   // an analysis finished since the last call
    const std::vector<AmCarrier>& carriers() const { return out_; }   // strongest first
    double binHz() const { return binHz_; }
    double secondsPerAnalysis() const { return L_ * (double)averages_ / decRate_; }
private:
    void analyse(double now);
    AmCarrierParams p_;
    double rate_ = 0, decRate_ = 0, binHz_ = 0;
    int D_ = 1, L_ = 0, averages_ = 4, have_ = 0, segs_ = 0;
    std::complex<double> acc_{0, 0}; int accN_ = 0;
    std::vector<std::complex<float>> seg_;
    std::vector<double> pow_, win_;
    std::vector<AmCarrier> out_;
    struct Hist { double off, t; };
    std::vector<std::vector<Hist>> hist_;
    std::vector<AmBookEntry> book_;
    bool fresh_ = false;
};

} // namespace smart5
} // namespace dect2
