#include "dcs/util/TemplateExpr.h"

#include <cctype>
#include <charconv>
#include <cstdint>
#include <format>

namespace dsc {

std::string TemplateExpr::Value::text() const {
    return has_text ? s : std::to_string(i);
}

bool TemplateExpr::fail(std::string msg) {
    if (_error.empty())
        _error = std::move(msg);
    return false;
}

void TemplateExpr::skipSpace() {
    while (_pos < _expr.size() && std::isspace((unsigned char) _expr[_pos]))
        _pos++;
}

bool TemplateExpr::resolve(std::string &out) {
    out.clear();
    size_t pos = 0;
    while (pos < _text.size()) {
        const size_t start = _text.find("${", pos);
        if (start == std::string::npos) {
            out.append(_text, pos, std::string::npos);
            break;
        }
        const size_t end = _text.find('}', start + 2);
        if (end == std::string::npos) {
            out.append(_text, pos, std::string::npos);
            break;
        }
        out.append(_text, pos, start - pos);

        std::string_view inner = _text.substr(start + 2, end - start - 2);
        size_t b = 0, e = inner.size();
        while (b < e && std::isspace((unsigned char) inner[b]))
            b++;
        while (e > b && std::isspace((unsigned char) inner[e - 1]))
            e--;
        inner = inner.substr(b, e - b);
        if (inner.empty())
            return fail("模板表达式为空");

        // 整个表达式就是一个参数名时按原值输出（参数名可以包含运算符等字符）
        auto direct = _params.find(std::string(inner));
        if (direct != _params.end()) {
            out += direct->second;
            pos = end + 1;
            continue;
        }

        Value value;
        if (!eval(inner, value))
            return false;
        out += value.text();
        pos = end + 1;
    }
    return true;
}

bool TemplateExpr::eval(std::string_view expr, Value &out) {
    _expr = expr;
    _pos = 0;
    if (!parseSum(out))
        return false;
    skipSpace();
    if (_pos != _expr.size())
        return fail("表达式里有多余字符");
    return true;
}

bool TemplateExpr::parseSum(Value &out) {
    if (!parseProduct(out))
        return false;
    for (;;) {
        skipSpace();
        if (_pos >= _expr.size() || (_expr[_pos] != '+' && _expr[_pos] != '-'))
            return true;
        const char op = _expr[_pos++];
        Value rhs;
        if (!parseProduct(rhs))
            return false;
        if (op == '+') {
            if (out.is_int && rhs.is_int) {
                int64_t r;
                if (__builtin_add_overflow(out.i, rhs.i, &r))
                    return fail("整数溢出");
                out.i = r;
                out.s.clear();
                out.has_text = false;
            } else {
                out.s = out.text() + rhs.text();
                out.is_int = false;
                out.has_text = true;
            }
        } else {
            if (!out.is_int || !rhs.is_int)
                return fail("'-' 只能用于整数");
            int64_t r;
            if (__builtin_sub_overflow(out.i, rhs.i, &r))
                return fail("整数溢出");
            out.i = r;
            out.s.clear();
            out.has_text = false;
        }
    }
}

bool TemplateExpr::parseProduct(Value &out) {
    if (!parseFactor(out))
        return false;
    for (;;) {
        skipSpace();
        if (_pos >= _expr.size() || (_expr[_pos] != '*' && _expr[_pos] != '/'))
            return true;
        const char op = _expr[_pos++];
        Value rhs;
        if (!parseFactor(rhs))
            return false;
        if (!out.is_int || !rhs.is_int)
            return fail(op == '*' ? "'*' 只能用于整数" : "'/' 只能用于整数");
        if (op == '*') {
            int64_t r;
            if (__builtin_mul_overflow(out.i, rhs.i, &r))
                return fail("整数溢出");
            out.i = r;
        } else {
            if (rhs.i == 0)
                return fail("除数为 0");
            if (out.i == INT64_MIN && rhs.i == -1)
                return fail("整数溢出");
            out.i /= rhs.i;
        }
        out.s.clear();
        out.has_text = false;
    }
}

bool TemplateExpr::parseFactor(Value &out) {
    skipSpace();
    if (_pos < _expr.size() && (_expr[_pos] == '+' || _expr[_pos] == '-')) {
        const char sign = _expr[_pos++];
        if (!parseFactor(out))
            return false;
        if (!out.is_int)
            return fail("正负号只能用于整数");
        if (sign == '-') {
            if (out.i == INT64_MIN)
                return fail("整数溢出");
            out.i = -out.i;
            out.s.clear();
            out.has_text = false;
        }
        return true;
    }
    return parsePrimary(out);
}

bool TemplateExpr::parsePrimary(Value &out) {
    skipSpace();
    if (_pos >= _expr.size())
        return fail("表达式不完整");

    if (_expr[_pos] == '(') {
        _pos++;
        if (!parseSum(out))
            return false;
        skipSpace();
        if (_pos >= _expr.size() || _expr[_pos] != ')')
            return fail("缺少 ')'");
        _pos++;
        return true;
    }

    const unsigned char c = (unsigned char) _expr[_pos];
    if (std::isdigit(c)) {
        const size_t begin = _pos;
        while (_pos < _expr.size() && std::isdigit((unsigned char) _expr[_pos]))
            _pos++;
        int64_t v = 0;
        auto [ptr, ec] = std::from_chars(_expr.data() + begin, _expr.data() + _pos, v);
        if (ec != std::errc() || ptr != _expr.data() + _pos)
            return fail("整数字面量溢出");
        out.is_int = true;
        out.i = v;
        out.s = std::string(_expr.substr(begin, _pos - begin));
        out.has_text = true;
        return true;
    }

    if (std::isalpha(c) || c == '_') {
        const size_t begin = _pos;
        while (_pos < _expr.size()) {
            const unsigned char k = (unsigned char) _expr[_pos];
            if (!std::isalnum(k) && k != '_' && k != '.')
                break;
            _pos++;
        }
        const std::string name(_expr.substr(begin, _pos - begin));
        auto it = _params.find(name);
        if (it == _params.end()) {
            _missing = name;
            return fail(std::format("未定义参数 '{}'", name));
        }
        out.s = it->second;
        out.has_text = true;
        // 参数值是纯整数时参与算术，否则按字符串用
        int64_t v = 0;
        const char *first = out.s.data();
        const char *last = first + out.s.size();
        auto [ptr, ec] = std::from_chars(first, last, v);
        out.is_int = !out.s.empty() && ec == std::errc() && ptr == last;
        if (out.is_int)
            out.i = v;
        return true;
    }

    return fail(std::format("表达式里有非法字符 '{}'", (char) c));
}

} // namespace dsc
