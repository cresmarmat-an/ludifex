// Skeletons, animation clips, and the blending between them.
// Not installed and not part of the public API.
//
// A skeleton is parsed alongside the geometry and lives in the ModelMesh,
// because it belongs to the model, not to any one actor: every actor loaded
// from the file shares one skeleton and one set of clips. What each actor
// carries is a Pose (which clips it is playing, how far into them, and how
// much of each to mix), defined here.
//
// An actor holds several clips so it can blend between them: a new clip
// fades in over what is playing, and anything already playing fades out.
// Weights are normalized when sampled, so a crossfade never scales down or
// doubles the pose.

#pragma once

// The public header rather than Internal.h, which includes this one: the
// skeleton needs Mat4 and Transform3 and nothing else the library holds.
#include <ludifex/ludifex.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ludifex::detail
{

// One bone. Parent is an index into the same array and always lower than this
// joint's own, so composing world matrices is a single forward pass. The
// parser sorts them this way once.
struct ModelJoint
{
    int Parent = -1;

    // The joint's own transform when nothing is playing. A clip that animates
    // rotation and nothing else leaves translation and scale at these.
    Transform3 Rest;

    std::string Name;
};

// One entry of the palette a skinned vertex indexes: a bone, and the matrix
// that takes a vertex from its skin's bind pose into that bone's space.
//
// A model with one skin has an entry per bone. A model whose meshes use
// several skins (a body and a separately exported head over one skeleton, or
// two rigs in one file) has an entry per joint of each skin, because two
// skins may bind the same bone differently, and a vertex has to be carried by
// the binding its own skin was made with.
struct SkinJoint
{
    int Bone = 0;
    Mat4 InverseBind;
};

// One blend shape: how far each vertex of one mesh in the model moves when
// the shape is at full weight. glTF calls these morph targets.
//
// The model's vertices are one array, and a shape moves a contiguous run of
// them (the mesh it belongs to), so it records that run and where its
// displacements start rather than storing a zero for every other vertex.
struct MorphTarget
{
    std::string Name;
    float DefaultWeight = 0.0f;

    uint32_t VertexStart = 0;
    uint32_t VertexCount = 0;

    // Into the model's displacement array, counted in float4s. Two a vertex:
    // the position's displacement, then the normal's.
    uint32_t DeltaOffset = 0;
};

// How a sampler gets from one key to the next.
enum class AnimationInterpolation
{
    Linear,
    Step,
    CubicSpline,
};

// What a channel drives.
enum class AnimationPath
{
    Translation,
    Rotation,
    Scale,

    // The weights of a mesh's morph targets, all of them in one curve.
    Weights,
};

// One curve: one joint, one property, one list of keys. glTF splits an
// animation this way and so does this, because a clip that rotates an elbow
// and nothing else should cost one curve rather than a full pose per key.
struct AnimationChannel
{
    int Joint = -1;

    // For a weights curve, the model's first morph target it drives and how
    // many; each key then holds that many values.
    int FirstMorph = -1;
    int MorphCount = 0;

    AnimationPath Path = AnimationPath::Translation;
    AnimationInterpolation Interpolation = AnimationInterpolation::Linear;

    std::vector<float> Times;

    // Three floats a key for translation and scale, four for rotation, and
    // three times that for cubic spline, which stores in-tangent, value, and
    // out-tangent together.
    std::vector<float> Values;
};

struct ModelAnimation
{
    std::string Name;
    float Duration = 0.0f;
    std::vector<AnimationChannel> Channels;
};

// One clip an actor is playing, with where it is and how loud.
struct PoseLayer
{
    int Animation = -1;
    float Time = 0.0f;
    float Speed = 1.0f;
    bool Loop = true;

    // Where the weight is now and where it is heading. A layer fading in has
    // Weight below Target; one fading out has Target at zero and is dropped
    // when it gets there.
    float Weight = 0.0f;
    float Target = 1.0f;

    // Weight units a second. Zero means arrive immediately.
    float Rate = 0.0f;

    // True once a non-looping clip has run off its end, so the actor can be
    // asked whether it is still animating.
    bool Finished = false;
};

// At most this many clips mix at once. A fifth request retires the quietest
// layer instead of being refused.
constexpr int MaximumPoseLayers = 4;

// What an actor carries. Empty for everything that is not an animated
// model.
struct Pose
{
    std::vector<PoseLayer> Layers;

    // One matrix a joint, model space, ready for the palette. Rebuilt only on
    // frames where a layer moved.
    std::vector<Mat4> Palette;

    // Where each joint itself ended up, before the inverse bind is folded in.
    // The palette cannot give that (it holds the matrices that move vertices,
    // not joint positions), and attaching something to a joint needs it.
    std::vector<Mat4> Model;

    // Where this actor's joints start in the frame's shared palette buffer,
    // written during rendering.
    uint32_t PaletteOffset = 0;

    // The morph weights the actor was given, and the ones it is drawn with
    // once any clips driving weights are mixed in. Both empty until the
    // actor's model has morph targets and something has asked about them;
    // the model's own defaults stand in until then.
    std::vector<float> MorphBase;
    std::vector<float> MorphWeights;

    bool Dirty = true;
};

// Takes a matrix apart into the three things a transform is made of. Exact
// for the rigid, uniformly scaled matrices a skeleton produces.
Transform3 DecomposeMatrix(const Mat4& matrix);

// Advances every layer by dt, retires the ones that have faded out or run off
// the end, and returns whether anything moved.
bool AdvancePose(Pose& pose, const std::vector<ModelAnimation>& animations, float deltaSeconds);

// Samples every live layer, blends them, and writes joint matrices into
// pose.Palette and morph weights into pose.MorphWeights. Cheap to call when
// nothing is dirty: it returns at once.
void BuildPose(Pose& pose, const std::vector<ModelJoint>& bones, const std::vector<SkinJoint>& skin,
               const std::vector<MorphTarget>& morphs, const std::vector<ModelAnimation>& animations);

// Sizes the pose's morph weights to the model's targets, starting from the
// model's defaults, when they are not that size already.
void EnsureMorphWeights(Pose& pose, const std::vector<MorphTarget>& morphs);

// Starts a clip, fading whatever is playing out over the same time. A fade of
// zero is a cut.
void StartLayer(Pose& pose, int animation, bool loop, float speed, float startTime, float fadeSeconds,
                float weight);

// Fades everything out. A fade of zero stops immediately.
void StopLayers(Pose& pose, float fadeSeconds);

} // namespace ludifex::detail
