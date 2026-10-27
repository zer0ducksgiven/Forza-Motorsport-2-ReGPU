// SMAA 1x (Jimenez et al., MIT; thirdparty/smaa) on the whole frame as it is shown: luma edge
// detection, blending weights, neighbourhood blending, as three compute passes. SMAA's pixel
// shader functions are called as they are; each pass's offsets come from its vertex function.
// Built once per preset (SMAA_PRESET_LOW to _ULTRA) by tools/embed_hlsl.py. (Not smaa.hlsl: on
// Windows that name would include itself instead of SMAA.hlsl.)
#define SMAA_CUSTOM_SL 1

cbuffer Constants : register(b0)
{
    float4 rtMetrics; // 1 / width, 1 / height, width, height
};
#define SMAA_RT_METRICS rtMetrics

SamplerState LinearSampler : register(s0);
SamplerState PointSampler : register(s1);
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAASample(tex, coord) tex.SampleLevel(LinearSampler, coord, 0)
#define SMAASamplePoint(tex, coord) tex.SampleLevel(PointSampler, coord, 0)
#define SMAASampleOffset(tex, coord, offset) tex.SampleLevel(LinearSampler, coord, 0, offset)
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)
// Edge detection discards pixels without an edge; a compute pass writes zero there instead.
#define discard

#include "SMAA.hlsl"

Texture2D in0 : register(t0); // edges: the image; weights: the edges; blending: the image
Texture2D in1 : register(t1); // weights: the area texture; blending: the weights
Texture2D in2 : register(t2); // weights: the search texture
RWTexture2D<float4> output : register(u0);

float2 Texcoord(uint2 p) { return (float2(p) + 0.5) * rtMetrics.xy; }

[numthreads(8, 8, 1)]
void SmaaEdges(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(rtMetrics.zw)))
        return;
    const float2 uv = Texcoord(id.xy);
    float4 offset[3];
    SMAAEdgeDetectionVS(uv, offset);
    output[id.xy] = float4(SMAALumaEdgeDetectionPS(uv, offset, in0), 0, 0);
}

[numthreads(8, 8, 1)]
void SmaaWeights(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(rtMetrics.zw)))
        return;
    const float2 uv = Texcoord(id.xy);
    float2 pixcoord;
    float4 offset[3];
    SMAABlendingWeightCalculationVS(uv, pixcoord, offset);
    output[id.xy] = SMAABlendingWeightCalculationPS(uv, pixcoord, offset, in0, in1, in2, float4(0, 0, 0, 0));
}

[numthreads(8, 8, 1)]
void SmaaBlend(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(rtMetrics.zw)))
        return;
    const float2 uv = Texcoord(id.xy);
    float4 offset;
    SMAANeighborhoodBlendingVS(uv, offset);
    output[id.xy] = SMAANeighborhoodBlendingPS(uv, offset, in0, in1);
}
