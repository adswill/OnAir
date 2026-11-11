// Product cadence watchdog (3.9, docs/SMART5-sat-3.9.md item 18). Geostationary weather satellites send each product on a fixed
// schedule (GOES full disk every 10 min, GK-2A every 10 min, Elektro every 15 or 30 min, text and admin files hourly). This learns
// each product's period from its arrivals and, when one is overdue, tells the two causes apart:
//   lock held and frames kept arriving through the expected time -> "the satellite did not send it"
//   lock lost (or no frames) during that time                     -> "you lost lock"
// Feed: product(name, t) for each completed file, link(t, locked, framesOk) once a second.
#pragma once
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace dect2 {
namespace smart5 {

enum class MissCause { None, NotSent, LostLock };

struct ProductState {
    std::string name;
    double periodSec = 0;     // 0 until learned (three intervals)
    double last = 0, nextDue = 0;
    MissCause missing = MissCause::None;
    int missedCount = 0;
};

class ProductCadence {
public:
    explicit ProductCadence(double graceFrac = 0.5) : grace_(graceFrac) {}
    void product(const std::string& name, double t);
    void link(double t, bool locked, bool framesOk);
    void tick(double now);
    std::vector<ProductState> states() const;
    std::string text(const std::string& name) const;
private:
    struct P { std::deque<double> arr; ProductState s; };
    std::map<std::string, P> p_;
    std::deque<std::pair<double, bool>> link_;   // (t, good) for the last day
    double grace_;
    bool linkGoodBetween(double a, double b) const;
};

} // namespace smart5
} // namespace dect2
