struct Point { float4 camera; float4 screen; };
[[vk::binding(0,0)]] StructuredBuffer<Point> points : register(t0,space0);
[[vk::binding(1,0)]] StructuredBuffer<uint4> faces : register(t1,space0);
[[vk::binding(0,1)]] RWStructuredBuffer<uint> visible : register(u0,space1);
[[vk::binding(0,2)]] cbuffer Settings : register(b0,space2) { uint count; uint pointCount; uint2 padding; };
// Compensated FP32 arithmetic keeps cancellation around edge-on faces from
// replacing the CPU's 1e-12 tangent tolerance with ordinary float noise.
// No shaderFloat64 requirement: the same operations compile for Metal.
float2 sum2(float a,float b) {
    precise float s=a+b;
    precise float v=s-a;
    precise float e=(a-(s-v))+(b-v);
    return float2(s,e);
}
float2 add2(float2 a,float2 b) {
    precise float2 s=sum2(a.x,b.x);
    precise float tail=(a.y+b.y)+s.y;
    return sum2(s.x,tail);
}
float2 product2(float a,float b) {
    precise float ca=4097.0*a,cb=4097.0*b;
    precise float ah=ca-(ca-a),bh=cb-(cb-b);
    precise float al=a-ah,bl=b-bh;
    precise float p=a*b;
    precise float e=((ah*bh-p)+ah*bl+al*bh)+al*bl;
    return float2(p,e);
}
float2 triple(float a,float b,float c) {
    precise float2 p=product2(a,b);
    precise float2 q=product2(p.x,c);
    return sum2(q.x,q.y+p.y*c);
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=count) return;
    if(faces[id.x].w==1U) {visible[id.x]=1;return;}
    if(faces[id.x].w==3U) {
        uint centre=faces[id.x].x;
        visible[id.x]=centre<pointCount?uint(points[centre].screen.w>0):0;
        return;
    }
    uint3 index=faces[id.x].xyz;
    if(any(index>=pointCount)) {visible[id.x]=0;return;}
    float4 av=points[index.x].camera,bv=points[index.y].camera,cv=points[index.z].camera;
    if(av.w<0 || bv.w<0 || cv.w<0) {visible[id.x]=0;return;}
    // A common affine transform preserves source collinearity. Independently
    // rounded float camera vertices can invent a tiny triangle, even when a
    // compensated determinant is exact for those already-rounded values.
    // This flag is established using exact source-coordinate arithmetic.
    if(faces[id.x].w==2U) {visible[id.x]=1;return;}
    float3 maximum=max(max(abs(av.xyz),abs(bv.xyz)),abs(cv.xyz));
    float scale=max(1.0,max(maximum.x,max(maximum.y,maximum.z)));
    // Exact power-of-two normalization avoids overflow without rounding each
    // coordinate by a different arbitrary ratio before the determinant.
    float power=asfloat(asuint(scale)&0x7f800000U);
    precise float3 a=av.xyz/power,b=bv.xyz/power,c=cv.xyz/power;
    precise float2 determinant=add2(triple(a.x,b.y,c.z),-triple(a.x,b.z,c.y));
    determinant=add2(determinant,-triple(a.y,b.x,c.z));
    determinant=add2(determinant,triple(a.y,b.z,c.x));
    determinant=add2(determinant,triple(a.z,b.x,c.y));
    determinant=add2(determinant,-triple(a.z,b.y,c.x));
    precise float normalized=scale/power;
    precise float tolerance=normalized*normalized*normalized*1e-12;
    precise float2 difference=add2(determinant,float2(-tolerance,0));
    visible[id.x]=(difference.x<0 || (difference.x==0 && difference.y<=0))?1:0;
}
