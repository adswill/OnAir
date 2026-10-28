// TV channel plans (core/include/dect2/channels/tvplan.h): every plan is sorted, has no overlapping channels, has sane widths and unique labels;
// every channel makes the round trip label -> frequency -> label; known channels have the published edges and centres; the defaults per country
// and standard; the analog carrier positions; and docs/TV-CHANNELS.md names every plan.
#include "dect2/channels/tvplan.h"
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

using namespace dect2::tvplan;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { failures++; std::printf("FAIL line %d: ", __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static bool near(double a, double b, double tol = 1.0) { return std::fabs(a - b) <= tol; }

// a channel of a plan must have these edges (Hz) and centre
static void edges(const char* plan, const char* label, double lowMhz, double highMhz, double offsetHz = 0) {
    const Plan* p = find(plan);
    CHECK(p, "plan %s exists", plan);
    if (!p) return;
    const Channel* c = p->byLabel(label);
    CHECK(c, "%s: channel %s exists", plan, label);
    if (!c) return;
    CHECK(near(c->lowHz(), lowMhz * 1e6 + offsetHz, 2) && near(c->highHz(), highMhz * 1e6 + offsetHz, 2) && near(c->centreHz, (lowMhz + highMhz) / 2 * 1e6 + offsetHz, 2),
          "%s %s: %.4f-%.4f MHz, centre %.4f", plan, label, c->lowHz() / 1e6, c->highHz() / 1e6, c->centreHz / 1e6);
}

int main() {
    const std::vector<Plan>& all = plans();
    CHECK(all.size() >= 16, "plans: %zu", all.size());
    std::set<std::string> ids;
    for (const Plan& p : all) {
        CHECK(ids.insert(p.id).second, "plan id %s is unique", p.id.c_str());
        CHECK(!p.name.empty() && !p.source.empty() && p.stds != 0 && !p.channels.empty(), "%s: name, source, standards, channels", p.id.c_str());
        for (const std::string& cc : p.countries) CHECK(cc.size() == 2 && std::isupper((unsigned char)cc[0]) && std::isupper((unsigned char)cc[1]), "%s: country code %s", p.id.c_str(), cc.c_str());
        const bool analog = (p.stds & Analog) != 0;
        CHECK(!analog || (p.visionOffHz > 0 && p.soundOffHz > 0 && !p.system.empty()), "%s: analog plan has its carriers", p.id.c_str());
        std::set<std::string> labels;
        double prevHigh = -1, prevCentre = -1;
        for (const Channel& c : p.channels) {
            CHECK(labels.insert(c.label).second, "%s: label %s is unique", p.id.c_str(), c.label.c_str());
            CHECK(c.bwHz >= 5e6 && c.bwHz <= 8e6, "%s %s: width %.0f", p.id.c_str(), c.label.c_str(), c.bwHz);
            CHECK(c.centreHz > prevCentre, "%s %s: ascending", p.id.c_str(), c.label.c_str());
            CHECK(c.centreHz > 40e6 && c.centreHz < 1000e6, "%s %s: centre %.3f MHz in the TV bands", p.id.c_str(), c.label.c_str(), c.centreHz / 1e6);
            if (!p.overlaps) CHECK(c.lowHz() >= prevHigh - 1, "%s %s: overlaps the channel before it", p.id.c_str(), c.label.c_str());
            prevHigh = std::max(prevHigh, c.highHz()); prevCentre = c.centreHz;
            // round trip: the label finds the channel, its centre finds the label, and so do both edges' insides
            const Channel* b = p.byLabel(c.label);
            CHECK(b == &c, "%s %s: byLabel", p.id.c_str(), c.label.c_str());
            const Channel* a = p.at(c.centreHz);
            CHECK(a && a->label == c.label, "%s %s: at(centre) gave %s", p.id.c_str(), c.label.c_str(), a ? a->label.c_str() : "none");
            if (!p.overlaps) {
                const Channel* lo = p.at(c.lowHz() + 100e3); const Channel* hi = p.at(c.highHz() - 100e3);
                CHECK(lo && lo->label == c.label && hi && hi->label == c.label, "%s %s: just inside the edges", p.id.c_str(), c.label.c_str());
            }
            CHECK(p.at(c.centreHz + c.bwHz) != &c, "%s %s: a whole width away is another channel", p.id.c_str(), c.label.c_str());
            // sloppy typing
            CHECK(p.byLabel("ch " + c.label) == &c && p.byLabel("Channel" + c.label) == &c, "%s %s: typed with 'ch'", p.id.c_str(), c.label.c_str());
        }
        // step goes through the whole plan and ends
        int n = 0;
        for (const Channel* c = &p.channels.front(); c; c = p.step(c, +1)) n++;
        CHECK(n == (int)p.channels.size(), "%s: step visits %d of %zu", p.id.c_str(), n, p.channels.size());
        CHECK(!p.step(&p.channels.back(), +1) && !p.step(&p.channels.front(), -1), "%s: step ends", p.id.c_str());
    }

    // ---- known channels: lower and upper edge in MHz (the sources are in docs/TV-CHANNELS.md) ----
    edges("eu", "E5", 174, 181); edges("eu", "E12", 223, 230); edges("eu", "E2", 47, 54); edges("eu", "21", 470, 478); edges("eu", "48", 686, 694); edges("eu", "69", 854, 862);
    edges("ru", "6", 174, 182); edges("ru", "12", 222, 230); edges("ru", "21", 470, 478);
    edges("au", "6", 174, 181); edges("au", "9A", 202, 209); edges("au", "12", 223, 230); edges("au", "28", 526, 533); edges("au", "51", 687, 694);
    edges("us", "2", 54, 60); edges("us", "4", 66, 72); edges("us", "5", 76, 82); edges("us", "6", 82, 88); edges("us", "7", 174, 180); edges("us", "13", 210, 216);
    edges("us", "14", 470, 476); edges("us", "36", 602, 608); edges("us", "51", 692, 698); edges("us", "69", 800, 806);
    const double seventh = 1e6 / 7.0;
    edges("jp", "13", 470, 476, seventh); edges("jp", "52", 704, 710, seventh); edges("jp", "62", 764, 770, seventh);
    edges("jp", "7", 188, 194, seventh); edges("jp", "8", 192, 198, seventh);
    edges("br", "2", 54, 60, seventh); edges("br", "7", 174, 180, seventh); edges("br", "14", 470, 476, seventh); edges("br", "51", 692, 698, seventh);
    edges("cn", "1", 48.5, 56.5); edges("cn", "6", 167, 175); edges("cn", "12", 215, 223); edges("cn", "13", 470, 478); edges("cn", "24", 558, 566);
    edges("cn", "25", 606, 614); edges("cn", "68", 950, 958);
    edges("cable-eu", "S1", 104, 111); edges("cable-eu", "S10", 167, 174); edges("cable-eu", "S11", 230, 237); edges("cable-eu", "S20", 293, 300);
    edges("cable-eu", "S21", 302, 310); edges("cable-eu", "S41", 462, 470); edges("cable-eu", "E5", 174, 181); edges("cable-eu", "U21", 470, 478);
    // the cable plan reaches UHF by its plain number too
    CHECK(find("cable-eu")->byLabel("21") == find("cable-eu")->byLabel("U21"), "cable: 21 is the UHF channel");
    CHECK(find("cable-eu")->byLabel("S21") != find("cable-eu")->byLabel("21"), "cable: S21 is not UHF 21");
    CHECK(find("eu")->byLabel("E21") == find("eu")->byLabel("21"), "eu: E21 typed is channel 21");

    // ---- the centres people know ----
    CHECK(near(find("eu")->byLabel("21")->centreHz, 474e6), "UHF 21 centre 474 MHz");
    CHECK(near(find("us")->byLabel("2")->centreHz, 57e6), "US channel 2 centre 57 MHz");
    CHECK(near(find("us")->byLabel("30")->centreHz, 569e6), "US channel 30 centre 569 MHz");
    CHECK(near(find("au")->byLabel("28")->centreHz, 529.5e6), "AU 28 centre 529.5 MHz");
    CHECK(near(find("eu")->byLabel("E2")->centreHz, 50.5e6), "E2 centre 50.5");
    // the same number is not the same frequency in another plan
    CHECK(find("eu")->byLabel("21")->centreHz == find("ru")->byLabel("21")->centreHz && find("eu")->byLabel("21")->centreHz != find("us")->byLabel("21")->centreHz, "channel 21 differs between Europe and the US");
    CHECK(find("us")->byLabel("2")->centreHz != find("cn")->byLabel("2")->centreHz && find("cn")->byLabel("2")->centreHz != find("jp")->byLabel("2")->centreHz, "channel 2 differs by region");

    // ---- legacy marks ----
    CHECK(find("us")->byLabel("37")->legacy && find("us")->byLabel("52")->legacy && !find("us")->byLabel("36")->legacy && !find("us")->byLabel("51")->legacy, "US legacy marks");
    CHECK(find("eu")->byLabel("49")->legacy && !find("eu")->byLabel("48")->legacy, "EU 700 MHz band is legacy");
    CHECK(find("jp")->byLabel("53")->legacy && !find("jp")->byLabel("52")->legacy, "Japan 53-62 legacy");

    // ---- analog carriers: vision above the lower edge, sound above the vision ----
    {
        const Plan* p = find("an-i"); const Channel* c = p->byLabel("21");
        CHECK(near(c->lowHz() + p->visionOffHz, 471.25e6) && near(c->lowHz() + p->visionOffHz + p->soundOffHz, 477.25e6), "PAL I ch 21: vision 471.25, sound 477.25");
        p = find("an-bg"); c = p->byLabel("E5");
        CHECK(near(c->lowHz() + p->visionOffHz, 175.25e6) && near(c->lowHz() + p->visionOffHz + p->soundOffHz, 180.75e6), "B/G E5: vision 175.25, sound 180.75");
        p = find("an-m"); c = p->byLabel("2");
        CHECK(near(c->lowHz() + p->visionOffHz, 55.25e6) && near(c->lowHz() + p->visionOffHz + p->soundOffHz, 59.75e6), "NTSC M ch 2: vision 55.25, sound 59.75");
        p = find("an-dk"); c = p->byLabel("1");
        CHECK(near(c->lowHz() + p->visionOffHz, 49.75e6) && near(c->lowHz() + p->visionOffHz + p->soundOffHz, 56.25e6), "D/K ch 1: vision 49.75, sound 56.25");
        p = find("an-au"); c = p->byLabel("5A");
        CHECK(near(c->lowHz() + p->visionOffHz, 138.25e6), "AU 5A vision 138.25");
        p = find("an-j"); c = p->byLabel("13");
        CHECK(near(c->lowHz() + p->visionOffHz, 471.25e6), "Japan 13 vision 471.25");
    }

    // ---- defaults per country and standard ----
    auto def = [](const char* cc, unsigned m) { const Plan* p = defaultPlan(cc, m); return p ? p->id : std::string("-"); };
    CHECK(def("DE", DvbAny) == "eu" && def("lt", DvbT2) == "eu" && def("GB", DvbT) == "eu" && def("ZA", DvbT2) == "eu", "Europe/Africa DVB-T default");
    CHECK(def("RU", DvbT2) == "ru" && def("AU", DvbT) == "au" && def("CO", DvbT2) == "am6-dvb", "Russia, Australia, Colombia");
    CHECK(def("US", Atsc) == "us" && def("CA", Atsc3) == "us" && def("MX", Atsc) == "us" && def("KR", Atsc) == "kr", "ATSC defaults");
    CHECK(def("JP", IsdbT) == "jp" && def("BR", IsdbT) == "br" && def("AR", IsdbT) == "br" && def("CO", IsdbT) == "br", "ISDB-T defaults (South America falls back to Brazil)");
    CHECK(def("CN", Dtmb) == "cn" && def("XX", Dtmb) == "cn", "DTMB default");
    CHECK(def("US", DvbAny) == "eu" && def("", DvbAny) == "eu", "no DVB-T plan for the US: the European one");
    CHECK(def("DE", DvbC) == "cable-eu" && def("US", DvbC) == "cable-eu", "DVB-C default");
    CHECK(def("GB", Analog) == "an-i" && def("FR", Analog) == "an-l" && def("US", Analog) == "an-m" && def("RU", Analog) == "an-dk" && def("JP", Analog) == "an-j" && def("DE", Analog) == "an-bg" && def("AU", Analog) == "an-au" && def("CN", Analog) == "an-cn", "analog defaults");
    // a country is the default of only one plan per standard
    for (unsigned bit : {DvbT, DvbT2, Atsc, Atsc3, IsdbT, Dtmb, DvbC, Analog}) {
        std::set<std::string> seen;
        for (const Plan& p : all) if (p.stds & bit) for (const std::string& cc : p.countries) CHECK(seen.insert(cc).second, "%s listed twice for standard bit %u", cc.c_str(), bit);
    }
    CHECK(plansFor(Atsc).size() >= 2 && plansFor(DvbAny).size() >= 4 && plansFor(Analog).size() >= 7, "plansFor");

    // ---- step from a frequency that is no channel ----
    {
        const Plan* p = find("eu");
        const Channel* up = p->step(nullptr, +1, 476e6);   // between channels: the next one above
        CHECK(up && up->label == "22", "next above 476 MHz is 22 (got %s)", up ? up->label.c_str() : "none");
        const Channel* dn = p->step(nullptr, -1, 485e6);
        CHECK(dn && dn->label == "22", "next below 485 MHz is 22 (got %s)", dn ? dn->label.c_str() : "none");
        CHECK(!p->step(nullptr, +1, 900e6), "above the plan: none");
        CHECK(p->at(100e6) == nullptr && describe(*p, 474e6) == "21 (474.000 MHz)" && describe(*p, 100e6).empty(), "describe");
    }

    // ---- the docs name every plan ----
    {
        std::ifstream f(std::string(DECT2_SOURCE_DIR) + "/docs/TV-CHANNELS.md");
        std::stringstream ss; ss << f.rdbuf();
        const std::string d = ss.str();
        CHECK(!d.empty(), "docs/TV-CHANNELS.md is readable");
        for (const Plan& p : all) CHECK(d.find("`" + p.id + "`") != std::string::npos, "docs name plan %s", p.id.c_str());
    }

    std::printf(failures ? "%d failures\n" : "all tv plan checks passed\n", failures);
    return failures ? 1 : 0;
}
