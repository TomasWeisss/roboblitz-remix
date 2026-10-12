#pragma once

#include <functional>
#include <memory>
#include <string>

// UE3 generates each material's HLSL from its expression graph and fills it into
// MaterialTemplate.usf, e.g. `half3 GetMaterialDiffuseColor(FMaterialParameters Parameters)
// { return <expression>; }`. The albedo is read from that expression.

// The expression a generated material function returns, with newlines and tabs turned into
// spaces. Empty if `source` does not define the function.
std::string ExtractMaterialExpression(const std::string& source, const char* function);

// A texture-less colour expression, parsed for evaluation at draw time
struct ColorExpression;

struct MaterialAlbedo {
    std::string sampler; // first sampler the expression reads with tex2D*, e.g. "Texture2D_1"; empty if none
    std::shared_ptr<const ColorExpression> color; // without a sampler: the expression, if it can be evaluated
};

MaterialAlbedo AnalyzeAlbedo(const std::string& expression);

// Value of a uniform parameter (UniformVector_0, ...) as currently set, or null if unknown
using UniformLookup = std::function<const float*(const std::string& name)>;

// Evaluates the colour, clamped to [0, 1]. Vertex colour counts as white. Returns false if a
// uniform is unknown or the result is not a scalar or a colour.
bool EvaluateColor(const ColorExpression& expression, const UniformLookup& uniforms, float rgb[3]);
