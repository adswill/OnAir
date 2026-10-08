// The shared channel database (github.com/adswill/OnAir-channels): CSV files per mode and country, the merge of a submission into them,
// and the text a user submits (a pre-filled GitHub issue). Pure functions, no network: scripts/ingest.py in the data repo follows the same rules.
#pragma once
#include <string>
#include <vector>

namespace dect2 {
namespace chdb {

struct Row {                          // one channel of one city
    std::string city;
    double freqMhz = 0, bwMhz = 0;
    std::string standard, network;
    std::vector<std::string> services;
    double snrDb = 0;
    int reports = 1;
    std::string firstSeen, lastSeen;  // YYYY-MM-DD
};

struct City { std::string name, admin1; double lat = 0, lon = 0; long population = 0; };
struct Country { std::string iso2, name; };

struct Submission {
    std::string mode, country, city;  // mode folder (dvb-t, atsc, ...), ISO 3166 alpha-2 code, city name from cities/<ISO2>.csv
    std::vector<Row> rows;            // only freq, bw, standard, network, services and snr are sent
};

// CSV (RFC 4180): fields with a comma, quote or line break are quoted, quotes doubled.
std::string csvQuote(const std::string& f);
std::vector<std::vector<std::string>> csvParse(const std::string& text);   // every record; blank lines skipped
std::vector<std::string> splitServices(const std::string& s, char sep);   // "a|b" -> {"a","b"}, empty parts dropped

std::vector<Row> parseChannels(const std::string& csv);   // <mode>/<ISO2>.csv, header city,freq_mhz,bw_mhz,standard,network,services,snr_db,reports,first_seen,last_seen
std::string writeChannels(std::vector<Row> rows);         // sorted by city then frequency
std::vector<City> parseCities(const std::string& csv);    // name,admin1,lat,lon,population
std::vector<Country> parseCountries(const std::string& csv);   // iso2,name

// Folds the rows of one submission (all for `city`) into db. Same city and frequency within 0.05 MHz: reports +1, last_seen = date, best SNR,
// services merged; otherwise a new row. Rows of one submission that sit on the same frequency count once.
void merge(std::vector<Row>& db, const std::string& city, const std::vector<Row>& in, const std::string& date);

// The issue text:  mode: x / country: XX / city: Name / a ```csv block (freq_mhz,bw_mhz,standard,network,services,snr_db).
std::string issueBody(const Submission& s, bool withServices = true);
bool parseIssueBody(const std::string& body, Submission& out, std::string& err);

std::string urlEncode(const std::string& s);
// The links that open the pre-filled issue; each is at most maxLen characters. Too long: first without the services, then split into several.
std::string submissionUrl(const Submission& s, bool withServices, const std::string& repo = "adswill/OnAir-channels");
// The issues a submission is sent as: one when it fits (withServices tells whether the services are in it), else the same rows split.
std::vector<Submission> submissionParts(const Submission& s, bool& withServices, size_t maxLen = 7000, const std::string& repo = "adswill/OnAir-channels");
std::vector<std::string> submissionUrls(const Submission& s, size_t maxLen = 7000, const std::string& repo = "adswill/OnAir-channels");
std::string submissionTitle(const Submission& s);

} // namespace chdb
} // namespace dect2
