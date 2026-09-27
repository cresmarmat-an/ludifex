#include "Animation.h"

#include <algorithm>
#include <cmath>

namespace ludifex::detail
{
namespace
{

Quat Normalized(Quat q)
{
    const float length = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
    if (length <= 0.0f)
    {
        return Quat{};
    }
    const float inverse = 1.0f / length;
    return Quat{ q.X * inverse, q.Y * inverse, q.Z * inverse, q.W * inverse };
}

// Shortest-arc spherical interpolation. The sign flip is what keeps a blend
// from taking the long way round when two clips happen to store the same
// rotation with opposite signs, which is legal and common in exported files.
Quat Slerp(Quat a, Quat b, float t)
{
    float dot = a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    if (dot < 0.0f)
    {
        b = Quat{ -b.X, -b.Y, -b.Z, -b.W };
        dot = -dot;
    }

    // Close enough that the sines underflow: straight lerp, then renormalize.
    if (dot > 0.9995f)
    {
        return Normalized(Quat{ a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t,
                                a.W + (b.W - a.W) * t });
    }

    const float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sine = std::sin(angle);
    const float first = std::sin((1.0f - t) * angle) / sine;
    const float second = std::sin(t * angle) / sine;

    return Quat{ a.X * first + b.X * second, a.Y * first + b.Y * second, a.Z * first + b.Z * second,
                 a.W * first + b.W * second };
}

Vec3 Lerp(Vec3 a, Vec3 b, float t)
{
    return Vec3{ a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, a.Z + (b.Z - a.Z) * t };
}

// The two keys a time falls between, and how far along it is. Times are
// sorted, so this is a binary search.
struct KeyPair
{
    size_t First = 0;
    size_t Second = 0;
    float Fraction = 0.0f;
};

KeyPair FindKeys(const std::vector<float>& times, float time)
{
    KeyPair pair;
    if (times.size() < 2)
    {
        return pair;
    }

    if (time <= times.front())
    {
        return pair;
    }
    if (time >= times.back())
    {
        pair.First = times.size() - 1;
        pair.Second = times.size() - 1;
        return pair;
    }

    const auto upper = std::upper_bound(times.begin(), times.end(), time);
    const size_t second = static_cast<size_t>(upper - times.begin());
    const size_t first = second - 1;

    const float span = times[second] - times[first];
    pair.First = first;
    pair.Second = second;
    pair.Fraction = span > 0.0f ? (time - times[first]) / span : 0.0f;
    return pair;
}

// glTF's cubic spline stores three values per key (in-tangent, value,
// out-tangent), and the Hermite basis below is the one the specification
// gives. Tangents are scaled by the span because they are expressed per
// second rather than per key.
float CubicSample(float previousValue, float outTangent, float nextValue, float inTangent, float t,
                  float span)
{
    const float t2 = t * t;
    const float t3 = t2 * t;
    return (2.0f * t3 - 3.0f * t2 + 1.0f) * previousValue + span * (t3 - 2.0f * t2 + t) * outTangent +
           (-2.0f * t3 + 3.0f * t2) * nextValue + span * (t3 - t2) * inTangent;
}

// A weights curve's values at a time, written into out, one per morph
// target the curve drives. Returns false, leaving out alone, for a curve
// whose keys do not hold as many values as they should.
bool SampleWeights(const AnimationChannel& channel, float time, float* out)
{
    const int stride = channel.MorphCount;
    if (channel.Times.empty() || stride <= 0)
    {
        return false;
    }

    const bool cubic = channel.Interpolation == AnimationInterpolation::CubicSpline;
    const size_t perKey = static_cast<size_t>(cubic ? stride * 3 : stride);
    if (channel.Values.size() < channel.Times.size() * perKey)
    {
        return false;
    }

    const KeyPair keys = FindKeys(channel.Times, time);
    const size_t valueOffset = cubic ? static_cast<size_t>(stride) : 0;

    for (int component = 0; component < stride; ++component)
    {
        const size_t c = static_cast<size_t>(component);
        const float first = channel.Values[keys.First * perKey + valueOffset + c];
        if (channel.Interpolation == AnimationInterpolation::Step || keys.First == keys.Second)
        {
            out[component] = first;
            continue;
        }

        const float second = channel.Values[keys.Second * perKey + valueOffset + c];
        if (cubic)
        {
            const float span = channel.Times[keys.Second] - channel.Times[keys.First];
            const float outTangent = channel.Values[keys.First * perKey + static_cast<size_t>(stride) * 2 + c];
            const float inTangent = channel.Values[keys.Second * perKey + c];
            out[component] = CubicSample(first, outTangent, second, inTangent, keys.Fraction, span);
        }
        else
        {
            out[component] = first + (second - first) * keys.Fraction;
        }
    }
    return true;
}

void SampleChannel(const AnimationChannel& channel, float time, Transform3& target)
{
    if (channel.Times.empty() || channel.Path == AnimationPath::Weights)
    {
        return;
    }

    const int stride = channel.Path == AnimationPath::Rotation ? 4 : 3;
    const int perKey = channel.Interpolation == AnimationInterpolation::CubicSpline ? stride * 3 : stride;

    if (channel.Values.size() < channel.Times.size() * static_cast<size_t>(perKey))
    {
        return;
    }

    const KeyPair keys = FindKeys(channel.Times, time);

    auto ValueAt = [&](size_t key, int component) {
        const size_t base = key * static_cast<size_t>(perKey);
        // Cubic keys are laid out in-tangent, value, out-tangent, so the value
        // itself sits one stride in.
        const size_t offset = channel.Interpolation == AnimationInterpolation::CubicSpline
                                  ? static_cast<size_t>(stride)
                                  : 0;
        return channel.Values[base + offset + static_cast<size_t>(component)];
    };

    auto TangentAt = [&](size_t key, int component, bool out) {
        const size_t base = key * static_cast<size_t>(perKey);
        const size_t offset = out ? static_cast<size_t>(stride) * 2 : 0;
        return channel.Values[base + offset + static_cast<size_t>(component)];
    };

    float sampled[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

    if (channel.Interpolation == AnimationInterpolation::Step || keys.First == keys.Second)
    {
        for (int component = 0; component < stride; ++component)
        {
            sampled[component] = ValueAt(keys.First, component);
        }
    }
    else if (channel.Interpolation == AnimationInterpolation::CubicSpline)
    {
        const float span = channel.Times[keys.Second] - channel.Times[keys.First];
        for (int component = 0; component < stride; ++component)
        {
            sampled[component] =
                CubicSample(ValueAt(keys.First, component), TangentAt(keys.First, component, true),
                            ValueAt(keys.Second, component), TangentAt(keys.Second, component, false),
                            keys.Fraction, span);
        }
    }
    else
    {
        for (int component = 0; component < stride; ++component)
        {
            const float a = ValueAt(keys.First, component);
            const float b = ValueAt(keys.Second, component);
            sampled[component] = a + (b - a) * keys.Fraction;
        }
    }

    switch (channel.Path)
    {
    case AnimationPath::Translation:
        target.Position = Vec3{ sampled[0], sampled[1], sampled[2] };
        break;
    case AnimationPath::Scale:
        target.Scale = Vec3{ sampled[0], sampled[1], sampled[2] };
        break;
    case AnimationPath::Weights:
        break;
    case AnimationPath::Rotation:
        // Rotation is the one property a straight lerp gets wrong, so linear
        // keys are slerped between rather than mixed component by component.
        if (channel.Interpolation == AnimationInterpolation::Linear && keys.First != keys.Second)
        {
            const Quat a{ ValueAt(keys.First, 0), ValueAt(keys.First, 1), ValueAt(keys.First, 2),
                          ValueAt(keys.First, 3) };
            const Quat b{ ValueAt(keys.Second, 0), ValueAt(keys.Second, 1), ValueAt(keys.Second, 2),
                          ValueAt(keys.Second, 3) };
            target.Rotation = Slerp(Normalized(a), Normalized(b), keys.Fraction);
        }
        else
        {
            target.Rotation = Normalized(Quat{ sampled[0], sampled[1], sampled[2], sampled[3] });
        }
        break;
    }
}

} // namespace

bool AdvancePose(Pose& pose, const std::vector<ModelAnimation>& animations, float deltaSeconds)
{
    if (pose.Layers.empty())
    {
        return false;
    }

    bool moved = false;

    for (PoseLayer& layer : pose.Layers)
    {
        if (layer.Animation < 0 || layer.Animation >= static_cast<int>(animations.size()))
        {
            layer.Target = 0.0f;
            layer.Weight = 0.0f;
            continue;
        }

        const ModelAnimation& clip = animations[static_cast<size_t>(layer.Animation)];

        // The weight first, so a layer that reaches zero this frame is not
        // also sampled this frame.
        if (layer.Weight != layer.Target)
        {
            if (layer.Rate <= 0.0f)
            {
                layer.Weight = layer.Target;
            }
            else
            {
                const float step = layer.Rate * deltaSeconds;
                layer.Weight = layer.Weight < layer.Target
                                   ? std::min(layer.Target, layer.Weight + step)
                                   : std::max(layer.Target, layer.Weight - step);
            }
            moved = true;
        }

        const float advance = layer.Speed * deltaSeconds;
        if (advance != 0.0f && clip.Duration > 0.0f)
        {
            layer.Time += advance;

            if (layer.Loop)
            {
                // Wrapping rather than clamping, and by remainder rather than
                // by subtraction, so a clip played at ten times speed does not
                // need ten passes to come back into range.
                layer.Time = std::fmod(layer.Time, clip.Duration);
                if (layer.Time < 0.0f)
                {
                    layer.Time += clip.Duration;
                }
            }
            else if (layer.Time >= clip.Duration)
            {
                layer.Time = clip.Duration;
                layer.Finished = true;
            }
            else if (layer.Time <= 0.0f)
            {
                layer.Time = 0.0f;
                layer.Finished = true;
            }

            moved = true;
        }
    }

    // A layer that has faded to nothing is gone; keeping it would cost a
    // sample a frame for a contribution of zero.
    const size_t before = pose.Layers.size();
    pose.Layers.erase(std::remove_if(pose.Layers.begin(), pose.Layers.end(),
                                     [](const PoseLayer& layer) {
                                         return layer.Target <= 0.0f && layer.Weight <= 0.0f;
                                     }),
                      pose.Layers.end());

    if (pose.Layers.size() != before)
    {
        moved = true;
    }

    pose.Dirty = pose.Dirty || moved;
    return moved;
}

namespace
{

void BuildSkeleton(Pose& pose, const std::vector<ModelJoint>& joints, const std::vector<SkinJoint>& skin,
                   const std::vector<ModelAnimation>& animations, float inverseTotal)
{
    // Rest first. A clip that drives six joints of a thirty-joint skeleton
    // leaves the other twenty-four exactly where the model was authored.
    std::vector<Transform3> local(joints.size());
    for (size_t index = 0; index < joints.size(); ++index)
    {
        local[index] = joints[index].Rest;
    }

    if (inverseTotal > 0.0f)
    {
        std::vector<Transform3> blended(joints.size());
        std::vector<float> applied(joints.size(), 0.0f);

        bool first = true;
        for (const PoseLayer& layer : pose.Layers)
        {
            const float weight = std::max(0.0f, layer.Weight) * inverseTotal;
            if (weight <= 0.0f || layer.Animation < 0 || layer.Animation >= static_cast<int>(animations.size()))
            {
                continue;
            }

            std::vector<Transform3> sampled = local;
            for (const AnimationChannel& channel : animations[static_cast<size_t>(layer.Animation)].Channels)
            {
                if (channel.Joint >= 0 && channel.Joint < static_cast<int>(joints.size()))
                {
                    SampleChannel(channel, layer.Time, sampled[static_cast<size_t>(channel.Joint)]);
                }
            }

            for (size_t index = 0; index < joints.size(); ++index)
            {
                if (first)
                {
                    blended[index] = sampled[index];
                    applied[index] = weight;
                }
                else
                {
                    // Accumulated pairwise rather than summed: mixing the
                    // running result with the next layer at its share of the
                    // weight so far keeps rotations on the sphere, which a
                    // weighted sum of quaternions does not.
                    const float share = applied[index] + weight;
                    const float t = share > 0.0f ? weight / share : 0.0f;

                    blended[index].Position = Lerp(blended[index].Position, sampled[index].Position, t);
                    blended[index].Scale = Lerp(blended[index].Scale, sampled[index].Scale, t);
                    blended[index].Rotation = Slerp(blended[index].Rotation, sampled[index].Rotation, t);
                    applied[index] = share;
                }
            }

            first = false;
        }

        if (!first)
        {
            local = blended;
        }
    }

    // Parents come before children, so one forward pass composes the whole
    // skeleton.
    pose.Model.resize(joints.size());
    for (size_t index = 0; index < joints.size(); ++index)
    {
        const Mat4 self = Mat4::FromTransform(local[index]);
        const int parent = joints[index].Parent;
        pose.Model[index] = parent >= 0 ? pose.Model[static_cast<size_t>(parent)] * self : self;
    }

    // One matrix a palette entry: the bone where it has gone, after the
    // binding of the skin the entry belongs to.
    pose.Palette.resize(skin.size());
    for (size_t index = 0; index < skin.size(); ++index)
    {
        const int bone = skin[index].Bone;
        pose.Palette[index] = bone >= 0 && bone < static_cast<int>(pose.Model.size())
                                  ? pose.Model[static_cast<size_t>(bone)] * skin[index].InverseBind
                                  : Mat4::Identity();
    }
}

void BuildMorphWeights(Pose& pose, const std::vector<MorphTarget>& morphs,
                       const std::vector<ModelAnimation>& animations, float inverseTotal)
{
    EnsureMorphWeights(pose, morphs);
    pose.MorphWeights = pose.MorphBase;

    if (inverseTotal <= 0.0f)
    {
        return;
    }

    // Each layer contributes what its clip says for the weights it drives and
    // the actor's own weight for the ones it does not, in proportion to its
    // share, so a clip that animates a blink leaves a smile set by hand
    // alone.
    const size_t count = morphs.size();
    std::vector<float> blended(count, 0.0f);
    std::vector<float> sampled(count, 0.0f);
    bool any = false;

    for (const PoseLayer& layer : pose.Layers)
    {
        const float weight = std::max(0.0f, layer.Weight) * inverseTotal;
        if (weight <= 0.0f || layer.Animation < 0 || layer.Animation >= static_cast<int>(animations.size()))
        {
            continue;
        }

        sampled = pose.MorphBase;
        for (const AnimationChannel& channel : animations[static_cast<size_t>(layer.Animation)].Channels)
        {
            if (channel.Path == AnimationPath::Weights && channel.FirstMorph >= 0 &&
                channel.FirstMorph + channel.MorphCount <= static_cast<int>(count))
            {
                SampleWeights(channel, layer.Time, sampled.data() + channel.FirstMorph);
            }
        }

        for (size_t index = 0; index < count; ++index)
        {
            blended[index] += sampled[index] * weight;
        }
        any = true;
    }

    if (any)
    {
        pose.MorphWeights = blended;
    }
}

} // namespace

void EnsureMorphWeights(Pose& pose, const std::vector<MorphTarget>& morphs)
{
    if (pose.MorphBase.size() == morphs.size())
    {
        return;
    }

    pose.MorphBase.resize(morphs.size());
    for (size_t index = 0; index < morphs.size(); ++index)
    {
        pose.MorphBase[index] = morphs[index].DefaultWeight;
    }
    pose.MorphWeights = pose.MorphBase;
}

void BuildPose(Pose& pose, const std::vector<ModelJoint>& bones, const std::vector<SkinJoint>& skin,
               const std::vector<MorphTarget>& morphs, const std::vector<ModelAnimation>& animations)
{
    if (!pose.Dirty)
    {
        return;
    }
    pose.Dirty = false;

    float totalWeight = 0.0f;
    for (const PoseLayer& layer : pose.Layers)
    {
        totalWeight += std::max(0.0f, layer.Weight);
    }

    // Normalized, so a crossfade halfway through (both layers at 0.5, or both
    // at 1.0) lands on the pose between them instead of a scaled-down or
    // doubled one.
    const float inverseTotal = totalWeight > 0.0f ? 1.0f / totalWeight : 0.0f;

    if (!bones.empty())
    {
        BuildSkeleton(pose, bones, skin, animations, inverseTotal);
    }
    if (!morphs.empty())
    {
        BuildMorphWeights(pose, morphs, animations, inverseTotal);
    }
}

Transform3 DecomposeMatrix(const Mat4& matrix)
{
    const float* m = matrix.M;

    Transform3 transform;
    transform.Position = Vec3{ m[12], m[13], m[14] };

    // Column lengths are the scale; dividing them out leaves a rotation.
    float columns[3][3];
    float scale[3];
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            columns[column][row] = m[column * 4 + row];
        }
        scale[column] =
            std::sqrt(columns[column][0] * columns[column][0] + columns[column][1] * columns[column][1] +
                      columns[column][2] * columns[column][2]);
        if (scale[column] > 1e-20f)
        {
            columns[column][0] /= scale[column];
            columns[column][1] /= scale[column];
            columns[column][2] /= scale[column];
        }
    }
    transform.Scale = Vec3{ scale[0], scale[1], scale[2] };

    const float trace = columns[0][0] + columns[1][1] + columns[2][2];
    Quat rotation;
    if (trace > 0.0f)
    {
        const float root = std::sqrt(trace + 1.0f) * 2.0f;
        rotation.W = 0.25f * root;
        rotation.X = (columns[1][2] - columns[2][1]) / root;
        rotation.Y = (columns[2][0] - columns[0][2]) / root;
        rotation.Z = (columns[0][1] - columns[1][0]) / root;
    }
    else if (columns[0][0] > columns[1][1] && columns[0][0] > columns[2][2])
    {
        const float root = std::sqrt(1.0f + columns[0][0] - columns[1][1] - columns[2][2]) * 2.0f;
        rotation.W = (columns[1][2] - columns[2][1]) / root;
        rotation.X = 0.25f * root;
        rotation.Y = (columns[1][0] + columns[0][1]) / root;
        rotation.Z = (columns[2][0] + columns[0][2]) / root;
    }
    else if (columns[1][1] > columns[2][2])
    {
        const float root = std::sqrt(1.0f + columns[1][1] - columns[0][0] - columns[2][2]) * 2.0f;
        rotation.W = (columns[2][0] - columns[0][2]) / root;
        rotation.X = (columns[1][0] + columns[0][1]) / root;
        rotation.Y = 0.25f * root;
        rotation.Z = (columns[2][1] + columns[1][2]) / root;
    }
    else
    {
        const float root = std::sqrt(1.0f + columns[2][2] - columns[0][0] - columns[1][1]) * 2.0f;
        rotation.W = (columns[0][1] - columns[1][0]) / root;
        rotation.X = (columns[2][0] + columns[0][2]) / root;
        rotation.Y = (columns[2][1] + columns[1][2]) / root;
        rotation.Z = 0.25f * root;
    }

    transform.Rotation = Normalized(rotation);
    return transform;
}

void StartLayer(Pose& pose, int animation, bool loop, float speed, float startTime, float fadeSeconds,
                float weight)
{
    const float rate = fadeSeconds > 0.0f ? 1.0f / fadeSeconds : 0.0f;

    // Anything already playing fades out over the same time, which makes this
    // a crossfade.
    for (PoseLayer& layer : pose.Layers)
    {
        layer.Target = 0.0f;
        layer.Rate = rate;
    }

    PoseLayer layer;
    layer.Animation = animation;
    layer.Loop = loop;
    layer.Speed = speed;
    layer.Time = startTime;
    layer.Target = std::max(0.0f, weight);
    layer.Rate = rate;
    layer.Weight = rate > 0.0f ? 0.0f : layer.Target;

    // Above the limit the quietest layer goes, because it is the one
    // contributing least to what is on screen.
    if (pose.Layers.size() >= static_cast<size_t>(MaximumPoseLayers))
    {
        auto quietest = std::min_element(pose.Layers.begin(), pose.Layers.end(),
                                         [](const PoseLayer& a, const PoseLayer& b) {
                                             return a.Weight < b.Weight;
                                         });
        pose.Layers.erase(quietest);
    }

    pose.Layers.push_back(layer);
    pose.Dirty = true;
}

void StopLayers(Pose& pose, float fadeSeconds)
{
    const float rate = fadeSeconds > 0.0f ? 1.0f / fadeSeconds : 0.0f;
    for (PoseLayer& layer : pose.Layers)
    {
        layer.Target = 0.0f;
        layer.Rate = rate;
        if (rate <= 0.0f)
        {
            layer.Weight = 0.0f;
        }
    }
    pose.Dirty = true;
}

} // namespace ludifex::detail
