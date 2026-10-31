// A user-chosen data folder (issue #383): <default data folder>/datadir.txt holds the path of the folder to use instead. The pointer stays in the
// default folder because that is the one place OnAir can find before it knows anything else. DECT2_DATA_DIR still wins over it.
#pragma once
#include <filesystem>
#include <fstream>
#include <string>

namespace plat {

inline std::filesystem::path dataDirPath(const std::string& u8) { return std::filesystem::path(reinterpret_cast<const char8_t*>(u8.c_str())); }

// The chosen folder (created if missing), or empty when there is none or it cannot be made.
inline std::string readDataDirPointer(const std::string& defaultDir) {
    std::ifstream f(dataDirPath(defaultDir + "/datadir.txt"));
    std::string line;
    if (!f || !std::getline(f, line)) return {};
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.pop_back();
    if (line.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dataDirPath(line), ec);
    if (!std::filesystem::is_directory(dataDirPath(line), ec)) return {};
    return line;
}

// Empty path removes the pointer (back to the default folder). False when the file cannot be written.
inline bool writeDataDirPointer(const std::string& defaultDir, const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p = dataDirPath(defaultDir + "/datadir.txt");
    if (path.empty()) { std::filesystem::remove(p, ec); return true; }
    std::filesystem::create_directories(dataDirPath(path), ec);
    std::ofstream f(p, std::ios::trunc);
    if (!f) return false;
    f << path << "\n";
    return (bool)f;
}

}  // namespace plat
