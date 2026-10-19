#include "Common/ConstantBuffers.hlsli"
#include "Common/ForwardLightData.hlsli"

StructuredBuffer<FAABB> gClusterAABBs : register(t0);
StructuredBuffer<FLightInfo> gLights : register(t1);

RWStructuredBuffer<uint> gLightIndexList : register(u1);
RWStructuredBuffer<uint2> gLightGrid : register(u2);
RWStructuredBuffer<uint> gGlobalCounter : register(u3);

#define CLUSTER_LIGHT_CULLING_THREADS 256

// One thread group owns one cluster. These values are shared by the 256 threads
// inside that group while they split the light loop.
groupshared uint gsLightIndices[256];
groupshared uint gsHitCount;
groupshared float4 gsClusterPlanes[6];
groupshared float3 gsClusterCenter;
groupshared uint gsCandidateIndices[256];
groupshared float gsCandidateScores[256];
groupshared float gsHeapScores[256];
groupshared uint gsHeapCount;

float SliceToViewDepth(uint zSlice)
{
    return CullState.NearZ * pow(CullState.FarZ / CullState.NearZ, (float) zSlice / CullState.ClusterZ);
}

float3 NDCToViewSpace(float2 ndc, float viewDepth)
{
    float4 clipPos = float4(ndc.x, ndc.y, 1.0f, 1.0f);
    float4 viewPos = mul(clipPos, InvProj);
    viewPos /= viewPos.w;
    return viewPos.xyz / viewPos.z * viewDepth;
}

float4 MakePlane(float3 a, float3 b, float3 c, float3 insidePoint)
{
    float3 n = normalize(cross(b - a, c - a));
    float d = -dot(n, a);
    if (dot(n, insidePoint) + d < 0.0f)
    {
        n = -n;
        d = -d;
    }
    return float4(n, d);
}

bool SphereInsidePlane(float3 center, float radius, float4 plane)
{
    return dot(plane.xyz, center) + plane.w >= -radius;
}

void BuildClusterPlanes(float3 corners[8], out float4 planes[6])
{
    float3 insidePoint = 0.0f;
    [unroll]
    for (int i = 0; i < 8; ++i)
    {
        insidePoint += corners[i];
    }
    insidePoint *= 0.125f;

    planes[0] = MakePlane(corners[0], corners[1], corners[2], insidePoint); // near
    planes[1] = MakePlane(corners[4], corners[6], corners[5], insidePoint); // far
    planes[2] = MakePlane(corners[0], corners[2], corners[4], insidePoint); // left
    planes[3] = MakePlane(corners[1], corners[5], corners[3], insidePoint); // right
    planes[4] = MakePlane(corners[0], corners[4], corners[1], insidePoint); // bottom
    planes[5] = MakePlane(corners[2], corners[3], corners[6], insidePoint); // top
}

bool SphereOverlapsClusterPlanes(float3 center, float radius)
{
    [unroll]
    for (int p = 0; p < 6; ++p)
    {
        if (!SphereInsidePlane(center, radius, gsClusterPlanes[p]))
        {
            return false;
        }
    }

    return true;
}

float ComputeLightImportance(FLightInfo light, float3 viewLightPos)
{
    float dist = length(viewLightPos - gsClusterCenter);
    float ratio = saturate(dist / max(light.AttenuationRadius, 0.0001f));
    float attenuation = pow(1.0f - ratio, light.FalloffExponent);
    float luminance = dot(light.Color.rgb, float3(0.299f, 0.587f, 0.114f));
    return max(luminance * light.Intensity * attenuation, 0.0f);
}

// The heap root is always the least important retained light. Equal scores use
// the light index as a stable tie breaker so the selected set is deterministic.
bool IsLessImportant(float scoreA, uint indexA, float scoreB, uint indexB)
{
    return scoreA < scoreB || (scoreA == scoreB && indexA > indexB);
}

void SwapHeapEntries(uint a, uint b)
{
    float score = gsHeapScores[a];
    uint index = gsLightIndices[a];
    gsHeapScores[a] = gsHeapScores[b];
    gsLightIndices[a] = gsLightIndices[b];
    gsHeapScores[b] = score;
    gsLightIndices[b] = index;
}

void InsertTopLight(uint lightIndex, float score, uint capacity)
{
    if (gsHeapCount < capacity)
    {
        uint cursor = gsHeapCount++;
        gsHeapScores[cursor] = score;
        gsLightIndices[cursor] = lightIndex;

        while (cursor > 0)
        {
            uint parent = (cursor - 1) >> 1;
            if (!IsLessImportant(gsHeapScores[cursor], gsLightIndices[cursor],
                                 gsHeapScores[parent], gsLightIndices[parent]))
            {
                break;
            }

            SwapHeapEntries(cursor, parent);
            cursor = parent;
        }
        return;
    }

    if (!IsLessImportant(gsHeapScores[0], gsLightIndices[0], score, lightIndex))
    {
        return;
    }

    gsHeapScores[0] = score;
    gsLightIndices[0] = lightIndex;

    uint cursor = 0;
    while (true)
    {
        uint left = cursor * 2 + 1;
        if (left >= gsHeapCount)
        {
            break;
        }

        uint right = left + 1;
        uint leastImportant = left;
        if (right < gsHeapCount &&
            IsLessImportant(gsHeapScores[right], gsLightIndices[right],
                            gsHeapScores[left], gsLightIndices[left]))
        {
            leastImportant = right;
        }

        if (!IsLessImportant(gsHeapScores[leastImportant], gsLightIndices[leastImportant],
                             gsHeapScores[cursor], gsLightIndices[cursor]))
        {
            break;
        }

        SwapHeapEntries(cursor, leastImportant);
        cursor = leastImportant;
    }
}

void BuildCurrentClusterPlanes(uint clusterIdx)
{
    uint sliceSize = CullState.ClusterX * CullState.ClusterY;
    uint clusterZ = clusterIdx / sliceSize;
    uint clusterXY = clusterIdx - clusterZ * sliceSize;
    uint clusterY = clusterXY / CullState.ClusterX;
    uint clusterX = clusterXY - clusterY * CullState.ClusterX;
    uint3 clusterCoord = uint3(clusterX, clusterY, clusterZ);

    float2 tileSize = float2(2.0f / CullState.ClusterX, 2.0f / CullState.ClusterY);
    float2 ndcMin = float2(-1.0f + clusterCoord.x * tileSize.x, 1.0f - (clusterCoord.y + 1) * tileSize.y);
    float2 ndcMax = float2(ndcMin.x + tileSize.x, 1.0f - clusterCoord.y * tileSize.y);
    float nearZ = SliceToViewDepth(clusterCoord.z);
    float farZ = SliceToViewDepth(clusterCoord.z + 1);

    float3 corners[8];
    corners[0] = NDCToViewSpace(float2(ndcMin.x, ndcMin.y), nearZ);
    corners[1] = NDCToViewSpace(float2(ndcMax.x, ndcMin.y), nearZ);
    corners[2] = NDCToViewSpace(float2(ndcMin.x, ndcMax.y), nearZ);
    corners[3] = NDCToViewSpace(float2(ndcMax.x, ndcMax.y), nearZ);
    corners[4] = NDCToViewSpace(float2(ndcMin.x, ndcMin.y), farZ);
    corners[5] = NDCToViewSpace(float2(ndcMax.x, ndcMin.y), farZ);
    corners[6] = NDCToViewSpace(float2(ndcMin.x, ndcMax.y), farZ);
    corners[7] = NDCToViewSpace(float2(ndcMax.x, ndcMax.y), farZ);

    float4 planes[6];
    BuildClusterPlanes(corners, planes);

    [unroll]
    for (int p = 0; p < 6; ++p)
    {
        gsClusterPlanes[p] = planes[p];
    }
}

void BuildCurrentClusterCenter(uint clusterIdx)
{
    uint sliceSize = CullState.ClusterX * CullState.ClusterY;
    uint clusterZ = clusterIdx / sliceSize;
    uint clusterXY = clusterIdx - clusterZ * sliceSize;
    uint clusterY = clusterXY / CullState.ClusterX;
    uint clusterX = clusterXY - clusterY * CullState.ClusterX;

    float2 tileSize = float2(2.0f / CullState.ClusterX, 2.0f / CullState.ClusterY);
    float2 ndcMin = float2(-1.0f + clusterX * tileSize.x, 1.0f - (clusterY + 1) * tileSize.y);
    float2 ndcMax = float2(ndcMin.x + tileSize.x, 1.0f - clusterY * tileSize.y);
    float2 ndcCenter = (ndcMin + ndcMax) * 0.5f;
    float nearZ = SliceToViewDepth(clusterZ);
    float farZ = SliceToViewDepth(clusterZ + 1);

    gsClusterCenter = (NDCToViewSpace(ndcCenter, nearZ) + NDCToViewSpace(ndcCenter, farZ)) * 0.5f;
}

void PublishClusterLightList(uint clusterIdx, uint localCount)
{
    uint offset;
    InterlockedAdd(gGlobalCounter[0], localCount, offset);

    gLightGrid[clusterIdx] = uint2(offset, localCount);
    for (uint j = 0; j < localCount; ++j)
    {
        gLightIndexList[offset + j] = gsLightIndices[j];
    }
}

// One dispatch group maps to one cluster.
// The 256 threads in that group divide the light list with i += 256.
[numthreads(CLUSTER_LIGHT_CULLING_THREADS, 1, 1)]
void CSMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint clusterCount = CullState.ClusterX * CullState.ClusterY * CullState.ClusterZ;
    uint clusterIdx = groupId.x;
    uint threadIndex = groupThreadId.x;
    uint localCapacity = min(CullState.MaxLightsPerCluster, 256);

    if (clusterIdx >= clusterCount)
    {
        return;
    }

    // Thread 0 prepares the cluster data once. The rest of the group only tests
    // lights against the shared planes, so plane construction is not repeated per light.
    if (threadIndex == 0)
    {
        gsHitCount = 0;
        BuildCurrentClusterPlanes(clusterIdx);
    }
    GroupMemoryBarrierWithGroupSync();

    uint totalLights = NumActivePointLights + NumActiveSpotLights;
    if (localCapacity > 0)
    {
        for (uint i = threadIndex; i < totalLights; i += CLUSTER_LIGHT_CULLING_THREADS)
        {
            FLightInfo light = gLights[i];
            float4 viewPos = mul(float4(light.Position, 1.0f), View);
            if (SphereOverlapsClusterPlanes(viewPos.xyz, light.AttenuationRadius))
            {
                uint slot;
                InterlockedAdd(gsHitCount, 1, slot);
                if (slot < localCapacity)
                {
                    gsLightIndices[slot] = i;
                }
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // Only one thread publishes the compacted list to the global buffers.
    if (threadIndex == 0)
    {
        PublishClusterLightList(clusterIdx, min(gsHitCount, localCapacity));
    }
}

// Deterministic Top-N variant. It is compiled as a separate compute shader so
// the original atomic shader keeps its own register/shared-memory footprint.
[numthreads(CLUSTER_LIGHT_CULLING_THREADS, 1, 1)]
void CSTopNMain(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    uint clusterCount = CullState.ClusterX * CullState.ClusterY * CullState.ClusterZ;
    uint clusterIdx = groupId.x;
    uint threadIndex = groupThreadId.x;
    uint localCapacity = min(CullState.MaxLightsPerCluster, 256);

    if (clusterIdx >= clusterCount)
    {
        return;
    }

    if (threadIndex == 0)
    {
        gsHeapCount = 0;
        BuildCurrentClusterPlanes(clusterIdx);
        BuildCurrentClusterCenter(clusterIdx);
    }
    GroupMemoryBarrierWithGroupSync();

    uint totalLights = NumActivePointLights + NumActiveSpotLights;
    if (localCapacity > 0)
    {
        // Intersection and score calculation remain parallel. Candidates are
        // processed in fixed 256-light batches, then thread 0 merges each batch
        // into a deterministic fixed-capacity min-heap.
        for (uint batchStart = 0; batchStart < totalLights; batchStart += CLUSTER_LIGHT_CULLING_THREADS)
        {
            uint lightIndex = batchStart + threadIndex;
            gsCandidateIndices[threadIndex] = 0xffffffffu;
            gsCandidateScores[threadIndex] = 0.0f;

            if (lightIndex < totalLights)
            {
                FLightInfo light = gLights[lightIndex];
                float4 viewPos = mul(float4(light.Position, 1.0f), View);
                if (SphereOverlapsClusterPlanes(viewPos.xyz, light.AttenuationRadius))
                {
                    gsCandidateIndices[threadIndex] = lightIndex;
                    gsCandidateScores[threadIndex] = ComputeLightImportance(light, viewPos.xyz);
                }
            }
            GroupMemoryBarrierWithGroupSync();

            if (threadIndex == 0)
            {
                uint batchCount = min(CLUSTER_LIGHT_CULLING_THREADS, totalLights - batchStart);
                for (uint candidate = 0; candidate < batchCount; ++candidate)
                {
                    uint candidateIndex = gsCandidateIndices[candidate];
                    if (candidateIndex != 0xffffffffu)
                    {
                        InsertTopLight(candidateIndex, gsCandidateScores[candidate], localCapacity);
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (threadIndex == 0)
    {
        PublishClusterLightList(clusterIdx, gsHeapCount);
    }
}
