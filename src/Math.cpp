#include "Internal.h"

#include <cmath>

namespace ludifex
{
namespace
{

// Storage is column-major: M[column * 4 + row]. This matches HLSL's default
// constant-buffer packing, so a matrix can be memcpy'd into a uniform block
// and used with mul(matrix, vector) without transposing.
inline void SetElement(Mat4& matrix, int row, int column, float value)
{
    matrix.M[column * 4 + row] = value;
}

inline float GetElement(const Mat4& matrix, int row, int column)
{
    return matrix.M[column * 4 + row];
}

Vec3 Subtract(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.X - b.X, a.Y - b.Y, a.Z - b.Z };
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X };
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

Vec3 Normalize(const Vec3& v)
{
    const float lengthSquared = Dot(v, v);
    if (lengthSquared <= 1e-20f)
    {
        return Vec3{ 0.0f, 0.0f, 1.0f };
    }
    const float inverse = 1.0f / std::sqrt(lengthSquared);
    return Vec3{ v.X * inverse, v.Y * inverse, v.Z * inverse };
}

} // namespace

Mat4 Mat4::Perspective(float fieldOfViewRadians, float aspect, float nearPlane, float farPlane)
{
    Mat4 result;
    for (float& value : result.M)
    {
        value = 0.0f;
    }

    if (aspect <= 0.0f || fieldOfViewRadians <= 0.0f || farPlane <= nearPlane)
    {
        return Identity();
    }

    const float focal = 1.0f / std::tan(fieldOfViewRadians * 0.5f);

    // Right-handed, with clip depth in [0, 1], as Direct3D 12, Vulkan, and
    // Metal expect.
    SetElement(result, 0, 0, focal / aspect);
    SetElement(result, 1, 1, focal);
    SetElement(result, 2, 2, farPlane / (nearPlane - farPlane));
    SetElement(result, 3, 2, -1.0f);
    SetElement(result, 2, 3, (nearPlane * farPlane) / (nearPlane - farPlane));

    return result;
}

Mat4 Mat4::Orthographic(float left, float right, float bottom, float top, float nearPlane,
                        float farPlane)
{
    Mat4 result;
    for (float& value : result.M)
    {
        value = 0.0f;
    }

    if (right == left || top == bottom || farPlane == nearPlane)
    {
        return Identity();
    }

    // Right-handed, clip depth in [0, 1], matching the perspective form above.
    SetElement(result, 0, 0, 2.0f / (right - left));
    SetElement(result, 1, 1, 2.0f / (top - bottom));
    SetElement(result, 2, 2, 1.0f / (nearPlane - farPlane));

    SetElement(result, 0, 3, -(right + left) / (right - left));
    SetElement(result, 1, 3, -(top + bottom) / (top - bottom));
    SetElement(result, 2, 3, nearPlane / (nearPlane - farPlane));
    SetElement(result, 3, 3, 1.0f);

    return result;
}

Mat4 Mat4::LookAt(Vec3 eye, Vec3 target, Vec3 up)
{
    // Right-handed: the camera looks down its own negative Z.
    const Vec3 forward = Normalize(Subtract(eye, target));
    Vec3 right = Cross(up, forward);

    if (Dot(right, right) <= 1e-12f)
    {
        // The up vector is parallel to the view direction. Pick any
        // perpendicular rather than producing a degenerate matrix.
        right = Cross(Vec3{ 0.0f, 0.0f, 1.0f }, forward);
        if (Dot(right, right) <= 1e-12f)
        {
            right = Cross(Vec3{ 1.0f, 0.0f, 0.0f }, forward);
        }
    }

    right = Normalize(right);
    const Vec3 trueUp = Cross(forward, right);

    Mat4 result = Identity();

    SetElement(result, 0, 0, right.X);
    SetElement(result, 0, 1, right.Y);
    SetElement(result, 0, 2, right.Z);
    SetElement(result, 0, 3, -Dot(right, eye));

    SetElement(result, 1, 0, trueUp.X);
    SetElement(result, 1, 1, trueUp.Y);
    SetElement(result, 1, 2, trueUp.Z);
    SetElement(result, 1, 3, -Dot(trueUp, eye));

    SetElement(result, 2, 0, forward.X);
    SetElement(result, 2, 1, forward.Y);
    SetElement(result, 2, 2, forward.Z);
    SetElement(result, 2, 3, -Dot(forward, eye));

    SetElement(result, 3, 0, 0.0f);
    SetElement(result, 3, 1, 0.0f);
    SetElement(result, 3, 2, 0.0f);
    SetElement(result, 3, 3, 1.0f);

    return result;
}

Mat4 Mat4::FromTransform(const Transform3& transform)
{
    const Quat& q = transform.Rotation;

    const float xx = q.X * q.X;
    const float yy = q.Y * q.Y;
    const float zz = q.Z * q.Z;
    const float xy = q.X * q.Y;
    const float xz = q.X * q.Z;
    const float yz = q.Y * q.Z;
    const float wx = q.W * q.X;
    const float wy = q.W * q.Y;
    const float wz = q.W * q.Z;

    // Rotation basis vectors, then scaled per axis and placed as columns.
    const Vec3 axisX{ 1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy) };
    const Vec3 axisY{ 2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx) };
    const Vec3 axisZ{ 2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy) };

    Mat4 result = Identity();

    SetElement(result, 0, 0, axisX.X * transform.Scale.X);
    SetElement(result, 1, 0, axisX.Y * transform.Scale.X);
    SetElement(result, 2, 0, axisX.Z * transform.Scale.X);
    SetElement(result, 3, 0, 0.0f);

    SetElement(result, 0, 1, axisY.X * transform.Scale.Y);
    SetElement(result, 1, 1, axisY.Y * transform.Scale.Y);
    SetElement(result, 2, 1, axisY.Z * transform.Scale.Y);
    SetElement(result, 3, 1, 0.0f);

    SetElement(result, 0, 2, axisZ.X * transform.Scale.Z);
    SetElement(result, 1, 2, axisZ.Y * transform.Scale.Z);
    SetElement(result, 2, 2, axisZ.Z * transform.Scale.Z);
    SetElement(result, 3, 2, 0.0f);

    SetElement(result, 0, 3, transform.Position.X);
    SetElement(result, 1, 3, transform.Position.Y);
    SetElement(result, 2, 3, transform.Position.Z);
    SetElement(result, 3, 3, 1.0f);

    return result;
}

Mat4 Mat4::operator*(const Mat4& other) const
{
    Mat4 result;

    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                sum += GetElement(*this, row, k) * GetElement(other, k, column);
            }
            SetElement(result, row, column, sum);
        }
    }

    return result;
}

// ---------------------------------------------------------------------------
// Quaternions
// ---------------------------------------------------------------------------

Quat Quat::FromAxisAngle(Vec3 axis, float radians)
{
    const float lengthSquared = axis.X * axis.X + axis.Y * axis.Y + axis.Z * axis.Z;
    if (!detail::IsFinite(axis) || !std::isfinite(radians) || lengthSquared <= 1e-12f)
    {
        // No axis is no rotation, which is more useful than a NaN.
        return Quat{};
    }

    const float inverse = 1.0f / std::sqrt(lengthSquared);
    const float half = radians * 0.5f;
    const float sine = std::sin(half);

    Quat result;
    result.X = axis.X * inverse * sine;
    result.Y = axis.Y * inverse * sine;
    result.Z = axis.Z * inverse * sine;
    result.W = std::cos(half);
    return result;
}

Quat Quat::FromEuler(float yawRadians, float pitchRadians, float rollRadians)
{
    const Quat yaw = FromAxisAngle(Vec3{ 0.0f, 1.0f, 0.0f }, yawRadians);
    const Quat pitch = FromAxisAngle(Vec3{ 1.0f, 0.0f, 0.0f }, pitchRadians);
    const Quat roll = FromAxisAngle(Vec3{ 0.0f, 0.0f, 1.0f }, rollRadians);
    return roll.Then(pitch).Then(yaw);
}

Quat Quat::Inverse() const
{
    return Quat{ -X, -Y, -Z, W };
}

Quat Quat::Then(const Quat& next) const
{
    Quat result;
    result.W = next.W * W - next.X * X - next.Y * Y - next.Z * Z;
    result.X = next.W * X + next.X * W + next.Y * Z - next.Z * Y;
    result.Y = next.W * Y - next.X * Z + next.Y * W + next.Z * X;
    result.Z = next.W * Z + next.X * Y - next.Y * X + next.Z * W;
    return result;
}

Vec3 Quat::Rotate(Vec3 value) const
{
    // v + 2w(q x v) + 2(q x (q x v)), which is the quaternion sandwich
    // without building the matrix.
    const float crossX = Y * value.Z - Z * value.Y;
    const float crossY = Z * value.X - X * value.Z;
    const float crossZ = X * value.Y - Y * value.X;

    const float secondX = Y * crossZ - Z * crossY;
    const float secondY = Z * crossX - X * crossZ;
    const float secondZ = X * crossY - Y * crossX;

    return Vec3{ value.X + 2.0f * (W * crossX + secondX), value.Y + 2.0f * (W * crossY + secondY),
                 value.Z + 2.0f * (W * crossZ + secondZ) };
}

} // namespace ludifex
