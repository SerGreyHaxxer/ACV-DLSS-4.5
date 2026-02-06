cbuffer Constants : register(b0)
{
    uint Width;
    uint Height;
    uint SampleCountX;
    uint SampleCountY;
}

Texture2D<float4> InputTexture : register(t0);
RWStructuredBuffer<float4> OutputSamples : register(u0);

// We will dispatch (SampleCountX, SampleCountY, 1) threads.
// Each thread picks a pixel from the input texture relative to its normalized position.

[numthreads(1, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= SampleCountX || id.y >= SampleCountY)
        return;

    // Calculate UV (0.0 to 1.0)
    float u = (float)id.x / (float)SampleCountX;
    float v = (float)id.y / (float)SampleCountY;

    // Map to pixel coordinates
    uint px = (uint)(u * Width);
    uint py = (uint)(v * Height);

    // Clamp
    px = min(px, Width - 1);
    py = min(py, Height - 1);

    // Read pixel
    float4 pixel = InputTexture[uint2(px, py)];

    // Write to linear buffer
    uint outIndex = id.y * SampleCountX + id.x;
    OutputSamples[outIndex] = pixel;
}