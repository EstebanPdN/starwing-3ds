Texture2D<float4> sourceColor : register(t0,space0);
Texture2D<float> sourceDepth : register(t1,space0);
Texture2D<float2> sourceMotion : register(t2,space0);
[[vk::image_format("rgba8")]] RWTexture2D<float4> targetColor : register(u0,space1);
RWTexture2D<float> targetDepth : register(u1,space1);
RWTexture2D<float2> targetMotion : register(u2,space1);
cbuffer Settings : register(b0,space2) {uint sourceWidth,sourceHeight,targetWidth,targetHeight;};
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=targetWidth || id.y>=targetHeight) return;
    float2 ratio=float2(sourceWidth,sourceHeight)/float2(targetWidth,targetHeight);
    float2 lo=float2(id.xy)*ratio,hi=float2(id.xy+1)*ratio;
    uint2 first=uint2(floor(lo)),last=min(uint2(ceil(hi)),uint2(sourceWidth,sourceHeight));
    float4 color=0;float weight=0,closest=2;float2 motion=asfloat(0xff7fffffu);
    // Area-filter color; depth and motion must come from the same nearest
    // visible sample, never average across foreground/background boundaries.
    for(uint y=first.y;y<last.y;++y) for(uint x=first.x;x<last.x;++x) {
        float w=max(0,min(hi.x,x+1.0)-max(lo.x,float(x)))*max(0,min(hi.y,y+1.0)-max(lo.y,float(y)));
        color+=sourceColor.Load(int3(x,y,0))*w;weight+=w;
        float z=sourceDepth.Load(int3(x,y,0));
        if(z<closest) {closest=z;motion=sourceMotion.Load(int3(x,y,0));}
    }
    bool valid=all(abs(motion)<1e20);
    targetColor[id.xy]=color/max(weight,1e-8);
    targetDepth[id.xy]=min(closest,1.0);
    targetMotion[id.xy]=valid?motion/ratio:float2(asfloat(0xff7fffffu),asfloat(0xff7fffffu));
}
