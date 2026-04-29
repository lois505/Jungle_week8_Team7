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

    output.position = mul(mainClip, ShadowPSMLightViewProjection);
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
