// NAVTEX messages saved as text files: once per message, with the header, and not again for repeats.
#include "data/marine/rf_util.h"
#include "dect2/marine_navtex_save.h"
#include <filesystem>
#include <fstream>
#include <sstream>
using namespace mt;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static int countFiles(const fs::path& d) {
    int n = 0; std::error_code ec;
    for (auto& e : fs::recursive_directory_iterator(d, ec)) if (e.is_regular_file()) n++;
    return n;
}
static std::string slurp(const std::string& p) { std::ifstream f(p, std::ios::binary); std::stringstream b; b << f.rdbuf(); return b.str(); }

int main() {
    Scenario s; s.secs = 20; s.idle = 2;
    Outcome o = run(s);
    CHECK(!o.tel.navtex.empty(), "the receiver decoded no message");
    const fs::path dir = fs::temp_directory_path() / "onair_navtex_save_test";
    std::error_code ec; fs::remove_all(dir, ec);

    NavtexSaver sv;
    const int w = sv.update(o.tel.navtex, dir.string(), 518e3);
    CHECK(w == (int)o.tel.navtex.size() && w >= 1, "wrote %d of %zu", w, o.tel.navtex.size());
    CHECK(countFiles(dir) == w, "%d files", countFiles(dir));
    const NavtexMessage m = o.tel.navtex.front();
    const std::string path = sv.lastPath();
    // the same list again, a repeat count bumped, and the same message heard on a later broadcast: nothing new
    CHECK(sv.update(o.tel.navtex, dir.string(), 518e3) == 0, "second update wrote files");
    auto again = o.tel.navtex; for (auto& x : again) { x.repeats++; x.rxTime += 3600; }
    CHECK(sv.update(again, dir.string(), 518e3) == 0, "a repeat was written again");
    CHECK(countFiles(dir) == w, "files after repeats: %d", countFiles(dir));

    std::string content = slurp(path);
    CHECK(content.find("NAVTEX " + m.header) == 0, "header line: %.40s", content.c_str());
    CHECK(content.find("518.0 kHz") != std::string::npos, "frequency");
    CHECK(content.find(m.text) != std::string::npos, "text");
    CHECK(path.find(NavtexSaver::idOf(m)) != std::string::npos, "id in the name");

    // an unterminated, damaged message is saved and flagged
    NavtexMessage bad; bad.station = '?'; bad.subject = 'A'; bad.header = "?A0*"; bad.text = "GALE *** WARNING"; bad.errors = 3; bad.chars = 16; bad.cer = 0.19f; bad.rxTime = 1700000000;
    CHECK(sv.update({bad}, dir.string(), 0) == 1, "damaged message not saved");
    content = slurp(sv.lastPath());
    CHECK(content.find("INCOMPLETE") != std::string::npos && content.find("GALE *** WARNING") != std::string::npos, "flag");
    CHECK(sv.lastPath().find("2023-11-14") != std::string::npos && sv.lastPath().find("221320__") != std::string::npos, "name %s", sv.lastPath().c_str());

    // a shortened entry is skipped; a folder that cannot be made is reported once and not retried
    NavtexMessage cut = bad; cut.textCut = true; cut.text = "x";
    CHECK(sv.update({cut}, dir.string(), 0) == 0, "cut message saved");
    { std::ofstream blk(dir / "blocker"); }
    NavtexSaver sv2; NavtexMessage n2 = bad; n2.text = "OTHER";
    CHECK(sv2.update({n2}, (dir / "blocker").string(), 0) == 0 && sv2.failed() && !sv2.error().empty(), "error not reported");
    sv2.clearError();
    CHECK(sv2.update({n2}, (dir / "blocker").string(), 0) == 0 && !sv2.failed(), "failed write retried every frame");

    fs::remove_all(dir, ec);
    if (fails) return 1;
    printf("ok\n");
    return 0;
}
