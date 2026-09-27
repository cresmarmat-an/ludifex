#include "Models.h"

#include "Loading.h"

#include "Assets.h"
#include "WorldTextures.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <atomic>
#include <filesystem>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996) // deprecated CRT function
#pragma warning(disable : 4244) // conversion, possible loss of data
#pragma warning(disable : 4245) // signed/unsigned mismatch
#endif

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace ludifex::detail
{
namespace
{

// Reads one accessor into a float array with the requested component count.
// cgltf handles the stride, sparse storage, normalisation, and component type
// conversion, so this only has to size the destination correctly.
bool ReadFloats(const cgltf_accessor* accessor, cgltf_size components, std::vector<float>& outValues)
{
    if (accessor == nullptr)
    {
        return false;
    }

    const cgltf_size actual = cgltf_num_components(accessor->type);
    if (actual != components)
    {
        return false;
    }

    outValues.resize(accessor->count * components);
    const cgltf_size read = cgltf_accessor_unpack_floats(accessor, outValues.data(), outValues.size());

    return read == outValues.size();
}

// Faceted normals for a primitive that did not supply any, so an untextured
// model still catches the light instead of rendering flat.
void GenerateNormals(std::vector<ModelVertex>& vertices, const std::vector<uint32_t>& indices,
                     uint32_t firstVertex, uint32_t firstIndex)
{
    for (size_t index = firstVertex; index < vertices.size(); ++index)
    {
        vertices[index].Normal[0] = 0.0f;
        vertices[index].Normal[1] = 0.0f;
        vertices[index].Normal[2] = 0.0f;
    }

    for (size_t i = firstIndex; i + 2 < indices.size(); i += 3)
    {
        ModelVertex& a = vertices[indices[i]];
        ModelVertex& b = vertices[indices[i + 1]];
        ModelVertex& c = vertices[indices[i + 2]];

        const float edge1[3] = { b.Position[0] - a.Position[0], b.Position[1] - a.Position[1],
                                 b.Position[2] - a.Position[2] };
        const float edge2[3] = { c.Position[0] - a.Position[0], c.Position[1] - a.Position[1],
                                 c.Position[2] - a.Position[2] };

        const float normal[3] = { edge1[1] * edge2[2] - edge1[2] * edge2[1],
                                  edge1[2] * edge2[0] - edge1[0] * edge2[2],
                                  edge1[0] * edge2[1] - edge1[1] * edge2[0] };

        for (ModelVertex* vertex : { &a, &b, &c })
        {
            vertex->Normal[0] += normal[0];
            vertex->Normal[1] += normal[1];
            vertex->Normal[2] += normal[2];
        }
    }

    for (size_t index = firstVertex; index < vertices.size(); ++index)
    {
        ModelVertex& vertex = vertices[index];
        const float lengthSquared = vertex.Normal[0] * vertex.Normal[0] + vertex.Normal[1] * vertex.Normal[1] +
                                    vertex.Normal[2] * vertex.Normal[2];
        if (lengthSquared > 1e-20f)
        {
            const float inverse = 1.0f / std::sqrt(lengthSquared);
            vertex.Normal[0] *= inverse;
            vertex.Normal[1] *= inverse;
            vertex.Normal[2] *= inverse;
        }
        else
        {
            vertex.Normal[1] = 1.0f;
        }
    }
}

struct LoadContext
{
    const cgltf_data* Data = nullptr;
    std::string ModelPath;     // as resolved on disk
    std::string Directory;     // its folder, for relative image URIs
    ModelMesh* Mesh = nullptr;
    bool AnyGeometry = false;
    bool FirstVertex = true;

    // For each skin in the file, its joints' places in the model's palette,
    // in the order the skin lists them: what a vertex's JOINTS_0 becomes.
    // Parsed before any geometry so a primitive can remap as it reads. Empty
    // for a skin that could not be used.
    std::vector<std::vector<int>> SkinPalette;

    // Node index -> position in mesh.Joints, used to find an animation
    // channel's bone. -1 for nodes that are not joints.
    std::vector<int> NodeToJoint;

    // Node index -> the model's first morph target for that node's mesh, and
    // how many, used to find what a weights channel drives.
    std::vector<std::pair<int, int>> NodeToMorphs;
};

// A glTF image is a file beside the model, a data URI, or a slice of a binary
// buffer. All three end up in the process-wide texture store.
TextureId LoadImage(LoadContext& context, const cgltf_image* image, TextureUsage usage = TextureUsage::Color)
{
    if (image == nullptr)
    {
        return TextureId{};
    }

    const cgltf_size imageIndex = cgltf_image_index(context.Data, image);
    const std::string key = context.ModelPath + "#image" + std::to_string(imageIndex);
    WorldTextureStore& textures = GetWorldTextures();

    if (image->buffer_view != nullptr)
    {
        const cgltf_buffer_view* view = image->buffer_view;
        if (view->buffer == nullptr || view->buffer->data == nullptr)
        {
            return textures.GetMissing();
        }
        const auto* bytes = static_cast<const uint8_t*>(view->buffer->data) + view->offset;
        return textures.LoadEncoded(bytes, view->size, key, usage);
    }

    if (image->uri == nullptr)
    {
        return TextureId{};
    }

    const std::string uri = image->uri;
    if (uri.rfind("data:", 0) == 0)
    {
        const size_t comma = uri.find(',');
        if (comma == std::string::npos || uri.find(";base64") == std::string::npos)
        {
            LogMessage(LogLevel::Error, "asset", "An image in \"%s\" uses a data URI this build cannot read.",
                       context.ModelPath.c_str());
            return textures.GetMissing();
        }

        const std::string encoded = uri.substr(comma + 1);

        // Base64 packs three bytes into four characters, less the padding.
        size_t padding = 0;
        if (!encoded.empty() && encoded.back() == '=')
        {
            padding = (encoded.size() > 1 && encoded[encoded.size() - 2] == '=') ? 2 : 1;
        }
        const cgltf_size size = encoded.size() / 4 * 3 - padding;

        cgltf_options options{};
        void* decoded = nullptr;
        if (cgltf_load_buffer_base64(&options, size, encoded.c_str(), &decoded) != cgltf_result_success)
        {
            return textures.GetMissing();
        }

        const TextureId id = textures.LoadEncoded(static_cast<const uint8_t*>(decoded), size, key, usage);
        std::free(decoded);
        return id;
    }

    // A relative file, percent-decoded as URIs require.
    std::vector<char> path(uri.begin(), uri.end());
    path.push_back('\0');
    cgltf_decode_uri(path.data());

    return textures.Load(context.Directory + path.data(), usage);
}

const cgltf_image* ImageOf(const cgltf_texture_view& view)
{
    return view.texture != nullptr ? view.texture->image : nullptr;
}

void ReadMaterial(LoadContext& context, const cgltf_material* material, ModelPart& part)
{
    if (material == nullptr)
    {
        return;
    }

    part.HasMaterial = true;

    if (material->has_pbr_metallic_roughness)
    {
        const cgltf_pbr_metallic_roughness& pbr = material->pbr_metallic_roughness;
        part.BaseColorFactor = Color{ pbr.base_color_factor[0], pbr.base_color_factor[1],
                                      pbr.base_color_factor[2], pbr.base_color_factor[3] };
        part.Roughness = pbr.roughness_factor;
        part.Metallic = pbr.metallic_factor;
        part.BaseColorTexture = LoadImage(context, pbr.base_color_texture.texture != nullptr
                                                       ? pbr.base_color_texture.texture->image
                                                       : nullptr);

        // Numbers, not colours: stored and sampled exactly as authored.
        part.MetallicRoughnessTexture =
            LoadImage(context, ImageOf(pbr.metallic_roughness_texture), TextureUsage::Data);
    }

    part.NormalTexture = LoadImage(context, ImageOf(material->normal_texture), TextureUsage::Normal);
    part.NormalScale = material->normal_texture.texture != nullptr ? material->normal_texture.scale : 1.0f;

    part.OcclusionTexture = LoadImage(context, ImageOf(material->occlusion_texture), TextureUsage::Data);
    part.OcclusionStrength =
        material->occlusion_texture.texture != nullptr ? material->occlusion_texture.scale : 1.0f;

    part.EmissiveTexture = LoadImage(context, ImageOf(material->emissive_texture), TextureUsage::Color);
    part.EmissiveFactor = Vec3{ material->emissive_factor[0], material->emissive_factor[1],
                                material->emissive_factor[2] };

    // KHR_materials_emissive_strength lifts emission past 1, which a bright
    // screen or a lamp needs in a renderer that tone maps.
    if (material->has_emissive_strength)
    {
        const float strength = material->emissive_strength.emissive_strength;
        part.EmissiveFactor = Vec3{ part.EmissiveFactor.X * strength, part.EmissiveFactor.Y * strength,
                                    part.EmissiveFactor.Z * strength };
    }

    part.Transparent = (material->alpha_mode == cgltf_alpha_mode_blend);
}

// A node's own transform, as the three parts an animation drives. glTF
// stores either TRS or a matrix; a matrix has to be taken apart, because a
// clip that animates rotation alone still needs somewhere to put the
// translation and scale it leaves alone.
Transform3 LocalTransform(const cgltf_node* node)
{
    Transform3 transform;

    if (node->has_matrix)
    {
        return DecomposeNodeMatrix(node->matrix);
    }

    if (node->has_translation)
    {
        transform.Position = Vec3{ node->translation[0], node->translation[1], node->translation[2] };
    }
    if (node->has_rotation)
    {
        transform.Rotation = Quat{ node->rotation[0], node->rotation[1], node->rotation[2],
                                   node->rotation[3] };
    }
    if (node->has_scale)
    {
        transform.Scale = Vec3{ node->scale[0], node->scale[1], node->scale[2] };
    }
    return transform;
}

// Builds the skeleton, in an order where every joint comes after its parent.
//
// glTF makes no such promise about skin->joints, and the sampling code wants
// it: composing model matrices in one forward pass is the difference between
// a flat loop and a recursive walk per actor per frame. Sorting here and
// remapping the vertices as they are read pays for it once, at load.
//
// Every skin in the file contributes: the bones are every node any skin names
// as a joint, and the palette has an entry for each joint of each skin, since
// two skins may bind one bone differently. Vertices index the palette with a
// byte, so it holds at most 255 entries; a skin that would take it past that
// is left out, and the meshes using it are drawn unskinned.
void ParseSkeleton(LoadContext& context)
{
    const cgltf_data* data = context.Data;
    context.SkinPalette.assign(data->skins_count, {});
    context.NodeToJoint.assign(data->nodes_count, -1);

    if (data->skins_count == 0)
    {
        return;
    }

    // Which skins fit, in the order the file lists them.
    std::vector<bool> usable(data->skins_count, false);
    size_t paletteSize = 0;
    for (cgltf_size skinIndex = 0; skinIndex < data->skins_count; ++skinIndex)
    {
        const cgltf_skin& skin = data->skins[skinIndex];
        if (skin.joints_count == 0)
        {
            continue;
        }
        if (paletteSize + skin.joints_count > 255)
        {
            LogMessage(LogLevel::Error, "asset",
                       "\"%s\": skin %zu would take the model past 255 joints, which is what vertices can "
                       "index. The meshes it binds are drawn unskinned.",
                       context.ModelPath.c_str(), static_cast<size_t>(skinIndex));
            continue;
        }
        usable[skinIndex] = true;
        paletteSize += skin.joints_count;
    }

    std::vector<bool> isJoint(data->nodes_count, false);
    for (cgltf_size skinIndex = 0; skinIndex < data->skins_count; ++skinIndex)
    {
        if (!usable[skinIndex])
        {
            continue;
        }
        const cgltf_skin& skin = data->skins[skinIndex];
        for (cgltf_size index = 0; index < skin.joints_count; ++index)
        {
            const cgltf_size nodeIndex = cgltf_node_index(data, skin.joints[index]);
            if (nodeIndex < data->nodes_count)
            {
                isJoint[nodeIndex] = true;
            }
        }
    }

    // Depth first from the roots, so parents come before children.
    // A joint whose parent is not a joint is a root as far as this skeleton is
    // concerned.
    std::vector<cgltf_size> order;
    std::vector<bool> placed(data->nodes_count, false);

    struct Walker
    {
        const cgltf_data* Data;
        const std::vector<bool>& IsJoint;
        std::vector<cgltf_size>& Order;
        std::vector<bool>& Placed;

        void Visit(const cgltf_node* node)
        {
            const cgltf_size nodeIndex = cgltf_node_index(Data, node);
            if (nodeIndex < Data->nodes_count && IsJoint[nodeIndex] && !Placed[nodeIndex])
            {
                Placed[nodeIndex] = true;
                Order.push_back(nodeIndex);
            }

            for (cgltf_size child = 0; child < node->children_count; ++child)
            {
                Visit(node->children[child]);
            }
        }
    };

    Walker walker{ data, isJoint, order, placed };
    for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex)
    {
        if (!isJoint[nodeIndex])
        {
            continue;
        }
        const cgltf_node* parent = data->nodes[nodeIndex].parent;
        const bool parentIsJoint = parent != nullptr && isJoint[cgltf_node_index(data, parent)];
        if (!parentIsJoint)
        {
            walker.Visit(&data->nodes[nodeIndex]);
        }
    }

    // A cycle would leave gaps; taking them in declaration order is wrong for
    // those but better than dropping them.
    for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex)
    {
        if (isJoint[nodeIndex] && !placed[nodeIndex])
        {
            placed[nodeIndex] = true;
            order.push_back(nodeIndex);
        }
    }

    ModelMesh& mesh = *context.Mesh;
    mesh.Joints.resize(order.size());
    for (size_t position = 0; position < order.size(); ++position)
    {
        context.NodeToJoint[order[position]] = static_cast<int>(position);
    }

    for (size_t position = 0; position < order.size(); ++position)
    {
        const cgltf_node* node = &data->nodes[order[position]];

        ModelJoint& joint = mesh.Joints[position];
        joint.Rest = LocalTransform(node);
        joint.Name = node->name != nullptr ? node->name : "";
        joint.Parent = node->parent != nullptr ? context.NodeToJoint[cgltf_node_index(data, node->parent)] : -1;
    }

    // The palette: each usable skin's joints in the order it lists them, with
    // the binding that skin gives each one.
    for (cgltf_size skinIndex = 0; skinIndex < data->skins_count; ++skinIndex)
    {
        if (!usable[skinIndex])
        {
            continue;
        }
        const cgltf_skin& skin = data->skins[skinIndex];

        std::vector<float> inverseBinds;
        const bool hasInverseBinds =
            skin.inverse_bind_matrices != nullptr && ReadFloats(skin.inverse_bind_matrices, 16, inverseBinds);

        std::vector<int>& palette = context.SkinPalette[skinIndex];
        palette.assign(skin.joints_count, 0);
        for (cgltf_size index = 0; index < skin.joints_count; ++index)
        {
            SkinJoint entry;
            entry.Bone = context.NodeToJoint[cgltf_node_index(data, skin.joints[index])];
            if (hasInverseBinds && (index + 1) * 16 <= inverseBinds.size())
            {
                std::memcpy(entry.InverseBind.M, &inverseBinds[index * 16], sizeof(float) * 16);
            }

            palette[index] = static_cast<int>(mesh.SkinJoints.size());
            mesh.SkinJoints.push_back(entry);
        }
    }

    LogMessage(LogLevel::Info, "asset", "\"%s\" has a skeleton of %zu joints across %zu skin%s.",
               context.ModelPath.c_str(), mesh.Joints.size(), static_cast<size_t>(data->skins_count),
               data->skins_count == 1 ? "" : "s");
}

// Turns glTF's samplers and channels into clips. A channel whose target is not
// a joint of this skin is skipped rather than guessed at: a clip that moves
// the camera has nothing to do with the skeleton.
void ParseAnimations(LoadContext& context)
{
    const cgltf_data* data = context.Data;
    ModelMesh& mesh = *context.Mesh;

    if (data->animations_count == 0 || (mesh.Joints.empty() && mesh.MorphTargets.empty()))
    {
        return;
    }

    mesh.Animations.reserve(data->animations_count);

    for (cgltf_size animationIndex = 0; animationIndex < data->animations_count; ++animationIndex)
    {
        const cgltf_animation& source = data->animations[animationIndex];

        ModelAnimation clip;
        clip.Name = source.name != nullptr ? source.name : ("clip " + std::to_string(animationIndex));

        for (cgltf_size channelIndex = 0; channelIndex < source.channels_count; ++channelIndex)
        {
            const cgltf_animation_channel& channel = source.channels[channelIndex];
            if (channel.target_node == nullptr || channel.sampler == nullptr)
            {
                continue;
            }

            const cgltf_size nodeIndex = cgltf_node_index(data, channel.target_node);
            AnimationChannel curve;

            switch (channel.target_path)
            {
            case cgltf_animation_path_type_translation:
                curve.Path = AnimationPath::Translation;
                break;
            case cgltf_animation_path_type_rotation:
                curve.Path = AnimationPath::Rotation;
                break;
            case cgltf_animation_path_type_scale:
                curve.Path = AnimationPath::Scale;
                break;
            case cgltf_animation_path_type_weights:
                curve.Path = AnimationPath::Weights;
                break;
            default:
                continue;
            }

            if (curve.Path == AnimationPath::Weights)
            {
                // The weights of the targets this node's mesh brought in.
                if (nodeIndex >= context.NodeToMorphs.size() || context.NodeToMorphs[nodeIndex].second <= 0)
                {
                    continue;
                }
                curve.FirstMorph = context.NodeToMorphs[nodeIndex].first;
                curve.MorphCount = context.NodeToMorphs[nodeIndex].second;
            }
            else
            {
                if (nodeIndex >= context.NodeToJoint.size() || context.NodeToJoint[nodeIndex] < 0)
                {
                    continue;
                }
                curve.Joint = context.NodeToJoint[nodeIndex];
            }

            switch (channel.sampler->interpolation)
            {
            case cgltf_interpolation_type_step:
                curve.Interpolation = AnimationInterpolation::Step;
                break;
            case cgltf_interpolation_type_cubic_spline:
                curve.Interpolation = AnimationInterpolation::CubicSpline;
                break;
            default:
                curve.Interpolation = AnimationInterpolation::Linear;
                break;
            }

            if (!ReadFloats(channel.sampler->input, 1, curve.Times) || curve.Times.empty())
            {
                continue;
            }

            if (curve.Path == AnimationPath::Weights)
            {
                // One scalar accessor holding every target's weight for every
                // key, three times over for a cubic spline.
                const size_t perKey = static_cast<size_t>(curve.MorphCount) *
                                      (curve.Interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
                if (!ReadFloats(channel.sampler->output, 1, curve.Values) ||
                    curve.Values.size() < curve.Times.size() * perKey)
                {
                    continue;
                }
            }
            else
            {
                const cgltf_size components = curve.Path == AnimationPath::Rotation ? 4 : 3;
                if (!ReadFloats(channel.sampler->output, components, curve.Values))
                {
                    continue;
                }
            }

            clip.Duration = std::max(clip.Duration, curve.Times.back());
            clip.Channels.push_back(std::move(curve));
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

// The displacements one mesh's primitives gather for its morph targets while
// they are read, one array per target, eight floats a vertex.
using MorphGather = std::vector<std::vector<float>>;

// skinIndex names the node's skin, or is -1 for a mesh that is not skinned.
// morphs is null for a mesh without morph targets.
void AddPrimitive(LoadContext& context, const cgltf_primitive& primitive, const float world[16], int skinIndex,
                  MorphGather* morphs)
{
    if (primitive.type != cgltf_primitive_type_triangles)
    {
        return;
    }

    const cgltf_accessor* positionAccessor = nullptr;
    const cgltf_accessor* normalAccessor = nullptr;
    const cgltf_accessor* uvAccessor = nullptr;
    const cgltf_accessor* jointAccessor = nullptr;
    const cgltf_accessor* weightAccessor = nullptr;

    for (cgltf_size attributeIndex = 0; attributeIndex < primitive.attributes_count; ++attributeIndex)
    {
        const cgltf_attribute& attribute = primitive.attributes[attributeIndex];
        if (attribute.type == cgltf_attribute_type_position)
        {
            positionAccessor = attribute.data;
        }
        else if (attribute.type == cgltf_attribute_type_normal)
        {
            normalAccessor = attribute.data;
        }
        else if (attribute.type == cgltf_attribute_type_texcoord && attribute.index == 0)
        {
            uvAccessor = attribute.data;
        }
        else if (attribute.type == cgltf_attribute_type_joints && attribute.index == 0)
        {
            jointAccessor = attribute.data;
        }
        else if (attribute.type == cgltf_attribute_type_weights && attribute.index == 0)
        {
            weightAccessor = attribute.data;
        }
    }

    std::vector<float> positions;
    if (!ReadFloats(positionAccessor, 3, positions))
    {
        return;
    }

    std::vector<float> normals;
    const bool hasNormals = ReadFloats(normalAccessor, 3, normals);

    std::vector<float> uvs;
    const bool hasUVs = ReadFloats(uvAccessor, 2, uvs);

    // A primitive is skinned only when its node has a usable skin and the
    // indices and weights are both there. JOINTS_0 means nothing without the
    // skin those indices refer to, and that must be the node's own skin;
    // indexing another skin's joints would tear the mesh apart.
    const std::vector<int>* palette =
        skinIndex >= 0 && static_cast<size_t>(skinIndex) < context.SkinPalette.size() &&
                !context.SkinPalette[static_cast<size_t>(skinIndex)].empty()
            ? &context.SkinPalette[static_cast<size_t>(skinIndex)]
            : nullptr;

    std::vector<float> jointIndices;
    std::vector<float> jointWeights;
    const bool skinned = palette != nullptr && ReadFloats(jointAccessor, 4, jointIndices) &&
                         ReadFloats(weightAccessor, 4, jointWeights);

    float normalMatrix[9];
    NormalMatrix(world, normalMatrix);

    // A mirroring transform turns the triangles inside out, so their winding
    // is reversed to keep the front faces facing out.
    const bool mirrored = Determinant3(world) < 0.0f;

    ModelMesh& mesh = *context.Mesh;
    const uint32_t baseVertex = static_cast<uint32_t>(mesh.Vertices.size());
    const uint32_t firstIndex = static_cast<uint32_t>(mesh.Indices.size());
    const size_t vertexCount = positions.size() / 3;

    for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        const float x = positions[vertexIndex * 3 + 0];
        const float y = positions[vertexIndex * 3 + 1];
        const float z = positions[vertexIndex * 3 + 2];

        ModelVertex vertex{};
        vertex.Position[0] = world[0] * x + world[4] * y + world[8] * z + world[12];
        vertex.Position[1] = world[1] * x + world[5] * y + world[9] * z + world[13];
        vertex.Position[2] = world[2] * x + world[6] * y + world[10] * z + world[14];

        if (hasNormals)
        {
            const float nx = normals[vertexIndex * 3 + 0];
            const float ny = normals[vertexIndex * 3 + 1];
            const float nz = normals[vertexIndex * 3 + 2];
            float n[3] = { normalMatrix[0] * nx + normalMatrix[1] * ny + normalMatrix[2] * nz,
                           normalMatrix[3] * nx + normalMatrix[4] * ny + normalMatrix[5] * nz,
                           normalMatrix[6] * nx + normalMatrix[7] * ny + normalMatrix[8] * nz };
            const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length > 1e-20f)
            {
                n[0] /= length;
                n[1] /= length;
                n[2] /= length;
            }
            std::memcpy(vertex.Normal, n, sizeof(n));
        }

        if (hasUVs)
        {
            vertex.UV[0] = uvs[vertexIndex * 2 + 0];
            vertex.UV[1] = uvs[vertexIndex * 2 + 1];
        }

        if (skinned)
        {
            QuantizeSkinWeights(&jointWeights[vertexIndex * 4], vertex.Weights);

            for (int slot = 0; slot < 4; ++slot)
            {
                const int skinJoint =
                    static_cast<int>(jointIndices[vertexIndex * 4 + static_cast<size_t>(slot)]);

                int joint = 0;
                if (skinJoint >= 0 && skinJoint < static_cast<int>(palette->size()))
                {
                    joint = (*palette)[static_cast<size_t>(skinJoint)];
                }

                vertex.Joints[slot] = static_cast<uint8_t>(joint);
            }
        }

        if (context.FirstVertex)
        {
            mesh.Minimum = Vec3{ vertex.Position[0], vertex.Position[1], vertex.Position[2] };
            mesh.Maximum = mesh.Minimum;
            context.FirstVertex = false;
        }

        mesh.Minimum.X = std::min(mesh.Minimum.X, vertex.Position[0]);
        mesh.Minimum.Y = std::min(mesh.Minimum.Y, vertex.Position[1]);
        mesh.Minimum.Z = std::min(mesh.Minimum.Z, vertex.Position[2]);
        mesh.Maximum.X = std::max(mesh.Maximum.X, vertex.Position[0]);
        mesh.Maximum.Y = std::max(mesh.Maximum.Y, vertex.Position[1]);
        mesh.Maximum.Z = std::max(mesh.Maximum.Z, vertex.Position[2]);

        mesh.Vertices.push_back(vertex);
    }

    // Every target the mesh has gets this primitive's displacements, zero
    // where the primitive does not move for that target, so each target's run
    // stays in step with the vertices. They are carried by the node's
    // transform as the vertices are: positions by its linear part, normals by
    // its normal matrix.
    if (morphs != nullptr)
    {
        for (size_t target = 0; target < morphs->size(); ++target)
        {
            std::vector<float> positionDeltas;
            std::vector<float> normalDeltas;
            if (target < primitive.targets_count)
            {
                const cgltf_morph_target& morph = primitive.targets[target];
                for (cgltf_size attributeIndex = 0; attributeIndex < morph.attributes_count; ++attributeIndex)
                {
                    const cgltf_attribute& attribute = morph.attributes[attributeIndex];
                    if (attribute.type == cgltf_attribute_type_position)
                    {
                        ReadFloats(attribute.data, 3, positionDeltas);
                    }
                    else if (attribute.type == cgltf_attribute_type_normal)
                    {
                        ReadFloats(attribute.data, 3, normalDeltas);
                    }
                }
            }
            const bool movesPositions = positionDeltas.size() >= vertexCount * 3;
            const bool movesNormals = normalDeltas.size() >= vertexCount * 3;

            std::vector<float>& out = (*morphs)[target];
            for (size_t vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
            {
                float p[3] = { 0.0f, 0.0f, 0.0f };
                float n[3] = { 0.0f, 0.0f, 0.0f };
                if (movesPositions)
                {
                    const float x = positionDeltas[vertexIndex * 3 + 0];
                    const float y = positionDeltas[vertexIndex * 3 + 1];
                    const float z = positionDeltas[vertexIndex * 3 + 2];
                    p[0] = world[0] * x + world[4] * y + world[8] * z;
                    p[1] = world[1] * x + world[5] * y + world[9] * z;
                    p[2] = world[2] * x + world[6] * y + world[10] * z;
                }
                if (movesNormals)
                {
                    const float x = normalDeltas[vertexIndex * 3 + 0];
                    const float y = normalDeltas[vertexIndex * 3 + 1];
                    const float z = normalDeltas[vertexIndex * 3 + 2];
                    n[0] = normalMatrix[0] * x + normalMatrix[1] * y + normalMatrix[2] * z;
                    n[1] = normalMatrix[3] * x + normalMatrix[4] * y + normalMatrix[5] * z;
                    n[2] = normalMatrix[6] * x + normalMatrix[7] * y + normalMatrix[8] * z;
                }
                out.insert(out.end(), { p[0], p[1], p[2], 0.0f, n[0], n[1], n[2], 0.0f });
            }
        }
    }

    auto PushTriangle = [&](uint32_t a, uint32_t b, uint32_t c) {
        mesh.Indices.push_back(baseVertex + a);
        mesh.Indices.push_back(baseVertex + (mirrored ? c : b));
        mesh.Indices.push_back(baseVertex + (mirrored ? b : c));
    };

    if (primitive.indices != nullptr)
    {
        for (cgltf_size i = 0; i + 2 < primitive.indices->count; i += 3)
        {
            PushTriangle(static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i)),
                         static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 1)),
                         static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i + 2)));
        }
    }
    else
    {
        // An unindexed primitive is a plain triangle list.
        for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
        {
            PushTriangle(i, i + 1, i + 2);
        }
    }

    if (!hasNormals)
    {
        GenerateNormals(mesh.Vertices, mesh.Indices, baseVertex, firstIndex);
    }

    ModelPart part;
    part.FirstIndex = firstIndex;
    part.IndexCount = static_cast<uint32_t>(mesh.Indices.size()) - firstIndex;
    ReadMaterial(context, primitive.material, part);

    if (part.IndexCount > 0)
    {
        mesh.Parts.push_back(part);
        context.AnyGeometry = true;
    }
}

void AddNode(LoadContext& context, const cgltf_node* node)
{
    if (node->mesh != nullptr)
    {
        // The node's full transform down from the scene root: glTF places
        // meshes through the node hierarchy, and an exporter commonly puts a
        // rotation or a unit scale there rather than in the vertices.
        float world[16];
        cgltf_node_transform_world(node, world);

        // Except for a skinned mesh, whose node transform glTF says to ignore:
        // its vertices are already in the skin's space, and the joint matrices
        // are what move them. Baking the node transform in as well would apply
        // it twice.
        int skinIndex = -1;
        if (node->skin != nullptr)
        {
            const cgltf_size index = cgltf_skin_index(context.Data, node->skin);
            if (index < context.SkinPalette.size() && !context.SkinPalette[index].empty())
            {
                skinIndex = static_cast<int>(index);
                const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
                std::memcpy(world, identity, sizeof(world));
            }
        }

        // Morph targets belong to the mesh, and their weights to the node, so
        // each node showing a mesh gets its own set: two faces from one mesh
        // can smile separately.
        const cgltf_mesh* source = node->mesh;
        size_t targetCount = source->weights_count;
        for (cgltf_size primitiveIndex = 0; primitiveIndex < source->primitives_count; ++primitiveIndex)
        {
            targetCount = std::max(targetCount, static_cast<size_t>(source->primitives[primitiveIndex].targets_count));
        }

        ModelMesh& mesh = *context.Mesh;
        const size_t firstTarget = mesh.MorphTargets.size();
        const uint32_t vertexStart = static_cast<uint32_t>(mesh.Vertices.size());
        MorphGather gather(targetCount);

        for (cgltf_size primitiveIndex = 0; primitiveIndex < source->primitives_count; ++primitiveIndex)
        {
            AddPrimitive(context, source->primitives[primitiveIndex], world, skinIndex,
                         targetCount > 0 ? &gather : nullptr);
        }

        const uint32_t vertexCount = static_cast<uint32_t>(mesh.Vertices.size()) - vertexStart;
        if (targetCount > 0 && vertexCount > 0)
        {
            const std::string meshName = source->name != nullptr ? source->name : "mesh";
            for (size_t target = 0; target < targetCount; ++target)
            {
                MorphTarget morph;
                morph.Name = target < source->target_names_count && source->target_names[target] != nullptr
                                 ? source->target_names[target]
                                 : meshName + "." + std::to_string(target);
                morph.DefaultWeight = target < node->weights_count    ? node->weights[target]
                                      : target < source->weights_count ? source->weights[target]
                                                                        : 0.0f;
                morph.VertexStart = vertexStart;
                morph.VertexCount = vertexCount;
                morph.DeltaOffset = static_cast<uint32_t>(mesh.MorphDeltas.size() / 4);
                mesh.MorphDeltas.insert(mesh.MorphDeltas.end(), gather[target].begin(), gather[target].end());
                mesh.MorphTargets.push_back(std::move(morph));
            }

            const cgltf_size nodeIndex = cgltf_node_index(context.Data, node);
            if (nodeIndex < context.NodeToMorphs.size())
            {
                context.NodeToMorphs[nodeIndex] = { static_cast<int>(firstTarget), static_cast<int>(targetCount) };
            }
        }
    }

    for (cgltf_size childIndex = 0; childIndex < node->children_count; ++childIndex)
    {
        AddNode(context, node->children[childIndex]);
    }
}

} // namespace

// The matrix that carries normals: the inverse transpose of the upper 3x3, so
// a node scaled unevenly still has normals perpendicular to its surfaces.
void NormalMatrix(const float world[16], float out[9])
{
    // Column-major: element (row r, column c) is world[c * 4 + r].
    const float a = world[0], b = world[4], c = world[8];
    const float d = world[1], e = world[5], f = world[9];
    const float g = world[2], h = world[6], i = world[10];

    const float cofactor[9] = { e * i - f * h, -(d * i - f * g), d * h - e * g,
                                -(b * i - c * h), a * i - c * g, -(a * h - b * g),
                                b * f - c * e, -(a * f - c * d), a * e - b * d };

    const float determinant = a * cofactor[0] + b * cofactor[1] + c * cofactor[2];
    const float inverse = std::fabs(determinant) > 1e-20f ? 1.0f / determinant : 0.0f;

    // The inverse transpose is the cofactor matrix over the determinant.
    // Stored row-major here: out[r * 3 + col].
    for (int index = 0; index < 9; ++index)
    {
        out[index] = cofactor[index] * inverse;
    }
}

float Determinant3(const float world[16])
{
    const float a = world[0], b = world[4], c = world[8];
    const float d = world[1], e = world[5], f = world[9];
    const float g = world[2], h = world[6], i = world[10];
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

Transform3 DecomposeNodeMatrix(const float m[16])
{
    Transform3 transform;

    transform.Position = Vec3{ m[12], m[13], m[14] };

    // Column lengths are the scale, and dividing them out leaves a pure
    // rotation to read the quaternion from.
    float columns[3][3];
    float scale[3];
    for (int column = 0; column < 3; ++column)
    {
        for (int row = 0; row < 3; ++row)
        {
            columns[column][row] = m[column * 4 + row];
        }
        scale[column] = std::sqrt(columns[column][0] * columns[column][0] +
                                  columns[column][1] * columns[column][1] +
                                  columns[column][2] * columns[column][2]);
        if (scale[column] > 1e-20f)
        {
            columns[column][0] /= scale[column];
            columns[column][1] /= scale[column];
            columns[column][2] /= scale[column];
        }
    }

    // A mirroring transform has a negative determinant, which no rotation
    // has; the sign goes on one axis of the scale instead.
    if (Determinant3(m) < 0.0f)
    {
        scale[0] = -scale[0];
        columns[0][0] = -columns[0][0];
        columns[0][1] = -columns[0][1];
        columns[0][2] = -columns[0][2];
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

    const float length = std::sqrt(rotation.X * rotation.X + rotation.Y * rotation.Y +
                                   rotation.Z * rotation.Z + rotation.W * rotation.W);
    if (length > 1e-20f)
    {
        rotation.X /= length;
        rotation.Y /= length;
        rotation.Z /= length;
        rotation.W /= length;
    }
    transform.Rotation = rotation;
    return transform;
}

// Quantized to a byte apiece and made to sum to exactly 255, so the shader can
// divide by 255 and trust the result. Handing the rounding error to the largest
// weight puts it where it is least visible.
void QuantizeSkinWeights(const float weights[4], uint8_t out[4])
{
    int quantized[4] = { 0, 0, 0, 0 };
    int total = 0;
    int largest = 0;

    float sum = 0.0f;
    for (int slot = 0; slot < 4; ++slot)
    {
        sum += std::max(0.0f, weights[slot]);
    }

    if (sum > 1e-8f)
    {
        for (int slot = 0; slot < 4; ++slot)
        {
            const float weight = std::max(0.0f, weights[slot]) / sum;
            quantized[slot] = static_cast<int>(weight * 255.0f + 0.5f);
            total += quantized[slot];
            if (quantized[slot] > quantized[largest])
            {
                largest = slot;
            }
        }
        quantized[largest] += 255 - total;
        quantized[largest] = std::clamp(quantized[largest], 0, 255);
    }

    for (int slot = 0; slot < 4; ++slot)
    {
        out[slot] = static_cast<uint8_t>(quantized[slot]);
    }
}

std::string ModelPathAt(const World3DState& state, int modelIndex)
{
    if (state.Models == nullptr || modelIndex < 0)
    {
        return std::string{};
    }

    const ModelMesh* mesh = state.Models->Get(modelIndex);
    return mesh != nullptr ? mesh->Path : std::string{};
}

bool ModelStore::Parse(const std::string& path, ModelMesh& outMesh)
{
    const std::string resolved = ResolveAsset(path, "asset");
    if (resolved.empty())
    {
        return false;
    }

    // glTF is read by cgltf; every other format by Assimp.
    std::string extension = std::filesystem::path(resolved).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".gltf" && extension != ".glb")
    {
        outMesh.Path = path;
        if (!ParseWithAssimp(resolved, outMesh))
        {
            return false;
        }
        size_t textured = 0;
        for (const ModelPart& part : outMesh.Parts)
        {
            textured += part.BaseColorTexture.IsValid() ? 1 : 0;
        }
        LogMessage(LogLevel::Info, "asset",
                   "Loaded \"%s\": %zu vertices, %zu triangles, %zu part%s (%zu textured), %zu joint%s, %zu "
                   "clip%s, %zu morph target%s.",
                   resolved.c_str(), outMesh.Vertices.size(), outMesh.Indices.size() / 3, outMesh.Parts.size(),
                   outMesh.Parts.size() == 1 ? "" : "s", textured, outMesh.Joints.size(),
                   outMesh.Joints.size() == 1 ? "" : "s", outMesh.Animations.size(),
                   outMesh.Animations.size() == 1 ? "" : "s", outMesh.MorphTargets.size(),
                   outMesh.MorphTargets.size() == 1 ? "" : "s");
        return true;
    }

    cgltf_options options{};
    cgltf_data* data = nullptr;

    if (cgltf_parse_file(&options, resolved.c_str(), &data) != cgltf_result_success)
    {
        LogMessage(LogLevel::Error, "asset", "\"%s\" is not a glTF file this build can read.", resolved.c_str());
        return false;
    }

    // Buffers may be external files or embedded data URIs; this resolves both.
    if (cgltf_load_buffers(&options, data, resolved.c_str()) != cgltf_result_success)
    {
        LogMessage(LogLevel::Error, "asset", "\"%s\" refers to buffers that could not be loaded.",
                   resolved.c_str());
        cgltf_free(data);
        return false;
    }

    ModelMesh& mesh = outMesh;
    mesh.Path = path;

    LoadContext context;
    context.Data = data;
    context.ModelPath = resolved;
    context.Mesh = &mesh;
    const std::filesystem::path parent = std::filesystem::path(resolved).parent_path();
    context.Directory = parent.empty() ? std::string() : parent.generic_string() + "/";

    // Before any geometry: a primitive remaps its joint indices as it reads
    // them, which needs the sorted skeleton to exist already.
    ParseSkeleton(context);
    context.NodeToMorphs.assign(data->nodes_count, { -1, 0 });

    // The default scene's hierarchy when there is one; otherwise every root
    // node, as glTF specifies for a file without a scene.
    const cgltf_scene* scene = data->scene != nullptr ? data->scene
                                                      : (data->scenes_count > 0 ? &data->scenes[0] : nullptr);
    if (scene != nullptr)
    {
        for (cgltf_size nodeIndex = 0; nodeIndex < scene->nodes_count; ++nodeIndex)
        {
            AddNode(context, scene->nodes[nodeIndex]);
        }
    }
    else
    {
        for (cgltf_size nodeIndex = 0; nodeIndex < data->nodes_count; ++nodeIndex)
        {
            if (data->nodes[nodeIndex].parent == nullptr)
            {
                AddNode(context, &data->nodes[nodeIndex]);
            }
        }
    }

    // A file with meshes but no nodes at all still has something to show.
    if (!context.AnyGeometry && data->nodes_count == 0)
    {
        const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
        {
            for (cgltf_size primitiveIndex = 0; primitiveIndex < data->meshes[meshIndex].primitives_count;
                 ++primitiveIndex)
            {
                AddPrimitive(context, data->meshes[meshIndex].primitives[primitiveIndex], identity, -1, nullptr);
            }
        }
    }

    ParseAnimations(context);

    cgltf_free(data);

    if (!context.AnyGeometry || mesh.Indices.empty())
    {
        LogMessage(LogLevel::Error, "asset", "\"%s\" contains no triangle geometry this build can draw.",
                   resolved.c_str());
        return false;
    }

    size_t textured = 0;
    for (const ModelPart& part : mesh.Parts)
    {
        textured += part.BaseColorTexture.IsValid() ? 1 : 0;
    }

    LogMessage(LogLevel::Info, "asset",
               "Loaded \"%s\": %zu vertices, %zu triangles, %zu part%s (%zu textured), %zu morph target%s.",
               resolved.c_str(), mesh.Vertices.size(), mesh.Indices.size() / 3, mesh.Parts.size(),
               mesh.Parts.size() == 1 ? "" : "s", textured, mesh.MorphTargets.size(),
               mesh.MorphTargets.size() == 1 ? "" : "s");

    return true;
}

int ModelStore::Load(const std::string& path)
{
    // Loading the same file twice hands back the same mesh, so two hundred
    // crates cost one parse and one set of GPU buffers.
    for (size_t index = 0; index < m_Meshes.size(); ++index)
    {
        if (m_Meshes[index]->Path == path)
        {
            return static_cast<int>(index);
        }
    }

    // The same contents under a different name are the same model. Checking
    // costs one read of a file that is about to be read anyway, and saves a
    // second parse and set of GPU buffers when one file is reachable under
    // several names.
    const std::string resolved = ResolveAsset(path, "asset", LogLevel::Warning);
    const uint64_t hash = resolved.empty() ? 0 : HashFileContents(resolved);

    // Except while hot reload is on, when the two want opposite things. A
    // shared mesh can only watch one of the files it came from, so editing
    // either of the others would change nothing and look like a broken
    // reload. Sharing is what a shipping build wants and reloading is what a
    // development build wants, so whichever is on decides.
    if (hash != 0 && !IsHotReloadEnabled())
    {
        for (size_t index = 0; index < m_Meshes.size(); ++index)
        {
            if (m_Meshes[index]->Ready && m_Meshes[index]->Watch.Hash == hash)
            {
                LogMessage(LogLevel::Info, "asset", "\"%s\" is \"%s\" again; the parse is shared.",
                           path.c_str(), m_Meshes[index]->Path.c_str());
                return static_cast<int>(index);
            }
        }
    }

    auto mesh = std::make_unique<ModelMesh>();
    if (!Parse(path, *mesh))
    {
        return -1;
    }

    if (!resolved.empty())
    {
        mesh->Watch.Path = resolved;
        mesh->Watch.Timestamp = FileTimestamp(resolved);
        mesh->Watch.Hash = hash;
    }

    m_Meshes.push_back(std::move(mesh));
    return static_cast<int>(m_Meshes.size() - 1);
}

int ModelStore::ReloadChanged(std::vector<int>& outChanged)
{
    int reloaded = 0;

    for (size_t index = 0; index < m_Meshes.size(); ++index)
    {
        std::unique_ptr<ModelMesh>& mesh = m_Meshes[index];
        if (!mesh->Ready || mesh->Watch.Path.empty() || !HasFileChanged(mesh->Watch))
        {
            continue;
        }

        // Parsed beside the live one instead of over it, so a half-written or
        // broken file logs an error and leaves what is on screen alone.
        ModelMesh replacement;
        if (!Parse(mesh->Path, replacement))
        {
            LogMessage(LogLevel::Error, "asset",
                       "Reload of \"%s\" failed; the model on screen is the one that worked.",
                       mesh->Path.c_str());
            continue;
        }

        const uint32_t revision = mesh->Revision;
        const FileWatch watch = mesh->Watch;

        *mesh = std::move(replacement);
        mesh->Watch = watch;
        mesh->Ready = true;
        mesh->Revision = revision + 1;
        outChanged.push_back(static_cast<int>(index));
        ++reloaded;

        LogMessage(LogLevel::Info, "asset", "Reloaded \"%s\": %zu vertices, %zu triangles.",
                   mesh->Path.c_str(), mesh->Vertices.size(), mesh->Indices.size() / 3);
    }

    return reloaded;
}

int ModelStore::LoadInBackground(const std::string& path)
{
    for (size_t index = 0; index < m_Meshes.size(); ++index)
    {
        if (m_Meshes[index]->Path == path)
        {
            return static_cast<int>(index);
        }
    }

    // The slot exists immediately, empty: whatever asked for it can carry on
    // this frame and show a placeholder until the parse lands.
    auto mesh = std::make_unique<ModelMesh>();
    mesh->Path = path;
    mesh->Ready = false;

    ModelMesh* slot = mesh.get();
    m_Meshes.push_back(std::move(mesh));

    // Parsed into a mesh of its own on the loading thread, then moved into the
    // slot on the thread that collects, where nothing else is reading it.
    auto staging = std::make_shared<ModelMesh>();
    auto parsed = std::make_shared<bool>(false);

    LoadQueue::Get().Submit([staging, parsed, path] { *parsed = ModelStore::Parse(path, *staging); },
                            [slot, staging, parsed, path] {
                                if (*parsed)
                                {
                                    const uint32_t revision = slot->Revision;
                                    *slot = std::move(*staging);
                                    slot->Path = path;
                                    slot->Ready = true;
                                    slot->Revision = revision + 1;

                                    // Watched from here, not from the loading
                                    // thread: hashing a file the main thread
                                    // is about to draw from belongs on the
                                    // thread that owns the slot.
                                    const std::string resolved =
                                        ResolveAsset(path, "asset", LogLevel::Warning);
                                    if (!resolved.empty())
                                    {
                                        BeginWatching(slot->Watch, resolved);
                                    }
                                }
                                else
                                {
                                    // It failed: the slot stays empty and
                                    // whatever waited on it keeps its
                                    // placeholder.
                                    slot->Ready = true;
                                    slot->Revision += 1;
                                }
                            });

    return static_cast<int>(m_Meshes.size() - 1);
}

const ModelMesh* ModelStore::Get(int index) const
{
    if (index < 0 || static_cast<size_t>(index) >= m_Meshes.size())
    {
        return nullptr;
    }
    return m_Meshes[static_cast<size_t>(index)].get();
}

ModelMesh* ModelStore::Get(int index)
{
    if (index < 0 || static_cast<size_t>(index) >= m_Meshes.size())
    {
        return nullptr;
    }
    return m_Meshes[static_cast<size_t>(index)].get();
}

} // namespace ludifex::detail

namespace ludifex
{

// The public switch, beside the machinery it turns on.
void SetAssetHotReload(bool enabled)
{
    detail::SetHotReloadEnabled(enabled);
}

bool IsAssetHotReloadEnabled()
{
    return detail::IsHotReloadEnabled();
}

} // namespace ludifex
