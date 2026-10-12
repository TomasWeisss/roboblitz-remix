"""Tests for the material expression evaluator: python -m unittest discover tools/material_bake"""

import unittest

import numpy as np

import hlsl


def context(textures=None, uniforms=None):
    uv = np.array([[[0.25, 0.75]]], dtype=np.float32)

    def sample(name, coordinates):
        if name not in (textures or {}):
            raise hlsl.Unsupported(name)
        return np.broadcast_to(np.asarray(textures[name], np.float32), coordinates.shape[:-1] + (4,))

    return hlsl.Context(sample=sample, uniforms=uniforms or {}, uv=uv)


def value(text, ctx=None):
    return hlsl.evaluate_text(text, ctx or context()).reshape(-1).tolist()


class EvaluateTest(unittest.TestCase):
    def test_arithmetic_and_constructors(self):
        self.assertEqual(value("((float4(0.2,0.4,0.6,1.0) * 2.0) - float4(0.1,0.1,0.1,0.1)).rgb"),
                         [np.float32(0.3), np.float32(0.7), np.float32(1.1)])
        self.assertEqual(value("float3(1, float2(2, 3))"), [1, 2, 3])
        self.assertEqual(value("(float3)float4(1,2,3,4)"), [1, 2, 3])
        self.assertEqual(value("-(0.5).rgb"), [-0.5, -0.5, -0.5])

    def test_ternary_and_comparison(self):
        uniforms = {"UniformScalar_0": (0.75, 9, 9, 9)}
        ctx = context(uniforms=uniforms)
        self.assertEqual(value("(UniformScalar_0 >= (0.5)) ? float3(1,0,0) : float3(0,1,0)", ctx), [1, 0, 0])

    def test_uniforms(self):
        ctx = context(uniforms={"UniformVector_2": (1.5, 0.25, 0, 1), "UniformScalar_1": (2, 7, 7, 7)})
        self.assertEqual(value("UniformVector_2.rgb * UniformScalar_1", ctx), [3, 0.5, 0])
        with self.assertRaises(hlsl.Unsupported):
            value("UniformVector_9.rgb", ctx)

    def test_textures_use_uv0(self):
        ctx = context(textures={"Texture2D_1": (0.5, 1, 0, 1)})
        player = ("((tex2D(Texture2D_1,Parameters.TexCoords[0].xy) * float4(1,1,1,1)) + float4(0,0,0,0)).r"
                  " * float3(0.005, 0.24, 1.0)")
        self.assertEqual(value(player, ctx), [np.float32(0.0025), np.float32(0.12), 0.5])
        self.assertEqual(value("Parameters.TexCoords[1].xy", ctx), [0.25, 0.75])

    def test_view_dependent_terms_are_neutral(self):
        # Parallax offsets vanish; (1 + dot(R, C)) is 1, as seen at 45 degrees
        self.assertEqual(value("Parameters.TangentCameraVector.rg"), [0, 0])
        self.assertEqual(value("(1.0) + dot(Parameters.TangentReflectionVector,Parameters.TangentCameraVector)"), [1])
        self.assertEqual(value("texCUBE(TextureCube_0,Parameters.TangentReflectionVector).rgb"), [0, 0, 0])

    def test_intrinsics(self):
        self.assertEqual(value("lerp(float3(0,0,0), float3(1,0.5,0.25), 0.5)"), [0.5, 0.25, 0.125])
        self.assertEqual(value("clamp(float2(-1, 2), 0, 1)"), [0, 1])
        self.assertEqual(value("pow(float2(-1, 4), 0.5)"), [0, 2])
        self.assertEqual(value("dot(float3(1,2,3), float3(1,1,1))"), [6])

    def test_unsupported(self):
        for text in ("Parameters.ScreenPosition.xy", "foo(1)", "float3(1,1,1) +"):
            with self.assertRaises(hlsl.Unsupported):
                value(text)


if __name__ == "__main__":
    unittest.main()
