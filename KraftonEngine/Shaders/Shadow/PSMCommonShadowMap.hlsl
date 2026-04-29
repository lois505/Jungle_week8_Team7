#include "Common/ConstantBuffers.hlsli"
#include "Common/VertexLayouts.hlsli"

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

void PS(ShadowVSOutput input)
{
}
