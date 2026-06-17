#pragma once
#include <dcs/CircuitLibrary.h>
#include <dcs/components/Composite.h>
#include <dcs/components/Gates.h>

// 注册半加器定义
inline void regHalfAdder() {
    if (dsc::CircuitLibrary::instance().exists("half_adder"))
        return;
    dsc::CircuitDefinition def;
    def.name = "half_adder";
    def.nets = {"a", "b", "sum", "carry"};
    def.components = {
            {"xor", "xor", {{"inputs", "2"}, {"bit_width", "1"}}, {{"in0", "a"}, {"in1", "b"}, {"out", "sum"}}},
            {"and", "and", {{"inputs", "2"}, {"bit_width", "1"}}, {{"in0", "a"}, {"in1", "b"}, {"out", "carry"}}},
    };
    def.input_expose = {{"a", "a"}, {"b", "b"}};
    def.output_expose = {{"sum", "sum"}, {"carry", "carry"}};
    dsc::CircuitLibrary::instance().add(std::move(def));
}

// 注册全加器定义（用两个半加器 + OR 门）
inline void regFullAdder() {
    if (dsc::CircuitLibrary::instance().exists("full_adder"))
        return;
    regHalfAdder();
    dsc::CircuitDefinition def;
    def.name = "full_adder";
    def.nets = {"a", "b", "cin", "s1", "c1", "c2", "sum", "cout"};
    def.components = {
            {"composite",
             "ha1",
             {{"definition", "half_adder"}},
             {{"a", "a"}, {"b", "b"}, {"sum", "s1"}, {"carry", "c1"}}},
            {"composite",
             "ha2",
             {{"definition", "half_adder"}},
             {{"a", "s1"}, {"b", "cin"}, {"sum", "sum"}, {"carry", "c2"}}},
            {"or", "or", {{"inputs", "2"}, {"bit_width", "1"}}, {{"in0", "c1"}, {"in1", "c2"}, {"out", "cout"}}},
    };
    def.input_expose = {{"a", "a"}, {"b", "b"}, {"cin", "cin"}};
    def.output_expose = {{"sum", "sum"}, {"cout", "cout"}};
    dsc::CircuitLibrary::instance().add(std::move(def));
}
