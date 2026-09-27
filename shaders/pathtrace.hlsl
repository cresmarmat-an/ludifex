// The path tracer: each pixel is traced back into the world from surface to
// surface, gathering the light that travels the other way. At each bounce it
// samples the sun and one point light directly, so small bright lights are
// found far more often than by chance, then continues in a direction drawn
// from the surface's response: anywhere over the hemisphere for matte
// surfaces, toward the highlight for glossy ones. Emissive surfaces light the
// scene when a path hits them.
//
// Each frame adds its samples to a running sum, so a still image converges.
//
// Input0 the sum so far, Input1 the backdrop image, when there is one.

#include "ray_common.hlsli"

// The running sum: light in rgb, the samples it holds in alpha. Then the
// image, the sum divided out, which the rest of the frame reads.
[[vk::image_format("rgba32f")]] RAY_SLOT(0, 3, 1) RWTexture2D<float4> Sum : register(u0, space1);
[[vk::image_format("rgba16f")]] RAY_SLOT(1, 4, 1) RWTexture2D<float4> Image : register(u1, space1);

// What a camera ray that hits nothing shows: the backdrop image, the sky
// with its sun, or the flat sky colour, as the rasterizer draws them.
float3 Background(float2 uv, float3 direction)
{
    if (SkyColor.w > 0.5)
    {
        return Input1.SampleLevel(Input1Sampler, uv * Backdrop.xy + Backdrop.zw, 0).rgb;
    }
    if (SkyLightsScene())
    {
        return SkyAlong(direction, true);
    }
    return SkyColor.rgb;
}

// The light arriving at a surface from the sun and from one point light chosen
// at random, weighted so that on average it accounts for all of them.
float3 DirectLight(HitSurface surface, float3 toView, float3 diffuse, float3 f0, inout uint seed)
{
    float3 light = 0.0;
    float3 origin = LeaveSurface(surface.Position, surface.Geometric);
    const uint skip = RayFlagTransparent | RayFlagUnlit;

    if (any(SunLight.rgb > 0.0))
    {
        float3 toSun = ConeDirection(ToSun(), SunDirection.w, NextRandom2(seed));
        if (dot(surface.Normal, toSun) > 0.0 && !Blocked(origin, toSun, 1e5, skip))
        {
            light += EvaluateLight(surface.Normal, toView, toSun, diffuse, f0, surface.Roughness) * SunLight.rgb;
        }
    }

    uint lights = Settings.z;
    if (lights > 0)
    {
        uint index = min(uint(NextRandom(seed) * float(lights)), lights - 1);
        RayLight chosen = Lights[index];
        float3 offset = chosen.PositionRange.xyz - surface.Position;
        float distance2 = dot(offset, offset);
        float falloff = PointLightFalloff(distance2, chosen.PositionRange.w);
        if (falloff > 0.0)
        {
            float distance = sqrt(distance2);
            float3 toLight = offset / max(distance, 1e-6);
            if (dot(surface.Normal, toLight) > 0.0 && !Blocked(origin, toLight, distance * 0.999, skip))
            {
                light += EvaluateLight(surface.Normal, toView, toLight, diffuse, f0, surface.Roughness) *
                         chosen.ColorIntensity.rgb * chosen.ColorIntensity.w * falloff * float(lights);
            }
        }
    }
    return light;
}

float3 TracePath(float2 uv, inout uint seed)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 nearPoint = mul(InverseViewProjection, float4(ndc, 0.0, 1.0));
    float4 farPoint = mul(InverseViewProjection, float4(ndc, 1.0, 1.0));
    float3 origin = nearPoint.xyz / nearPoint.w;
    float3 direction = normalize(farPoint.xyz / farPoint.w - origin);

    float3 radiance = 0.0;
    float3 throughput = 1.0;
    float firstDistance = -1.0;
    uint bounce = 0;
    uint passes = 0;

    [loop] while (true)
    {
        RayHit hit;
        if (!TraceScene(origin, direction, 1e6, 0, false, hit))
        {
            radiance += throughput * (bounce == 0 ? Background(uv, direction) : EnvironmentAlong(direction));
            break;
        }

        HitSurface surface = SurfaceAt(hit, origin, direction);

        // A translucent surface lets a path through in proportion to how
        // clear it is. The path carries on as it was, and that costs no
        // bounce.
        if ((surface.Flags & RayFlagTransparent) != 0 && NextRandom(seed) > surface.Opacity && passes < 8)
        {
            ++passes;
            origin = surface.Position + direction * 1e-3;
            continue;
        }

        if (bounce == 0)
        {
            firstDistance = length(surface.Position - CameraPosition.xyz);
        }

        if ((surface.Flags & RayFlagUnlit) != 0)
        {
            radiance += throughput * (surface.Albedo + surface.Emission);
            break;
        }

        radiance += throughput * surface.Emission;

        float3 toView = -direction;
        float3 diffuse = surface.Albedo * (1.0 - surface.Metallic);
        float3 f0 = lerp(float3(0.04, 0.04, 0.04), surface.Albedo, surface.Metallic);
        float roughness = clamp(surface.Roughness, 0.045, 1.0);
        surface.Roughness = roughness;

        radiance += throughput * DirectLight(surface, toView, diffuse, f0, seed);

        if (bounce >= Progress.z)
        {
            break;
        }

        // Onward: into the highlight, or anywhere over the hemisphere, chosen
        // in proportion to how much each is likely to carry.
        float facing = max(dot(surface.Normal, toView), 1e-4);
        float3 fresnel = FresnelSchlick(f0, facing);
        float glossyShare = clamp(lerp(Luminance(fresnel), 1.0, surface.Metallic), 0.05, 0.95);

        float3 next;
        float3 weight;
        if (NextRandom(seed) < glossyShare)
        {
            next = GlossyDirection(surface.Normal, toView, roughness, NextRandom2(seed));
            float lightFacing = dot(surface.Normal, next);
            if (lightFacing <= 0.0)
            {
                break;
            }
            float alpha = roughness * roughness;
            float3 halfway = normalize(toView + next);
            float lambdaView = SmithLambda(facing, alpha);
            float lambdaLight = SmithLambda(lightFacing, alpha);
            weight = FresnelSchlick(f0, dot(toView, halfway)) * (1.0 + lambdaView) /
                     (1.0 + lambdaView + lambdaLight) / glossyShare;
        }
        else
        {
            next = CosineDirection(surface.Normal, NextRandom2(seed));
            weight = diffuse * (1.0 - fresnel) / (1.0 - glossyShare);
        }

        throughput *= weight;

        // Past a few bounces, a path that carries little light is ended at
        // random, and the ones that survive carry more to make up for it.
        if (bounce >= 2)
        {
            float survive = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (NextRandom(seed) > survive)
            {
                break;
            }
            throughput /= survive;
        }

        origin = LeaveSurface(surface.Position, surface.Geometric);
        direction = next;
        ++bounce;
    }

    if (firstDistance >= 0.0 && FogColor.w > 0.5)
    {
        float amount = saturate((firstDistance - FogParams.x) / max(FogParams.y - FogParams.x, 1e-3));
        radiance = lerp(radiance, FogColor.rgb, amount);
    }
    return radiance;
}

[numthreads(8, 8, 1)]
void PathTraceMain(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= Size.xy))
    {
        return;
    }

    float3 gathered = 0.0;
    uint samples = max(Progress.y, 1u);
    for (uint index = 0; index < samples; ++index)
    {
        uint seed = SeedFor(id.xy, Progress.x + index);

        // A different point of the pixel each sample, which is the path
        // tracer's anti-aliasing.
        float2 uv = (float2(id.xy) + NextRandom2(seed)) * Size.zw;
        float3 radiance = TracePath(uv, seed);

        // One extreme sample (a path that reached the sun through a small gap)
        // would otherwise speckle the image for hundreds of frames.
        float peak = max(radiance.r, max(radiance.g, radiance.b));
        if (peak > 32.0)
        {
            radiance *= 32.0 / peak;
        }
        if (any(isnan(radiance)) || any(isinf(radiance)))
        {
            radiance = 0.0;
        }
        gathered += radiance;
    }

    float2 centre = (float2(id.xy) + 0.5) * Size.zw;
    float4 sum = Progress.x > 0 ? Input0.SampleLevel(Input0Sampler, centre, 0) : 0.0;
    sum += float4(gathered, float(samples));

    Sum[id.xy] = sum;
    Image[id.xy] = float4(sum.rgb / max(sum.a, 1.0), 1.0);
}
