// Vertex correspondence for temporal reconstruction. Inputs are paired
// ContinuousProjectedPoint buffers from the same topology and eye. Neither
// buffer may include temporal jitter. Output is previous-minus-current in
// render pixels, current linear camera depth, and an explicit validity flag.
// The optional depth path reconstructs the visible surface after clipping and
// applies rigid-object correspondence. It must run before merging objects.
struct Point { float4 camera; float4 screen; };
[[vk::binding(0,0)]] ByteAddressBuffer currentPoints : register(t0,space0);
[[vk::binding(1,0)]] ByteAddressBuffer previousPoints : register(t1,space0);
[[vk::binding(0,1)]] RWStructuredBuffer<float4> motion : register(u0,space1);
[[vk::binding(0,2)]] cbuffer Settings : register(b0,space2) {
    uint count; uint resetHistory; float scaleX; float scaleY;
    uint width,height,surfaceReset,reserved;
    float4 currentProjection,previousProjection;
    float4 previousRow0,previousRow1,previousRow2;
    float jitterX,jitterY,previousNear,padding;
};
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID) {
    if(id.x>=count) return;
    if(width!=0) {
        float z=asfloat(currentPoints.Load(id.x*4));
        float4 result=0;
        if(resetHistory==0 && isfinite(z) && z>0) {
            float2 pixel=float2(id.x%width,id.x/width)+.5;
            float2 unjittered=pixel-float2(jitterX,jitterY);
            float4 camera=float4((unjittered-currentProjection.zw)/currentProjection.xy*z,z,1);
            float3 previous=float3(dot(previousRow0,camera),dot(previousRow1,camera),dot(previousRow2,camera));
            if(all(isfinite(previous)) && previous.z>previousNear) {
                float2 delta=previous.xy/previous.z*previousProjection.xy+previousProjection.zw-unjittered;
                if(all(isfinite(delta))) result=float4(delta,z,1);
            }
        }
        motion[id.x]=result;return;
    }
    Point now,old;
    now.camera=asfloat(currentPoints.Load4(id.x*32));now.screen=asfloat(currentPoints.Load4(id.x*32+16));
    old.camera=asfloat(previousPoints.Load4(id.x*32));old.screen=asfloat(previousPoints.Load4(id.x*32+16));
    float4 result=0;
    if(resetHistory==0 && now.camera.w>0 && old.camera.w>0
        && now.camera.z>0 && old.camera.z>0 && now.screen.w>0 && old.screen.w>0
        && all(isfinite(now.screen)) && all(isfinite(old.screen))
        && all(isfinite(now.camera)) && all(isfinite(old.camera))
        && isfinite(scaleX) && isfinite(scaleY) && scaleX>0 && scaleY>0) {
        float2 delta=(old.screen.xy-now.screen.xy)*float2(scaleX,scaleY);
        if(all(isfinite(delta))) result=float4(delta,now.camera.z,1);
    }
    motion[id.x]=result;
}
