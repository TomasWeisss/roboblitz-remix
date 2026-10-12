#pragma once

#include <windows.h>

#include <d3d9types.h>

// Matrices use the D3D9 fixed-function convention: row vectors, clip = v * M.

// Reads a float4x4 shader constant as the game's shaders use it: MulMatrix(M, v) = mul(M, v).
D3DMATRIX ReadMatrixConstant(const float (*registers)[4], bool rowMajor);

D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b);
D3DMATRIX Identity();

struct ViewProjection {
    D3DMATRIX view;
    D3DMATRIX projection;
    float error;       // max difference between view * projection and the input, relative to
                       // each element's magnitude (at least 1)
    float handedness;  // determinant of the view rotation: +1 proper, -1 mirrored
    float nearPlane;
    float farPlane;    // +inf for an infinite projection (UE3 uses one)
};

// Splits a combined view-projection matrix into a rigid view matrix and a perspective
// projection of the form
//   sx  0   0  0
//   0   sy  0  0
//   ox  oy  a  1
//   0   0   b  0
// Returns false if the matrix is not a perspective projection (e.g. orthographic).
bool DecomposeViewProjection(const D3DMATRIX& viewProjection, ViewProjection& out);
