StructuredBuffer<uint> cpuPixels : register(t0,space0);
StructuredBuffer<uint> nativePixels : register(t1,space0);
StructuredBuffer<float4> nativeSurfaces : register(t2,space0);
StructuredBuffer<uint> palette : register(t3,space0);
StructuredBuffer<uint> latePixels : register(t4,space0);
StructuredBuffer<uint> backgroundPixels : register(t5,space0);
StructuredBuffer<float> nativeDepth : register(t6,space0);
StructuredBuffer<float4> nativeMotion : register(t7,space0);
[[vk::image_format("rgba8")]] RWTexture2D<float4> rgba : register(u0,space1);
RWStructuredBuffer<uint> pixels : register(u1,space1);
RWStructuredBuffer<float4> surfaces : register(u2,space1);
RWStructuredBuffer<uint> edgeColours : register(u3,space1);
RWStructuredBuffer<float> geometryDepth : register(u4,space1);
RWStructuredBuffer<float4> motion : register(u5,space1);
cbuffer Settings : register(b0,space2) {
    uint width,height,sourceWidth,sourceHeight;
    uint scale,sourceScale,mosaic,hasSurfaces;
    int offsetX,offsetY,clipLeft,clipTop;
    int clipRight,clipBottom,originX,originY;
    uint hasLate,hasBackground,phase,marginOrigin;
    uint marginWidth,repairMargins,hasDepth,hasMotion;
    uint worldOnly,pad0,pad1,pad2;
};
int mosaicAt(int at,int origin) {
    int value=at-origin,rem=value%int(mosaic);
    if(rem<0) rem+=int(mosaic);
    return at-rem;
}
uint composedValue(uint2 at) {
    uint i=at.y*width+at.x;
    uint value=cpuPixels[i]&65535;
    if(worldOnly && ((value>>8)&255u)==1u) value=0;
    if(hasBackground && !(cpuPixels[i]&0xc0000000u) && (backgroundPixels[i]&0x04000000u))
        value=backgroundPixels[i]&0x0800ffffu;
    int2 logical=int2(at/scale),source=logical-int2(offsetX,offsetY);
    if(marginOrigin && repairMargins && !(cpuPixels[i]&0xc0000000u)
        && (logical.x<int(marginOrigin) || logical.x>=int(marginOrigin+marginWidth))
        && !(backgroundPixels[(at.y/scale*scale)*width+(at.x/scale*scale)]&255u))
        value=edgeColours[0]|(1u<<8);
    uint2 sub=min(sourceScale-1,((at%scale)*2+1)*sourceScale/(scale*2));
    if(all(source>=0) && all(source<int2(sourceWidth,sourceHeight)/int(sourceScale))
        && logical.x>=clipLeft && logical.y>=clipTop && logical.x<clipRight && logical.y<clipBottom
        && !(cpuPixels[i]&0x80000000u)) {
        if(mosaic>1) source=int2(mosaicAt(logical.x,originX),mosaicAt(logical.y,originY))-int2(offsetX,offsetY);
        if(all(source>=0) && all(source<int2(sourceWidth,sourceHeight)/int(sourceScale))) {
            uint2 p=uint2(source)*sourceScale+sub;uint native=nativePixels[p.y*sourceWidth+p.x];
            if((native&255u) && (!worldOnly || ((native>>8)&255u)!=1u)) value=native&65535u;
        }
    }
    return value;
}
[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(!phase) {
        if(id.y || id.x>1) return;
        if(repairMargins) {edgeColours[id.x]=backgroundPixels[marginOrigin*scale]&255u;return;}
        uint counts[256];for(uint c=0;c<256;++c) counts[c]=0;
        uint x=(marginOrigin+(id.x?marginWidth-1:0))*scale;
        for(uint y=0;y<height/scale;++y) ++counts[composedValue(uint2(x,y*scale))&255u];
        uint best=0;for(uint c=1;c<256;++c) if(counts[c]>counts[best]) best=c;
        edgeColours[id.x]=best;return;
    }
    if(id.x>=width || id.y>=height) return;
    uint i=id.y*width+id.x,value=composedValue(id.xy);
    int2 logical=int2(id.xy/scale),source=logical-int2(offsetX,offsetY);
    uint2 sub=min(sourceScale-1,((id.xy%scale)*2+1)*sourceScale/(scale*2));
    bool inSource=all(source>=0) && all(source<int2(sourceWidth,sourceHeight)/int(sourceScale));
    float4 normal=0;uint flags=0;
    float depth=0;float4 temporal=0;
    // Temporal guides follow visible colour ownership, unlike lighting's
    // historical unmosaicked metadata. Screen-space mosaic has no trustworthy
    // pinhole correspondence and is intentionally left unknown.
    if(inSource && mosaic==1 && logical.x>=clipLeft && logical.x<clipRight
        && logical.y>=clipTop && logical.y<clipBottom && !(cpuPixels[i]&0x80000000u)) {
        uint2 p=uint2(source)*sourceScale+sub;uint n=p.y*sourceWidth+p.x;
        if((nativePixels[n]&255u) && (!worldOnly || ((nativePixels[n]>>8)&255u)!=1u)) {
            if(hasDepth) depth=nativeDepth[n];
            if(hasMotion) {temporal=nativeMotion[n];temporal.xy*=float(scale)/float(sourceScale);}
        }
    }
    // GAMEOVER's background fade excludes whole source cells containing
    // foreground at their top-left sample, independently of surface normals.
    if(inSource && (nativePixels[(source.y*sourceScale)*sourceWidth+source.x*sourceScale]&255))
        flags|=0x02000000u;
    // Metadata follows the original, unmosaicked surface projection. Keeping
    // its palette owner lets the effects shader reject pixels overwritten by
    // foreground or mosaic, exactly as the CPU SurfaceBuffer path does.
    if(hasSurfaces && inSource) {
        uint2 p=uint2(source)*sourceScale+sub;
        uint n=p.y*sourceWidth+p.x;
        flags|=nativePixels[n]&0x01ff0000;
        normal=nativeSurfaces[n];
    }
    if(marginOrigin && !repairMargins && (logical.x<int(marginOrigin) || logical.x>=int(marginOrigin+marginWidth))) {
        value=edgeColours[logical.x<int(marginOrigin)?0:1]|(1u<<8);
        flags&=0x02000000u;normal=0;
        depth=0;temporal=0;
    }
    if(hasLate && (latePixels[i]&0x04000000u) && (!worldOnly || ((latePixels[i]>>8)&255u)!=1u)) {
        value=latePixels[i]&65535u;
        flags&=0x02000000u;normal=0;
        depth=0;temporal=0;
    }
    if(cpuPixels[i]&0x20000000u) {
        value=cpuPixels[i]&65535u;flags&=0x02000000u;normal=0;
        depth=0;temporal=0;
    }
    pixels[i]=value|flags;surfaces[i]=normal;
    if(hasDepth) geometryDepth[i]=depth;
    if(hasMotion) motion[i]=temporal;
    uint colour=palette[value&255];
    rgba[id.xy]=float4(colour&255,(colour>>8)&255,(colour>>16)&255,colour>>24)/255.0;
}
