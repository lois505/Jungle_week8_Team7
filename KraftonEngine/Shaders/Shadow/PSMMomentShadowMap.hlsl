#include "Common/ConstantBuffers.hlsli"
#include "Common/VertexLayouts.hlsli"
#include "Common/ForwardLightData.hlsli"

#define SHADOW_FILTER_ESM 3

cbuffer PSMShadowBuffer : register(b2)
{
    float4x4 ShadowPSMMainViewProjection;
    float4x4 ShadowPSMLightViewProjection;
}

struct ShadowVSOutput
{
    float4 position : SV_POSITION;
};

ShadowVSOutput VS(VS_Input_PNCT input)
{
    ShadowVSOutput output;

    float4 world = mul(float4(input.position, 1.0f), Model);
    float4 mainClip = mul(world, ShadowPSMMainViewProjection);
    float invMainW = abs(mainClip.w) > 0.0001f ? rcp(mainClip.w) : 0.0f;
    float4 mainPP = float4(mainClip.xyz * invMainW, 1.0f);

    output.position = mul(mainPP, ShadowPSMLightViewProjection);
    return output;
}

float2 PS(ShadowVSOutput input) : SV_Target0
{
    float depth = saturate(input.position.z);

    if (ShadowFilterMode == SHADOW_FILTER_ESM)
    {
        const float exponent = 40.0f;
        return float2(exp(exponent * depth), 0.0f);
    }

    return float2(depth, depth * depth);
}
