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

    output.position = mul(mainClip, ShadowPSMLightViewProjection);
    return output;
}

void PS(ShadowVSOutput input)
{
}
