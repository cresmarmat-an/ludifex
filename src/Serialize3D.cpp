// fopen is used on purpose; the _s variants are not portable.
#define _CRT_SECURE_NO_WARNINGS

// Saving and restoring a world.
//
// A world is captured as a block of bytes and rebuilt from it, for save games,
// level editing, and undo. The data covers the world's actors (shapes,
// materials, positions, velocities, appearance, and parenting), its lights,
// its camera, and its step settings.
//
// Not captured: callbacks the program registered, textures and custom
// materials it created through handles, and joints between actors. The
// program restores those itself. The format carries a version, so a file from
// an incompatible build is refused with a message instead of being misread.

#include "Internal.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ludifex
{
namespace
{

using detail::ActorRecord3;
using detail::World3DState;

constexpr uint32_t Magic = 0x5744554C;  // "LUDW"
constexpr uint32_t Version = 1;

constexpr uint32_t NoParent = 0xFFFFFFFFu;

// --- writing ---------------------------------------------------------------

struct Writer
{
    std::vector<uint8_t> Bytes;

    void Raw(const void* data, size_t size)
    {
        const auto* source = static_cast<const uint8_t*>(data);
        Bytes.insert(Bytes.end(), source, source + size);
    }

    void U32(uint32_t value) { Raw(&value, sizeof(value)); }
    void U8(uint8_t value) { Raw(&value, sizeof(value)); }
    void F32(float value) { Raw(&value, sizeof(value)); }

    void Vector(const Vec3& value)
    {
        F32(value.X);
        F32(value.Y);
        F32(value.Z);
    }

    void Rotation(const Quat& value)
    {
        F32(value.X);
        F32(value.Y);
        F32(value.Z);
        F32(value.W);
    }

    void Colour(const Color& value)
    {
        F32(value.R);
        F32(value.G);
        F32(value.B);
        F32(value.A);
    }

    void Text(const std::string& value)
    {
        U32(static_cast<uint32_t>(value.size()));
        Raw(value.data(), value.size());
    }
};

// --- reading ---------------------------------------------------------------

struct Reader
{
    const uint8_t* Data = nullptr;
    size_t Size = 0;
    size_t At = 0;
    bool Failed = false;

    bool Raw(void* out, size_t size)
    {
        if (Failed || At + size > Size)
        {
            Failed = true;
            return false;
        }
        std::memcpy(out, Data + At, size);
        At += size;
        return true;
    }

    uint32_t U32()
    {
        uint32_t value = 0;
        Raw(&value, sizeof(value));
        return value;
    }

    uint8_t U8()
    {
        uint8_t value = 0;
        Raw(&value, sizeof(value));
        return value;
    }

    float F32()
    {
        float value = 0.0f;
        Raw(&value, sizeof(value));
        return value;
    }

    Vec3 Vector()
    {
        Vec3 value;
        value.X = F32();
        value.Y = F32();
        value.Z = F32();
        return value;
    }

    Quat Rotation()
    {
        Quat value;
        value.X = F32();
        value.Y = F32();
        value.Z = F32();
        value.W = F32();
        return value;
    }

    Color Colour()
    {
        Color value;
        value.R = F32();
        value.G = F32();
        value.B = F32();
        value.A = F32();
        return value;
    }

    std::string Text()
    {
        const uint32_t length = U32();
        if (Failed || At + length > Size)
        {
            Failed = true;
            return std::string{};
        }
        std::string value(reinterpret_cast<const char*>(Data + At), length);
        At += length;
        return value;
    }
};

// The first shape of a body carries the material values worth keeping.
void ReadMaterial(b3BodyId body, float& friction, float& restitution)
{
    friction = 0.3f;
    restitution = 0.0f;

    b3ShapeId shape{};
    if (b3Body_GetShapes(body, &shape, 1) > 0 && b3Shape_IsValid(shape))
    {
        friction = b3Shape_GetFriction(shape);
        restitution = b3Shape_GetRestitution(shape);
    }
}

uint8_t BodyTypeToByte(b3BodyType type)
{
    switch (type)
    {
        case b3_staticBody: return 1;
        case b3_kinematicBody: return 2;
        default: return 0;
    }
}

BodyType ByteToBodyType(uint8_t value)
{
    switch (value)
    {
        case 1: return BodyType::Static;
        case 2: return BodyType::Kinematic;
        default: return BodyType::Dynamic;
    }
}

} // namespace

std::vector<uint8_t> World3D::Save() const
{
    Writer writer;

    if (m_State == nullptr)
    {
        return writer.Bytes;
    }

    writer.U32(Magic);
    writer.U32(Version);

    // How the world steps, so a restored world behaves as the saved one did.
    const World3DConfig& config = m_State->Config;
    writer.Vector(config.Gravity);
    writer.F32(config.FixedTimeStep);
    writer.U32(static_cast<uint32_t>(config.SubStepCount));
    writer.U32(static_cast<uint32_t>(config.MaxStepsPerFrame));
    writer.U8(config.PhysicsEnabled ? 1 : 0);
    writer.U8(config.EnableSleep ? 1 : 0);
    writer.U8(config.EnableContinuous ? 1 : 0);
    writer.U8(config.Deterministic ? 1 : 0);
    writer.U32(config.WorkerCount);

    const Camera3D& camera = m_State->Camera;
    writer.Vector(camera.Position);
    writer.Vector(camera.Target);
    writer.Vector(camera.Up);
    writer.F32(camera.FieldOfViewDegrees);
    writer.F32(camera.NearPlane);
    writer.F32(camera.FarPlane);

    // Actors are written in slot order, so a parent is named by the slot it
    // will occupy when the world is rebuilt.
    std::vector<uint32_t> slotOf(m_State->Actors.size(), NoParent);
    uint32_t written = 0;
    for (size_t index = 0; index < m_State->Actors.size(); ++index)
    {
        if (m_State->Actors[index].Alive)
        {
            slotOf[index] = written++;
        }
    }

    writer.U32(written);

    for (size_t index = 0; index < m_State->Actors.size(); ++index)
    {
        const ActorRecord3& record = m_State->Actors[index];
        if (!record.Alive)
        {
            continue;
        }

        writer.U8(static_cast<uint8_t>(record.Shape));
        writer.U8(BodyTypeToByte(b3Body_GetType(record.Body)));

        uint8_t flags = 0;
        flags |= record.IsSensor ? 1u : 0u;
        flags |= record.Look.Visible ? 2u : 0u;
        flags |= record.Look.Unlit ? 4u : 0u;
        flags |= record.Look.SurfaceOverridden ? 8u : 0u;
        flags |= record.Look.FlipX ? 16u : 0u;
        flags |= record.IsCharacter ? 32u : 0u;
        writer.U8(flags);
        writer.U8(0); // reserved, so the record stays four-byte aligned

        writer.Vector(record.Scale);
        writer.Vector(detail::FromB3Pos(b3Body_GetPosition(record.Body)));
        writer.Rotation(detail::FromB3(b3Body_GetRotation(record.Body)));
        writer.Vector(detail::FromB3(b3Body_GetLinearVelocity(record.Body)));
        writer.Vector(detail::FromB3(b3Body_GetAngularVelocity(record.Body)));

        float friction = 0.3f;
        float restitution = 0.0f;
        ReadMaterial(record.Body, friction, restitution);
        writer.F32(friction);
        writer.F32(restitution);

        writer.Colour(record.Look.Tint);
        writer.F32(record.Look.Roughness);
        writer.F32(record.Look.Metallic);
        writer.F32(record.Look.UVScale.X);
        writer.F32(record.Look.UVScale.Y);
        writer.F32(record.Look.UVOffset.X);
        writer.F32(record.Look.UVOffset.Y);
        writer.U32(static_cast<uint32_t>(record.Look.Layer));

        // The hierarchy, by slot rather than by handle: handles do not survive
        // a save, and slots do.
        const uint32_t parent =
            record.Parent.IsValid() && record.Parent.Index < slotOf.size() ? slotOf[record.Parent.Index]
                                                                          : NoParent;
        writer.U32(parent);
        writer.Vector(record.Local.Position);
        writer.Rotation(record.Local.Rotation);

        writer.Text(record.Name);
        writer.Text(record.ModelIndex >= 0 ? detail::ModelPathAt(*m_State, record.ModelIndex)
                                           : std::string{});
    }

    uint32_t lightCount = 0;
    for (const detail::LightRecord3& light : m_State->Lights)
    {
        lightCount += light.Alive ? 1u : 0u;
    }
    writer.U32(lightCount);

    for (const detail::LightRecord3& light : m_State->Lights)
    {
        if (!light.Alive)
        {
            continue;
        }
        writer.Vector(light.Desc.Position);
        writer.Colour(light.Desc.Tint);
        writer.F32(light.Desc.Intensity);
        writer.F32(light.Desc.Range);
    }

    return writer.Bytes;
}

bool World3D::Load(const std::vector<uint8_t>& bytes)
{
    if (m_State == nullptr)
    {
        return false;
    }

    Reader reader{ bytes.data(), bytes.size(), 0, false };

    if (reader.U32() != Magic)
    {
        LogMessage(LogLevel::Error, "world",
                   "Load was given bytes that are not a saved world. The world was left as it was.");
        return false;
    }

    const uint32_t version = reader.U32();
    if (version != Version)
    {
        LogMessage(LogLevel::Error, "world",
                   "This world was saved by format version %u and this build reads version %u. The "
                   "world was left as it was.",
                   version, Version);
        return false;
    }

    // Everything already here goes, including anything queued.
    ForEachActor([](Actor3D& actor) { actor.Destroy(); });
    ApplyPendingChanges();

    for (detail::LightRecord3& light : m_State->Lights)
    {
        if (light.Alive)
        {
            light.Alive = false;
            ++light.Generation;
            if (light.Generation == 0)
            {
                light.Generation = 1;
            }
        }
    }
    m_State->Lights.clear();
    m_State->FreeLights.clear();
    m_State->LiveLights = 0;

    World3DConfig config;
    config.Gravity = reader.Vector();
    config.FixedTimeStep = reader.F32();
    config.SubStepCount = static_cast<int>(reader.U32());
    config.MaxStepsPerFrame = static_cast<int>(reader.U32());
    config.PhysicsEnabled = reader.U8() != 0;
    config.EnableSleep = reader.U8() != 0;
    config.EnableContinuous = reader.U8() != 0;
    config.Deterministic = reader.U8() != 0;
    config.WorkerCount = reader.U32();

    SetGravity(config.Gravity);
    m_State->Config.FixedTimeStep = config.FixedTimeStep;
    m_State->Config.SubStepCount = config.SubStepCount;
    m_State->Config.MaxStepsPerFrame = config.MaxStepsPerFrame;
    m_State->Config.PhysicsEnabled = config.PhysicsEnabled;

    Camera3D camera;
    camera.Position = reader.Vector();
    camera.Target = reader.Vector();
    camera.Up = reader.Vector();
    camera.FieldOfViewDegrees = reader.F32();
    camera.NearPlane = reader.F32();
    camera.FarPlane = reader.F32();
    SetCamera(camera);

    const uint32_t actorCount = reader.U32();
    if (reader.Failed)
    {
        LogMessage(LogLevel::Error, "world", "This saved world ends before its actors do.");
        return false;
    }

    std::vector<Actor3D> restored;
    std::vector<uint32_t> parents;
    std::vector<Transform3> locals;
    restored.reserve(actorCount);
    parents.reserve(actorCount);
    locals.reserve(actorCount);

    for (uint32_t index = 0; index < actorCount && !reader.Failed; ++index)
    {
        const auto shape = static_cast<detail::ShapeKind>(reader.U8());
        const BodyType type = ByteToBodyType(reader.U8());
        const uint8_t flags = reader.U8();
        reader.U8();

        const Vec3 scale = reader.Vector();
        const Vec3 position = reader.Vector();
        const Quat rotation = reader.Rotation();
        const Vec3 linear = reader.Vector();
        const Vec3 angular = reader.Vector();
        const float friction = reader.F32();
        const float restitution = reader.F32();

        const Color tint = reader.Colour();
        const float roughness = reader.F32();
        const float metallic = reader.F32();
        const float uvScaleX = reader.F32();
        const float uvScaleY = reader.F32();
        const float uvOffsetX = reader.F32();
        const float uvOffsetY = reader.F32();
        const int layer = static_cast<int>(reader.U32());

        const uint32_t parent = reader.U32();
        Transform3 local;
        local.Position = reader.Vector();
        local.Rotation = reader.Rotation();

        const std::string name = reader.Text();
        const std::string model = reader.Text();

        if (reader.Failed)
        {
            break;
        }

        Actor3D actor;
        switch (shape)
        {
            case detail::ShapeKind::Sphere:
                actor = AddSphere({
                    .Radius = scale.X * 0.5f,
                    .Position = position,
                    .Type = type,
                    .Friction = friction,
                    .Restitution = restitution,
                    .IsSensor = (flags & 1u) != 0,
                    .Name = name,
                });
                break;

            case detail::ShapeKind::Capsule:
                actor = AddCapsule({
                    .Radius = scale.X * 0.5f,
                    .Height = scale.Y,
                    .Position = position,
                    .Type = type,
                    .Friction = friction,
                    .Restitution = restitution,
                    .IsSensor = (flags & 1u) != 0,
                    .Name = name,
                });
                break;

            case detail::ShapeKind::Model:
                actor = AddModel({
                    .Path = model,
                    .Position = position,
                    .Rotation = rotation,
                    .Type = type,
                    .Friction = friction,
                    .Restitution = restitution,
                    .Name = name,
                });
                break;

            case detail::ShapeKind::Box:
            default:
                actor = AddBox({
                    .Scale = scale,
                    .Position = position,
                    .Rotation = rotation,
                    .Type = type,
                    .Friction = friction,
                    .Restitution = restitution,
                    .IsSensor = (flags & 1u) != 0,
                    .Name = name,
                });
                break;
        }

        if (actor.IsValid())
        {
            actor.SetRotation(rotation);
            actor.SetLinearVelocity(linear);
            actor.SetAngularVelocity(angular);
            actor.SetColor(tint);
            actor.SetVisible((flags & 2u) != 0);
            actor.SetTextureTiling({ uvScaleX, uvScaleY }, { uvOffsetX, uvOffsetY });

            if ((flags & 8u) != 0)
            {
                actor.SetRoughness(roughness);
                actor.SetMetallic(metallic);
            }

            ActorRecord3* record = detail::Resolve(m_State, actor.GetId(), "Load");
            if (record != nullptr)
            {
                record->Look.Unlit = (flags & 4u) != 0;
                record->Look.FlipX = (flags & 16u) != 0;
                record->Look.Layer = layer;
            }
        }

        restored.push_back(actor);
        parents.push_back(parent);
        locals.push_back(local);
    }

    ApplyPendingChanges();

    // The hierarchy, once every actor exists to be pointed at.
    for (size_t index = 0; index < restored.size(); ++index)
    {
        if (parents[index] == NoParent || parents[index] >= restored.size())
        {
            continue;
        }
        restored[index].SetParent(restored[parents[index]]);
    }
    ApplyPendingChanges();

    for (size_t index = 0; index < restored.size(); ++index)
    {
        if (parents[index] != NoParent && parents[index] < restored.size() &&
            restored[index].IsValid())
        {
            ActorRecord3* record = detail::Resolve(m_State, restored[index].GetId(), "Load");
            if (record != nullptr)
            {
                record->Local = locals[index];
            }
        }
    }

    const uint32_t lightCount = reader.U32();
    for (uint32_t index = 0; index < lightCount && !reader.Failed; ++index)
    {
        PointLightDesc light;
        light.Position = reader.Vector();
        light.Tint = reader.Colour();
        light.Intensity = reader.F32();
        light.Range = reader.F32();

        if (!reader.Failed)
        {
            AddPointLight(light);
        }
    }

    if (reader.Failed)
    {
        LogMessage(LogLevel::Warning, "world",
                   "This saved world ended early; what could be read was restored.");
    }

    LogMessage(LogLevel::Info, "world", "Restored a world of %zu actors and %u lights.",
               restored.size(), lightCount);
    return !reader.Failed;
}

bool World3D::SaveToFile(const std::string& path) const
{
    const std::vector<uint8_t> bytes = Save();
    if (bytes.empty())
    {
        return false;
    }

    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr)
    {
        LogMessage(LogLevel::Error, "world", "Could not open \"%s\" to save the world.", path.c_str());
        return false;
    }

    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    if (written != bytes.size())
    {
        LogMessage(LogLevel::Error, "world", "Only part of the world reached \"%s\".", path.c_str());
        return false;
    }
    return true;
}

bool World3D::LoadFromFile(const std::string& path)
{
    const std::string resolved = ResolveAssetPath(path);

    std::FILE* file = std::fopen(resolved.c_str(), "rb");
    if (file == nullptr)
    {
        LogMessage(LogLevel::Error, "world", "Could not open \"%s\" to load a world.", path.c_str());
        return false;
    }

    std::fseek(file, 0, SEEK_END);
    const long size = std::ftell(file);
    std::fseek(file, 0, SEEK_SET);

    if (size <= 0)
    {
        std::fclose(file);
        LogMessage(LogLevel::Error, "world", "\"%s\" is empty.", path.c_str());
        return false;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    const size_t read = std::fread(bytes.data(), 1, bytes.size(), file);
    std::fclose(file);

    if (read != bytes.size())
    {
        LogMessage(LogLevel::Error, "world", "\"%s\" could not be read to its end.", path.c_str());
        return false;
    }

    return Load(bytes);
}

} // namespace ludifex
