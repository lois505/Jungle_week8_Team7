#include "Common/ConstantBuffers.hlsli"
#include "Common/VertexLayouts.hlsli"

cbuffer PSMCB : register(b2)
{
    float4x4 PSMMatrix;
}

struct ShadowVS_Output
{
    float4 position : SV_POSITION;
    float depth : TEXCOORD0;
};

ShadowVS_Output VS(VS_Input_PNCTT input)
{
    ShadowVS_Output output;
    
    float4 worldPos = mul(float4(input.position, 1.0f), Model);
    output.position = mul(worldPos, PSMMatrix);
    
    output.depth = output.position.z / output.position.w;
    
    return output;    
}

void PS(ShadowVS_Output input)
{
    
}