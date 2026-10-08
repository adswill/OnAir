// The shared channel database: CSV with quotes and commas, the merge rules, the submission text and its links (length limit), and a body
// as scripts/ingest.py in the data repo expects it. No network.
#include "dect2/channel_db.h"
#include <cstdio>

using namespace dect2::chdb;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static Row mk(double f, const char* net, std::vector<std::string> sv, double snr, const char* std_ = "DVB-T2") {
    Row r; r.freqMhz = f; r.bwMhz = 8; r.standard = std_; r.network = net; r.services = sv; r.snrDb = snr; return r;
}

int main() {
    // quoting round trip
    {
        CHECK(csvQuote("a,b") == "\"a,b\"" && csvQuote("say \"hi\"") == "\"say \"\"hi\"\"\"" && csvQuote("plain") == "plain", "quote");
        auto v = csvParse("a,\"b,c\",\"d\"\"e\"\n\n x ,,\r\n");
        CHECK(v.size() == 2 && v[0].size() == 3 && v[0][1] == "b,c" && v[0][2] == "d\"e" && v[1].size() == 3 && v[1][2].empty(), "parse");
        std::vector<Row> rows = {mk(522, "Net, \"One\"", {"A", "B, C"}, 17.25)};
        rows[0].city = "Dubai"; rows[0].firstSeen = rows[0].lastSeen = "2026-10-08";
        auto back = parseChannels(writeChannels(rows));
        CHECK(back.size() == 1 && back[0].network == "Net, \"One\"" && back[0].services.size() == 2 && back[0].services[1] == "B, C" && back[0].reports == 1, "round trip");
    }
    // merge
    {
        std::vector<Row> db;
        merge(db, "Berlin", {mk(506, "N1", {"A"}, 10), mk(514, "N2", {"X"}, 5)}, "2026-10-01");
        CHECK(db.size() == 2 && db[0].reports == 1 && db[0].firstSeen == "2026-10-01", "first merge");
        merge(db, "Berlin", {mk(506.03, "N1", {"A", "B"}, 12), mk(530, "N3", {}, 9)}, "2026-10-05");
        CHECK(db.size() == 3, "new row added, near one merged (size %zu)", db.size());
        CHECK(db[0].reports == 2 && db[0].snrDb == 12 && db[0].lastSeen == "2026-10-05" && db[0].firstSeen == "2026-10-01" && db[0].services.size() == 2, "merged row");
        merge(db, "Munich", {mk(506, "N1", {"A"}, 3)}, "2026-10-06");
        CHECK(db.size() == 4, "another city is a new row");
        merge(db, "Berlin", {mk(506, "N1", {}, 20), mk(506.02, "N1", {}, 21)}, "2026-10-07");
        CHECK(db[0].reports == 3 && db[0].snrDb == 21, "one submission counts once (%d, %.1f)", db[0].reports, db[0].snrDb);
        auto sorted = parseChannels(writeChannels(db));
        CHECK(sorted.front().city == "Berlin" && sorted.back().city == "Munich" && sorted[0].freqMhz < sorted[1].freqMhz, "sorted");
    }
    // body and links
    {
        Submission s; s.mode = "dvb-t"; s.country = "DE"; s.city = "Berlin";
        s.rows = {mk(522, "Net, One", {"Das Erste", "ZDF|x"}, 17.25), mk(473.143, "", {}, 9.5, "ISDB-T")};
        const std::string body = issueBody(s);
        CHECK(body.find("mode: dvb-t\ncountry: DE\ncity: Berlin\n") == 0, "body head");
        CHECK(body.find("522.0,8.0,DVB-T2,\"Net, One\",Das Erste|ZDF x,17.2") != std::string::npos || body.find("17.3") != std::string::npos, "body row: %s", body.c_str());
        Submission p; std::string err;
        CHECK(parseIssueBody(body, p, err) && p.rows.size() == 2 && p.city == "Berlin" && p.rows[0].network == "Net, One" && p.rows[0].services.size() == 2 && p.rows[1].freqMhz == 473.143, "parse body: %s", err.c_str());
        // a body as GitHub renders an issue form
        CHECK(parseIssueBody("### Scan result\n\n" + body, p, err) && p.rows.size() == 2, "form body");
        CHECK(!parseIssueBody("mode: fm\ncountry: DE\n", p, err), "no block");
        CHECK(urlEncode("a b&c/d,é") == "a%20b%26c%2Fd%2C%C3%A9", "url encode");
        auto u = submissionUrls(s);
        CHECK(u.size() == 1 && u[0].find("https://github.com/adswill/OnAir-channels/issues/new?template=submit.yml&title=") == 0, "one link");
        // many rows with long services: services cut first, then split
        Submission big = s; big.rows.clear();
        for (int i = 0; i < 60; i++) big.rows.push_back(mk(470 + i * 0.5, "Network name", {"Service number one of many", "Service number two of many", "Service three of many"}, 12));
        const auto noSv = submissionUrls(big);
        CHECK(noSv.size() == 1 && noSv[0].size() <= 7000 && noSv[0].find("Service%20number") == std::string::npos, "services cut (%zu links, %zu chars)", noSv.size(), noSv.empty() ? 0 : noSv[0].size());
        big.rows.clear();
        for (int i = 0; i < 300; i++) big.rows.push_back(mk(470 + i * 0.25, "Network name", {}, 12));
        const auto many = submissionUrls(big);
        size_t total = 0; bool ok = many.size() > 1;
        for (auto& l : many) { ok = ok && l.size() <= 7000; }
        CHECK(ok, "split links within the limit (%zu links)", many.size());
        (void)total;
    }
    printf(fails ? "channel_db: %d FAILED\n" : "channel_db: all passed\n", fails);
    return fails ? 1 : 0;
}
