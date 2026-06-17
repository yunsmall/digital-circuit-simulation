#pragma once
#include <dcs/util/ExePath.h>
#include <filesystem>
#include <string>

inline std::string dllPath() {
    namespace fs = std::filesystem;
    auto dir = fs::path(dsc::exeDir());
#ifdef _WIN32
    const char *name = "dll_test_and.dll";
#else
    const char *name = "libdll_test_and.so";
#endif
    auto dll = dir / name;
    if (fs::exists(dll))
        return dll.string();
    if (fs::exists(name))
        return name;
    return dll.string();
}
