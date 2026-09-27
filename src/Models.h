// Model loading: glTF through cgltf, every other format through Assimp.
// Not installed and not part of the public API.
//
// Models are parsed into plain CPU meshes here and uploaded by the renderer on
// first use. Keeping the two apart means a model can be loaded before a GPU
// device exists (in a test, or on a machine with no display), and the same
// cache serves a world that never renders.

#pragma once

#include "Animation.h"
#include "Assets.h"
#include "Internal.h"

#include <memory>
#include <string>
#include <vector>

namespace ludifex::detail
{

// Mirrors VertexInput in world.hlsl. The layouts must agree.
//
// Skinning data is carried by every vertex, including those that never bend,
// and is zero for them. A second vertex layout would need a second pipeline
// for every custom material; this costs eight bytes a vertex and a branch in
// the vertex shader that every lane of a draw takes together.
struct ModelVertex
{
    float Position[3];
    float Normal[3];
    float UV[2];

    // Indices into this model's skin palette. Unused slots hold zero, which is
    // harmless because their weight is zero too.
    uint8_t Joints[4];

    // Normalized to 255, summing to 255 across the four. A vertex with no
    // skinning has all four at zero; the shader tests for that.
    uint8_t Weights[4];
};

// One glTF primitive: a range of the shared index buffer drawn with one
// material. A model with a painted body and glass windows is two parts.
struct ModelPart
{
    uint32_t FirstIndex = 0;
    uint32_t IndexCount = 0;

    // From the glTF material's metallic-roughness block. Colour factors are
    // linear, as glTF specifies.
    TextureId BaseColorTexture;
    Color BaseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
    float Roughness = 0.6f;
    float Metallic = 0.0f;

    // The rest of what a glTF material describes. Roughness is read from the
    // metallic-roughness map's green channel and metalness from its blue, each
    // times its factor; occlusion from the occlusion map's red channel; and
    // emission is the emissive map times the emissive factor, in linear light.
    TextureId MetallicRoughnessTexture;
    TextureId NormalTexture;
    float NormalScale = 1.0f;
    TextureId OcclusionTexture;
    float OcclusionStrength = 1.0f;
    TextureId EmissiveTexture;
    Vec3 EmissiveFactor{ 0.0f, 0.0f, 0.0f };

    // False for a primitive with no material, which then takes the actor's
    // own roughness and metallic instead of glTF's default of bare metal.
    bool HasMaterial = false;

    // glTF's BLEND alpha mode: drawn in the transparent pass.
    bool Transparent = false;
};

struct ModelMesh
{
    std::vector<ModelVertex> Vertices;
    std::vector<uint32_t> Indices;
    std::vector<ModelPart> Parts;

    // The skeleton and the clips that drive it, shared by every actor using
    // this model: a hundred guards are one skeleton and one set of curves.
    // Joints are the bones; SkinJoints is the palette skinned vertices index,
    // one entry per joint of each skin. Empty for a model with no skin, which
    // is most of them.
    std::vector<ModelJoint> Joints;
    std::vector<SkinJoint> SkinJoints;
    std::vector<ModelAnimation> Animations;

    // Blend shapes, and every shape's displacements one after another: two
    // float4s a vertex, position then normal. Empty for a model without them.
    std::vector<MorphTarget> MorphTargets;
    std::vector<float> MorphDeltas;

    // Axis-aligned bounds in model space, with every node transform applied,
    // used to size the collider and to report a sensible scale.
    Vec3 Minimum{ 0.0f, 0.0f, 0.0f };
    Vec3 Maximum{ 0.0f, 0.0f, 0.0f };

    std::string Path;

    // The file this came from, its timestamp, and a hash of its contents. The
    // hash is what lets two names for one file share a parse, and what keeps a
    // re-saved but unedited file from being reloaded.
    FileWatch Watch;

    // False while a background load is still parsing this one. Everything
    // waiting on it draws a placeholder until it turns true.
    bool Ready = true;

    // Bumped when the geometry changes, so a renderer holding GPU buffers
    // knows to build them again.
    uint32_t Revision = 1;

    bool IsSkinned() const { return !SkinJoints.empty(); }
    bool HasMorphs() const { return !MorphTargets.empty(); }

    // Anything a clip or a weight can move.
    bool IsAnimated() const { return IsSkinned() || HasMorphs(); }
};

// Shared by the glTF and Assimp parsers. Matrices are column-major.

// The matrix that carries normals: the inverse transpose of the upper 3x3,
// stored row-major in out.
void NormalMatrix(const float world[16], float out[9]);

// Negative for a transform that mirrors, which turns triangles inside out.
float Determinant3(const float world[16]);

// Takes a node's matrix apart into translation, rotation, and scale, putting
// a mirroring on the scale's first axis.
Transform3 DecomposeNodeMatrix(const float matrix[16]);

// Four skin weights as bytes that sum to exactly 255, the rounding given to
// the largest. All zero when every weight is.
void QuantizeSkinWeights(const float weights[4], uint8_t out[4]);

// Reads any format Assimp supports (FBX, OBJ, Collada, 3DS, Blender, PLY,
// STL, and more) into a mesh. False, having logged why, when it cannot; always
// false in a build made without Assimp.
bool ParseWithAssimp(const std::string& resolvedPath, ModelMesh& outMesh);
bool IsAssimpAvailable();

class ModelStore
{
public:
    // Returns an index into the store, or -1 on failure. Loading the same path
    // twice returns the same index rather than parsing it again.
    int Load(const std::string& path);

    // Reserves a slot and parses the file on the loading thread. The index is
    // usable at once; the mesh in it is empty and not Ready until the parse
    // lands, which happens inside Collect on the thread that calls it.
    int LoadInBackground(const std::string& path);

    const ModelMesh* Get(int index) const;
    ModelMesh* Get(int index);

    // Re-parses any watched model whose file has changed since it was loaded,
    // and bumps its revision so the renderer rebuilds its buffers. The indices
    // of the ones that changed are appended to outChanged, because an actor
    // built around a model's bounds has to be built again. Cheap when nothing
    // changed: one stat a model.
    int ReloadChanged(std::vector<int>& outChanged);

    size_t GetCount() const { return m_Meshes.size(); }

    // Parses a file into a mesh without touching the store. The loading
    // thread runs this.
    static bool Parse(const std::string& path, ModelMesh& outMesh);

private:
    // Slots never move: a background load holds an index across frames, and
    // anything already drawing from one holds a pointer.
    std::vector<std::unique_ptr<ModelMesh>> m_Meshes;
};

} // namespace ludifex::detail
