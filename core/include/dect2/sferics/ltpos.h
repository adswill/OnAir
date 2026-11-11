// Lightning position (network): several OnAir stations time the same sferic, one of them (the server) collects the time-stamped detections and
// finds the strike by time difference of arrival (TDOA). This header is the pure part (no sockets, no clock, no GUI), so every rule is tested:
//
//  1. timing sources, ranked: GPS-disciplined radio time (PPS / GPSDO hardware timestamps), OnAir's GNSS receiver, a common-view reference
//     transmitter, PTP, NTP, the bare system clock. Only the first three are fine enough for a fix (1 us is 300 m); PTP / NTP only associate.
//  2. SampleClock: one radio's sample clock disciplined against a source's time marks (offset and ppm drift), so a detection is stamped to the sample.
//  3. PulseTimer: a sferic detector that times the leading edge (half of the peak, interpolated between samples) and measures waveform features.
//  4. MskRefTimer: the symbol clock of an MSK station (VLF / LF naval transmitters) from the two spectral lines of the squared signal, for the
//     common-view alignment; CommonViewAligner (server) turns the stations' readings into clock corrections.
//  5. geodesy (WGS-84, Vincenty), propagation (ground wave, optional ionosphere hops), the robust least-squares solver with its error ellipse.
//  6. Matcher (server): groups detections of one strike from different stations and rejects false matches.
//  7. the compact packet format between stations, authenticated with HMAC-SHA256 and protected against replays.
// Receive only: nothing is transmitted on air. docs/LIGHTNING-POSITION-3.9.md has the measured accuracy and what is simulated only.
#pragma once
#include <complex>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2::ltpos {

constexpr double kC = 299792458.0;            // m/s
constexpr double kFineLimitNs = 20000;        // a detection timed better than this (1 sigma) takes part in the fix; worse ones only associate

// ---------------------------------------------------------------- 1. timing sources
enum class TimeSrc : uint8_t { None = 0, GpsHw = 1, Gnss = 2, CommonView = 3, Ptp = 4, Ntp = 5, System = 6 };
const char* timeSrcName(TimeSrc s);           // "GPS (radio time)", "GNSS receiver", "common view", "PTP", "NTP", "system clock", "none"
int timeSrcRank(TimeSrc s);                   // 1 = best ... 6, 99 for None
double timeSrcTypicalNs(TimeSrc s);           // the 1-sigma error to expect before any measurement says otherwise
bool timeSrcFineCapable(TimeSrc s);           // GPS, GNSS and common view can reach the microsecond level; PTP, NTP and the system clock cannot
inline bool fineTiming(TimeSrc s, double sigmaNs) { return timeSrcFineCapable(s) && sigmaNs > 0 && sigmaNs <= kFineLimitNs; }

// One time mark on a radio's sample clock: sample time sampleS (seconds of samples since the stream began, n / nominal rate) was at utcNs.
// sigmaNs is the random part (it averages down over many marks), biasNs the part that does not (cable, driver and filter delays).
struct TimeMark {
    TimeSrc src = TimeSrc::None;
    double sampleS = 0;
    int64_t utcNs = 0;
    double sigmaNs = 0;
    double biasNs = 0;
};

// One radio's sample clock disciplined against one source: utc = a + b (s - sRef), weighted least squares over the last windowS seconds of marks,
// with a prior that the rate error is within +-50 ppm (so one mark already stamps, with the drift in its error). A mark far off the fit is refused;
// three refused in a row mean the timeline jumped (lost samples, a restart): the clock starts again from the newest mark.
class SampleClock {
public:
    explicit SampleClock(double windowS = 120.0, size_t maxMarks = 256) : windowS_(windowS), maxMarks_(maxMarks) {}
    void reset();
    bool add(const TimeMark& m);              // false: refused as an outlier
    bool valid() const { return !marks_.empty(); }
    bool stamp(double sampleS, int64_t& utcNs, double& sigmaNs) const;   // false while there is no mark
    double ppm() const;                       // the radio's clock rate error: + = it runs fast (more samples per second than its nominal rate)
    double ppmSigma() const;
    TimeSrc source() const { return src_; }
    size_t marks() const { return marks_.size(); }
    uint64_t refused() const { return refused_; }
    uint64_t restarts() const { return restarts_; }
    double lastMarkS() const { return marks_.empty() ? -1e300 : marks_.back().sampleS; }
    double rmsNs() const { return rmsNs_; }   // scatter of the marks about the fit
private:
    void fit();
    double windowS_;
    size_t maxMarks_;
    std::deque<TimeMark> marks_;
    TimeSrc src_ = TimeSrc::None;
    // fit, relative to the newest mark's utcNs (baseNs_) so a double keeps nanoseconds
    int64_t baseNs_ = 0;
    double sRef_ = 0, a_ = 0, b_ = 1, varA_ = 0, varB_ = 0, covAB_ = 0, biasNs_ = 0, rmsNs_ = 0;
    int badRun_ = 0;
    uint64_t refused_ = 0, restarts_ = 0;
};

// The system clock as a source: the time a block of samples arrived marks its last sample (the radio's buffering is unknown: it is the error).
// kind: 0 not synchronised, 1 NTP, 2 PTP. userSigmaUs > 0 replaces the typical figure for the clock itself.
TimeMark systemClockMark(int kind, double sampleS, int64_t nowNs, double userSigmaUs, double bufferS);

// OnAir's GNSS receiver as a source (a second radio): its fix gives UTC at its own signal time. streamToLocalS moves that signal time onto this
// radio's sample time (0 when both streams come from one device / one clock); alignSigmaNs says how well that is known.
struct GnssTime { double signalS = 0; double unixAtSignal = 0; float tdop = 0; float residualRmsM = 0; bool valid = false; };
bool gnssMark(const GnssTime& g, double streamToLocalS, double alignSigmaNs, TimeMark& out);

// ---------------------------------------------------------------- 3. the sferic detector with precise timing
struct Pulse {
    double sampleS = 0;      // the leading edge: where the envelope first reaches half of its peak, interpolated (sample time, seconds)
    double sigmaS = 0;       // its 1-sigma error from the noise and the slope
    float peakDb = 0;        // peak power over the noise floor
    float amp = 0;           // peak amplitude, linear (full scale 1)
    float widthUs = 0;       // time above half of the peak
    float riseUs = 0;        // 10 % to 90 % of the peak on the leading edge
    float freqHz = 0;        // the mean frequency of the burst relative to the tuned centre (a waveform feature)
};
class PulseTimer {
public:
    void configure(double rate);
    void reset();
    void setThresholdDb(double db) { thrDb_ = db; }
    void feed(const std::complex<float>* x, size_t n, std::vector<Pulse>& out);
    double floorDb() const;
    bool floorValid() const { return floorOk_; }
    double rate() const { return rate_; }
    uint64_t samples() const { return n_; }
    uint64_t tooLong() const { return tooLong_; }
private:
    void finish(std::vector<Pulse>& out);
    double rate_ = 0, thrDb_ = 12;
    size_t W_ = 1, B_ = 1, hold_ = 1, pre_ = 1, maxLen_ = 1;
    std::vector<float> win_;
    size_t wpos_ = 0;
    double wsum_ = 0, blockSum_ = 0;
    size_t blockN_ = 0, sinceMedian_ = 0;
    std::deque<float> blocks_;
    double floorLin_ = 0;
    bool floorOk_ = false;
    uint64_t n_ = 0, tooLong_ = 0;
    std::deque<std::complex<float>> hist_;    // the last pre_ samples, for the leading edge
    bool active_ = false;
    std::vector<std::complex<float>> ev_;     // the samples of the event being collected
    uint64_t evStart_ = 0;
    size_t below_ = 0;
};

// ---------------------------------------------------------------- 4. common view
// A reference transmitter that every station receives (a VLF / LF MSK station: the symbol clock of a frequency-standard-driven transmitter).
struct RefTx {
    int id = 0;                 // 1..255, the same number on every station
    std::string name;           // e.g. "NAA 24.0 kHz"
    double freqHz = 0;          // its carrier, RF
    double baud = 200;          // MSK symbol rate
    double lat = 0, lon = 0;    // its position (the propagation delay to each station is taken off)
};
struct RefObs {
    int refId = 0;
    double sampleS = 0;         // a symbol boundary, sample time (the one nearest the middle of the block)
    double sigmaS = 0;          // its 1-sigma error (from the signal to noise ratio of the fit)
    float snrDb = 0;            // the two lines of the squared signal over their noise
    double freqErrHz = 0;       // the carrier offset found (tracked)
};
// MSK symbol timing: mix the carrier down, low-pass and decimate to about 8 samples per symbol, square (MSK squared has lines at +-baud/2),
// correlate the two lines over 50 ms sub-blocks: arg(A-) - arg(A+) = 2 pi tau / T gives the boundary tau modulo T, free of the carrier phase and
// of a small carrier offset (both lines move together). That first estimate carries the data's self-noise (tens of microseconds), so it is
// refined decision-directed: the bits are read with it, and the timing of the whole phase path (bits known, sample clock drift from block to
// block) is the maximum of the summed sub-block correlations. One observation per blockS.
class MskRefTimer {
public:
    void configure(double rate, double offsetHz, double baud, int refId, double blockS = 1.0);
    void reset();
    void feed(const std::complex<float>* x, size_t n, std::vector<RefObs>& out);
    bool locked() const { return locked_; }
    double lastSigmaS() const { return lastSigma_; }
    float lastSnrDb() const { return lastSnr_; }
    double freqErrHz() const { return fErr_; }
private:
    void subBlock();
    void block(std::vector<RefObs>& out);
    double rate_ = 0, off_ = 0, baud_ = 200, blockS_ = 1;
    int id_ = 0;
    size_t D_ = 1, subN_ = 1, subsPerBlock_ = 1;
    double ph_ = 0, dph_ = 0;
    std::complex<double> acc_ = 0;
    size_t accN_ = 0;
    uint64_t nIn_ = 0, nDec_ = 0;
    double fs2_ = 0;
    std::complex<double> ap_ = 0, am_ = 0;    // the two lines over the running sub-block
    double pz_ = 0;                           // power of the squared signal in it
    size_t subFill_ = 0;
    std::vector<std::complex<double>> subD_, subP_;   // A- conj(A+) and A+ A- of each sub-block
    std::vector<double> subPow_, subT_;
    std::vector<std::complex<double>> yb_;    // the decimated samples of the running block and their sample times (the refinement)
    std::vector<double> tb_;
    std::deque<double> hist_;                 // the last boundaries found
    double eps_ = 0;                          // the sample clock rate error from them
    int nObs_ = 0, nBlocks_ = 0;
    double fErr_ = 0;
    bool locked_ = false;
    double lastSigma_ = 0;
    float lastSnr_ = 0;
};

// The server's view of one reference: each station reports the time (on its own disciplined clock) of a symbol boundary; the true arrival is
// E + k T + d_i / v (E is the same for everyone). The correction for a station is (E + d_i / v - reading) folded into one symbol period.
// E comes from the stations that also have GPS time; with none, E = 0 and those stations form their own clock group (the solver gives each
// group its own time offset, so position stays right and only the strike time carries the unknown E).
struct PropModel;
class CommonViewAligner {
public:
    void setRef(const RefTx& r) { ref_ = r; }
    const RefTx& ref() const { return ref_; }
    // a station's boundary reading anchorNs (any boundary: they are T apart), its 1-sigma error, its position and whether its clock is absolute (GPS)
    void report(uint32_t station, int64_t anchorNs, double sigmaNs, double lat, double lon, bool absolute, const PropModel& pm);
    // the correction to add to that station's detections, its 1-sigma error, and whether it is tied to UTC (group 0) or only to the reference
    bool correction(uint32_t station, double& corrNs, double& sigmaNs, bool& tiedToUtc) const;
    double periodNs() const { return ref_.baud > 0 ? 1e9 / ref_.baud : 0; }
    size_t stations() const { return st_.size(); }
private:
    struct St { int64_t anchorNs = 0; double sigmaNs = 0, delayNs = 0; bool absolute = false; };
    RefTx ref_;
    std::map<uint32_t, St> st_;
};
double wrapPeriod(double x, double T);      // into (-T/2, T/2]

// ---------------------------------------------------------------- 5. geodesy, propagation, the solver
double geodesicM(double lat1, double lon1, double lat2, double lon2, double* azDeg = nullptr);   // WGS-84 (Vincenty); azimuth at point 1
void geodesicDirect(double lat, double lon, double azDeg, double distM, double& lat2, double& lon2);
struct PropModel {
    double velocityFactor = 0.9993;   // ground-wave group velocity over c (VLF / LF over land and sea: about 0.999)
    bool hops = false;                // long paths by ionosphere hops instead of the ground wave
    double hopHeightKm = 80;          // reflection height (about 70 km by day, 90 km at night)
    double hopFromKm = 1500;          // paths longer than this take the hop model when hops is on
    double hopMaxKm = 3000;           // the longest single hop; a longer path takes several
};
double propagationS(const PropModel& pm, double distM);   // travel time over a path of this ground length

struct Obs {
    uint32_t station = 0;
    double lat = 0, lon = 0;
    double tS = 0;        // arrival, seconds from the group's reference time (all observations share one reference)
    double sigmaS = 1e-6;
    int group = 0;        // clock group: 0 = UTC (GPS, GNSS, tied common view); others: common view without a UTC tie, one offset each
    double ambS = 0;      // >0: this time is known only modulo ambS (common view symbol period); the solver may move it by whole periods
};
struct SolveOpts {
    int maxIter = 30;
    double huberK = 2.5;          // residuals beyond k sigma are down-weighted
    double outlierSigma = 4.0;    // with two degrees of freedom left, a residual beyond this is dropped and the fix done again
    double chi2Prob = 0.999;      // the final fit must pass the chi-square test at this level (false matches fail it)
    double searchKm = 4000;       // the start grid reaches this far from the stations' centre
    bool allowThree = true;       // three stations: an exact fix with no check (flagged)
};
struct Fix {
    bool ok = false;
    double lat = 0, lon = 0;
    double t0S = 0;               // strike time (group 0, or the first group present), same reference as Obs::tS
    double semiMajorM = 0, semiMinorM = 0, majorAzDeg = 0;   // 95 % error ellipse
    double sigmaNorthM = 0, sigmaEastM = 0;
    double chi2 = 0, rmsS = 0;
    int dof = 0, used = 0;
    bool unchecked = false;       // no degree of freedom left: nothing could reject a wrong match
    bool ambiguous = false;       // a second, different position fits as well (three stations, poor geometry)
    std::vector<uint32_t> dropped;   // stations left out as outliers
    std::vector<double> residualS;   // per used observation, in the order of Obs (dropped ones: NaN)
    std::string why;              // when !ok
};
Fix solveTdoa(const std::vector<Obs>& obs, const PropModel& pm, const SolveOpts& o = SolveOpts());
double chi2Quantile(double p, int dof);   // Wilson-Hilferty

// ---------------------------------------------------------------- 6. matching on the server
struct Det {
    uint32_t station = 0;
    int64_t tNs = 0;              // corrected arrival (UTC, or the station's common-view clock group)
    double sigmaNs = 0;
    bool fine = false;            // usable for the fix (fineTiming); else only for association
    int group = 0;
    double ambNs = 0;
    double lat = 0, lon = 0;
    float peakDb = 0, amp = 0, widthUs = 0, riseUs = 0, freqHz = 0;
    TimeSrc src = TimeSrc::None;
};
struct MatchOpts {
    double holdS = 2.0;           // a detection waits this long for the others (network delay) before its group is solved
    int minStations = 4;          // fine-timed stations needed for a fix (3 allowed, then unchecked)
    double slackNs = 2000;        // added to the light-time bound between two stations
    double maxWidthRatio = 8;     // a candidate whose pulse width differs more than this is not the same strike
    size_t maxPending = 20000;
};
struct Strike {
    Fix fix;
    int64_t timeNs = 0;           // strike time (UTC when group 0 took part)
    std::vector<uint32_t> stations;      // fine stations used
    std::vector<uint32_t> alsoHeard;     // coarse-timed stations whose detection fits the strike
    bool utc = true;
};
struct MatchStats { uint64_t groups = 0, fixes = 0, rejected = 0, unmatched = 0, coarseOnly = 0, tooFew = 0; };
class Matcher {
public:
    explicit Matcher(MatchOpts o = MatchOpts(), PropModel pm = PropModel(), SolveOpts so = SolveOpts()) : o_(o), pm_(pm), so_(so) {}
    void setOpts(const MatchOpts& o) { o_ = o; }
    void setProp(const PropModel& pm) { pm_ = pm; }
    void add(const Det& d);
    // solves every detection older than nowNs - holdS; returns the strikes found
    std::vector<Strike> process(int64_t nowNs);
    const MatchStats& stats() const { return st_; }
    size_t pending() const { return pend_.size(); }
private:
    MatchOpts o_;
    PropModel pm_;
    SolveOpts so_;
    std::deque<Det> pend_;        // sorted by tNs
    MatchStats st_;
};

// ---------------------------------------------------------------- 7. packets between stations
// [4 "OALP"][u8 version 1][u8 type][u32 station][u32 seq][i64 send time ns][body][16 bytes HMAC-SHA256 of everything before, truncated]
// type 1 detections: station header, u8 count, count x 32-byte records; type 2 reference: station header, refId, anchor, sigma, snr, baud;
// type 3 hello: station header + name (<= 24 bytes). Station header: i32 lat 1e-7 deg, i32 lon, i16 alt m, u8 time source, u8 flags,
// u32 clock sigma ns (saturating). Little endian. The key is SHA-256 of the network key text, so any length works.
enum class PktType : uint8_t { Detections = 1, Reference = 2, Hello = 3 };
constexpr size_t kMaxPacket = 1200;
constexpr size_t kMaxDetPerPacket = 32;
struct StationHdr {
    double lat = 0, lon = 0, altM = 0;
    TimeSrc src = TimeSrc::None;
    uint8_t flags = 0;            // bit 0 position set by the user, bit 1 common-view clock in use (detections need the server's correction)
    double clockSigmaNs = 0;
};
struct DetRec { int64_t utcNs = 0; double sigmaNs = 0; float peakDb = 0, amp = 0, widthUs = 0, riseUs = 0, freqHz = 0; };
struct RefRec { int refId = 0; int64_t anchorNs = 0; double sigmaNs = 0; float snrDb = 0; double baud = 0; };
struct Packet {
    PktType type = PktType::Detections;
    uint32_t station = 0;
    uint32_t seq = 0;
    int64_t sendNs = 0;
    StationHdr hdr;
    std::vector<DetRec> dets;
    RefRec ref;
    std::string name;
};
std::vector<uint8_t> networkKey(const std::string& text);    // SHA-256 of the text
std::vector<uint8_t> encodePacket(const Packet& p, const std::vector<uint8_t>& key);
enum class PktErr { Ok, Short, Magic, Version, Auth, Malformed };
PktErr decodePacket(const uint8_t* d, size_t n, const std::vector<uint8_t>& key, Packet& out);
uint32_t stationIdOf(const std::string& name);              // FNV-1a of the name, never 0

// Replays and stale packets: per station, a sequence window of 64 and a send time within maxSkewS of the server's clock.
class ReplayGuard {
public:
    explicit ReplayGuard(double maxSkewS = 600, size_t maxStations = 256) : skewNs_((int64_t)(maxSkewS * 1e9)), max_(maxStations) {}
    enum Result { Fresh, Replay, Stale, Full };
    Result check(uint32_t station, uint32_t seq, int64_t sendNs, int64_t nowNs);
private:
    struct W { uint32_t top = 0; uint64_t bits = 0; bool any = false; int64_t lastSend = 0; };
    int64_t skewNs_;
    size_t max_;
    std::map<uint32_t, W> w_;
};

} // namespace dect2::ltpos
