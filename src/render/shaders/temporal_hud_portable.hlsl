Texture2D<float4> original : register(t0,space0);
Texture2D<float4> reconstructed : register(t1,space0);
StructuredBuffer<uint> packedPixels : register(t2,space0);
[[vk::image_format("rgba8")]] RWTexture2D<float4> result : register(u0,space1);
cbuffer Settings : register(b0,space2) {uint width,height,pad0,pad1;};
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width || id.y>=height) return;
    bool hud=((packedPixels[id.y*width+id.x]>>8)&255u)==1u;
    result[id.xy]=hud?original.Load(int3(id.xy,0)):reconstructed.Load(int3(id.xy,0));
}
