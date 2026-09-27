// What the tracing passes share: the scene as the GPU sees it for tracing, how
// a ray finds what it hits, what a hit surface is made of, and the random
// numbers and directions a tracer draws.
//
// The scene is a two-level tree of boxes. Each distinct mesh (or part of one)
// has a tree of its own over its triangles, built once, in the mesh's own
// space; the top tree, rebuilt every frame, holds the instances, each a mesh
// placed in the world. A ray finds the instances whose boxes it crosses, is
// carried into each one's space, and walks that mesh's tree. Moving an
// object only rebuilds the top tree.
//
// Resources, in the order compute stages take them: three textures, whose
// first two each pass names for itself and whose third is the texture atlas;
// then six read-only buffers; then, declared by each pass, the textures it
// writes.

#ifndef LUDIFEX_RAY_COMMON_HLSLI
#define LUDIFEX_RAY_COMMON_HLSLI

#include "ray_uniforms.hlsli"
#include "sky_model.hlsli"

// Vulkan numbers compute resources from zero in each set; Metal numbers
// buffers after the uniform blocks and written textures after the sampled
// ones, across all sets; Direct3D reads the registers.
#if defined(__spirv__) && defined(LUDIFEX_METAL)
#define RAY_SLOT(vulkan, metal, set) [[vk::binding(metal, set)]]
#elif defined(__spirv__)
#define RAY_SLOT(vulkan, metal, set) [[vk::binding(vulkan, set)]]
#else
#define RAY_SLOT(vulkan, metal, set)
#endif

#ifdef __spirv__
#define RAY_COMBINED [[vk::combinedImageSampler]]
#else
#define RAY_COMBINED
#endif

static const float Pi = 3.14159265358979;

// --- the scene ---------------------------------------------------------------------

// A box in a tree. For an inner node, Min.w holds the index of its first
// child, the second following it, and Max.w holds zero; for a leaf, Min.w holds
// its first item and Max.w how many. Both as bits.
struct BvhNode
{
    float4 Min;
    float4 Max;
};

// A triangle in its mesh's own space: a corner and the two edges from it.
struct RayTriangle
{
    float4 Vertex;
    float4 Edge1;
    float4 Edge2;
};

// What shading a hit needs of the triangle: its three normals, packed two
// sixteen-bit numbers apiece, and its three texture coordinates.
struct RayTriangleShading
{
    uint4  Normals;
    float4 UV01;
    float4 UV2;
};

struct RayInstance
{
    float4 WorldToObject[3]; // the rows of a 3x4 matrix
    uint4  Geometry;         // x its tree's root in Nodes, y flags, z colour cell + 1, w emission cell + 1
    float4 BaseColor;        // linear rgb, and opacity
    float4 Surface;          // x roughness, y metallic
    float4 Emission;         // linear rgb
    float4 UVTransform;      // xy scale, zw offset
};

struct RayLight
{
    float4 PositionRange;
    float4 ColorIntensity;
};

static const uint RayFlagUnlit = 1;
static const uint RayFlagTransparent = 2;

// The atlas every texture a traced surface can show is copied into, sixteen
// cells of 256 texels a row. Mirrors the renderer's layout.
static const uint AtlasCellsPerRow = 16;
static const float AtlasCell = 1.0 / 16.0;
static const float AtlasTexel = 1.0 / 4096.0;

RAY_COMBINED RAY_SLOT(0, 0, 0) Texture2D<float4> Input0 : register(t0, space0);
RAY_COMBINED RAY_SLOT(0, 0, 0) SamplerState Input0Sampler : register(s0, space0);
RAY_COMBINED RAY_SLOT(1, 1, 0) Texture2D<float4> Input1 : register(t1, space0);
RAY_COMBINED RAY_SLOT(1, 1, 0) SamplerState Input1Sampler : register(s1, space0);
RAY_COMBINED RAY_SLOT(2, 2, 0) Texture2D<float4> Atlas : register(t2, space0);
RAY_COMBINED RAY_SLOT(2, 2, 0) SamplerState AtlasSampler : register(s2, space0);

RAY_SLOT(3, 1, 0) StructuredBuffer<BvhNode> TopNodes : register(t3, space0);
RAY_SLOT(4, 2, 0) StructuredBuffer<BvhNode> Nodes : register(t4, space0);
RAY_SLOT(5, 3, 0) StructuredBuffer<RayTriangle> Triangles : register(t5, space0);
RAY_SLOT(6, 4, 0) StructuredBuffer<RayTriangleShading> Shading : register(t6, space0);
RAY_SLOT(7, 5, 0) StructuredBuffer<RayInstance> Instances : register(t7, space0);
RAY_SLOT(8, 6, 0) StructuredBuffer<RayLight> Lights : register(t8, space0);

// --- finding what a ray hits ---------------------------------------------------------

struct RayHit
{
    float  T;            // how far along the ray, in units of its direction
    uint   Instance;
    uint   Triangle;
    float2 Barycentrics;
};

static const uint NoInstance = 0xffffffffu;

// Deep enough for any tree the renderer builds, which stops splitting before
// this.
#define RAY_STACK 32

float3 SafeInverse(float3 direction)
{
    float3 magnitude = max(abs(direction), 1e-12);
    float3 flip = float3(direction.x < 0.0 ? -1.0 : 1.0, direction.y < 0.0 ? -1.0 : 1.0,
                         direction.z < 0.0 ? -1.0 : 1.0);
    return flip / magnitude;
}

// Where the ray enters the box, if it does before tMax.
bool CrossesBox(float3 origin, float3 inverse, BvhNode node, float tMax, out float entry)
{
    float3 t0 = (node.Min.xyz - origin) * inverse;
    float3 t1 = (node.Max.xyz - origin) * inverse;
    float3 low = min(t0, t1);
    float3 high = max(t0, t1);
    entry = max(max(low.x, low.y), max(low.z, 0.0));
    float exit = min(min(high.x, high.y), min(high.z, tMax));
    return entry <= exit;
}

// Moller and Trumbore's test. Both faces count: a traced shadow falls from a
// plane seen from behind as well.
bool HitsTriangle(float3 origin, float3 direction, RayTriangle face, float tMax, out float t,
                  out float2 barycentrics)
{
    t = 0.0;
    barycentrics = 0.0;

    float3 p = cross(direction, face.Edge2.xyz);
    float determinant = dot(face.Edge1.xyz, p);
    if (determinant == 0.0)
    {
        return false;
    }
    float inverse = 1.0 / determinant;

    float3 s = origin - face.Vertex.xyz;
    float u = dot(s, p) * inverse;
    if (u < 0.0 || u > 1.0)
    {
        return false;
    }

    float3 q = cross(s, face.Edge1.xyz);
    float v = dot(direction, q) * inverse;
    if (v < 0.0 || u + v > 1.0)
    {
        return false;
    }

    t = dot(face.Edge2.xyz, q) * inverse;
    barycentrics = float2(u, v);
    return t > 0.0 && t < tMax;
}

// Walks one mesh's tree with a ray already in that mesh's space. Nearer
// children are taken first, and a box further than the nearest hit so far is
// passed over, so most of the tree is never visited.
bool TraceMesh(float3 origin, float3 direction, uint root, uint instance, bool anyHit, inout RayHit hit)
{
    float3 inverse = SafeInverse(direction);

    float entry;
    if (!CrossesBox(origin, inverse, Nodes[root], hit.T, entry))
    {
        return false;
    }

    uint stack[RAY_STACK];
    float stackEntry[RAY_STACK];
    uint count = 1;
    stack[0] = root;
    stackEntry[0] = entry;

    bool found = false;
    [loop] while (count > 0)
    {
        --count;
        if (stackEntry[count] >= hit.T)
        {
            continue;
        }

        BvhNode node = Nodes[stack[count]];
        uint first = asuint(node.Min.w);
        uint items = asuint(node.Max.w);

        if (items > 0)
        {
            for (uint item = 0; item < items; ++item)
            {
                float t;
                float2 barycentrics;
                if (HitsTriangle(origin, direction, Triangles[first + item], hit.T, t, barycentrics))
                {
                    hit.T = t;
                    hit.Instance = instance;
                    hit.Triangle = first + item;
                    hit.Barycentrics = barycentrics;
                    found = true;
                    if (anyHit)
                    {
                        return true;
                    }
                }
            }
            continue;
        }

        float entryLeft;
        float entryRight;
        bool left = CrossesBox(origin, inverse, Nodes[first], hit.T, entryLeft);
        bool right = CrossesBox(origin, inverse, Nodes[first + 1], hit.T, entryRight);

        if (left && right && count + 2 <= RAY_STACK)
        {
            bool leftFirst = entryLeft <= entryRight;
            stack[count] = leftFirst ? first + 1 : first;
            stackEntry[count] = leftFirst ? entryRight : entryLeft;
            ++count;
            stack[count] = leftFirst ? first : first + 1;
            stackEntry[count] = leftFirst ? entryLeft : entryRight;
            ++count;
        }
        else if (left && count < RAY_STACK)
        {
            stack[count] = first;
            stackEntry[count] = entryLeft;
            ++count;
        }
        else if (right && count < RAY_STACK)
        {
            stack[count] = first + 1;
            stackEntry[count] = entryRight;
            ++count;
        }
    }
    return found;
}

float3 IntoInstance(RayInstance instance, float3 value, float w)
{
    float4 point4 = float4(value, w);
    return float3(dot(instance.WorldToObject[0], point4), dot(instance.WorldToObject[1], point4),
                  dot(instance.WorldToObject[2], point4));
}

// The nearest thing along the ray before tMax, or, with anyHit, whether there
// is anything at all (all a shadow ray needs to know). Instances with any of
// the skip flags are passed through.
bool TraceScene(float3 origin, float3 direction, float tMax, uint skip, bool anyHit, out RayHit hit)
{
    hit.T = tMax;
    hit.Instance = NoInstance;
    hit.Triangle = 0;
    hit.Barycentrics = 0.0;

    if (Settings.w == 0)
    {
        return false;
    }

    float3 inverse = SafeInverse(direction);
    float entry;
    if (!CrossesBox(origin, inverse, TopNodes[0], hit.T, entry))
    {
        return false;
    }

    uint stack[RAY_STACK];
    float stackEntry[RAY_STACK];
    uint count = 1;
    stack[0] = 0;
    stackEntry[0] = entry;

    [loop] while (count > 0)
    {
        --count;
        if (stackEntry[count] >= hit.T)
        {
            continue;
        }

        BvhNode node = TopNodes[stack[count]];
        uint first = asuint(node.Min.w);
        uint items = asuint(node.Max.w);

        if (items > 0)
        {
            for (uint item = 0; item < items; ++item)
            {
                RayInstance instance = Instances[first + item];
                if ((instance.Geometry.y & skip) != 0)
                {
                    continue;
                }

                // The direction is carried over unnormalized, so a distance
                // along it means the same inside the instance as out here.
                float3 localOrigin = IntoInstance(instance, origin, 1.0);
                float3 localDirection = IntoInstance(instance, direction, 0.0);
                if (TraceMesh(localOrigin, localDirection, instance.Geometry.x, first + item, anyHit, hit) && anyHit)
                {
                    return true;
                }
            }
            continue;
        }

        float entryLeft;
        float entryRight;
        bool left = CrossesBox(origin, inverse, TopNodes[first], hit.T, entryLeft);
        bool right = CrossesBox(origin, inverse, TopNodes[first + 1], hit.T, entryRight);

        if (left && right && count + 2 <= RAY_STACK)
        {
            bool leftFirst = entryLeft <= entryRight;
            stack[count] = leftFirst ? first + 1 : first;
            stackEntry[count] = leftFirst ? entryRight : entryLeft;
            ++count;
            stack[count] = leftFirst ? first : first + 1;
            stackEntry[count] = leftFirst ? entryLeft : entryRight;
            ++count;
        }
        else if (left && count < RAY_STACK)
        {
            stack[count] = first;
            stackEntry[count] = entryLeft;
            ++count;
        }
        else if (right && count < RAY_STACK)
        {
            stack[count] = first + 1;
            stackEntry[count] = entryRight;
            ++count;
        }
    }
    return hit.Instance != NoInstance;
}

// Whether anything stands between the point and a distance along direction.
bool Blocked(float3 origin, float3 direction, float distance, uint skip)
{
    RayHit hit;
    return TraceScene(origin, direction, distance, skip, true, hit);
}

// --- what a hit is made of --------------------------------------------------------------

struct HitSurface
{
    float3 Position;
    float3 Normal;      // for shading, facing back along the ray
    float3 Geometric;   // the triangle's own, facing back along the ray
    float3 Albedo;      // linear
    float  Opacity;
    float  Roughness;
    float  Metallic;
    float3 Emission;
    uint   Flags;
};

float3 DecodeNormal(uint packed)
{
    int2 bits = int2(int(packed << 16) >> 16, int(packed) >> 16);
    float2 e = max(float2(bits) / 32767.0, -1.0);
    float3 normal = float3(e, 1.0 - abs(e.x) - abs(e.y));
    float fold = saturate(-normal.z);
    normal.x += normal.x >= 0.0 ? -fold : fold;
    normal.y += normal.y >= 0.0 ? -fold : fold;
    return normalize(normal);
}

// A cell of the atlas, repeated the way the texture repeats on the surface.
float3 AtlasColor(uint cellPlusOne, float2 uv)
{
    if (cellPlusOne == 0)
    {
        return 1.0;
    }
    uint cell = cellPlusOne - 1;
    float2 corner = float2(cell % AtlasCellsPerRow, cell / AtlasCellsPerRow) * AtlasCell;
    float2 inside = frac(uv);
    float2 at = corner + AtlasTexel * 0.5 + inside * (AtlasCell - AtlasTexel);
    return Atlas.SampleLevel(AtlasSampler, at, 0).rgb;
}

// Normals go to the world by the inverse transpose, which for the world-to-
// object matrix is its transpose.
float3 NormalToWorld(RayInstance instance, float3 normal)
{
    return normalize(instance.WorldToObject[0].xyz * normal.x + instance.WorldToObject[1].xyz * normal.y +
                     instance.WorldToObject[2].xyz * normal.z);
}

HitSurface SurfaceAt(RayHit hit, float3 origin, float3 direction)
{
    RayInstance instance = Instances[hit.Instance];
    RayTriangle face = Triangles[hit.Triangle];
    RayTriangleShading shading = Shading[hit.Triangle];

    float3 weights = float3(1.0 - hit.Barycentrics.x - hit.Barycentrics.y, hit.Barycentrics);

    float3 normal = DecodeNormal(shading.Normals.x) * weights.x + DecodeNormal(shading.Normals.y) * weights.y +
                    DecodeNormal(shading.Normals.z) * weights.z;
    float3 geometric = cross(face.Edge1.xyz, face.Edge2.xyz);

    HitSurface surface;
    surface.Position = origin + direction * hit.T;
    surface.Geometric = NormalToWorld(instance, geometric);
    surface.Normal = NormalToWorld(instance, normal);

    // Seen from behind, a face turns to face the ray; a smoothed normal that
    // still points away is replaced by the face's own.
    if (dot(surface.Geometric, direction) > 0.0)
    {
        surface.Geometric = -surface.Geometric;
        surface.Normal = -surface.Normal;
    }
    if (dot(surface.Normal, direction) >= 0.0)
    {
        surface.Normal = surface.Geometric;
    }

    float2 uv = shading.UV01.xy * weights.x + shading.UV01.zw * weights.y + shading.UV2.xy * weights.z;
    uv = uv * instance.UVTransform.xy + instance.UVTransform.zw;

    surface.Albedo = instance.BaseColor.rgb * AtlasColor(instance.Geometry.z, uv);
    surface.Opacity = instance.BaseColor.a;
    surface.Roughness = instance.Surface.x;
    surface.Metallic = instance.Surface.y;
    surface.Emission = instance.Emission.rgb * (instance.Geometry.w != 0 ? AtlasColor(instance.Geometry.w, uv) : 1.0);
    surface.Flags = instance.Geometry.y;
    return surface;
}

// A point just off a surface, so a ray leaving it does not find the surface
// itself. The push grows with the distance from the origin, as the precision
// of a float does.
float3 LeaveSurface(float3 position, float3 normal)
{
    float scale = max(max(abs(position.x), abs(position.y)), abs(position.z));
    return position + normal * (1e-3 + scale * 2e-5);
}

// --- light ---------------------------------------------------------------------------------

float3 ToSun()
{
    return normalize(-SunDirection.xyz);
}

bool SkyLightsScene()
{
    return AmbientColor.w > 0.5;
}

float3 SkyAlong(float3 direction, bool withSun)
{
    return SkyModel(direction, withSun, SkyZenith, SkyHorizon, SkyGround, ToSun(), SunLight.rgb);
}

// The ambient light a surface facing `normal` receives, as the rasterizer has
// it: the sky's, or the ambient colour, full from above and less from below.
float3 AmbientFrom(float3 normal)
{
    if (SkyLightsScene())
    {
        return SkyAmbientModel(normal, SkyZenith, SkyHorizon, SkyGround);
    }
    return AmbientColor.rgb * lerp(0.55, 1.0, normal.y * 0.5 + 0.5);
}

// The light arriving along a ray that escapes the world: the sky without the
// sun, whose light is gathered by aiming at it, or the ambient light.
float3 EnvironmentAlong(float3 direction)
{
    return SkyLightsScene() ? SkyAlong(direction, false) : AmbientFrom(direction);
}

float3 FresnelSchlick(float3 f0, float cosine)
{
    return f0 + (1.0 - f0) * pow(1.0 - saturate(cosine), 5.0);
}

// The rasterizer's lighting for one light, so traced and rasterized surfaces
// agree: GGX with height-correlated Smith visibility and Schlick's Fresnel, in
// the same units, where a light of intensity 1 lights a white surface to 1.
float3 EvaluateLight(float3 normal, float3 toView, float3 toLight, float3 diffuseColor, float3 f0, float roughness)
{
    float nl = saturate(dot(normal, toLight));
    if (nl <= 0.0)
    {
        return 0.0;
    }

    float3 halfway = normalize(toView + toLight);
    float nv = max(dot(normal, toView), 1e-4);
    float nh = saturate(dot(normal, halfway));
    float vh = saturate(dot(toView, halfway));

    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float d = nh * nh * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 / (Pi * d * d);

    float lambdaV = nl * sqrt(nv * nv * (1.0 - alpha2) + alpha2);
    float lambdaL = nv * sqrt(nl * nl * (1.0 - alpha2) + alpha2);
    float visibility = 0.5 / max(lambdaV + lambdaL, 1e-5);

    float3 fresnel = FresnelSchlick(f0, vh);
    float3 specular = distribution * visibility * fresnel * Pi;
    float3 diffuse = diffuseColor * (1.0 - fresnel);
    return (diffuse + specular) * nl;
}

// The same falloff the rasterizer gives a point light: inverse square,
// windowed to reach exactly zero at its range.
float PointLightFalloff(float distanceSquared, float range)
{
    float ratio = distanceSquared / (range * range);
    float window = saturate(1.0 - ratio * ratio);
    return window * window / (distanceSquared + 1.0);
}

// --- random numbers and directions ---------------------------------------------------------

uint SeedFor(uint2 pixel, uint frame)
{
    uint seed = pixel.x * 1973u + pixel.y * 9277u + frame * 26699u;
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return seed | 1u;
}

// O'Neill's PCG: a well-spread number from 0 up to 1, advancing the state.
float NextRandom(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    word = (word >> 22u) ^ word;
    return float(word) * (1.0 / 4294967296.0);
}

float2 NextRandom2(inout uint state)
{
    float a = NextRandom(state);
    float b = NextRandom(state);
    return float2(a, b);
}

void Basis(float3 normal, out float3 tangent, out float3 bitangent)
{
    float flip = normal.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (flip + normal.z);
    float b = normal.x * normal.y * a;
    tangent = float3(1.0 + flip * normal.x * normal.x * a, flip * b, -flip * normal.x);
    bitangent = float3(b, flip + normal.y * normal.y * a, -normal.y);
}

// Directions over the hemisphere, more of them near the normal, in proportion
// to how much light from each direction a matte surface receives.
float3 CosineDirection(float3 normal, float2 u)
{
    float3 tangent;
    float3 bitangent;
    Basis(normal, tangent, bitangent);
    float radius = sqrt(u.x);
    float angle = 2.0 * Pi * u.y;
    return normalize(tangent * (radius * cos(angle)) + bitangent * (radius * sin(angle)) +
                     normal * sqrt(max(1.0 - u.x, 0.0)));
}

// Directions spread evenly across a cone: toward a random point of the sun's
// disc, which makes traced shadows soft.
float3 ConeDirection(float3 axis, float cosineMax, float2 u)
{
    float3 tangent;
    float3 bitangent;
    Basis(axis, tangent, bitangent);
    float cosine = lerp(1.0, cosineMax, u.x);
    float sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    float angle = 2.0 * Pi * u.y;
    return normalize(tangent * (sine * cos(angle)) + bitangent * (sine * sin(angle)) + axis * cosine);
}

// A microfacet normal a GGX surface of roughness alpha shows to the viewer,
// drawn in proportion to how visible each is (Heitz, 2018). Local space, with
// the surface normal along z.
float3 SampleVisibleNormal(float3 view, float alpha, float2 u)
{
    float3 stretched = normalize(float3(alpha * view.x, alpha * view.y, view.z));
    float lengthSquared = stretched.x * stretched.x + stretched.y * stretched.y;
    float3 t1 = lengthSquared > 0.0 ? float3(-stretched.y, stretched.x, 0.0) * rsqrt(lengthSquared)
                                    : float3(1.0, 0.0, 0.0);
    float3 t2 = cross(stretched, t1);

    float radius = sqrt(u.x);
    float angle = 2.0 * Pi * u.y;
    float p1 = radius * cos(angle);
    float p2 = radius * sin(angle);
    float s = 0.5 * (1.0 + stretched.z);
    p2 = (1.0 - s) * sqrt(max(1.0 - p1 * p1, 0.0)) + s * p2;

    float3 normal = p1 * t1 + p2 * t2 + sqrt(max(1.0 - p1 * p1 - p2 * p2, 0.0)) * stretched;
    return normalize(float3(alpha * normal.x, alpha * normal.y, max(normal.z, 0.0)));
}

// A direction a glossy surface reflects the viewer's ray into, drawn from its
// GGX lobe. World space.
float3 GlossyDirection(float3 normal, float3 toView, float roughness, float2 u)
{
    float3 tangent;
    float3 bitangent;
    Basis(normal, tangent, bitangent);
    float3 view = float3(dot(toView, tangent), dot(toView, bitangent), dot(toView, normal));
    float alpha = max(roughness * roughness, 0.002);
    float3 micro = SampleVisibleNormal(view, alpha, u);
    float3 halfway = tangent * micro.x + bitangent * micro.y + normal * micro.z;
    return reflect(-toView, halfway);
}

// Smith's masking for GGX, one direction.
float SmithLambda(float cosine, float alpha)
{
    float cosine2 = max(cosine * cosine, 1e-6);
    float tangent2 = (1.0 - cosine2) / cosine2;
    return 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * tangent2));
}

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

#endif // LUDIFEX_RAY_COMMON_HLSLI
