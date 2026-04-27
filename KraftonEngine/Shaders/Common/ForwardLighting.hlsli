#ifndef FORWARD_LIGHTING_HLSLI
#define FORWARD_LIGHTING_HLSLI

#include "Common/ForwardLightData.hlsli"
#include "Common/SystemSamplers.hlsli"
#include "Common/SystemResources.hlsli"

#pragma region __FILTERING_MODE__

#define NONE 0
#define PCF 1
#define VSM 2
#define ESM 3

#pragma endregion

float SampleShadowAtlas(float2 uv)
{
    return ShadowMapAtlasTexture.SampleLevel(PointClampSampler, uv, 0).r;
}

//  PCF (주변 픽셀을 다 더하고 평균내어 결정)
//  rect : xy(top-left), zw(bottom-right)
float SampleShadowAtlasPCF(float4 rect,float2 localUV, float currentDepth, float bias, float2 atlasTileSize)
{
    //  output means visibility
    float output = 0.0f;
    
    float2 texelInTile = 1.0f / atlasTileSize;
    
    
    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            float2 sampleLocalUV = localUV + float2(x,y) * texelInTile;
            
            sampleLocalUV = clamp(sampleLocalUV, texelInTile * 0.5f, 1.0f - texelInTile * 0.5f);
            
            float2 uv = rect.xy + sampleLocalUV * rect.zw;
            
            float storedDepth = ShadowMapAtlasTexture.SampleLevel(PointClampSampler, uv, 0).r;
            
            output += (currentDepth + bias >= storedDepth) ? 1.0f : 0.0f;
        }
    }
    
    //  Average
    return output / 9.0f;
}

float2 ProjectToShadowUV(float4 lightClip)
{
    float3 ndc = lightClip.xyz / max(lightClip.w, 0.0001f);
    return float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
}

bool IsValidShadowRect(float4 rect)
{
    return rect.z > 0.0f && rect.w > 0.0f;
}


float CalcShadowFromView(FLocalShadowInfo shadow, uint viewIndex, float3 worldPos)
{
    float4 rect = shadow.AtlasRect[viewIndex];
    if (!IsValidShadowRect(rect))
    {
        return 1.0f;
    }

    float4 lightClip = mul(float4(worldPos, 1.0f), shadow.LightViewProj[viewIndex]);
    if (lightClip.w <= 0.0f)
    {
        return 1.0f;
    }

    float3 ndc = lightClip.xyz / lightClip.w;
    if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < 0.0f || ndc.z > 1.0f)
    {
        return 1.0f;
    }

    float2 localUV = float2(ndc.x * 0.5f + 0.5f, -ndc.y * 0.5f + 0.5f);
    uint atlasWidth;
    uint atlasHeight;
    ShadowMapAtlasTexture.GetDimensions(atlasWidth, atlasHeight);
    float2 tileSize = max(rect.zw * float2(atlasWidth, atlasHeight), float2(1.0f, 1.0f));
    float2 halfTexelInTile = 0.5f / tileSize;
    localUV = clamp(localUV, halfTexelInTile, 1.0f - halfTexelInTile);

    float2 atlasUV = rect.xy + localUV * rect.zw;
    
    switch (ShadowFilterMode)
    {
    case PCF :
        {
            return SampleShadowAtlasPCF(rect, localUV, ndc.z, shadow.Bias, tileSize);    
        }
    case NONE :
    default:
        {
            float storedDepth = SampleShadowAtlas(atlasUV);

            // This renderer uses reversed depth for shadow maps: clear 0, DepthGreaterEqual.
            return (ndc.z + shadow.Bias >= storedDepth) ? 1.0f : 0.0f;   
        }
    }
}

uint SelectPointShadowFace(float3 lightToPixel)
{
    float3 a = abs(lightToPixel);
    if (a.x >= a.y && a.x >= a.z)
    {
        return lightToPixel.x >= 0.0f ? 0 : 1;
    }
    if (a.y >= a.x && a.y >= a.z)
    {
        return lightToPixel.y >= 0.0f ? 2 : 3;
    }
    return lightToPixel.z >= 0.0f ? 4 : 5;
}

float CalcLocalShadow(uint lightIndex, FLightInfo light, float3 worldPos)
{
    FLocalShadowInfo shadow = LocalLights[lightIndex];
    if (shadow.CastShadow == 0)
    {
        return 1.0f;
    }

    if (shadow.ShadowType == LIGHT_TYPE_SPOT)
    {
        return CalcShadowFromView(shadow, 0, worldPos);
    }

    if (shadow.ShadowType == LIGHT_TYPE_POINT)
    {
        uint faceIndex = SelectPointShadowFace(worldPos - light.Position);
        return CalcShadowFromView(shadow, faceIndex, worldPos);
    }

    return 1.0f;
}

float CalcAttenuation(float dist, float radius, float falloff)
{
    float ratio = saturate(dist / max(radius, 0.0001f));
    return pow(1.0f - ratio, falloff);
}

float3 CalcAmbient(float3 lightColor, float intensity)
{
    return lightColor * intensity;
}

float3 CalcDirectionalDiffuse(float3 lightColor, float3 lightDir, float intensity, float3 N)
{
    float NdotL = saturate(dot(N, -lightDir));
    return lightColor * intensity * NdotL;
}

float3 CalcDirectionalSpecular(float3 lightColor, float3 lightDir, float intensity,
                               float3 N, float3 V, float shininess)
{
    float3 H = normalize(-lightDir + V);
    float NdotH = saturate(dot(N, H));
    return lightColor * intensity * pow(NdotH, max(shininess, 1.0f));
}

float3 GetHeatmapColor(float value)
{
    float3 color;
    color.r = saturate(min(4.0 * value - 1.5, -4.0 * value + 4.5));
    color.g = saturate(min(4.0 * value - 0.5, -4.0 * value + 3.5));
    color.b = saturate(min(4.0 * value + 0.5, -4.0 * value + 2.5));
    return color;
}

uint DepthToClusterSlice(float viewDepth)
{
    float safeDepth = clamp(viewDepth, CullState.NearZ, CullState.FarZ);
    float logDepth = log(safeDepth / CullState.NearZ) / log(CullState.FarZ / CullState.NearZ);
    return min((uint) floor(logDepth * CullState.ClusterZ), CullState.ClusterZ - 1);
}

uint ComputeClusterIndex(float4 screenPos, float3 worldPos)
{
    float4 viewPos = mul(float4(worldPos, 1.0f), View);
    uint tileX = min((uint) (screenPos.x / CullState.ScreenWidth * CullState.ClusterX), CullState.ClusterX - 1);
    uint tileY = min((uint) (screenPos.y / CullState.ScreenHeight * CullState.ClusterY), CullState.ClusterY - 1);
    uint sliceZ = DepthToClusterSlice(abs(viewPos.z));

    return sliceZ * CullState.ClusterX * CullState.ClusterY
        + tileY * CullState.ClusterX
        + tileX;
}

float3 CalcLightDiffuse(FLightInfo light, float3 worldPos, float3 N)
{
    float3 L = light.Position - worldPos;
    float dist = length(L);
    L = normalize(L);

    float atten = CalcAttenuation(dist, light.AttenuationRadius, light.FalloffExponent);
    float NdotL = saturate(dot(N, L));

    float spotFactor = 1.0f;
    if (light.LightType == LIGHT_TYPE_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.Direction));
        spotFactor = smoothstep(light.OuterConeCos, light.InnerConeCos, cosAngle);
    }

    return light.Color.rgb * light.Intensity * NdotL * atten * spotFactor;
}

float3 CalcLightSpecular(FLightInfo light, float3 worldPos, float3 N, float3 V, float shininess)
{
    float3 L = normalize(light.Position - worldPos);
    float dist = length(light.Position - worldPos);
    float atten = CalcAttenuation(dist, light.AttenuationRadius, light.FalloffExponent);

    float spotFactor = 1.0f;
    if (light.LightType == LIGHT_TYPE_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.Direction));
        spotFactor = smoothstep(light.OuterConeCos, light.InnerConeCos, cosAngle);
    }

    float3 H = normalize(L + V);
    float NdotH = saturate(dot(N, H));
    return light.Color.rgb * light.Intensity * pow(NdotH, max(shininess, 1.0f)) * atten * spotFactor;
}

void AccumulatePointSpotDiffuse(float3 worldPos, float3 N, float4 screenPos, inout float3 result)
{
    if (LightCullingMode == LIGHT_CULLING_TILE && NumTilesX > 0 && NumTilesY > 0)
    {
        uint2 tileCoord = min(uint2(screenPos.xy) / TILE_SIZE, uint2(NumTilesX - 1, NumTilesY - 1));
        uint tileIdx = tileCoord.y * NumTilesX + tileCoord.x;
        uint2 gridData = TileLightGrid[tileIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = TileLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos);
        }
    }
    else if (LightCullingMode == LIGHT_CULLING_CLUSTER)
    {
        uint clusterIdx = ComputeClusterIndex(screenPos, worldPos);
        uint2 gridData = g_ClusterLightGrid[clusterIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = g_ClusterLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos);
        }
    }
    else
    {
        for (uint i = 0; i < NumActivePointLights + NumActiveSpotLights; ++i)
        {
            FLightInfo light = AllLights[i];
            result += CalcLightDiffuse(light, worldPos, N) * CalcLocalShadow(i, light, worldPos);
        }
    }
}

void AccumulatePointSpotSpecular(float3 worldPos, float3 N, float3 V, float shininess, float4 screenPos, inout float3 result)
{
    if (LightCullingMode == LIGHT_CULLING_TILE && NumTilesX > 0 && NumTilesY > 0)
    {
        uint2 tileCoord = min(uint2(screenPos.xy) / TILE_SIZE, uint2(NumTilesX - 1, NumTilesY - 1));
        uint tileIdx = tileCoord.y * NumTilesX + tileCoord.x;
        uint2 gridData = TileLightGrid[tileIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = TileLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightSpecular(light, worldPos, N, V, shininess) * CalcLocalShadow(lightIndex, light, worldPos);
        }
    }
    else if (LightCullingMode == LIGHT_CULLING_CLUSTER)
    {
        uint clusterIdx = ComputeClusterIndex(screenPos, worldPos);
        uint2 gridData = g_ClusterLightGrid[clusterIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = g_ClusterLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcLightSpecular(light, worldPos, N, V, shininess) * CalcLocalShadow(lightIndex, light, worldPos);
        }
    }
    else
    {
        for (uint i = 0; i < NumActivePointLights + NumActiveSpotLights; ++i)
        {
            FLightInfo light = AllLights[i];
            result += CalcLightSpecular(light, worldPos, N, V, shininess) * CalcLocalShadow(i, light, worldPos);
        }
    }
}

#if defined(LIGHTING_MODEL_TOON) && LIGHTING_MODEL_TOON
static const float g_ToonSteps = 4.0f;
static const float g_ToonDarknessFloor = 0.25f;
static const float g_ToonRimMin = 0.55f;
static const float g_ToonRimMax = 0.85f;
static const float g_ToonRimStrength = 0.25f;

float ToonStep(float NdotL)
{
    float x = saturate(NdotL);
    float stepped = smoothstep(g_ToonDarknessFloor, 1.0f, x * g_ToonSteps);
    stepped /= max(g_ToonSteps - 1.0f, 1.0f);
    return lerp(g_ToonDarknessFloor, 1.0f, saturate(stepped));
}

float3 CalcToonDirectionalDiffuse(float3 N)
{
    float band = ToonStep(saturate(dot(N, -DirectionalLight.Direction)));
    return DirectionalLight.Color.rgb * DirectionalLight.Intensity * band;
}

float3 CalcToonPointSpotDiffuse(FLightInfo light, float3 worldPos, float3 N)
{
    float3 L = light.Position - worldPos;
    float dist = length(L);
    L = normalize(L);

    float atten = CalcAttenuation(dist, light.AttenuationRadius, light.FalloffExponent);
    float band = ToonStep(saturate(dot(N, L)));

    float spotFactor = 1.0f;
    if (light.LightType == LIGHT_TYPE_SPOT)
    {
        float cosAngle = dot(-L, normalize(light.Direction));
        spotFactor = smoothstep(light.OuterConeCos, light.InnerConeCos, cosAngle);
    }

    return light.Color.rgb * light.Intensity * atten * spotFactor * band;
}

void AccumulateToonPointSpotDiffuse(float3 worldPos, float3 N, float4 screenPos, inout float3 result)
{
    if (LightCullingMode == LIGHT_CULLING_TILE && NumTilesX > 0 && NumTilesY > 0)
    {
        uint2 tileCoord = min(uint2(screenPos.xy) / TILE_SIZE, uint2(NumTilesX - 1, NumTilesY - 1));
        uint tileIdx = tileCoord.y * NumTilesX + tileCoord.x;
        uint2 gridData = TileLightGrid[tileIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = TileLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcToonPointSpotDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos);
        }
    }
    else if (LightCullingMode == LIGHT_CULLING_CLUSTER)
    {
        uint clusterIdx = ComputeClusterIndex(screenPos, worldPos);
        uint2 gridData = g_ClusterLightGrid[clusterIdx];
        for (uint t = 0; t < gridData.y; ++t)
        {
            uint lightIndex = g_ClusterLightIndices[gridData.x + t];
            FLightInfo light = AllLights[lightIndex];
            result += CalcToonPointSpotDiffuse(light, worldPos, N) * CalcLocalShadow(lightIndex, light, worldPos);
        }
    }
    else
    {
        for (uint i = 0; i < NumActivePointLights + NumActiveSpotLights; ++i)
        {
            FLightInfo light = AllLights[i];
            result += CalcToonPointSpotDiffuse(light, worldPos, N) * CalcLocalShadow(i, light, worldPos);
        }
    }
}

float3 AccumulateToonDiffuse(float3 worldPos, float3 N, float4 screenPos)
{
    float3 result = CalcAmbient(AmbientLight.Color.rgb, AmbientLight.Intensity) * 0.15f;
    result += CalcToonDirectionalDiffuse(N);
    AccumulateToonPointSpotDiffuse(worldPos, N, screenPos, result);
    return result;
}

float CalcRimMask(float3 N, float3 V)
{
    float rimDot = 1.0f - saturate(dot(N, V));
    return smoothstep(g_ToonRimMin, g_ToonRimMax, rimDot);
}
#endif

float3 AccumulateDiffuse(float3 worldPos, float3 N, float4 screenPos)
{
    float3 result = float3(0, 0, 0);
    result += CalcAmbient(AmbientLight.Color.rgb, AmbientLight.Intensity);
    result += CalcDirectionalDiffuse(DirectionalLight.Color.rgb, DirectionalLight.Direction,
                                     DirectionalLight.Intensity, N);
    AccumulatePointSpotDiffuse(worldPos, N, screenPos, result);
    return result;
}

float3 AccumulateSpecular(float3 worldPos, float3 N, float3 V, float shininess, float4 screenPos)
{
    float3 result = float3(0, 0, 0);
    result += CalcDirectionalSpecular(DirectionalLight.Color.rgb, DirectionalLight.Direction,
                                      DirectionalLight.Intensity, N, V, shininess);
    AccumulatePointSpotSpecular(worldPos, N, V, shininess, screenPos, result);
    return result;
}

#endif // FORWARD_LIGHTING_HLSLI
