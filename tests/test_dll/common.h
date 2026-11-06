#pragma once
#include <dcs/util/ExePath.h>
#include <filesystem>
#include <string>

inline std::string dllPathNamed(const std::string &base) {
    namespace fs = std::filesystem;
    auto dir = fs::path(dsc::exeDir());
#ifdef _WIN32
    std::string name = base + ".dll";
#else
    std::string name = "lib" + base + ".so";
#endif
    auto dll = dir / name;
    if (fs::exists(dll))
        return dll.string();
    if (fs::exists(name))
        return name;
    return dll.string();
}

inline std::string dllPath() {
    return dllPathNamed("dll_test_and");
}
