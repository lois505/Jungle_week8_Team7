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
    float4 clipDistance0 : SV_ClipDistance0;
    float2 clipDistance1 : SV_ClipDistance1;
};

ShadowVSOutput VS(VS_Input_PNCT input)
{
    ShadowVSOutput output;

    float4 world = mul(float4(input.position, 1.0f), Model);
    float4 mainClip = mul(world, ShadowPSMMainViewProjection);

    output.clipDistance0 = float4(
        mainClip.x + mainClip.w,
        mainClip.w - mainClip.x,
        mainClip.y + mainClip.w,
        mainClip.w - mainClip.y);
    output.clipDistance1 = float2(mainClip.z, mainClip.w - mainClip.z);

    float invW = rcp(max(mainClip.w, 1e-6f));
    float4 psmPosition = float4(mainClip.xyz * invW, 1.0f);

    output.position = mul(psmPosition, ShadowPSMLightViewProjection);
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
