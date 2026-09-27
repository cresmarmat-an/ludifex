// The procedural sky, for ludifex's own passes: the background it draws and
// the rays that miss everything when tracing.
//
// world_material.hlsli carries the same model as SkyRadiance and SkyAmbient,
// because a material must compile from that file alone. Keep the two alike.

#ifndef LUDIFEX_SKY_MODEL_HLSLI
#define LUDIFEX_SKY_MODEL_HLSLI

// zenith, horizon, ground: rgb linear; zenith.w the brightness, horizon.w the
// cosine of the sun disc's radius, ground.w the disc's brightness. toSun
// points at the sun and sunLight is its colour times its intensity.
float3 SkyModel(float3 direction, bool withSun, float4 zenith, float4 horizon, float4 ground, float3 toSun,
                float3 sunLight)
{
    float up = direction.y;
    float3 above = lerp(horizon.rgb, zenith.rgb, pow(saturate(up), 0.45));
    float3 below = lerp(horizon.rgb, ground.rgb, 1.0 - exp(-max(-up, 0.0) * 14.0));
    float3 color = up >= 0.0 ? above : below;

    if (withSun)
    {
        float facing = dot(direction, toSun);
        color += sunLight * (pow(saturate(facing), 400.0) * 0.6 + pow(saturate(facing), 12.0) * 0.08);
        float edge = max(1.0 - horizon.w, 1e-6) * 0.25;
        color += sunLight * ground.w * smoothstep(horizon.w - edge, horizon.w + edge, facing);
    }
    return color * zenith.w;
}

// The sky's light arriving at a surface facing `normal`.
float3 SkyAmbientModel(float3 normal, float4 zenith, float4 horizon, float4 ground)
{
    float3 above = lerp(horizon.rgb, zenith.rgb, 0.55);
    float3 below = lerp(horizon.rgb, ground.rgb, 0.85);
    float t = normal.y * 0.5 + 0.5;
    return lerp(below, above, t * t * (3.0 - 2.0 * t)) * zenith.w;
}

#endif // LUDIFEX_SKY_MODEL_HLSLI
