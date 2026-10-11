#pragma once

#include <windows.h>

#include <d3d9types.h>

// A UE3 dynamic light, as read from the constants of its light pass shaders.
struct GameLight {
    enum class Type { Point, Spot, Directional };

    Type type = Type::Point;
    float position[3] = {};  // point, spot: world position
    float radius = 0;        // point, spot: UE3 attenuation radius
    float direction[3] = {}; // spot: direction the cone points; directional: direction the light travels
    float color[3] = {};     // UE3 LightColor: colour times brightness
    float cosOuterCone = 0;  // spot
    float cosInnerCone = 0;  // spot
};

// Light pass constants -> GameLight. Arguments are the shader constants by name:
//   LightPositionAndInvRadius (VS, float4: xyz position, w = 1 / radius)
//   LightColorAndFalloffExponent / LightColor (PS, rgb)
//   SpotDirection (PS, float3), SpotAngles (PS, float2: cos(outer), 1 / (cos(inner) - cos(outer)))
//   LightDirection (VS, float3: points from surfaces towards the light)
GameLight MakePointLight(const float positionAndInvRadius[4], const float color[3]);
GameLight MakeSpotLight(const float positionAndInvRadius[4], const float color[3], const float spotDirection[3],
                        const float spotAngles[2]);
GameLight MakeDirectionalLight(const float lightDirection[3], const float color[3]);

// The same light drawn again (UE3 draws one light pass per lit object)
bool SameLight(const GameLight& a, const GameLight& b);

// Converts to a D3D9 light for Remix. Remix sizes a light without falloff terms from its Range,
// with intensity growing with Range^2, so UE3 brightness is folded in as radius * sqrt(brightness).
// Returns false for lights Remix cannot use (black, zero radius).
bool ToD3DLight(const GameLight& light, D3DLIGHT9& out);
