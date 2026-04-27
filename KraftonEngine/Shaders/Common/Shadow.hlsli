#ifndef SHADOW_HLSLI
#define SHADOW_HLSLI

#include "Common/Functions.hlsli"

cbuffer ShadowBuffer : register(b5)
{
    float4x4 LightViewProj;
    float ShadowBias;
    float ShadowSlopeBias;
    float bShadowEnabled;
    float _shadowPad;
}

Texture2D<float> ShadowMap : register(t5);
SamplerComparisonState ShadowSampler : register(s3);

float SampleShadow(float3 worldPos, float3 N)
{
    if (bShadowEnabled < 0.5f)
        return 1.0f;

    float4 CamClip = ApplyVP(worldPos);
    float4 psmClip = mul(CamClip, LightViewProj);
    float3 projectionPos = psmClip.xyz / psmClip.w;

    float2 shadowUV = projectionPos.xy * 0.5f + 0.5f;
    shadowUV.y = 1.0f - shadowUV.y;

    if (any(shadowUV < 0.0f) || any(shadowUV > 1.0f))
        return 1.0f;
    if (psmClip.z < 0.0f || psmClip.z > 1.0f)
        return 1.0f;
    
    float compareDepth = projectionPos.z - ShadowBias;

    return ShadowMap.SampleCmpLevelZero(ShadowSampler, shadowUV, compareDepth);
}

#endif
