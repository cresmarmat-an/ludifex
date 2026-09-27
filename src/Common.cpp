#include "Internal.h"
#include "WorldMaterials.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>

namespace ludifex
{
namespace
{

LogHandler g_Handler;
LogLevel g_MinimumLevel = LogLevel::Info;

const char* LevelName(LogLevel level)
{
    switch (level)
    {
        case LogLevel::Trace:   return "trace";
        case LogLevel::Info:    return "info";
        case LogLevel::Warning: return "warning";
        case LogLevel::Error:   return "error";
    }
    return "?";
}

} // namespace

void SetLogHandler(LogHandler handler)
{
    g_Handler = std::move(handler);
}

void SetMinimumLogLevel(LogLevel level)
{
    g_MinimumLevel = level;
}

void LogMessage(LogLevel level, const char* category, const char* format, ...)
{
    if (static_cast<int>(level) < static_cast<int>(g_MinimumLevel))
    {
        return;
    }

    char message[1024];

    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (g_Handler)
    {
        g_Handler(level, category, message);
        return;
    }

    std::FILE* stream = (level == LogLevel::Error || level == LogLevel::Warning) ? stderr : stdout;
    std::fprintf(stream, "[ludifex:%s] %s: %s\n", category, LevelName(level), message);
    std::fflush(stream);
}

namespace detail
{

bool ValidateExtent(float extent, const char* what)
{
    if (!IsFinite(extent))
    {
        LogMessage(LogLevel::Error, "physics", "%s is not a finite number; the shape was rejected.", what);
        return false;
    }

    if (extent < MinimumExtent)
    {
        LogMessage(LogLevel::Error, "physics",
                   "%s is %.6f m, below the %.3f m minimum. Sizes this small are outside the "
                   "range the solver is conditioned for; scale the world up or use a larger shape.",
                   what, static_cast<double>(extent), static_cast<double>(MinimumExtent));
        return false;
    }

    if (extent > MaximumExtent)
    {
        LogMessage(LogLevel::Warning, "physics",
                   "%s is %.1f m, above the %.1f m guideline. Contacts on shapes this large lose "
                   "precision; consider splitting the shape.",
                   what, static_cast<double>(extent), static_cast<double>(MaximumExtent));
    }

    return true;
}

Quat NLerp(const Quat& a, const Quat& b, float t)
{
    const float dot = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    const float sign = (dot < 0.0f) ? -1.0f : 1.0f;

    Quat result;
    result.X = Lerp(a.X, b.X * sign, t);
    result.Y = Lerp(a.Y, b.Y * sign, t);
    result.Z = Lerp(a.Z, b.Z * sign, t);
    result.W = Lerp(a.W, b.W * sign, t);

    const float lengthSquared =
        result.X * result.X + result.Y * result.Y + result.Z * result.Z + result.W * result.W;

    if (lengthSquared > 1e-12f)
    {
        const float inverseLength = 1.0f / std::sqrt(lengthSquared);
        result.X *= inverseLength;
        result.Y *= inverseLength;
        result.Z *= inverseLength;
        result.W *= inverseLength;
    }
    else
    {
        result = Quat{};
    }

    return result;
}

Transform3 LerpTransform(const Transform3& a, const Transform3& b, float t)
{
    Transform3 result;
    result.Position = Vec3{ Lerp(a.Position.X, b.Position.X, t),
                            Lerp(a.Position.Y, b.Position.Y, t),
                            Lerp(a.Position.Z, b.Position.Z, t) };
    result.Rotation = NLerp(a.Rotation, b.Rotation, t);
    result.Scale = b.Scale;
    return result;
}

Transform2 LerpTransform(const Transform2& a, const Transform2& b, float t)
{
    Transform2 result;
    result.Position = Vec2{ Lerp(a.Position.X, b.Position.X, t),
                            Lerp(a.Position.Y, b.Position.Y, t) };

    // Interpolate along the shortest arc so a body crossing the -pi/pi seam
    // does not appear to spin the long way around for one frame.
    float delta = b.Rotation - a.Rotation;
    constexpr float Pi = 3.14159265358979323846f;
    while (delta > Pi)
    {
        delta -= 2.0f * Pi;
    }
    while (delta < -Pi)
    {
        delta += 2.0f * Pi;
    }

    result.Rotation = a.Rotation + delta * t;
    result.Scale = b.Scale;
    return result;
}

ActorRecord3* Resolve(World3DState* state, const ActorId& id, const char* operation)
{
    if (state == nullptr)
    {
        return nullptr;
    }

    if (!id.IsValid() || id.Index >= state->Actors.size())
    {
        LogMessage(LogLevel::Warning, "actor",
                   "%s was called on an actor handle that never referred to anything.", operation);
        return nullptr;
    }

    ActorRecord3& record = state->Actors[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        LogMessage(LogLevel::Warning, "actor",
                   "%s was called on a destroyed actor (index %u, generation %u). The call did "
                   "nothing. Check the handle against IsValid() before using it.",
                   operation, id.Index, id.Generation);
        return nullptr;
    }

    return &record;
}

JointRecord3* Resolve(World3DState* state, const JointId& id, const char* operation)
{
    if (state == nullptr)
    {
        return nullptr;
    }

    if (!id.IsValid() || id.Index >= state->Joints.size())
    {
        LogMessage(LogLevel::Warning, "joint",
                   "%s was called on a joint handle that never referred to anything.", operation);
        return nullptr;
    }

    JointRecord3& record = state->Joints[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        LogMessage(LogLevel::Warning, "joint",
                   "%s was called on a destroyed joint (index %u, generation %u). The call did "
                   "nothing.",
                   operation, id.Index, id.Generation);
        return nullptr;
    }

    // Box3D destroys a joint when either of its bodies is destroyed, so the
    // record can still look alive while the joint underneath is gone.
    if (!b3Joint_IsValid(record.Joint))
    {
        LogMessage(LogLevel::Warning, "joint",
                   "%s was called on a joint whose actors were destroyed. The call did nothing.",
                   operation);
        return nullptr;
    }

    return &record;
}

JointRecord2* Resolve(World2DState* state, const JointId& id, const char* operation)
{
    if (state == nullptr)
    {
        return nullptr;
    }

    if (!id.IsValid() || id.Index >= state->Joints.size())
    {
        LogMessage(LogLevel::Warning, "joint",
                   "%s was called on a joint handle that never referred to anything.", operation);
        return nullptr;
    }

    JointRecord2& record = state->Joints[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        LogMessage(LogLevel::Warning, "joint",
                   "%s was called on a destroyed joint (index %u, generation %u). The call did "
                   "nothing.",
                   operation, id.Index, id.Generation);
        return nullptr;
    }

    // Box2D destroys a joint along with either of its bodies.
    if (!b2Joint_IsValid(record.Joint))
    {
        LogMessage(LogLevel::Warning, "joint",
                   "%s was called on a joint whose actors were destroyed. The call did nothing.",
                   operation);
        return nullptr;
    }

    return &record;
}

ActorRecord2* Resolve(World2DState* state, const ActorId& id, const char* operation)
{
    if (state == nullptr)
    {
        return nullptr;
    }

    if (!id.IsValid() || id.Index >= state->Actors.size())
    {
        LogMessage(LogLevel::Warning, "actor",
                   "%s was called on an actor handle that never referred to anything.", operation);
        return nullptr;
    }

    ActorRecord2& record = state->Actors[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        LogMessage(LogLevel::Warning, "actor",
                   "%s was called on a destroyed actor (index %u, generation %u). The call did "
                   "nothing. Check the handle against IsValid() before using it.",
                   operation, id.Index, id.Generation);
        return nullptr;
    }

    return &record;
}

LightRecord3* Resolve(World3DState* state, const LightId& id, const char* operation)
{
    if (state == nullptr)
    {
        return nullptr;
    }

    if (!id.IsValid() || id.Index >= state->Lights.size())
    {
        LogMessage(LogLevel::Warning, "light",
                   "%s was called on a light handle that never referred to anything.", operation);
        return nullptr;
    }

    LightRecord3& record = state->Lights[id.Index];
    if (!record.Alive || record.Generation != id.Generation)
    {
        LogMessage(LogLevel::Warning, "light",
                   "%s was called on a destroyed light (index %u, generation %u). The call did "
                   "nothing.",
                   operation, id.Index, id.Generation);
        return nullptr;
    }

    return &record;
}

void SetActorUniform(Appearance& look, const std::string& name, const Color& value, bool isColor)
{
    if (!look.Material.IsValid())
    {
        LogMessage(LogLevel::Warning, "material",
                   "SetUniform(\"%s\") was called on an actor with no material; give it one with "
                   "SetMaterial first.",
                   name.c_str());
        return;
    }

    const int slot = GetWorldMaterials().FindSlot(look.Material, name);
    if (slot < 0)
    {
        LogMessage(LogLevel::Warning, "material",
                   "\"%s\" is not a uniform of this actor's material. Declare it in "
                   "MaterialDesc::Uniforms.",
                   name.c_str());
        return;
    }

    // Colours are converted exactly as the material's own colour uniforms are,
    // so a per-actor tint and the material's default tint mean the same thing.
    const Color stored = isColor ? ToLinear(value) : value;
    look.Params[slot][0] = stored.R;
    look.Params[slot][1] = stored.G;
    look.Params[slot][2] = stored.B;
    look.Params[slot][3] = stored.A;
    look.ParamMask |= 1u << slot;
}

void PushDebugLine(RenderExtras& extras, const Vec3& from, const Vec3& to, const Color& color)
{
    // A frame's worth of lines is plenty; beyond this something is drawing
    // lines without ever rendering them.
    constexpr size_t MaxDebugLines = size_t{ 1 } << 20;
    if (extras.DebugLines.size() < MaxDebugLines)
    {
        extras.DebugLines.push_back(DebugLine{ from, to, color });
    }
}

void RemovePostProcessEntries(RenderExtras& extras, const MaterialId& material)
{
    auto& list = extras.PostProcess;
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&](const PostProcessEntry& entry) { return entry.Material == material; }),
               list.end());
}

} // namespace detail
} // namespace ludifex
