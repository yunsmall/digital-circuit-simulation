#include "dcs/util/ExePath.h"
#include <filesystem>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace dsc {

std::string exeDir() {
    namespace fs = std::filesystem;
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf))
        return fs::path(buf).parent_path().string();
#elif defined(__linux__)
    char buf[4096];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0) {
        buf[len] = '\0';
        return fs::path(buf).parent_path().string();
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return fs::path(buf).parent_path().string();
#endif
    return fs::current_path().string();
}

} // namespace dsc
