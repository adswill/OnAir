// Starting programs and desktop notifications, for the Automations. No shell is used anywhere: a value from a decoded message can be any text,
// so every argument is passed to the program as one argument.
#include "dect2/common/thread_name.h"
#include "platform.h"
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
extern char** environ;
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace plat {

std::string exePath() {
#if defined(_WIN32)
    char p[MAX_PATH * 2] = {0};
    return GetModuleFileNameA(nullptr, p, (DWORD)sizeof p - 1) ? std::string(p) : std::string();
#elif defined(__APPLE__)
    char p[4096]; uint32_t n = sizeof p;
    return _NSGetExecutablePath(p, &n) == 0 ? std::string(p) : std::string();
#else
    char p[4096]; const ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    return n > 0 ? std::string(p, (size_t)n) : std::string();
#endif
}

namespace {
std::string oneLine(const std::string& s, size_t n) {
    std::string o;
    for (unsigned char c : s) { if (o.size() >= n) break; o += (c < 0x20 || c == 0x7F) ? ' ' : (char)c; }
    return o;
}
}  // namespace

#ifdef _WIN32
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}
// one argument as the C runtime reads it back (backslashes before a quote are doubled)
std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    for (size_t i = 0; i < a.size(); i++) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { bs++; i++; }
        if (i == a.size()) { o.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { o.append(bs * 2 + 1, L'\\'); o += L'"'; }
        else { o.append(bs, L'\\'); o += a[i]; }
    }
    return o + L"\"";
}
}  // namespace

bool spawnProgram(const std::string& program, const std::vector<std::string>& args, std::string& err) {
    std::wstring cmd = quoteArg(wide(program));
    for (const std::string& a : args) cmd += L" " + quoteArg(wide(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) { err = "cannot start " + program; return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

long spawnChild(const std::string& program, const std::vector<std::string>& args, const std::string& logFile, std::string& err) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(wide(logFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) { err = "cannot write " + logFile; return 0; }
    std::wstring cmd = quoteArg(wide(program));
    for (const std::string& a : args) cmd += L" " + quoteArg(wide(a));
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = log;
    si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(log);
    if (!ok) { err = "cannot start " + program; return 0; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (long)pi.dwProcessId;
}

bool childAlive(long pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

void stopChild(long pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (!h) return;
    TerminateProcess(h, 0);
    CloseHandle(h);
}

bool notify(const std::string& title, const std::string& body) {
    SetEnvironmentVariableW(L"ONAIR_NOTE_T", wide(oneLine(title, 120)).c_str());
    SetEnvironmentVariableW(L"ONAIR_NOTE_B", wide(oneLine(body, 240)).c_str());
    const char* script =
        "[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] > $null;"
        "$t = [Windows.UI.Notifications.ToastNotificationManager]::GetTemplateContent([Windows.UI.Notifications.ToastTemplateType]::ToastText02);"
        "$x = $t.GetElementsByTagName('text');"
        "$x[0].AppendChild($t.CreateTextNode($env:ONAIR_NOTE_T)) > $null;"
        "$x[1].AppendChild($t.CreateTextNode($env:ONAIR_NOTE_B)) > $null;"
        "[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('OnAir').Show([Windows.UI.Notifications.ToastNotification]::new($t))";
    std::string err;
    return spawnProgram("powershell.exe", {"-NoProfile", "-NonInteractive", "-Command", script}, err);
}
#else
bool spawnProgram(const std::string& program, const std::vector<std::string>& args, std::string& err) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid;
    const int rc = posix_spawnp(&pid, program.c_str(), nullptr, nullptr, argv.data(), environ);
    if (rc != 0) { err = "cannot start " + program; return false; }
    dect2::namedThread("onair-reap", [pid] { int st = 0; waitpid(pid, &st, 0); }).detach();   // reaped when it ends, nobody waits for it
    return true;
}

long spawnChild(const std::string& program, const std::vector<std::string>& args, const std::string& logFile, std::string& err) {
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, logFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    posix_spawn_file_actions_adddup2(&fa, 1, 2);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, program.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { err = "cannot start " + program; return 0; }
    dect2::namedThread("onair-reap", [pid] { int st = 0; waitpid(pid, &st, 0); }).detach();
    return (long)pid;
}

bool childAlive(long pid) { return pid > 0 && kill((pid_t)pid, 0) == 0; }

void stopChild(long pid) { if (pid > 0) kill((pid_t)pid, SIGTERM); }

bool notify(const std::string& title, const std::string& body) {
    std::string err;
#ifdef __APPLE__
    auto q = [](const std::string& s) {   // inside an AppleScript string
        std::string o;
        for (char c : s) { if (c == '\\' || c == '"') o += '\\'; o += c; }
        return o;
    };
    const std::string script = "display notification \"" + q(oneLine(body, 240)) + "\" with title \"" + q(oneLine(title, 120)) + "\"";
    return spawnProgram("/usr/bin/osascript", {"-e", script}, err);
#else
    return spawnProgram("notify-send", {"--app-name=OnAir", "--", oneLine(title, 120), oneLine(body, 240)}, err);
#endif
}
#endif

}  // namespace plat
