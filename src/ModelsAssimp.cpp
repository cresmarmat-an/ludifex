// Every model format Assimp reads (FBX, OBJ, Collada, 3DS, Blender, PLY, STL,
// X, and many more), turned into the same ModelMesh the glTF path makes, so
// the rest of ludifex does not depend on the source format.
//
// glTF itself stays with cgltf: it is the format ludifex is built around, and
// cgltf reads it with less between the file and the mesh.
//
// What comes across: triangle geometry with normals and the first set of
// texture coordinates; materials, metallic-roughness where the file has it and
// converted from the older diffuse-and-shininess model where it does not;
// textures, whether files beside the model or embedded in it; skeletons and
// skins; clips that move bones or whole parts; and blend shapes with the clips
// that drive them.

#include "Models.h"

#include "Images.h"
#include "WorldTextures.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

#if LUDIFEX_HAS_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace ludifex::detail
{

#if LUDIFEX_HAS_ASSIMP

namespace
{

// Assimp's matrices are row-major with the translation in the fourth column;
// ludifex's are column-major.
Mat4 ToMat4(const aiMatrix4x4& m)
{
    const float rows[4][4] = { { m.a1, m.a2, m.a3, m.a4 },
                               { m.b1, m.b2, m.b3, m.b4 },
                               { m.c1, m.c2, m.c3, m.c4 },
                               { m.d1, m.d2, m.d3, m.d4 } };
    Mat4 out;
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            out.M[column * 4 + row] = rows[row][column];
        }
    }
    return out;
}

Vec3 TransformPoint(const Mat4& m, float x, float y, float z)
{
    return Vec3{ m.M[0] * x + m.M[4] * y + m.M[8] * z + m.M[12], m.M[1] * x + m.M[5] * y + m.M[9] * z + m.M[13],
                 m.M[2] * x + m.M[6] * y + m.M[10] * z + m.M[14] };
}

struct AssimpContext
{
    const aiScene* Scene = nullptr;
    std::string ModelPath;
    std::string Directory;
    ModelMesh* Mesh = nullptr;
    bool FirstVertex = true;
    bool AnyGeometry = false;

    std::unordered_map<std::string, const aiNode*> NodesByName;

    // Every node that is a joint: the bones, the parts clips move, and every
    // node above them up to the scene's root, so each joint's parent is a
    // joint too and one forward pass composes them all.
    std::unordered_map<const aiNode*, int> NodeToJoint;

    // Each node's first morph target and how many, across all its meshes.
    std::unordered_map<const aiNode*, std::pair<int, int>> NodeToMorphs;

    // For each node, how many anim meshes each of its meshes brought, in
    // order, so a weights clip can be spread over all of them.
    std::unordered_map<const aiNode*, std::vector<int>> NodeMorphBlocks;

    std::unordered_map<std::string, TextureId> Textures;
};

void IndexNodes(AssimpContext& context, const aiNode* node)
{
    context.NodesByName.emplace(node->mName.C_Str(), node);
    for (unsigned child = 0; child < node->mNumChildren; ++child)
    {
        IndexNodes(context, node->mChildren[child]);
    }
}

const aiNode* FindNode(const AssimpContext& context, const aiString& name)
{
    const auto found = context.NodesByName.find(name.C_Str());
    return found != context.NodesByName.end() ? found->second : nullptr;
}

Mat4 GlobalTransform(const aiNode* node)
{
    Mat4 global = ToMat4(node->mTransformation);
    for (const aiNode* parent = node->mParent; parent != nullptr; parent = parent->mParent)
    {
        global = ToMat4(parent->mTransformation) * global;
    }
    return global;
}

bool IsUnderAny(const aiNode* node, const std::unordered_set<const aiNode*>& set)
{
    for (const aiNode* at = node; at != nullptr; at = at->mParent)
    {
        if (set.count(at) != 0)
        {
            return true;
        }
    }
    return false;
}

// --- skeleton -----------------------------------------------------------------

// Joints in an order where every parent comes before its children, which is
// the order a depth-first walk from the root finds them in.
void OrderJoints(AssimpContext& context, const aiNode* node, const std::unordered_set<const aiNode*>& wanted,
                 std::vector<const aiNode*>& order)
{
    if (wanted.count(node) != 0)
    {
        context.NodeToJoint[node] = static_cast<int>(order.size());
        order.push_back(node);
    }
    for (unsigned child = 0; child < node->mNumChildren; ++child)
    {
        OrderJoints(context, node->mChildren[child], wanted, order);
    }
}

// The bones every skin names, and every part a clip moves, become joints,
// along with their ancestors, so a joint's parent is always a joint too.
void BuildSkeleton(AssimpContext& context, const std::unordered_set<const aiNode*>& moved)
{
    const aiScene* scene = context.Scene;
    std::unordered_set<const aiNode*> wanted;

    for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
    {
        const aiMesh* source = scene->mMeshes[meshIndex];
        for (unsigned boneIndex = 0; boneIndex < source->mNumBones; ++boneIndex)
        {
            if (const aiNode* node = FindNode(context, source->mBones[boneIndex]->mName))
            {
                wanted.insert(node);
            }
        }
    }
    wanted.insert(moved.begin(), moved.end());

    if (wanted.empty())
    {
        return;
    }

    std::vector<const aiNode*> ancestors;
    for (const aiNode* node : wanted)
    {
        for (const aiNode* parent = node->mParent; parent != nullptr; parent = parent->mParent)
        {
            ancestors.push_back(parent);
        }
    }
    wanted.insert(ancestors.begin(), ancestors.end());

    ModelMesh& mesh = *context.Mesh;
    std::vector<const aiNode*> order;
    OrderJoints(context, scene->mRootNode, wanted, order);

    for (const aiNode* node : order)
    {
        ModelJoint joint;
        joint.Name = node->mName.C_Str();
        joint.Rest = DecomposeNodeMatrix(ToMat4(node->mTransformation).M);
        joint.Parent = node->mParent != nullptr ? context.NodeToJoint.at(node->mParent) : -1;
        mesh.Joints.push_back(joint);
    }

    LogMessage(LogLevel::Info, "asset", "\"%s\" has a skeleton of %zu joints.", context.ModelPath.c_str(),
               mesh.Joints.size());
}

// The palette entry for a bone bound one way, shared by every mesh that binds
// it the same way. -1 when the palette is full: vertices index it with a byte.
int PaletteEntry(AssimpContext& context, int bone, const Mat4& inverseBind)
{
    std::vector<SkinJoint>& palette = context.Mesh->SkinJoints;
    for (size_t index = 0; index < palette.size(); ++index)
    {
        if (palette[index].Bone == bone &&
            std::memcmp(palette[index].InverseBind.M, inverseBind.M, sizeof(float) * 16) == 0)
        {
            return static_cast<int>(index);
        }
    }
    if (palette.size() >= 255)
    {
        return -1;
    }
    palette.push_back(SkinJoint{ bone, inverseBind });
    return static_cast<int>(palette.size() - 1);
}

// --- materials ------------------------------------------------------------------

// A texture a material names: embedded in the file, as "*3" or by its own
// name, or a file beside the model. Exporters often record the path on the
// artist's machine, so a file that is not where the path says is looked for
// beside the model under its own name.
TextureId LoadTexture(AssimpContext& context, const aiString& reference, TextureUsage usage)
{
    const std::string name = reference.C_Str();
    if (name.empty())
    {
        return TextureId{};
    }

    const char* kind = usage == TextureUsage::Color ? "#c" : usage == TextureUsage::Data ? "#d" : "#n";
    const std::string cacheKey = name + kind;
    if (const auto cached = context.Textures.find(cacheKey); cached != context.Textures.end())
    {
        return cached->second;
    }

    WorldTextureStore& textures = GetWorldTextures();
    TextureId id;

    if (const aiTexture* embedded = context.Scene->GetEmbeddedTexture(name.c_str()))
    {
        if (embedded->mHeight == 0)
        {
            // Compressed: mWidth is the size of the encoded file in bytes.
            id = textures.LoadEncoded(reinterpret_cast<const uint8_t*>(embedded->pcData), embedded->mWidth,
                                      context.ModelPath + "#" + name, usage);
        }
        else
        {
            const size_t count = static_cast<size_t>(embedded->mWidth) * embedded->mHeight;
            std::vector<uint8_t> pixels(count * 4);
            for (size_t index = 0; index < count; ++index)
            {
                const aiTexel& texel = embedded->pcData[index];
                pixels[index * 4 + 0] = texel.r;
                pixels[index * 4 + 1] = texel.g;
                pixels[index * 4 + 2] = texel.b;
                pixels[index * 4 + 3] = texel.a;
            }
            id = textures.Create(static_cast<int>(embedded->mWidth), static_cast<int>(embedded->mHeight),
                                 pixels.data(), usage);
        }
    }
    else
    {
        std::string path = name;
        std::replace(path.begin(), path.end(), '\\', '/');

        std::error_code error;
        const std::filesystem::path relative = std::filesystem::path(context.Directory + path);
        const std::filesystem::path asWritten = std::filesystem::path(path);
        const std::filesystem::path beside =
            std::filesystem::path(context.Directory) / asWritten.filename();

        std::string chosen = relative.generic_string();
        if (!std::filesystem::exists(relative, error))
        {
            if (asWritten.is_absolute() && std::filesystem::exists(asWritten, error))
            {
                chosen = asWritten.generic_string();
            }
            else if (std::filesystem::exists(beside, error))
            {
                chosen = beside.generic_string();
            }
        }
        id = textures.Load(chosen, usage);
    }

    context.Textures.emplace(cacheKey, id);
    return id;
}

bool FirstTexture(const aiMaterial* material, std::initializer_list<aiTextureType> types, aiString& outPath)
{
    for (aiTextureType type : types)
    {
        if (material->GetTextureCount(type) > 0 && material->GetTexture(type, 0, &outPath) == AI_SUCCESS)
        {
            return true;
        }
    }
    return false;
}

// Separate metalness and roughness maps (what most tools other than glTF
// exporters produce), packed into the one map the shader reads: roughness in green,
// metalness in blue.
TextureId PackMetallicRoughness(AssimpContext& context, const aiString* metalness, const aiString* roughness)
{
    auto Read = [&](const aiString* reference, DecodedImage& out) {
        if (reference == nullptr)
        {
            return false;
        }
        std::string error;
        const std::string name = reference->C_Str();
        if (const aiTexture* embedded = context.Scene->GetEmbeddedTexture(name.c_str()))
        {
            return embedded->mHeight == 0 &&
                   DecodeImage(reinterpret_cast<const uint8_t*>(embedded->pcData), embedded->mWidth, out, error);
        }
        std::string path = name;
        std::replace(path.begin(), path.end(), '\\', '/');
        const std::filesystem::path relative = std::filesystem::path(context.Directory + path);
        const std::filesystem::path beside =
            std::filesystem::path(context.Directory) / std::filesystem::path(path).filename();
        std::error_code exists;
        const std::string file =
            (std::filesystem::exists(relative, exists) ? relative : beside).generic_string();

        size_t size = 0;
        void* bytes = SDL_LoadFile(file.c_str(), &size);
        if (bytes == nullptr)
        {
            LogMessage(LogLevel::Warning, "asset", "\"%s\" names a texture, \"%s\", that is not there.",
                       context.ModelPath.c_str(), name.c_str());
            return false;
        }
        const bool decoded = DecodeImage(static_cast<const uint8_t*>(bytes), size, out, error);
        SDL_free(bytes);
        return decoded;
    };

    DecodedImage metal;
    DecodedImage rough;
    const bool hasMetal = Read(metalness, metal);
    const bool hasRough = Read(roughness, rough);
    if (!hasMetal && !hasRough)
    {
        return TextureId{};
    }

    const DecodedImage& size = hasRough ? rough : metal;
    const int width = size.Width;
    const int height = size.Height;
    std::vector<uint8_t> packed(static_cast<size_t>(width) * height * 4, 255);

    // Nearest texel when the two maps differ in size.
    auto Sample = [](const DecodedImage& image, int x, int y, int width, int height) {
        const int sx = std::min(image.Width - 1, x * image.Width / std::max(width, 1));
        const int sy = std::min(image.Height - 1, y * image.Height / std::max(height, 1));
        return image.Pixels[(static_cast<size_t>(sy) * image.Width + sx) * 4];
    };
    for (int y = 0; y < height; ++y)
    {
        for (int x = 0; x < width; ++x)
        {
            uint8_t* out = &packed[(static_cast<size_t>(y) * width + x) * 4];
            out[0] = 0;
            out[1] = hasRough ? Sample(rough, x, y, width, height) : 255;
            out[2] = hasMetal ? Sample(metal, x, y, width, height) : 255;
            out[3] = 255;
        }
    }
    return GetWorldTextures().Create(width, height, packed.data(), TextureUsage::Data);
}

void ReadMaterial(AssimpContext& context, const aiMaterial* material, ModelPart& part)
{
    if (material == nullptr)
    {
        return;
    }
    part.HasMaterial = true;

    aiColor4D color;
    aiString path;
    float value = 0.0f;

    // Metallic-roughness when the file describes it; otherwise the older
    // model, whose colours were chosen by eye on an sRGB screen.
    const bool physical = material->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS ||
                          material->GetTextureCount(aiTextureType_BASE_COLOR) > 0 ||
                          material->Get(AI_MATKEY_METALLIC_FACTOR, value) == AI_SUCCESS;

    if (physical)
    {
        if (material->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS ||
            material->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
        {
            part.BaseColorFactor = Color{ color.r, color.g, color.b, color.a };
        }
        part.Metallic = material->Get(AI_MATKEY_METALLIC_FACTOR, value) == AI_SUCCESS ? value : 0.0f;
        part.Roughness = material->Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == AI_SUCCESS ? value : 0.6f;
    }
    else
    {
        if (material->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS)
        {
            part.BaseColorFactor = ToLinear(Color{ color.r, color.g, color.b, 1.0f });
        }

        // A Blinn-Phong exponent turned into the roughness that gives a
        // highlight of about the same size.
        float shininess = 0.0f;
        if (material->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f)
        {
            part.Roughness = std::clamp(std::sqrt(2.0f / (shininess + 2.0f)), 0.05f, 1.0f);
        }
        part.Metallic = 0.0f;
    }

    if (FirstTexture(material, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE }, path))
    {
        part.BaseColorTexture = LoadTexture(context, path, TextureUsage::Color);
    }

    // Separate metalness and roughness maps, packed into one.
    aiString metalness;
    aiString roughness;
    const bool hasMetal = FirstTexture(material, { aiTextureType_METALNESS }, metalness);
    const bool hasRough = FirstTexture(material, { aiTextureType_DIFFUSE_ROUGHNESS }, roughness);
    if (hasMetal || hasRough)
    {
        part.MetallicRoughnessTexture =
            PackMetallicRoughness(context, hasMetal ? &metalness : nullptr, hasRough ? &roughness : nullptr);
        // The maps hold the values; the factors scale them.
        if (hasRough && material->Get(AI_MATKEY_ROUGHNESS_FACTOR, value) != AI_SUCCESS)
        {
            part.Roughness = 1.0f;
        }
        if (hasMetal && material->Get(AI_MATKEY_METALLIC_FACTOR, value) != AI_SUCCESS)
        {
            part.Metallic = 1.0f;
        }
    }

    if (FirstTexture(material, { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA }, path))
    {
        part.NormalTexture = LoadTexture(context, path, TextureUsage::Normal);
        if (material->Get(AI_MATKEY_BUMPSCALING, value) == AI_SUCCESS && value > 0.0f)
        {
            part.NormalScale = value;
        }
    }

    if (FirstTexture(material, { aiTextureType_AMBIENT_OCCLUSION }, path))
    {
        part.OcclusionTexture = LoadTexture(context, path, TextureUsage::Data);
    }

    aiColor3D emissive(0.0f, 0.0f, 0.0f);
    if (material->Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS)
    {
        const Color linear = physical ? Color{ emissive.r, emissive.g, emissive.b, 1.0f }
                                      : ToLinear(Color{ emissive.r, emissive.g, emissive.b, 1.0f });
        part.EmissiveFactor = Vec3{ linear.R, linear.G, linear.B };
    }
    if (FirstTexture(material, { aiTextureType_EMISSION_COLOR, aiTextureType_EMISSIVE }, path))
    {
        part.EmissiveTexture = LoadTexture(context, path, TextureUsage::Color);

        // Older formats pair an emissive map with a black colour and mean
        // the map as it is.
        if (part.EmissiveFactor.X == 0.0f && part.EmissiveFactor.Y == 0.0f && part.EmissiveFactor.Z == 0.0f)
        {
            part.EmissiveFactor = Vec3{ 1.0f, 1.0f, 1.0f };
        }
    }
    if (material->Get(AI_MATKEY_EMISSIVE_INTENSITY, value) == AI_SUCCESS && value > 0.0f)
    {
        part.EmissiveFactor = Vec3{ part.EmissiveFactor.X * value, part.EmissiveFactor.Y * value,
                                    part.EmissiveFactor.Z * value };
    }

    float opacity = 1.0f;
    if (material->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.999f)
    {
        part.BaseColorFactor.A *= std::max(opacity, 0.0f);
        part.Transparent = true;
    }
    if (material->GetTextureCount(aiTextureType_OPACITY) > 0)
    {
        part.Transparent = true;
    }
}

// --- geometry --------------------------------------------------------------------

// One Assimp mesh as one part. A skinned mesh keeps its own coordinates, which
// its bones' bindings expect; a mesh a clip moves as a whole is bound to its
// node the same way; anything else is placed by its node's transform.
void AddMesh(AssimpContext& context, const aiNode* node, const aiMesh* source, std::vector<float>* morphDeltas)
{
    ModelMesh& mesh = *context.Mesh;
    const Mat4 global = GlobalTransform(node);

    // A palette entry for each of the mesh's bones, and each vertex's
    // weights, gathered from the bones' lists.
    std::vector<int> bonePalette;
    bool skinned = source->mNumBones > 0;
    int rigidEntry = -1;

    if (skinned)
    {
        bonePalette.assign(source->mNumBones, -1);
        for (unsigned boneIndex = 0; boneIndex < source->mNumBones && skinned; ++boneIndex)
        {
            const aiBone* bone = source->mBones[boneIndex];
            const aiNode* boneNode = FindNode(context, bone->mName);
            const auto joint = boneNode != nullptr ? context.NodeToJoint.find(boneNode) : context.NodeToJoint.end();
            if (joint == context.NodeToJoint.end())
            {
                skinned = false;
                break;
            }
            bonePalette[boneIndex] = PaletteEntry(context, joint->second, ToMat4(bone->mOffsetMatrix));
            skinned = bonePalette[boneIndex] >= 0;
        }
        if (!skinned)
        {
            LogMessage(LogLevel::Error, "asset",
                       "\"%s\": the mesh \"%s\" needs more than the 255 joint bindings vertices can index, or "
                       "names a bone the file does not have. It is drawn unskinned.",
                       context.ModelPath.c_str(), source->mName.C_Str());
        }
    }
    else if (const auto joint = context.NodeToJoint.find(node); joint != context.NodeToJoint.end())
    {
        // A part a clip moves: every vertex follows its node, fully.
        rigidEntry = PaletteEntry(context, joint->second, Mat4{});
    }

    const bool bound = skinned || rigidEntry >= 0;

    // Bound vertices stay in the mesh's own space and the palette places
    // them; the rest are placed here. Either way the bounds are of where the
    // mesh stands at rest.
    const Mat4 place = bound ? Mat4{} : global;
    float normalMatrix[9];
    NormalMatrix(place.M, normalMatrix);
    const bool mirrored = Determinant3(place.M) < 0.0f;

    std::vector<std::vector<std::pair<int, float>>> influences;
    if (skinned)
    {
        influences.resize(source->mNumVertices);
        for (unsigned boneIndex = 0; boneIndex < source->mNumBones; ++boneIndex)
        {
            const aiBone* bone = source->mBones[boneIndex];
            for (unsigned weightIndex = 0; weightIndex < bone->mNumWeights; ++weightIndex)
            {
                const aiVertexWeight& weight = bone->mWeights[weightIndex];
                if (weight.mVertexId < source->mNumVertices && weight.mWeight > 0.0f)
                {
                    influences[weight.mVertexId].emplace_back(bonePalette[boneIndex], weight.mWeight);
                }
            }
        }
    }

    const uint32_t baseVertex = static_cast<uint32_t>(mesh.Vertices.size());
    const uint32_t firstIndex = static_cast<uint32_t>(mesh.Indices.size());

    for (unsigned vertexIndex = 0; vertexIndex < source->mNumVertices; ++vertexIndex)
    {
        const aiVector3D& p = source->mVertices[vertexIndex];
        ModelVertex vertex{};

        const Vec3 placed = TransformPoint(place, p.x, p.y, p.z);
        vertex.Position[0] = placed.X;
        vertex.Position[1] = placed.Y;
        vertex.Position[2] = placed.Z;

        if (source->HasNormals())
        {
            const aiVector3D& n = source->mNormals[vertexIndex];
            float normal[3] = { normalMatrix[0] * n.x + normalMatrix[1] * n.y + normalMatrix[2] * n.z,
                                normalMatrix[3] * n.x + normalMatrix[4] * n.y + normalMatrix[5] * n.z,
                                normalMatrix[6] * n.x + normalMatrix[7] * n.y + normalMatrix[8] * n.z };
            const float length = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > 1e-20f)
            {
                normal[0] /= length;
                normal[1] /= length;
                normal[2] /= length;
            }
            std::memcpy(vertex.Normal, normal, sizeof(normal));
        }
        else
        {
            vertex.Normal[1] = 1.0f;
        }

        if (source->HasTextureCoords(0))
        {
            vertex.UV[0] = source->mTextureCoords[0][vertexIndex].x;
            vertex.UV[1] = source->mTextureCoords[0][vertexIndex].y;
        }

        if (skinned)
        {
            // The four strongest, which LimitBoneWeights has already made the
            // only ones.
            std::vector<std::pair<int, float>>& list = influences[vertexIndex];
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            float weights[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (size_t slot = 0; slot < std::min<size_t>(4, list.size()); ++slot)
            {
                vertex.Joints[slot] = static_cast<uint8_t>(list[slot].first);
                weights[slot] = list[slot].second;
            }
            QuantizeSkinWeights(weights, vertex.Weights);
        }
        else if (rigidEntry >= 0)
        {
            vertex.Joints[0] = static_cast<uint8_t>(rigidEntry);
            vertex.Weights[0] = 255;
        }

        const Vec3 rest = bound ? TransformPoint(global, p.x, p.y, p.z) : placed;
        if (context.FirstVertex)
        {
            mesh.Minimum = rest;
            mesh.Maximum = rest;
            context.FirstVertex = false;
        }
        mesh.Minimum = Vec3{ std::min(mesh.Minimum.X, rest.X), std::min(mesh.Minimum.Y, rest.Y),
                             std::min(mesh.Minimum.Z, rest.Z) };
        mesh.Maximum = Vec3{ std::max(mesh.Maximum.X, rest.X), std::max(mesh.Maximum.Y, rest.Y),
                             std::max(mesh.Maximum.Z, rest.Z) };

        mesh.Vertices.push_back(vertex);
    }

    // Blend shapes: Assimp keeps each as a whole replacement mesh, and ludifex
    // as displacements, carried by the same transform as the vertices.
    if (morphDeltas != nullptr)
    {
        for (unsigned target = 0; target < source->mNumAnimMeshes; ++target)
        {
            const aiAnimMesh* shape = source->mAnimMeshes[target];
            for (unsigned vertexIndex = 0; vertexIndex < source->mNumVertices; ++vertexIndex)
            {
                float d[3] = { 0.0f, 0.0f, 0.0f };
                float n[3] = { 0.0f, 0.0f, 0.0f };
                if (shape->mVertices != nullptr && vertexIndex < shape->mNumVertices)
                {
                    const aiVector3D delta = shape->mVertices[vertexIndex] - source->mVertices[vertexIndex];
                    d[0] = place.M[0] * delta.x + place.M[4] * delta.y + place.M[8] * delta.z;
                    d[1] = place.M[1] * delta.x + place.M[5] * delta.y + place.M[9] * delta.z;
                    d[2] = place.M[2] * delta.x + place.M[6] * delta.y + place.M[10] * delta.z;
                }
                if (shape->mNormals != nullptr && source->HasNormals() && vertexIndex < shape->mNumVertices)
                {
                    const aiVector3D delta = shape->mNormals[vertexIndex] - source->mNormals[vertexIndex];
                    n[0] = normalMatrix[0] * delta.x + normalMatrix[1] * delta.y + normalMatrix[2] * delta.z;
                    n[1] = normalMatrix[3] * delta.x + normalMatrix[4] * delta.y + normalMatrix[5] * delta.z;
                    n[2] = normalMatrix[6] * delta.x + normalMatrix[7] * delta.y + normalMatrix[8] * delta.z;
                }
                morphDeltas[target].insert(morphDeltas[target].end(),
                                           { d[0], d[1], d[2], 0.0f, n[0], n[1], n[2], 0.0f });
            }
        }
    }

    for (unsigned faceIndex = 0; faceIndex < source->mNumFaces; ++faceIndex)
    {
        const aiFace& face = source->mFaces[faceIndex];
        if (face.mNumIndices != 3)
        {
            continue;
        }
        mesh.Indices.push_back(baseVertex + face.mIndices[0]);
        mesh.Indices.push_back(baseVertex + face.mIndices[mirrored ? 2 : 1]);
        mesh.Indices.push_back(baseVertex + face.mIndices[mirrored ? 1 : 2]);
    }

    ModelPart part;
    part.FirstIndex = firstIndex;
    part.IndexCount = static_cast<uint32_t>(mesh.Indices.size()) - firstIndex;
    ReadMaterial(context, source->mMaterialIndex < context.Scene->mNumMaterials
                              ? context.Scene->mMaterials[source->mMaterialIndex]
                              : nullptr,
                 part);
    if (part.IndexCount > 0)
    {
        mesh.Parts.push_back(part);
        context.AnyGeometry = true;
    }
}

void AddNode(AssimpContext& context, const aiNode* node)
{
    ModelMesh& mesh = *context.Mesh;
    const int firstTarget = static_cast<int>(mesh.MorphTargets.size());
    std::vector<int> blocks;

    for (unsigned index = 0; index < node->mNumMeshes; ++index)
    {
        if (node->mMeshes[index] >= context.Scene->mNumMeshes)
        {
            continue;
        }
        const aiMesh* source = context.Scene->mMeshes[node->mMeshes[index]];
        if ((source->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0 || !source->HasPositions())
        {
            continue;
        }

        const uint32_t vertexStart = static_cast<uint32_t>(mesh.Vertices.size());
        std::vector<std::vector<float>> deltas(source->mNumAnimMeshes);
        AddMesh(context, node, source, source->mNumAnimMeshes > 0 ? deltas.data() : nullptr);
        const uint32_t vertexCount = static_cast<uint32_t>(mesh.Vertices.size()) - vertexStart;

        blocks.push_back(static_cast<int>(source->mNumAnimMeshes));
        for (unsigned target = 0; target < source->mNumAnimMeshes && vertexCount > 0; ++target)
        {
            const aiAnimMesh* shape = source->mAnimMeshes[target];
            MorphTarget morph;
            morph.Name = shape->mName.length > 0 ? shape->mName.C_Str()
                                                 : std::string(source->mName.C_Str()) + "." + std::to_string(target);
            morph.DefaultWeight = shape->mWeight;
            morph.VertexStart = vertexStart;
            morph.VertexCount = vertexCount;
            morph.DeltaOffset = static_cast<uint32_t>(mesh.MorphDeltas.size() / 4);
            mesh.MorphDeltas.insert(mesh.MorphDeltas.end(), deltas[target].begin(), deltas[target].end());
            mesh.MorphTargets.push_back(std::move(morph));
        }
    }

    const int targetCount = static_cast<int>(mesh.MorphTargets.size()) - firstTarget;
    if (targetCount > 0)
    {
        context.NodeToMorphs[node] = { firstTarget, targetCount };
        context.NodeMorphBlocks[node] = blocks;
    }

    for (unsigned child = 0; child < node->mNumChildren; ++child)
    {
        AddNode(context, node->mChildren[child]);
    }
}

// --- animation ------------------------------------------------------------------

void ParseAnimations(AssimpContext& context)
{
    const aiScene* scene = context.Scene;
    ModelMesh& mesh = *context.Mesh;

    for (unsigned animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
    {
        const aiAnimation* source = scene->mAnimations[animationIndex];

        // Times are in ticks; a file that does not say how fast they go is
        // taken at Assimp's customary 25 a second.
        const double ticksPerSecond = source->mTicksPerSecond > 0.0 ? source->mTicksPerSecond : 25.0;
        auto Seconds = [&](double ticks) { return static_cast<float>(ticks / ticksPerSecond); };

        ModelAnimation clip;
        clip.Name = source->mName.length > 0 ? source->mName.C_Str() : "clip " + std::to_string(animationIndex);

        for (unsigned channelIndex = 0; channelIndex < source->mNumChannels; ++channelIndex)
        {
            const aiNodeAnim* channel = source->mChannels[channelIndex];
            const aiNode* node = FindNode(context, channel->mNodeName);
            const auto joint = node != nullptr ? context.NodeToJoint.find(node) : context.NodeToJoint.end();
            if (joint == context.NodeToJoint.end())
            {
                continue;
            }

            if (channel->mNumPositionKeys > 0)
            {
                AnimationChannel curve;
                curve.Joint = joint->second;
                curve.Path = AnimationPath::Translation;
                for (unsigned key = 0; key < channel->mNumPositionKeys; ++key)
                {
                    const aiVectorKey& k = channel->mPositionKeys[key];
                    curve.Times.push_back(Seconds(k.mTime));
                    curve.Values.insert(curve.Values.end(), { k.mValue.x, k.mValue.y, k.mValue.z });
                }
                clip.Channels.push_back(std::move(curve));
            }
            if (channel->mNumRotationKeys > 0)
            {
                AnimationChannel curve;
                curve.Joint = joint->second;
                curve.Path = AnimationPath::Rotation;
                for (unsigned key = 0; key < channel->mNumRotationKeys; ++key)
                {
                    const aiQuatKey& k = channel->mRotationKeys[key];
                    curve.Times.push_back(Seconds(k.mTime));
                    curve.Values.insert(curve.Values.end(), { k.mValue.x, k.mValue.y, k.mValue.z, k.mValue.w });
                }
                clip.Channels.push_back(std::move(curve));
            }
            if (channel->mNumScalingKeys > 0)
            {
                AnimationChannel curve;
                curve.Joint = joint->second;
                curve.Path = AnimationPath::Scale;
                for (unsigned key = 0; key < channel->mNumScalingKeys; ++key)
                {
                    const aiVectorKey& k = channel->mScalingKeys[key];
                    curve.Times.push_back(Seconds(k.mTime));
                    curve.Values.insert(curve.Values.end(), { k.mValue.x, k.mValue.y, k.mValue.z });
                }
                clip.Channels.push_back(std::move(curve));
            }
        }

        // Blend-shape weights. A key names the shapes it sets; the rest are
        // zero at that key. A node with several meshes has the same shapes on
        // each, so the key's weights are laid over every mesh's run.
        for (unsigned channelIndex = 0; channelIndex < source->mNumMorphMeshChannels; ++channelIndex)
        {
            const aiMeshMorphAnim* channel = source->mMorphMeshChannels[channelIndex];

            // Some importers name the channel "node*mesh"; the node is before
            // the star.
            std::string name = channel->mName.C_Str();
            if (const size_t star = name.find('*'); star != std::string::npos)
            {
                name.resize(star);
            }
            const auto node = context.NodesByName.find(name);
            if (node == context.NodesByName.end())
            {
                continue;
            }
            const auto morphs = context.NodeToMorphs.find(node->second);
            if (morphs == context.NodeToMorphs.end() || channel->mNumKeys == 0)
            {
                continue;
            }
            const std::vector<int>& blocks = context.NodeMorphBlocks[node->second];

            AnimationChannel curve;
            curve.Path = AnimationPath::Weights;
            curve.FirstMorph = morphs->second.first;
            curve.MorphCount = morphs->second.second;
            for (unsigned key = 0; key < channel->mNumKeys; ++key)
            {
                const aiMeshMorphKey& k = channel->mKeys[key];
                curve.Times.push_back(Seconds(k.mTime));
                const size_t start = curve.Values.size();
                curve.Values.resize(start + static_cast<size_t>(curve.MorphCount), 0.0f);

                size_t blockStart = start;
                for (int blockCount : blocks)
                {
                    for (unsigned value = 0; value < k.mNumValuesAndWeights; ++value)
                    {
                        if (static_cast<int>(k.mValues[value]) < blockCount)
                        {
                            curve.Values[blockStart + k.mValues[value]] = static_cast<float>(k.mWeights[value]);
                        }
                    }
                    blockStart += static_cast<size_t>(blockCount);
                }
            }
            clip.Channels.push_back(std::move(curve));
        }

        for (const AnimationChannel& curve : clip.Channels)
        {
            clip.Duration = std::max(clip.Duration, curve.Times.empty() ? 0.0f : curve.Times.back());
        }

        if (!clip.Channels.empty())
        {
            LogMessage(LogLevel::Info, "asset", "\"%s\" animation \"%s\": %.2fs, %zu curves.",
                       context.ModelPath.c_str(), clip.Name.c_str(), static_cast<double>(clip.Duration),
                       clip.Channels.size());
            mesh.Animations.push_back(std::move(clip));
        }
    }
}

} // namespace

bool IsAssimpAvailable()
{
    return true;
}

bool ParseWithAssimp(const std::string& resolvedPath, ModelMesh& outMesh)
{
    Assimp::Importer importer;

    // FBX's pivots otherwise become extra nodes between a bone and its
    // parent, each a joint for nothing.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    importer.SetPropertyInteger(AI_CONFIG_PP_LBW_MAX_WEIGHTS, 4);
    importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);

    // Normals made for a file that has none are smoothed across an edge only
    // when its faces meet at under 80 degrees, so a box keeps its corners and
    // a sphere is still round.
    importer.SetPropertyFloat(AI_CONFIG_PP_GSN_MAX_SMOOTHING_ANGLE, 80.0f);

    // Triangles only, shared vertices merged, normals where the file has none,
    // at most four bones a vertex, and texture coordinates with their origin
    // at the top left as ludifex has them. GlobalScale applies the file's own
    // unit, so FBX in centimetres arrives in metres; Assimp applies FBX's axis
    // settings while reading, so a Z-up file arrives Y up.
    const unsigned flags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices | aiProcess_GenSmoothNormals |
                           aiProcess_LimitBoneWeights | aiProcess_SortByPType | aiProcess_FlipUVs |
                           aiProcess_GlobalScale | aiProcess_ValidateDataStructure;

    const aiScene* scene = importer.ReadFile(resolvedPath, flags);
    if (scene == nullptr || scene->mRootNode == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0)
    {
        LogMessage(LogLevel::Error, "asset", "\"%s\" could not be read: %s", resolvedPath.c_str(),
                   importer.GetErrorString());
        return false;
    }

    AssimpContext context;
    context.Scene = scene;
    context.ModelPath = resolvedPath;
    context.Mesh = &outMesh;
    const std::filesystem::path parent = std::filesystem::path(resolvedPath).parent_path();
    context.Directory = parent.empty() ? std::string() : parent.generic_string() + "/";
    IndexNodes(context, scene->mRootNode);

    // The parts clips move, and the meshes on or under them: those are bound
    // to their node rather than placed once.
    std::unordered_set<const aiNode*> moved;
    for (unsigned animationIndex = 0; animationIndex < scene->mNumAnimations; ++animationIndex)
    {
        const aiAnimation* animation = scene->mAnimations[animationIndex];
        for (unsigned channel = 0; channel < animation->mNumChannels; ++channel)
        {
            if (const aiNode* node = FindNode(context, animation->mChannels[channel]->mNodeName))
            {
                moved.insert(node);
            }
        }
    }
    std::unordered_set<const aiNode*> rigid;
    if (!moved.empty())
    {
        std::vector<const aiNode*> stack{ scene->mRootNode };
        while (!stack.empty())
        {
            const aiNode* node = stack.back();
            stack.pop_back();
            if (node->mNumMeshes > 0 && IsUnderAny(node, moved))
            {
                rigid.insert(node);
            }
            for (unsigned child = 0; child < node->mNumChildren; ++child)
            {
                stack.push_back(node->mChildren[child]);
            }
        }
    }
    std::unordered_set<const aiNode*> joints = moved;
    joints.insert(rigid.begin(), rigid.end());

    BuildSkeleton(context, joints);
    AddNode(context, scene->mRootNode);
    if (!outMesh.Joints.empty() || !outMesh.MorphTargets.empty())
    {
        ParseAnimations(context);
    }

    // Nothing moves a joint but a clip or a skin; a skeleton with neither is
    // just the hierarchy the vertices were placed through.
    if (outMesh.SkinJoints.empty())
    {
        outMesh.Joints.clear();
        std::vector<ModelAnimation> kept;
        for (ModelAnimation& clip : outMesh.Animations)
        {
            clip.Channels.erase(std::remove_if(clip.Channels.begin(), clip.Channels.end(),
                                               [](const AnimationChannel& curve) {
                                                   return curve.Path != AnimationPath::Weights;
                                               }),
                                clip.Channels.end());
            if (!clip.Channels.empty())
            {
                kept.push_back(std::move(clip));
            }
        }
        outMesh.Animations = std::move(kept);
    }

    if (!context.AnyGeometry || outMesh.Indices.empty())
    {
        LogMessage(LogLevel::Error, "asset", "\"%s\" contains no triangle geometry this build can draw.",
                   resolvedPath.c_str());
        return false;
    }
    return true;
}

#else

bool IsAssimpAvailable()
{
    return false;
}

bool ParseWithAssimp(const std::string& resolvedPath, ModelMesh&)
{
    LogMessage(LogLevel::Error, "asset",
               "\"%s\" is not glTF, and this build of ludifex was made without Assimp (LUDIFEX_ASSIMP=OFF), "
               "which reads the other formats. Only .gltf and .glb load.",
               resolvedPath.c_str());
    return false;
}

#endif

} // namespace ludifex::detail
