#include "camera.h"

#include <cmath>
#include <limits>

namespace {

float Dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

D3DMATRIX ReadMatrixConstant(const float (*registers)[4], bool rowMajor) {
    // mul(M, v) computes clip[i] = sum_j M[i][j] * v[j], so the D3D row-vector matrix is
    // D[j][i] = M[i][j]. Column-major packing stores column j of M in register j, which makes
    // register j exactly row j of D. Row-major packing stores rows of M, i.e. columns of D.
    D3DMATRIX m;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m.m[r][c] = rowMajor ? registers[c][r] : registers[r][c];
        }
    }
    return m;
}

D3DMATRIX Multiply(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX m;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m.m[r][c] = a.m[r][0] * b.m[0][c] + a.m[r][1] * b.m[1][c] + a.m[r][2] * b.m[2][c] + a.m[r][3] * b.m[3][c];
        }
    }
    return m;
}

D3DMATRIX Identity() {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

bool DecomposeViewProjection(const D3DMATRIX& viewProjection, ViewProjection& out) {
    // Columns of the input; c[k][0..2] is the rotation part, c[k][3] the translation part
    float c[4][4];
    for (int k = 0; k < 4; ++k) {
        for (int r = 0; r < 4; ++r) c[k][r] = viewProjection.m[r][k];
    }

    // Column 3 is the view-space forward axis (clip w = view z). Normalize the whole matrix by
    // its length: a uniform scale of clip space does not change the projection.
    const float forwardLength = std::sqrt(Dot3(c[3], c[3]));
    if (forwardLength < 1e-6f) return false; // orthographic: w does not depend on position
    for (auto& column : c) {
        for (float& v : column) v /= forwardLength;
    }

    // Column k of the view matrix; the rotation columns are orthonormal
    float view[4][4] = {};
    for (int r = 0; r < 4; ++r) view[2][r] = c[3][r];

    // Columns 0 and 1: c = s * view_axis + o * forward (o = off-center projection offset)
    float scale[2], offset[2];
    for (int k = 0; k < 2; ++k) {
        offset[k] = Dot3(c[k], view[2]);
        float axis[4];
        for (int r = 0; r < 4; ++r) axis[r] = c[k][r] - offset[k] * view[2][r];
        scale[k] = std::sqrt(Dot3(axis, axis));
        if (scale[k] < 1e-6f) return false;
        for (int r = 0; r < 4; ++r) view[k][r] = axis[r] / scale[k];
    }

    // Column 2: c = a * forward + b * (0,0,0,1)
    const float a = Dot3(c[2], view[2]);
    const float b = c[2][3] - a * view[2][3];
    view[3][3] = 1.0f;

    D3DMATRIX& v = out.view;
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) v.m[r][k] = view[k][r];
    }

    D3DMATRIX& p = out.projection;
    p = {};
    p._11 = scale[0];
    p._22 = scale[1];
    p._31 = offset[0];
    p._32 = offset[1];
    p._33 = a;
    p._34 = 1.0f;
    p._43 = b;

    // Depth: z_clip / w = a + b / z_view. Near plane maps to 0, far plane to 1.
    out.nearPlane = -b / a;
    out.farPlane = std::fabs(a - 1.0f) < 1e-6f ? std::numeric_limits<float>::infinity() : -b / (a - 1.0f);

    const float(&R)[4][4] = view; // R[k] = view axis k
    out.handedness = R[0][0] * (R[1][1] * R[2][2] - R[1][2] * R[2][1]) -
                     R[0][1] * (R[1][0] * R[2][2] - R[1][2] * R[2][0]) +
                     R[0][2] * (R[1][0] * R[2][1] - R[1][1] * R[2][0]);

    // Check against the (normalized) input. Relative to each element's size: the translation
    // entries grow with the distance from the world origin, and so does their float rounding.
    const D3DMATRIX product = Multiply(out.view, out.projection);
    out.error = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) {
            const float difference = std::fabs(product.m[r][k] - c[k][r]);
            out.error = std::fmax(out.error, difference / std::fmax(1.0f, std::fabs(c[k][r])));
        }
    }
    return true;
}

void ViewPosition(const D3DMATRIX& view, float out[3]) {
    // p * V = 0 for the camera position p: p = -t * R^T with t the translation row
    for (int i = 0; i < 3; ++i) {
        out[i] = -(view.m[3][0] * view.m[i][0] + view.m[3][1] * view.m[i][1] + view.m[3][2] * view.m[i][2]);
    }
}

float VerticalFovDegrees(const D3DMATRIX& projection) {
    return 2.0f * std::atan(1.0f / projection._22) * 57.29578f;
}
