// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Kristijonas Adomaitis and OnAir contributors

// Band plans: the channel rasters and allocations of a country (TV channels, FM, DAB, AM, airband, PMR, marine, ham, ISM).
// The built-in plans come from data/bandplans/builtin.json (embedded at build time); users add their own from CSV or JSON files.
// The file format is described in docs/band-plans.md. All frequencies are in Hz.
#pragma once
#include <string>
#include <vector>

namespace dect2::bandplan {

struct NamedChannel { std::string label; double centreHz = 0; };

struct Channel { int number = -1; std::string label; double centreHz = 0; };   // number -1: the channel has no number

// One band of a plan. Channels are either a regular raster (stepHz > 0: centre = firstCentreHz + k * stepHz, numbered from firstChannel)
// or an explicit list. A band with neither is a plain range (a ham or ISM allocation).
struct Band {
    std::string name;                 // "UHF TV", "FM broadcast", "20 m"
    std::string service = "other";    // tv fm dab am air pmr marine ham ism other
    double startHz = 0, endHz = 0;
    double stepHz = 0;                // channel raster, 0: no regular raster
    double bandwidthHz = 0;           // channel width (0: a point on the raster, no width)
    int firstChannel = -1;            // number of the first channel; -1: the channels are not numbered
    double firstCentreHz = 0;         // centre of the first channel; 0: startHz
    std::vector<std::string> modes;   // suggested modes (names as in the mode list: "DVB-T2", "FM", "Airband" ...)
    std::vector<NamedChannel> channels;   // explicit channels instead of a raster
    std::string note, source;

    int count() const;                          // number of channels (0 for a plain range)
    std::vector<Channel> list(size_t cap = 100000) const;
    bool channelCentre(int number, double& hz) const;   // by number (raster bands that are numbered)
    // the channel whose centre is nearest to hz, if within tolHz (default: half the step, or half the width); false when none
    bool channelAt(double hz, Channel& out, double tolHz = -1) const;
    bool contains(double hz) const { return hz >= startHz && hz <= endHz; }
};

struct Plan {
    std::string id, name, region, source, note;
    int ituRegion = 0;                       // 1, 2, 3 or 0 when unknown
    std::vector<std::string> countries;      // ISO 3166 codes ("DE", "GB")
    bool verified = true;                    // false: some channel numbers or limits are not verified (see the note)
    bool builtin = false;
    std::vector<Band> bands;
    const Band* firstBand(const std::string& service) const;
    // the bands of one service, optionally only those that list `mode` among their suggested modes
    std::vector<const Band*> bandsFor(const std::string& service, const std::string& mode = std::string()) const;
};

bool validate(Plan& p, std::string& err);   // checks every field, fills in the defaults; the message names the band and the field
bool parseJson(const std::string& text, Plan& out, std::string& err);   // one plan; never throws, malformed input gives false and a message
bool parseCsv(const std::string& text, Plan& out, std::string& err);
std::string toJson(const Plan& p);
std::string toCsv(const Plan& p);
std::string slug(const std::string& name);   // "My plan (DE)" -> "my-plan-de"

// The built-in plans plus the ones in the user's folder. A user plan is one JSON file <id>.json, written atomically.
class Registry {
public:
    Registry();                                   // built-in plans only
    void setUserDir(const std::string& dir);      // and load what is in it (damaged files are skipped and listed in log())
    const std::vector<Plan>& plans() const { return plans_; }
    const Plan* find(const std::string& id) const;
    // import a CSV or JSON file (by its content); the plan gets the id "user-<slug of its name>", a plan with the same name is replaced
    bool importFile(const std::string& path, std::string& id, std::string& err, bool* replaced = nullptr);
    bool importText(const std::string& text, std::string& id, std::string& err, bool* replaced = nullptr);
    bool remove(const std::string& id, std::string& err);   // user plans only
    bool exportFile(const std::string& id, const std::string& path, bool csv, std::string& err) const;
    const std::vector<std::string>& log() const { return log_; }
    // the plan for a UI language ("de", "pt") and a locale ("en_GB.UTF-8"; may be empty): its id, or "" when nothing fits
    std::string defaultFor(const std::string& lang, const std::string& locale) const;
    static std::string localeFromEnv();

private:
    std::vector<Plan> plans_;
    std::string dir_;
    std::vector<std::string> log_;
    bool store(Plan& p, std::string& err, bool* replaced);
};

const char* builtinBandPlanText();   // data/bandplans/builtin.json as compiled in (Tools > Data packs exports it)
const std::vector<Plan>& builtinPlans();   // parsed once

} // namespace dect2::bandplan
