#include "Common/ConstantBuffers.hlsli"
#include "Common/VertexLayouts.hlsli"
#include "Common/functions.hlsli"

cbuffer PSMBuffer : register(b2)
{
    float4x4 LightViewProj;
}

struct ShadowVS_Output
{
    float4 position : SV_POSITION;
};

ShadowVS_Output VS(VS_Input_PNCTT input)
{
    ShadowVS_Output output;
    float4 CamClip = ApplyMVP(input.position);
    output.position = mul(CamClip, LightViewProj);
    return output;
}

void PS(ShadowVS_Output input)
{
}
