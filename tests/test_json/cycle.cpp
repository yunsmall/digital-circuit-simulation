// 蓝图定义层面的循环引用：展开必须报错，不能无限循环
#include <gtest/gtest.h>

#include <string>

#include <dcs/Circuit.h>

using namespace dsc;

TEST(JsonCycle, InlineDefinitionsCycleIsRejected) {
    const char *json = R"({
      "$meta": {"kind": "circuit", "format": 1},
      "definitions": {
        "cyc_A_xyz": {"components": [{"type": "composite", "name": "b", "params": {"definition": "cyc_B_xyz"}}]},
        "cyc_B_xyz": {"components": [{"type": "composite", "name": "a", "params": {"definition": "cyc_A_xyz"}}]}
      },
      "components": [{"type": "composite", "name": "top", "params": {"definition": "cyc_A_xyz"}}],
      "nets": []
    })";
    std::string err;
    auto c = Circuit::fromJson(json, err);
    ASSERT_NE(c, nullptr) << err;
    auto r = c->compile();
    EXPECT_NE(r.code, ErrorCode::OK);
    EXPECT_NE(r.message.find("循环"), std::string::npos) << r.message;
}
