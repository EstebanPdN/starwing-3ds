[[vk::binding(0,0)]] StructuredBuffer<uint> frontPixels : register(t0,space0);
[[vk::binding(1,0)]] StructuredBuffer<float4> frontSurfaces : register(t1,space0);
[[vk::binding(2,0)]] StructuredBuffer<uint> backPixels : register(t2,space0);
[[vk::binding(3,0)]] StructuredBuffer<float4> backSurfaces : register(t3,space0);
[[vk::binding(4,0)]] StructuredBuffer<float> frontDepth : register(t4,space0);
[[vk::binding(5,0)]] StructuredBuffer<float> backDepth : register(t5,space0);
[[vk::binding(6,0)]] StructuredBuffer<float4> frontMotion : register(t6,space0);
[[vk::binding(7,0)]] StructuredBuffer<float4> backMotion : register(t7,space0);
[[vk::binding(0,1)]] RWStructuredBuffer<uint> pixels : register(u0,space1);
[[vk::binding(1,1)]] RWStructuredBuffer<float4> surfaces : register(u1,space1);
[[vk::binding(2,1)]] RWStructuredBuffer<float> geometryDepth : register(u2,space1);
[[vk::binding(3,1)]] RWStructuredBuffer<float4> motion : register(u3,space1);
[[vk::binding(0,2)]] cbuffer Settings : register(b0,space2) {
    uint count,hasFrontSurfaces,hasBack,hasBackSurfaces;
    uint wantDepth,hasFrontDepth,hasBackDepth,padding;
    uint wantMotion,hasFrontMotion,hasBackMotion,motionPadding;
};
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=count) return;
    uint front=frontPixels[id.x],back=0;
    if(hasBack!=0) back=backPixels[id.x];
    // Colour and metadata have independent painter ownership. A later line
    // can paint black without erasing an earlier model's surface sample.
    uint colour=(front&0x04000000U)!=0?front:back;
    uint metadata=0;float4 normal=float4(0,0,1,0);
    if(hasFrontSurfaces!=0 && (front&0x01000000U)!=0) {
        metadata=front&0x01ff0000U;normal=frontSurfaces[id.x];
    } else if(hasBackSurfaces!=0 && (back&0x01000000U)!=0) {
        metadata=back&0x01ff0000U;normal=backSurfaces[id.x];
    }
    pixels[id.x]=(colour&0x0c00ffffU)|metadata;
    surfaces[id.x]=normal;
    if(wantDepth!=0) {
        // Unlike effects metadata, geometry belongs to the visible colour.
        // A covering sprite/HUD pixel with unknown depth must not inherit a
        // hidden model's depth. Transparent pixels retain the layer behind.
        float depth=0;
        if((front&0x04000000U)!=0) {
            if(hasFrontDepth!=0) depth=frontDepth[id.x];
        } else if(hasBackDepth!=0) depth=backDepth[id.x];
        geometryDepth[id.x]=depth;
    }
    if(wantMotion!=0) {
        float4 value=0;
        if((front&0x04000000U)!=0) {
            if(hasFrontMotion!=0) value=frontMotion[id.x];
        } else if(hasBackMotion!=0) value=backMotion[id.x];
        motion[id.x]=value;
    }
}
