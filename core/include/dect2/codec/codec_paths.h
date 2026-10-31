// Where OnAir looks for the codec plug-ins and libraries the user installs (docs/CODECS.md). One list for every loader.
// The list itself is a pure function of a few strings (codecSearchDirs), so it is unit-tested with made-up Windows, macOS and Linux paths.
// Order, first wins: the user folder, the extra folder set in Voice codecs, ONAIR_CODEC_DIR, then the folders next to the program.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace dect2::codec {

struct CodecDir {
    std::string path;     // no trailing separator; \ is kept when the input path used it
    std::string source;   // "user", "extra", "ONAIR_CODEC_DIR" or "program"
};

struct CodecPathInput {
    std::string exeDir;     // the folder the program is in (either slash, any trailing slash); empty if unknown
    std::string userDir;    // the per-user codecs folder (already ends in codecs); empty if unknown
    std::string envDir;     // ONAIR_CODEC_DIR
    std::string extraDir;   // the folder the user typed in Voice codecs
};

// Lexical clean-up of a folder path: one separator style (the style of the input), no trailing or doubled separators, "." and ".." resolved.
std::string normalizeDirPath(const std::string& p);
// True when two folder paths name the same place (Windows-style paths compare without regard to case).
bool samePath(const std::string& a, const std::string& b);
// dir + separator + name, in dir's separator style.
std::string joinPath(const std::string& dir, const std::string& name);

// The ordered, de-duplicated folders to search. Empty inputs are skipped.
std::vector<CodecDir> codecSearchDirs(const CodecPathInput& in);

// The folders in `dirs` that hold a file called `name`, in order. `exists` tells whether a path is a regular file.
std::vector<CodecDir> dirsWithFile(const std::vector<CodecDir>& dirs, const std::string& name, const std::function<bool(const std::string&)>& exists);

// The full paths of the library files called `names` that exist in `dirs` (plus `extraDirs`, the program's own folders), folder by folder in the
// order of the lists, and within a folder in the order of `names`. For the loaders of codecs that are single libraries (LAME, FAAD2): a copy in
// an OnAir codecs folder, which an update does not wipe, wins over the system's (#279, #376).
std::vector<std::string> libraryPaths(const std::vector<CodecDir>& dirs, const std::vector<std::string>& extraDirs, const std::vector<std::string>& names,
                                      const std::function<bool(const std::string&)>& exists);

// ---- the real machine

// The extra folder the user set in Voice codecs (the app keeps it in its prefs and hands it here at start and when it changes).
void setExtraCodecDir(const std::string& dir);
std::string extraCodecDir();
// The folder the running program is in; empty if the OS will not say.
std::string programDir();
// The per-user codecs folder: DECT2_DATA_DIR/codecs, else %LOCALAPPDATA%\OnAir\codecs, ~/Library/Application Support/OnAir/codecs,
// $XDG_DATA_HOME/onair/codecs or ~/.local/share/onair/codecs.
std::string userCodecDir();
// codecSearchDirs() for this machine, this run.
std::vector<CodecDir> systemCodecDirs();
// libraryPaths() for this machine, this run (the codecs folders, then the program's folder).
std::vector<std::string> systemLibraryPaths(const std::vector<std::string>& names);

} // namespace dect2::codec
