#include "material_albedo.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

bool IsIdentifierChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

size_t SkipSpace(const std::string& text, size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
    return pos;
}

// Position after `word` if it starts at `pos` as a whole identifier, else npos
size_t MatchWord(const std::string& text, size_t pos, const char* word) {
    const size_t length = strlen(word);
    if (text.compare(pos, length, word) != 0) return std::string::npos;
    if (pos + length < text.size() && IsIdentifierChar(text[pos + length])) return std::string::npos;
    return pos + length;
}

// First sampler passed to tex2D, tex2Dbias, tex2Dlod, ...
std::string FirstSampler(const std::string& expression) {
    for (size_t pos = expression.find("tex2D"); pos != std::string::npos; pos = expression.find("tex2D", pos + 1)) {
        if (pos > 0 && IsIdentifierChar(expression[pos - 1])) continue;
        size_t p = pos + 5;
        while (p < expression.size() && IsIdentifierChar(expression[p])) ++p;
        p = SkipSpace(expression, p);
        if (p >= expression.size() || expression[p] != '(') continue;
        p = SkipSpace(expression, p + 1);
        const size_t start = p;
        while (p < expression.size() && IsIdentifierChar(expression[p])) ++p;
        if (p > start) return expression.substr(start, p - start);
    }
    return {};
}

// --- Colour expressions: the HLSL subset UE3 generates for constant and parameter values ---

struct Value {
    float v[4] = {};
    int n = 0; // components
};

enum class Op { Constant, Uniform, VertexColor, Negate, Add, Sub, Mul, Div, Swizzle, Construct, Function };

} // namespace

struct ColorExpression {
    Op op = Op::Constant;
    Value constant;      // Constant
    std::string name;    // Uniform, Function
    int size = 0;        // Construct: components of the type
    int swizzle[4] = {}; // Swizzle
    int swizzleCount = 0;
    std::vector<std::shared_ptr<const ColorExpression>> args;
};

namespace {

using Node = std::shared_ptr<const ColorExpression>;

struct FunctionInfo {
    const char* name;
    size_t arguments;
};
constexpr FunctionInfo kFunctions[] = {
    {"lerp", 3}, {"clamp", 3}, {"saturate", 1}, {"min", 2}, {"max", 2}, {"abs", 1}, {"pow", 2},
    {"dot", 2},  {"frac", 1},  {"sqrt", 1},     {"floor", 1}, {"ceil", 1},
};

// Number of components of a scalar/vector type name (float3, half, ...), 0 if not one
int TypeSize(const std::string& name) {
    for (const char* base : {"float", "half"}) {
        const size_t length = strlen(base);
        if (name.compare(0, length, base) != 0) continue;
        if (name.size() == length) return 1;
        if (name.size() == length + 1 && name[length] >= '1' && name[length] <= '4') return name[length] - '0';
    }
    return 0;
}

int SwizzleIndex(char c) {
    switch (c) {
    case 'x': case 'r': return 0;
    case 'y': case 'g': return 1;
    case 'z': case 'b': return 2;
    case 'w': case 'a': return 3;
    }
    return -1;
}

class Parser {
public:
    explicit Parser(const std::string& text) : text_(text) {}

    Node ParseAll() {
        Node node = ParseAdditive();
        return node && SkipSpace(text_, pos_) == text_.size() ? node : nullptr;
    }

private:
    const std::string& text_;
    size_t pos_ = 0;

    char Peek() {
        pos_ = SkipSpace(text_, pos_);
        return pos_ < text_.size() ? text_[pos_] : '\0';
    }

    bool Accept(char c) {
        if (Peek() != c) return false;
        ++pos_;
        return true;
    }

    std::string Identifier() {
        Peek();
        const size_t start = pos_;
        if (pos_ < text_.size() && (std::isalpha(static_cast<unsigned char>(text_[pos_])) || text_[pos_] == '_')) {
            while (pos_ < text_.size() && IsIdentifierChar(text_[pos_])) ++pos_;
        }
        return text_.substr(start, pos_ - start);
    }

    static Node Make(Op op, std::vector<Node> args) {
        auto node = std::make_shared<ColorExpression>();
        node->op = op;
        node->args = std::move(args);
        return node;
    }

    Node ParseAdditive() {
        Node left = ParseMultiplicative();
        while (left) {
            const char c = Peek();
            if (c != '+' && c != '-') break;
            ++pos_;
            Node right = ParseMultiplicative();
            if (!right) return nullptr;
            left = Make(c == '+' ? Op::Add : Op::Sub, {left, right});
        }
        return left;
    }

    Node ParseMultiplicative() {
        Node left = ParseUnary();
        while (left) {
            const char c = Peek();
            if (c != '*' && c != '/') break;
            ++pos_;
            Node right = ParseUnary();
            if (!right) return nullptr;
            left = Make(c == '*' ? Op::Mul : Op::Div, {left, right});
        }
        return left;
    }

    Node ParseUnary() {
        if (Accept('-')) {
            Node operand = ParseUnary();
            return operand ? Make(Op::Negate, {operand}) : nullptr;
        }
        if (Accept('+')) return ParseUnary();
        return ParsePostfix();
    }

    Node ParsePostfix() {
        Node node = ParsePrimary();
        while (node && Accept('.')) {
            const std::string components = Identifier();
            if (components.empty() || components.size() > 4) return nullptr;
            auto swizzle = std::make_shared<ColorExpression>();
            swizzle->op = Op::Swizzle;
            swizzle->args = {node};
            for (char c : components) {
                const int index = SwizzleIndex(c);
                if (index < 0) return nullptr;
                swizzle->swizzle[swizzle->swizzleCount++] = index;
            }
            node = swizzle;
        }
        return node;
    }

    Node ParsePrimary() {
        const char c = Peek();
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') return ParseNumber();
        if (Accept('(')) {
            // Cast, e.g. (float3)x
            const size_t afterParen = pos_;
            const int castSize = TypeSize(Identifier());
            if (castSize && Accept(')')) {
                Node operand = ParseUnary();
                return operand ? Construct(castSize, {operand}) : nullptr;
            }
            pos_ = afterParen;
            Node inner = ParseAdditive();
            return inner && Accept(')') ? inner : nullptr;
        }

        const std::string name = Identifier();
        if (name.empty()) return nullptr;
        if (Accept('(')) return ParseCall(name);
        if (name == "Parameters") {
            if (!Accept('.') || Identifier() != "VertexColor") return nullptr;
            return Make(Op::VertexColor, {});
        }
        auto uniform = std::make_shared<ColorExpression>();
        uniform->op = Op::Uniform;
        uniform->name = name;
        return uniform;
    }

    Node ParseNumber() {
        const char* start = text_.c_str() + pos_;
        char* end = nullptr;
        const float value = std::strtof(start, &end);
        if (end == start) return nullptr;
        pos_ += end - start;
        if (pos_ < text_.size() && (text_[pos_] == 'f' || text_[pos_] == 'h')) ++pos_;
        auto node = std::make_shared<ColorExpression>();
        node->constant.v[0] = value;
        node->constant.n = 1;
        return node;
    }

    // After `name(`
    Node ParseCall(const std::string& name) {
        std::vector<Node> args;
        if (!Accept(')')) {
            do {
                Node arg = ParseAdditive();
                if (!arg) return nullptr;
                args.push_back(arg);
            } while (Accept(','));
            if (!Accept(')')) return nullptr;
        }
        if (const int size = TypeSize(name)) return Construct(size, std::move(args));
        for (const FunctionInfo& function : kFunctions) {
            if (name != function.name || args.size() != function.arguments) continue;
            auto node = std::make_shared<ColorExpression>();
            node->op = Op::Function;
            node->name = name;
            node->args = std::move(args);
            return node;
        }
        return nullptr;
    }

    static Node Construct(int size, std::vector<Node> args) {
        auto node = std::make_shared<ColorExpression>();
        node->op = Op::Construct;
        node->size = size;
        node->args = std::move(args);
        return node;
    }
};

// Scalars broadcast; otherwise HLSL truncates to the smaller vector
bool Componentwise(const Value& a, const Value& b, Value& out, float (*f)(float, float)) {
    if (a.n == 0 || b.n == 0) return false;
    out.n = a.n == 1 ? b.n : b.n == 1 ? a.n : std::min(a.n, b.n);
    for (int i = 0; i < out.n; ++i) out.v[i] = f(a.v[a.n == 1 ? 0 : i], b.v[b.n == 1 ? 0 : i]);
    return true;
}

bool Evaluate(const ColorExpression& node, const UniformLookup& uniforms, Value& out) {
    if (node.op == Op::Construct) {
        Value all;
        for (const Node& arg : node.args) {
            Value part;
            if (!Evaluate(*arg, uniforms, part) || all.n + part.n > 4) return false;
            std::copy(part.v, part.v + part.n, all.v + all.n);
            all.n += part.n;
        }
        if (all.n == 1) std::fill(all.v + 1, all.v + 4, all.v[0]); // float3(x)
        else if (all.n < node.size) return false;
        out = all;
        out.n = node.size;
        return true;
    }

    Value args[3];
    if (node.args.size() > 3) return false;
    for (size_t i = 0; i < node.args.size(); ++i) {
        if (!Evaluate(*node.args[i], uniforms, args[i])) return false;
    }
    const Value& a = args[0];
    const Value& b = args[1];
    const Value& c = args[2];

    switch (node.op) {
    case Op::Constant:
        out = node.constant;
        return true;
    case Op::Uniform: {
        const float* value = uniforms ? uniforms(node.name) : nullptr;
        if (!value) return false;
        out.n = 4;
        std::copy(value, value + 4, out.v);
        return true;
    }
    case Op::VertexColor:
        out.n = 4;
        std::fill(out.v, out.v + 4, 1.0f);
        return true;
    case Op::Negate:
        out = a;
        for (int i = 0; i < out.n; ++i) out.v[i] = -out.v[i];
        return true;
    case Op::Add: return Componentwise(a, b, out, [](float x, float y) { return x + y; });
    case Op::Sub: return Componentwise(a, b, out, [](float x, float y) { return x - y; });
    case Op::Mul: return Componentwise(a, b, out, [](float x, float y) { return x * y; });
    case Op::Div: return Componentwise(a, b, out, [](float x, float y) { return y != 0 ? x / y : 0.0f; });
    case Op::Swizzle:
        out.n = node.swizzleCount;
        for (int i = 0; i < out.n; ++i) {
            const int index = node.swizzle[i];
            if (index >= a.n && !(a.n == 1 && index == 0)) return false;
            out.v[i] = a.v[index];
        }
        return true;
    case Op::Construct: // handled above
    case Op::Function:
        break;
    }

    const std::string& f = node.name;
    if (f == "lerp") {
        Value difference, scaled;
        return Componentwise(b, a, difference, [](float x, float y) { return x - y; }) &&
               Componentwise(difference, c, scaled, [](float x, float y) { return x * y; }) &&
               Componentwise(a, scaled, out, [](float x, float y) { return x + y; });
    }
    if (f == "clamp") {
        Value low;
        return Componentwise(a, b, low, [](float x, float y) { return std::max(x, y); }) &&
               Componentwise(low, c, out, [](float x, float y) { return std::min(x, y); });
    }
    if (f == "min") return Componentwise(a, b, out, [](float x, float y) { return std::min(x, y); });
    if (f == "max") return Componentwise(a, b, out, [](float x, float y) { return std::max(x, y); });
    if (f == "pow") return Componentwise(a, b, out, [](float x, float y) { return std::pow(std::max(x, 0.0f), y); });
    if (f == "dot") {
        Value product;
        if (!Componentwise(a, b, product, [](float x, float y) { return x * y; })) return false;
        out.n = 1;
        out.v[0] = 0;
        for (int i = 0; i < product.n; ++i) out.v[0] += product.v[i];
        return true;
    }

    float (*unary)(float) = nullptr;
    if (f == "saturate") unary = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    else if (f == "abs") unary = [](float x) { return std::fabs(x); };
    else if (f == "frac") unary = [](float x) { return x - std::floor(x); };
    else if (f == "sqrt") unary = [](float x) { return std::sqrt(std::max(x, 0.0f)); };
    else if (f == "floor") unary = [](float x) { return std::floor(x); };
    else if (f == "ceil") unary = [](float x) { return std::ceil(x); };
    if (!unary) return false;
    out = a;
    for (int i = 0; i < out.n; ++i) out.v[i] = unary(out.v[i]);
    return true;
}

} // namespace

std::string ExtractMaterialExpression(const std::string& source, const char* function) {
    for (size_t pos = source.find(function); pos != std::string::npos; pos = source.find(function, pos + 1)) {
        if (pos > 0 && IsIdentifierChar(source[pos - 1])) continue;
        size_t p = MatchWord(source, pos, function);
        if (p == std::string::npos) continue;

        // The definition: name(...) { return <expression>; }  (calls are not followed by '{')
        p = SkipSpace(source, p);
        if (p >= source.size() || source[p] != '(') continue;
        p = source.find(')', p);
        if (p == std::string::npos) return {};
        p = SkipSpace(source, p + 1);
        if (p >= source.size() || source[p] != '{') continue;
        p = MatchWord(source, SkipSpace(source, p + 1), "return");
        if (p == std::string::npos) return {};

        const size_t end = source.find(';', p);
        if (end == std::string::npos) return {};
        std::string expression = source.substr(p, end - p);
        std::replace_if(expression.begin(), expression.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
        const size_t first = expression.find_first_not_of(' ');
        const size_t last = expression.find_last_not_of(' ');
        return first == std::string::npos ? std::string() : expression.substr(first, last - first + 1);
    }
    return {};
}

MaterialAlbedo AnalyzeAlbedo(const std::string& expression) {
    MaterialAlbedo albedo;
    albedo.sampler = FirstSampler(expression);
    if (albedo.sampler.empty() && !expression.empty()) albedo.color = Parser(expression).ParseAll();
    return albedo;
}

bool EvaluateColor(const ColorExpression& expression, const UniformLookup& uniforms, float rgb[3]) {
    Value value;
    if (!Evaluate(expression, uniforms, value) || value.n == 2 || value.n == 0) return false;
    for (int i = 0; i < 3; ++i) rgb[i] = std::clamp(value.v[value.n == 1 ? 0 : i], 0.0f, 1.0f);
    return true;
}
