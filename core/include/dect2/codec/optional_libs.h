#pragma once
// The optional libraries and plug-ins OnAir loads at run time and does not ship (docs/OPTIONAL-LIBRARIES.md): one entry per library with
// the file names looked for on this OS, the official place to get it, and whether it was found now. The same list is shown in
// Settings > Advanced > Optional libraries and codecs and in the Voice codecs window. Nothing is downloaded or loaded here: only folders are looked at.
#include <string>
#include <vector>

namespace dect2::codec {

struct OptionalLib {
    std::string id;            // "faad2", "lame", "rtlsdr" ...
    std::string name;          // "libfaad2 (DAB+ HE-AAC)"
    std::string group;         // "Sound and codecs", "Radio drivers", "Plug-ins"
    std::string usedFor;       // what stays off without it
    std::string files;         // the file names OnAir looks for on this OS, comma separated
    std::string where;         // the folder that works on this OS (the OnAir codecs folder), text
    std::string source;        // the official download / project page
    bool found = false;
    std::string foundPath;     // the first file found, empty if missing
};

// True when `name` matches `pattern` ('*' matches any run of characters, case-insensitive). Used for the file names above.
bool optionalNameMatches(const std::string& pattern, const std::string& name);

// The list for this OS, with the found state (looks at the OnAir codecs folders, the program's folder and the usual system folders).
std::vector<OptionalLib> optionalLibraries();

} // namespace dect2::codec
