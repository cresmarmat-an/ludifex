// ludifex's world shader.
//
// One pipeline draws every primitive the world holds: boxes, spheres, capsules,
// loaded models, sprites, and the ground. Per-object data lives in a storage
// buffer indexed by instance, so a thousand actors sharing a mesh are one draw
// call.
//
// The fragment stage below is what a custom world material replaces, and it
// uses the same world_material.hlsli that custom materials use.
//
// Lighting is forward, not deferred, which keeps multisampling affordable, so
// the default anti-aliasing can be real multisampling.
//
// The file also holds the engine-only stages: the shadow map, the depth and
// motion prepass used by temporal anti-aliasing and depth-reading effects, and
// debug lines.

#define LUDIFEX_DEFAULT_SHADER
#include "world_material.hlsli"

struct VertexInput
{
    float3 Position : TEXCOORD0;
    float3 Normal   : TEXCOORD1;
    float2 UV       : TEXCOORD2;

    // Every vertex carries these, and they are zero for everything that does
    // not bend. Static meshes pay eight bytes a vertex so that skinned and
    // static meshes share one pipeline, and a custom material only has to be
    // compiled once.
    uint4  Joints   : TEXCOORD3;
    float4 Weights  : TEXCOORD4;
};

// Per draw. InstanceOffset exists because SV_InstanceID restarts at zero for
// every draw call, so the base index into the shared buffer has to come from
// somewhere else. Jitter is the sub-pixel offset temporal anti-aliasing moves
// the image by each frame, in clip-space units; it is zero otherwise.
cbuffer DrawUniforms : register(b0, space1)
{
    float4x4 ViewProjection;
    float4x4 PreviousViewProjection;
    float4   Jitter;
    uint     InstanceOffset;
    uint3    DrawPadding;
};

// The same objects the fragment stage reads as Instances, seen from the
// vertex stage's register space.
// On Metal these follow DrawUniforms, the stage's one uniform block.
LUDIFEX_STORAGE(1) StructuredBuffer<InstanceData> VertexInstances : register(t0, space0);

// Every skinned actor's joint matrices, one after another, for this frame.
// InstanceData.Extra.w says where each actor's own block starts, so a hundred
// characters in different poses are still one buffer and one draw.
LUDIFEX_STORAGE(2) StructuredBuffer<float4x4> JointMatrices : register(t1, space0);

// Every morphing actor's targets in use this frame, one run after another:
// x where the target's displacements start, y the first vertex it moves, z
// how many, w the weight's bits. InstanceData.Morph says where an actor's run
// starts and how long it is.
LUDIFEX_STORAGE(3) StructuredBuffer<uint4> MorphChannels : register(t2, space0);

// The mesh's displacements, two a vertex a target: position, then normal.
// Bound per mesh; a mesh without morph targets has a placeholder here that
// nothing reads.
LUDIFEX_STORAGE(4) StructuredBuffer<float4> MorphDeltas : register(t3, space0);

// Moves a vertex toward each blend shape the instance has in use, by that
// shape's weight. glTF applies morph targets before skinning, and so does
// this. The loop runs over the actor's targets in use, not all of them, and
// every lane of a draw runs it the same number of times.
void Morph(InstanceData instance, uint vertexId, inout float3 position, inout float3 normal)
{
    uint first = instance.Morph.x;
    uint count = instance.Morph.y;

    for (uint index = 0; index < count; ++index)
    {
        uint4 channel = MorphChannels[first + index];
        uint local = vertexId - channel.y;

        // Unsigned, so a vertex before the target's run wraps past its end.
        if (local < channel.z)
        {
            float weight = asfloat(channel.w);
            uint at = channel.x + local * 2;
            position += MorphDeltas[at].xyz * weight;
            normal += MorphDeltas[at + 1].xyz * weight;
        }
    }
}

// The skinned position and normal, or the ones that came in, for an instance
// that does not bend.
//
// Every lane of a draw takes the same branch, because the flag comes from the
// instance rather than the vertex, so this costs a scalar test rather than
// divergence. The weights are a normalized byte apiece and sum to one, so no
// renormalizing is needed here.
void Skin(InstanceData instance, VertexInput input, uint vertexId, out float3 position, out float3 normal)
{
    position = input.Position;
    normal = input.Normal;

    Morph(instance, vertexId, position, normal);

    if ((instance.Extra.z & InstanceFlagSkinned) == 0)
    {
        return;
    }

    // A part of a skinned model that no bone moves (a prop placed through the
    // node hierarchy instead of bound) has no weights, and stays where the
    // model put it instead of collapsing onto the origin.
    if (dot(input.Weights, float4(1.0, 1.0, 1.0, 1.0)) <= 0.0)
    {
        return;
    }

    uint base = instance.Extra.w;

    float4x4 blend = JointMatrices[base + input.Joints.x] * input.Weights.x;
    blend += JointMatrices[base + input.Joints.y] * input.Weights.y;
    blend += JointMatrices[base + input.Joints.z] * input.Weights.z;
    blend += JointMatrices[base + input.Joints.w] * input.Weights.w;

    // The upper 3x3 carries the normal. Joint matrices are rigid apart from
    // whatever scale the skeleton itself has, so this holds without the
    // inverse transpose, exactly as the model matrix does.
    normal = mul((float3x3)blend, normal);
    position = mul(blend, float4(position, 1.0)).xyz;
}

float4 Jittered(float4 clip)
{
    clip.xy += Jitter.xy * clip.w;
    return clip;
}

// --- the surface pass --------------------------------------------------------

SurfaceInput VertexMain(VertexInput input, uint instanceId : SV_InstanceID, uint vertexId : SV_VertexID)
{
    uint index = instanceId + InstanceOffset;
    InstanceData instance = VertexInstances[index];

    SurfaceInput output;

    float3 localPosition;
    float3 localNormal;
    Skin(instance, input, vertexId, localPosition, localNormal);

    float4 worldPosition = mul(instance.Model, float4(localPosition, 1.0));

    output.Position = Jittered(mul(ViewProjection, worldPosition));
    output.WorldPosition = worldPosition.xyz;

    // Every primitive the world creates uses uniform or axis-aligned scale, and
    // models have their node transforms baked in, so the model matrix can carry
    // normals directly without its inverse transpose.
    output.WorldNormal = normalize(mul((float3x3)instance.Model, localNormal));
    output.Color = instance.BaseColor;
    output.LocalPosition = localPosition;

    float2 uv = input.UV;
    if ((instance.Extra.z & InstanceFlagFlipX) != 0)
    {
        uv.x = 1.0 - uv.x;
    }
    output.UV = uv * instance.UVTransform.xy + instance.UVTransform.zw;
    output.Instance = index;

    return output;
}

float4 FragmentMain(SurfaceInput input) : SV_Target
{
    float4 base = BaseColor(input);
    return float4(Shade(input, base.rgb), base.a);
}

// --- the shadow map ----------------------------------------------------------

struct DepthOnlyOutput
{
    float4 Position : SV_Position;
};

DepthOnlyOutput ShadowVertexMain(VertexInput input, uint instanceId : SV_InstanceID, uint vertexId : SV_VertexID)
{
    InstanceData instance = VertexInstances[instanceId + InstanceOffset];

    // Skinned and morphed here too, or a running character would cast the
    // shadow of a figure standing still.
    float3 localPosition;
    float3 localNormal;
    Skin(instance, input, vertexId, localPosition, localNormal);

    DepthOnlyOutput output;
    output.Position = mul(ViewProjection, mul(instance.Model, float4(localPosition, 1.0)));
    return output;
}

void ShadowFragmentMain(DepthOnlyOutput input)
{
}

// --- depth, motion, and normals ------------------------------------------------
//
// Everything the frame's screen-space passes read about the opaque surfaces:
// their depth, how far each point moved on screen since the last frame, and
// which way it faces and how rough it is. Used by temporal anti-aliasing,
// ambient occlusion, ray tracing, and post-process materials that read depth.

struct MotionOutput
{
    float4 Position  : SV_Position;
    float4 Current   : TEXCOORD0;
    float4 Previous  : TEXCOORD1;
    float3 Normal    : TEXCOORD2;
    nointerpolation float Roughness : TEXCOORD3;
};

struct MotionTargets
{
    float2 Motion : SV_Target0;
    float4 Normal : SV_Target1; // xyz the world normal, w roughness
};

MotionOutput MotionVertexMain(VertexInput input, uint instanceId : SV_InstanceID, uint vertexId : SV_VertexID)
{
    InstanceData instance = VertexInstances[instanceId + InstanceOffset];

    float3 localPosition;
    float3 localNormal;
    Skin(instance, input, vertexId, localPosition, localNormal);

    // The pose is this frame's for both, so a limb's own motion is not in the
    // vector, only the actor's. Including it would mean keeping the previous
    // frame's joint palette too, which is not worth it for limbs that move a
    // few pixels.
    float4 position = float4(localPosition, 1.0);
    float4 current = mul(ViewProjection, mul(instance.Model, position));
    float4 previous = mul(PreviousViewProjection, mul(instance.PreviousModel, position));

    MotionOutput output;
    output.Position = Jittered(current);
    output.Current = current;
    output.Previous = previous;
    output.Normal = mul((float3x3)instance.Model, localNormal);
    output.Roughness = instance.Surface.x;
    return output;
}

// How far this point moved on screen since the previous frame, in texture
// coordinates (unjittered, so a still scene reports exactly zero), and the
// surface's normal and roughness.
MotionTargets MotionFragmentMain(MotionOutput input)
{
    float2 current = input.Current.xy / input.Current.w;
    float2 previous = input.Previous.xy / input.Previous.w;

    MotionTargets output;
    output.Motion = (current - previous) * float2(0.5, -0.5);
    output.Normal = float4(normalize(input.Normal), input.Roughness);
    return output;
}

// --- debug lines -------------------------------------------------------------

struct LineInput
{
    float3 Position : TEXCOORD0;
    float4 Color    : TEXCOORD1;
};

struct LineOutput
{
    float4 Position : SV_Position;
    float4 Color    : TEXCOORD0;
};

LineOutput LineVertexMain(LineInput input)
{
    LineOutput output;
    output.Position = Jittered(mul(ViewProjection, float4(input.Position, 1.0)));
    output.Color = input.Color;
    return output;
}

float4 LineFragmentMain(LineOutput input) : SV_Target
{
    return input.Color;
}
