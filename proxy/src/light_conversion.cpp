#include "light_conversion.h"

#include <algorithm>
#include <cmath>

namespace {

void Copy3(float out[3], const float in[3]) {
    out[0] = in[0];
    out[1] = in[1];
    out[2] = in[2];
}

bool Normalize(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length < 1e-6f) return false;
    for (int i = 0; i < 3; ++i) v[i] /= length;
    return true;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

bool Near3(const float a[3], const float b[3], float tolerance) {
    return Near(a[0], b[0], tolerance) && Near(a[1], b[1], tolerance) && Near(a[2], b[2], tolerance);
}

GameLight MakeLocalLight(GameLight::Type type, const float positionAndInvRadius[4], const float color[3]) {
    GameLight light;
    light.type = type;
    Copy3(light.position, positionAndInvRadius);
    light.radius = positionAndInvRadius[3] > 0 ? 1.0f / positionAndInvRadius[3] : 0;
    Copy3(light.color, color);
    return light;
}

} // namespace

GameLight MakePointLight(const float positionAndInvRadius[4], const float color[3]) {
    return MakeLocalLight(GameLight::Type::Point, positionAndInvRadius, color);
}

GameLight MakeSpotLight(const float positionAndInvRadius[4], const float color[3], const float spotDirection[3],
                        const float spotAngles[2]) {
    GameLight light = MakeLocalLight(GameLight::Type::Spot, positionAndInvRadius, color);
    Copy3(light.direction, spotDirection);
    Normalize(light.direction);
    // UE3: falloff = saturate((dot(L, -SpotDirection) - SpotAngles.x) * SpotAngles.y)
    light.cosOuterCone = spotAngles[0];
    light.cosInnerCone = spotAngles[1] > 0 ? std::min(1.0f, spotAngles[0] + 1.0f / spotAngles[1]) : spotAngles[0];
    return light;
}

GameLight MakeDirectionalLight(const float lightDirection[3], const float color[3]) {
    GameLight light;
    light.type = GameLight::Type::Directional;
    // UE3 passes the direction towards the light; D3D9 wants the direction the light travels
    for (int i = 0; i < 3; ++i) light.direction[i] = -lightDirection[i];
    Normalize(light.direction);
    Copy3(light.color, color);
    return light;
}

bool SameLight(const GameLight& a, const GameLight& b) {
    if (a.type != b.type || !Near3(a.color, b.color, 1e-3f)) return false;
    if (a.type == GameLight::Type::Directional) return Near3(a.direction, b.direction, 1e-3f);
    return Near3(a.position, b.position, 0.5f) && Near(a.radius, b.radius, 0.5f) &&
           (a.type != GameLight::Type::Spot || Near3(a.direction, b.direction, 1e-3f));
}

bool ToD3DLight(const GameLight& light, D3DLIGHT9& out) {
    const float brightness = std::max(light.color[0], std::max(light.color[1], light.color[2]));
    if (brightness <= 0) return false;

    out = {};
    out.Diffuse = {light.color[0], light.color[1], light.color[2], 1.0f};
    out.Specular = out.Diffuse;
    out.Attenuation0 = 1.0f; // no falloff terms: Remix derives the intensity from Range

    switch (light.type) {
    case GameLight::Type::Directional:
        out.Type = D3DLIGHT_DIRECTIONAL;
        out.Direction = {light.direction[0], light.direction[1], light.direction[2]};
        return true;
    case GameLight::Type::Spot:
        out.Type = D3DLIGHT_SPOT;
        out.Direction = {light.direction[0], light.direction[1], light.direction[2]};
        out.Phi = 2 * std::acos(std::clamp(light.cosOuterCone, -1.0f, 1.0f));
        out.Theta = std::min(out.Phi, 2 * std::acos(std::clamp(light.cosInnerCone, -1.0f, 1.0f)));
        out.Falloff = 1.0f;
        break;
    case GameLight::Type::Point:
        out.Type = D3DLIGHT_POINT;
        break;
    }
    if (light.radius <= 0) return false;
    out.Position = {light.position[0], light.position[1], light.position[2]};
    out.Range = light.radius * std::sqrt(brightness);
    return true;
}
