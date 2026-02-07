// AC Valhalla Screen Space Ray Tracing (SSRT) Compute Shader
// Phase 2.1: Ray Tracing Injection

#define BLOCK_SIZE 8

RWTexture2D<float4> g_Output : register(u0);
Texture2D<float4> g_Color : register(t0);
Texture2D<float> g_Depth : register(t1);
Texture2D<float2> g_Motion : register(t2);

SamplerState g_PointClamp : register(s0);
SamplerState g_LinearClamp : register(s1);

cbuffer CB_Frame : register(b0)
{
    float4x4 g_ViewProjInv;
    float4x4 g_ViewProj;
    float3 g_CamPos;
    float g_Time;
    float2 g_Resolution;
    float2 g_InvResolution;
};

// Reconstruct World Position from Depth
float3 GetWorldPos(float2 uv, float depth)
{
    float4 clipPos = float4(uv * 2.0 - 1.0, depth, 1.0);
    // Y-flip adjustment if needed (DirectX usually maps 0..1 to -1..1, but check NDC)
    clipPos.y = -clipPos.y; 
    
    float4 worldPos = mul(g_ViewProjInv, clipPos);
    return worldPos.xyz / worldPos.w;
}

[numthreads(BLOCK_SIZE, BLOCK_SIZE, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Resolution.x || id.y >= g_Resolution.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) * g_InvResolution;
    float depth = g_Depth.SampleLevel(g_PointClamp, uv, 0);
    float4 color = g_Color.SampleLevel(g_PointClamp, uv, 0);

    // Skip skybox/far plane (assuming reverse Z or standard Z, need calibration)
    // Valhalla uses standard Z? Or Reverse? Usually 0 is near, 1 is far or vice versa.
    // For now, visualize depth to debug.
    
    // Debug: Simple edge detection/AO placeholder
    // In full implementation, we will march rays here.
    
    float3 worldPos = GetWorldPos(uv, depth);
    
    // Placeholder output: Tint based on world position to verify reconstruction
    float3 debugTint = frac(worldPos.xyz * 0.1);
    
    // Blend with original color (Additive for now)
    // g_Output[id.xy] = float4(color.rgb + debugTint * 0.1, 1.0);
    
    // For now, just pass through to ensure pipeline works
    g_Output[id.xy] = float4(color.rgb, 1.0);
}
