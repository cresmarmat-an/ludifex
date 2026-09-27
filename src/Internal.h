// Internal declarations shared between ludifex translation units.
// Not installed and not part of the public API.

#pragma once

#include <ludifex/ludifex.h>

#include "Animation.h"

#include <box2d/box2d.h>
#include <box3d/box3d.h>

#include <cmath>
#include <string>
#include <vector>

namespace ludifex::detail
{
class ModelStore;
class WorldAudio;
}

namespace ludifex::detail
{

// ---------------------------------------------------------------------------
// Validation
//
// Bad data is rejected at the setter instead of reaching the solver. Once a
// NaN enters a constraint island, the usual symptom is the whole scene
// disappearing, which is very hard to trace back.
// ---------------------------------------------------------------------------

inline bool IsFinite(float value)
{
    return std::isfinite(value);
}

inline bool IsFinite(const Vec2& v)
{
    return IsFinite(v.X) && IsFinite(v.Y);
}

inline bool IsFinite(const Vec3& v)
{
    return IsFinite(v.X) && IsFinite(v.Y) && IsFinite(v.Z);
}

inline bool IsFinite(const Quat& q)
{
    return IsFinite(q.X) && IsFinite(q.Y) && IsFinite(q.Z) && IsFinite(q.W);
}

// The solvers are tuned for real-world sizes in meters. Outside this range
// single-precision conditioning degrades and contacts become unreliable.
constexpr float MinimumExtent = 0.005f;
constexpr float MaximumExtent = 2000.0f;

bool ValidateExtent(float extent, const char* what);

// ---------------------------------------------------------------------------
// Backend conversions
// ---------------------------------------------------------------------------

inline b3Vec3 ToB3(const Vec3& v)
{
    return b3Vec3{ v.X, v.Y, v.Z };
}

inline Vec3 FromB3(const b3Vec3& v)
{
    return Vec3{ v.x, v.y, v.z };
}

// b3Pos is b3Vec3 in single precision and a double-precision triple when
// Box3D is built for large worlds. Assigning component-wise works for both.
inline b3Pos ToB3Pos(const Vec3& v)
{
    b3Pos p;
    p.x = v.X;
    p.y = v.Y;
    p.z = v.Z;
    return p;
}

inline Vec3 FromB3Pos(const b3Pos& p)
{
    return Vec3{ static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) };
}

// b3Quat stores a vector part and a scalar part.
inline b3Quat ToB3(const Quat& q)
{
    b3Quat r;
    r.v = b3Vec3{ q.X, q.Y, q.Z };
    r.s = q.W;
    return r;
}

inline Quat FromB3(const b3Quat& q)
{
    return Quat{ q.v.x, q.v.y, q.v.z, q.s };
}

inline b2Vec2 ToB2(const Vec2& v)
{
    return b2Vec2{ v.X, v.Y };
}

inline Vec2 FromB2(const b2Vec2& v)
{
    return Vec2{ v.x, v.y };
}

// --- quaternions -----------------------------------------------------------
//
// Joints are described with world-space axes and converted into the body-local
// frames the solver uses; these helpers do the conversion.

inline Quat QuatMultiply(const Quat& a, const Quat& b)
{
    return Quat{ a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
                 a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
                 a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
                 a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z };
}

inline Quat QuatConjugate(const Quat& q)
{
    return Quat{ -q.X, -q.Y, -q.Z, q.W };
}

inline float DotProduct(const Vec3& a, const Vec3& b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

inline Vec3 CrossProduct(const Vec3& a, const Vec3& b)
{
    return Vec3{ a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X };
}

inline Vec3 NormalizeVector(const Vec3& v)
{
    const float lengthSquared = v.X * v.X + v.Y * v.Y + v.Z * v.Z;
    if (lengthSquared <= 1e-20f)
    {
        return Vec3{ 0.0f, 0.0f, -1.0f };
    }
    const float inverse = 1.0f / std::sqrt(lengthSquared);
    return Vec3{ v.X * inverse, v.Y * inverse, v.Z * inverse };
}

// The shortest rotation taking one direction to another.
inline Quat QuatFromTo(const Vec3& from, const Vec3& to)
{
    const Vec3 f = NormalizeVector(from);
    const Vec3 t = NormalizeVector(to);

    const float dot = f.X * t.X + f.Y * t.Y + f.Z * t.Z;

    if (dot > 0.999999f)
    {
        return Quat{};
    }

    if (dot < -0.999999f)
    {
        // Opposite directions: any perpendicular axis will do, so one is
        // built from whichever cardinal axis is least aligned with f.
        Vec3 axis = CrossProduct(Vec3{ 1.0f, 0.0f, 0.0f }, f);
        if (axis.X * axis.X + axis.Y * axis.Y + axis.Z * axis.Z < 1e-12f)
        {
            axis = CrossProduct(Vec3{ 0.0f, 1.0f, 0.0f }, f);
        }
        axis = NormalizeVector(axis);
        return Quat{ axis.X, axis.Y, axis.Z, 0.0f };
    }

    const Vec3 axis = CrossProduct(f, t);
    const float scale = std::sqrt((1.0f + dot) * 2.0f);
    const float inverse = 1.0f / scale;

    return Quat{ axis.X * inverse, axis.Y * inverse, axis.Z * inverse, scale * 0.5f };
}

inline b3BodyType ToB3BodyType(BodyType type)
{
    switch (type)
    {
        case BodyType::Static:    return b3_staticBody;
        case BodyType::Kinematic: return b3_kinematicBody;
        case BodyType::Dynamic:   break;
    }
    return b3_dynamicBody;
}

inline b2BodyType ToB2BodyType(BodyType type)
{
    switch (type)
    {
        case BodyType::Static:    return b2_staticBody;
        case BodyType::Kinematic: return b2_kinematicBody;
        case BodyType::Dynamic:   break;
    }
    return b2_dynamicBody;
}

// ---------------------------------------------------------------------------
// Colour
//
// Every colour in the API is authored in sRGB, the way a colour picker or an
// image file gives it. Lighting is computed in linear light, so a colour is
// converted on its way to the GPU and nowhere else.
// ---------------------------------------------------------------------------

inline float SrgbToLinear(float value)
{
    return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

inline Color ToLinear(const Color& color)
{
    return Color{ SrgbToLinear(color.R), SrgbToLinear(color.G), SrgbToLinear(color.B), color.A };
}

// ---------------------------------------------------------------------------
// Appearance
//
// Everything about how an actor looks that the renderer reads and the physics
// never does. Shared by both dimensions so the renderer has one path.
// ---------------------------------------------------------------------------

struct Appearance
{
    Color Tint{ 0.82f, 0.62f, 0.32f, 1.0f };
    bool Visible = true;
    MaterialId Material;

    // Overrides the mesh's own texture when valid.
    TextureId Texture;

    // A normal map for a primitive, or in place of a model's own, and how
    // strongly it bends the surface.
    TextureId NormalMap;
    float NormalStrength = 1.0f;

    // Light the surface gives off, linear, added after lighting.
    Color Emission{ 0.0f, 0.0f, 0.0f, 1.0f };
    bool EmissionOverridden = false;

    float Roughness = 0.6f;
    float Metallic = 0.0f;

    // Set once SetRoughness or SetMetallic is called, after which the actor's
    // values win over whatever a model's own materials say.
    bool SurfaceOverridden = false;

    // Drawn at full brightness, ignoring every light: sprites and backdrops.
    bool Unlit = false;

    // Applied to texture coordinates: a scale above 1 tiles the texture, and
    // a scale below 1 with an offset picks one frame out of a sprite sheet.
    Vec2 UVScale{ 1.0f, 1.0f };
    Vec2 UVOffset{ 0.0f, 0.0f };

    // 2D drawing order and mirroring.
    int Layer = 0;
    bool FlipX = false;

    // Per-actor values for the material's Params[] slots. A set bit in the
    // mask means the slot is overridden; the rest read the material's value.
    uint32_t ParamMask = 0;
    float Params[8][4] = {};

    // Reset to a fresh actor's defaults. Used when a slot is reused.
    void Reset(const Color& tint)
    {
        *this = Appearance{};
        Tint = tint;
    }
};

// ---------------------------------------------------------------------------
// Interpolation
// ---------------------------------------------------------------------------

inline float Lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// Normalized linear interpolation with a sign fix so the blend takes the short
// way around. Cheaper than slerp and indistinguishable across one step.
Quat NLerp(const Quat& a, const Quat& b, float t);

Transform3 LerpTransform(const Transform3& a, const Transform3& b, float t);
Transform2 LerpTransform(const Transform2& a, const Transform2& b, float t);

// ---------------------------------------------------------------------------
// Deferred commands
//
// Structural changes are recorded and applied at a sync point so the world
// stays safe to mutate from inside a callback that is iterating it.
// ---------------------------------------------------------------------------

enum class CommandType
{
    Destroy,
    SetBodyType,
    SetParent
};

struct Command
{
    CommandType Type = CommandType::Destroy;
    ActorId Id;
    BodyType NewBodyType = BodyType::Dynamic;

    // For SetParent. An invalid id detaches.
    ActorId Parent;
};

// ---------------------------------------------------------------------------
// Actor storage
// ---------------------------------------------------------------------------

// Which primitive mesh represents an actor. Recorded at creation, because the
// physics backend does not hand back a shape's kind in a form worth querying
// every frame.
enum class ShapeKind
{
    Box,
    Sphere,
    Capsule,
    Model
};

// What a walking capsule needs to remember between one Move and the next.
struct CharacterState
{
    float Radius = 0.4f;
    float Height = 1.8f;
    float StepHeight = 0.35f;

    // The cosine of the steepest ground that still counts as ground, which is
    // what a normal is compared against.
    float SlopeLimit = 0.57f;

    Vec3 Position;
    Vec3 GroundNormal{ 0.0f, 1.0f, 0.0f };
    bool OnGround = false;
};

struct ActorRecord3
{
    uint32_t Generation = 0;
    bool Alive = false;
    b3BodyId Body{};
    Vec3 Scale{ 1.0f, 1.0f, 1.0f };
    Transform3 Previous;
    Transform3 Current;
    std::string Name;

    ShapeKind Shape = ShapeKind::Box;

    // Index into the world's model store; -1 for a generated primitive.
    int ModelIndex = -1;

    // Set while a background load has not landed yet: the actor is drawing a
    // placeholder box, and its collider and bounds are fitted to the model as
    // soon as it arrives.
    bool AwaitingModel = false;
    float ModelScale = 1.0f;
    float ModelDensity = 1.0f;
    float ModelFriction = 0.3f;
    float ModelRestitution = 0.0f;

    // Radius of a sphere enclosing the actor, used to reject it against the
    // view frustum without touching its mesh.
    float BoundingRadius = 1.0f;

    Appearance Look;

    // Which clips this actor is playing and the joint matrices they produce.
    // Empty and free for everything that is not a skinned model, which is
    // almost everything.
    Pose Animation;

    CollisionHandler OnCollided;

    // Sensors take part in overlap tests but never push anything, and start
    // hidden because a trigger volume is not usually meant to be seen.
    bool IsSensor = false;
    TriggerHandler OnEntered;
    TriggerHandler OnExited;

    // A character's capsule is a kinematic body its mover drives, rather than
    // a body the solver moves.
    bool IsCharacter = false;
    CharacterState Character;

    // The hierarchy. A child holds where it sits relative to its parent; the
    // parent holds its children so a subtree can be walked and carried.
    ActorId Parent;
    std::vector<ActorId> Children;
    Transform3 Local;

    // Set once when a dynamic body is parented, so the diagnostic is said
    // once rather than every frame.
    bool WarnedAboutDynamicParent = false;
};

// Box3D hands back a shape id; the actor it belongs to is recovered from the
// body's user data. Only the index is stored, so this works on 32-bit builds
// where a pointer cannot hold an index and a generation together. Zero means
// "no actor", so the stored value is the index plus one.
inline void* PackActorIndex(uint32_t index)
{
    return reinterpret_cast<void*>(static_cast<uintptr_t>(index) + 1);
}

inline bool UnpackActorIndex(void* userData, uint32_t& outIndex)
{
    const uintptr_t value = reinterpret_cast<uintptr_t>(userData);
    if (value == 0)
    {
        return false;
    }
    outIndex = static_cast<uint32_t>(value - 1);
    return true;
}

enum class ShapeKind2
{
    Rectangle,
    Circle,

    // Standing along its own Y axis; Scale is diameter by total height.
    Capsule,

    // A sprite whose collider is absent; its Scale is still its drawn size.
    None
};

// What a 2D walking capsule remembers between one Move and the next.
struct CharacterState2
{
    float Radius = 0.3f;
    float Height = 1.2f;
    float StepHeight = 0.2f;

    // The cosine of the steepest ground that still counts as ground.
    float SlopeLimit = 0.57f;

    Vec2 Position;
    Vec2 GroundNormal{ 0.0f, 1.0f };
    bool OnGround = false;
};

struct ActorRecord2
{
    uint32_t Generation = 0;
    bool Alive = false;
    b2BodyId Body{};
    Vec2 Scale{ 1.0f, 1.0f };
    Transform2 Previous;
    Transform2 Current;
    std::string Name;

    ShapeKind2 Shape = ShapeKind2::Rectangle;

    // A sprite draws as a flat quad showing its texture; anything else draws
    // as an extruded shape so the light can model it.
    bool IsSprite = false;

    // Insertion order, so sprites on one layer keep a stable draw order.
    uint64_t Sequence = 0;

    Appearance Look;

    CollisionHandler2D OnCollided;

    // Sensors take part in overlap tests but never push anything, and start
    // hidden because a trigger volume is not usually meant to be seen.
    bool IsSensor = false;
    TriggerHandler2D OnEntered;
    TriggerHandler2D OnExited;

    // A character's capsule is a kinematic body its mover drives.
    bool IsCharacter = false;
    CharacterState2 Character;
};

struct JointRecord2
{
    uint32_t Generation = 0;
    bool Alive = false;
    b2JointId Joint{};
    JointKind Kind = JointKind::Hinge;
};

struct JointRecord3
{
    uint32_t Generation = 0;
    bool Alive = false;
    b3JointId Joint{};
    JointKind Kind = JointKind::Hinge;
};

class Renderer3D;

struct LightRecord3
{
    uint32_t Generation = 0;
    bool Alive = false;
    PointLightDesc Desc;

    // When valid, Desc.Position is recomputed each frame from this actor's
    // interpolated transform plus Offset in the actor's own space.
    ActorId AttachedTo;
    Vec3 Offset;
};

struct PostProcessEntry
{
    MaterialId Material;
    PassPoint Point = PassPoint::AfterToneMap;
};

struct DebugLine
{
    Vec3 From;
    Vec3 To;
    Color Tint;
};

// What both world kinds hand the renderer beyond their actors.
struct RenderExtras
{
    TextureId Background;
    std::vector<PostProcessEntry> PostProcess;

    // Cleared after every Render, so a line drawn once shows for one frame.
    std::vector<DebugLine> DebugLines;
};

struct World3DState
{
    b3WorldId World{};
    World3DConfig Config;

    Camera3D Camera;
    RenderSettings Render;
    Renderer3D* Renderer = nullptr;
    int RenderWidth = 0;
    int RenderHeight = 0;

    std::vector<ActorRecord3> Actors;

    // Which actors are animating this frame, refilled each update. Kept here
    // so a steady frame allocates nothing: it grows to its capacity once and
    // stays there.
    std::vector<uint32_t> AnimatedActors;
    std::vector<uint32_t> FreeIndices;
    std::vector<Command> Commands;

    size_t LiveCount = 0;
    float Accumulator = 0.0f;
    float Alpha = 0.0f;
    uint64_t StepCount = 0;
    bool PhysicsRunning = true;
    PhysicsMode Mode = PhysicsMode::Automatic;

    std::vector<JointRecord3> Joints;
    std::vector<uint32_t> FreeJoints;
    std::vector<JointId> PendingJointDestroys;
    size_t LiveJoints = 0;

    CollisionHandler OnAnyCollision;
    float CollisionThreshold = 1.0f;

    TriggerHandler OnAnyEnter;
    TriggerHandler OnAnyExit;

    // Owned by the world so meshes live exactly as long as the actors using
    // them, and so a world that never renders still parses models correctly.
    ModelStore* Models = nullptr;

    std::vector<LightRecord3> Lights;
    std::vector<uint32_t> FreeLights;
    size_t LiveLights = 0;

    RenderExtras Extras;
    bool PhysicsDebugDraw = false;

    // Created on first use, so a silent world never starts the audio engine.
    WorldAudio* Audio = nullptr;
};

struct World2DState
{
    b2WorldId World{};
    World2DConfig Config;

    Camera2D Camera;
    RenderSettings Render;
    Renderer3D* Renderer = nullptr;
    int RenderWidth = 0;
    int RenderHeight = 0;

    std::vector<ActorRecord2> Actors;
    std::vector<uint32_t> FreeIndices;
    std::vector<Command> Commands;

    std::vector<JointRecord2> Joints;
    std::vector<uint32_t> FreeJoints;
    std::vector<JointId> PendingJointDestroys;
    size_t LiveJoints = 0;

    size_t LiveCount = 0;
    float Accumulator = 0.0f;
    float Alpha = 0.0f;
    uint64_t StepCount = 0;
    bool PhysicsRunning = true;
    PhysicsMode Mode = PhysicsMode::Automatic;

    CollisionHandler2D OnAnyCollision;
    float CollisionThreshold = 1.0f;

    TriggerHandler2D OnAnyEnter;
    TriggerHandler2D OnAnyExit;

    RenderExtras Extras;
    uint64_t NextSequence = 0;

    WorldAudio* Audio = nullptr;
};

// Which actor a shape belongs to. Queries come back with shapes; everything
// above them speaks in actors.
ActorId ActorIdForShape(World3DState& state, b3ShapeId shapeId);
ActorId ActorIdForShape(World2DState& state, b2ShapeId shapeId);

// Builds a handle to an actor without going through a World3D, so a character
// can return the capsule the world sees.
Actor3D MakeActor(World3DState* state, const ActorId& id);

// Moves a character's capsule to where its mover ended up.
void SyncCharacterBody(ActorRecord3& record, const Vec3& position);

// Carries every child to where its parent has gone. Runs at the sync points,
// after physics has had its say.
void PropagateHierarchy(World3DState& state);

// Attaches or detaches one actor, at the sync point.
void ApplyParent(World3DState& state, const ActorId& childId, const ActorId& parentId);

// Queues an actor and everything below it for destruction, children first.
void QueueSubtreeDestroy(World3DState& state, const ActorId& id, std::vector<Command>& commands);

// Unhooks a dying actor from its parent and children.
void DetachFromHierarchy(World3DState& state, const ActorId& id, ActorRecord3& record);

// The file a loaded model came from, so a saved world can name it again.
std::string ModelPathAt(const World3DState& state, int modelIndex);

// Returns nullptr and reports a diagnostic when the handle is stale, which is
// how "using a destroyed handle is a no-op, not a crash" is implemented.
ActorRecord3* Resolve(World3DState* state, const ActorId& id, const char* operation);
JointRecord3* Resolve(World3DState* state, const JointId& id, const char* operation);
LightRecord3* Resolve(World3DState* state, const LightId& id, const char* operation);
ActorRecord2* Resolve(World2DState* state, const ActorId& id, const char* operation);
JointRecord2* Resolve(World2DState* state, const JointId& id, const char* operation);

// Stores a per-actor override for one of its material's uniforms, reporting
// why when the actor has no material or the name is not one of its uniforms.
void SetActorUniform(Appearance& look, const std::string& name, const Color& value, bool isColor);

// Queues a line for the next frame. Capped, so a program that draws lines but
// never renders does not accumulate them without bound.
void PushDebugLine(RenderExtras& extras, const Vec3& from, const Vec3& to, const Color& color);

void RemovePostProcessEntries(RenderExtras& extras, const MaterialId& material);

} // namespace ludifex::detail
