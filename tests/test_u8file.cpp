// u8file.h: a UTF-8 path with an accent ("Área de Trabalho", what the save dialog gives on a Brazilian Windows whose desktop is in
// OneDrive) is written and read back through std::ofstream / std::ifstream and u8fopen, and lands in the folder with that name.
#include "dect2/u8file.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
using namespace dect2;

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::u8string tmp = (fs::temp_directory_path() / "onair-u8file").u8string();
    const std::string dir(tmp.begin(), tmp.end());
    const std::string sub = dir + "/\xC3\x81rea de Trabalho";   // "Área de Trabalho" in UTF-8
    fs::remove_all(u8file(dir), ec);
    fs::create_directories(u8file(sub), ec);
    CHECK(!ec, "could not create the folder: %s", ec.message().c_str());

    const std::string csv = sub + "/scan-mesh-nodes.csv";
    { std::ofstream o(u8file(csv), std::ios::binary); o << "id,name\n!e326743b,S\xC3\xA3o Jos\xC3\xA9\n"; CHECK((bool)o, "ofstream did not write %s", csv.c_str()); }
    { std::ifstream i(u8file(csv), std::ios::binary); std::string l; std::getline(i, l); CHECK(l == "id,name", "read back \"%s\"", l.c_str()); }

    const std::string txt = sub + "/mesh-report.txt";
    FILE* f = u8fopen(txt, "wb");
    CHECK(f != nullptr, "u8fopen did not create %s", txt.c_str());
    if (f) { fputs("report\n", f); fclose(f); }
    CHECK(fs::exists(u8file(txt), ec), "the report is not in the accented folder");

    int n = 0;   // both files are in the folder named "Área de Trabalho", not in a garbled sibling
    for (const auto& e : fs::directory_iterator(u8file(dir), ec)) { n++; CHECK(e.path().filename() == u8file("\xC3\x81rea de Trabalho"), "unexpected folder"); }
    CHECK(n == 1, "%d folders instead of 1", n);

    fs::remove_all(u8file(dir), ec);
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("u8file: ok\n");
    return 0;
}
