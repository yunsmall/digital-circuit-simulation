// 蓝图模板求值：把 "u_${i}_${w+1}" 里的 ${...} 替换成参数值或表达式结果
//
// 文法（优先级从低到高）：
//
//   template := ( TEXT | "${" expr "}" )*
//   expr     := add
//   add      := mul ( ("+" | "-") mul )*
//   mul      := unary ( ("*" | "/") unary )*
//   unary    := ("+" | "-") unary | primary
//   primary  := INTEGER | IDENT | "(" expr ")"
//   INTEGER  := [0-9]+
//   IDENT    := [A-Za-z_][A-Za-z0-9_.]*
//
// 语义：
//   - "+"：两边都是整数时相加，否则把两边转成文本拼接；
//   - "-" "*" "/"：只接受整数，"/" 向零截断，除数为 0 报错；
//   - 一元 "+" "-"：只接受整数；整数溢出报错；
//   - 参数值只有整个字符串都是十进制整数时才参与算术，否则按字符串处理；
//   - 求值结果不做范围钳制（位宽等合法范围由使用该值的地方校验）；
//   - "${...}" 的整个内容与某个参数名完全一致时直接取原值（允许参数名带运算符）；
//   - 没有字符串字面量；固定文本写在 ${} 外面，例如 u_${i}、${prefix}_${suffix}。
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace dsc {

class TemplateExpr {
public:
    using Params = std::map<std::string, std::string>;

    TemplateExpr(std::string_view text, const Params &params) : _text(text), _params(params) {}

    // 求值到 out。失败返回 false，原因在 error()；缺参数时 missingParam() 非空
    bool resolve(std::string &out);

    const std::string &error() const { return _error; }
    const std::string &missingParam() const { return _missing; }

private:
    struct Value {
        bool is_int = false;
        bool has_text = false; // s 是原始文本（参数值/数字字面量/拼接结果）
        int64_t i = 0;
        std::string s;

        std::string text() const;
    };

    bool eval(std::string_view expr, Value &out);
    bool parseSum(Value &out);
    bool parseProduct(Value &out);
    bool parseFactor(Value &out);
    bool parsePrimary(Value &out);
    void skipSpace();
    bool fail(std::string msg);

    std::string_view _text;
    const Params &_params;
    std::string_view _expr; // 当前 ${...} 里的表达式
    size_t _pos = 0;
    std::string _error;
    std::string _missing;
};

} // namespace dsc
