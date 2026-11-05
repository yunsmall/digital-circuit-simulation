// 同一个 DLL 的全局 init/deinit 在多实例、多电路下只执行一次
#include <gtest/gtest.h>

#include <dcs/Circuit.h>
#include <dcs/components/platform/Dll.h>

#include <filesystem>
#include <memory>

#include "common.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {

// 保持 DLL 一直加载着读计数器：电路 deinit 时会释放自己那份引用，
// 最后一个引用释放后模块会被卸载、静态计数器归零，读到的就永远是 0
struct LibPin {
    void *h = nullptr;
    explicit LibPin(const std::string &path) {
#ifdef _WIN32
        h = LoadLibraryA(path.c_str());
#else
        h = dlopen(path.c_str(), RTLD_NOW);
#endif
    }
    ~LibPin() {
#ifdef _WIN32
        if (h)
            FreeLibrary((HMODULE) h);
#else
        if (h)
            dlclose(h);
#endif
    }
    int call(const char *sym) const {
#ifdef _WIN32
        auto fn = reinterpret_cast<int (*)()>(GetProcAddress((HMODULE) h, sym));
#else
        auto fn = reinterpret_cast<int (*)()>(dlsym(h, sym));
#endif
        return fn ? fn() : -1;
    }
};

dsc::DllComponent *addAnd(const std::string &path, dsc::Circuit &c, const std::string &prefix) {
    auto dl = std::make_unique<dsc::DllComponent>(prefix + "_dl", path);
    auto *p = dl.get();
    c.addComponent(std::move(dl));
    auto *a = c.createNet(prefix + "_a"), *b = c.createNet(prefix + "_b"), *y = c.createNet(prefix + "_y");
    c.connect(p, "a", a);
    c.connect(p, "b", b);
    c.connect(p, "y", y);
    return p;
}

} // namespace

// 同一电路里两个元件指向同一个 DLL：dcs_dll_init/deinit 各只能执行一次
TEST(DllShared, TwoInstancesOneCircuit) {
    auto path = dllPath();
    ASSERT_TRUE(std::filesystem::exists(path));
    LibPin pin(path);
    ASSERT_NE(pin.h, nullptr);
    const int init0 = pin.call("dcs_dll_test_init_calls");
    const int deinit0 = pin.call("dcs_dll_test_deinit_calls");
    ASSERT_GE(init0, 0);

    dsc::Circuit c;
    addAnd(path, c, "u0");
    addAnd(path, c, "u1");
    ASSERT_EQ(c.compile().code, dsc::ErrorCode::OK);
    c.init();
    EXPECT_EQ(pin.call("dcs_dll_test_init_calls"), init0 + 1) << "全局 init 只能执行一次";

    c.deinit();
    EXPECT_EQ(pin.call("dcs_dll_test_deinit_calls"), deinit0 + 1) << "全局 deinit 只能执行一次";
}

// 两个电路共用一个 DLL：先销毁的电路不能抢先执行全局 deinit
TEST(DllShared, TwoCircuitsSharedDll) {
    auto path = dllPath();
    ASSERT_TRUE(std::filesystem::exists(path));
    LibPin pin(path);
    ASSERT_NE(pin.h, nullptr);
    const int init0 = pin.call("dcs_dll_test_init_calls");
    const int deinit0 = pin.call("dcs_dll_test_deinit_calls");
    ASSERT_GE(init0, 0);

    auto c1 = std::make_unique<dsc::Circuit>();
    auto c2 = std::make_unique<dsc::Circuit>();
    addAnd(path, *c1, "c1");
    addAnd(path, *c2, "c2");
    ASSERT_EQ(c1->compile().code, dsc::ErrorCode::OK);
    ASSERT_EQ(c2->compile().code, dsc::ErrorCode::OK);
    c1->init();
    c2->init();
    EXPECT_EQ(pin.call("dcs_dll_test_init_calls"), init0 + 1);

    c1->deinit();
    EXPECT_EQ(pin.call("dcs_dll_test_deinit_calls"), deinit0) << "还有电路在用，不能提前 deinit";
    c2->deinit();
    EXPECT_EQ(pin.call("dcs_dll_test_deinit_calls"), deinit0 + 1);
}
