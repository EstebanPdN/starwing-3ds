struct Command {
    int left,top,right,bottom;
    uint even,odd,dither,tag;
    uint texture_offset,u_mask,v_mask,colour_base;
    int u,v,du,dv;
    float4 surface;
    uint has_surface,textured,scroll_x,scroll_y;
};
RWStructuredBuffer<Command> spans:register(u0,space1);
cbuffer Settings:register(b0,space2) {
    float4 camera_hi,camera_lo; // x, y, z, focal length
    float4 view_hi,view_lo; // vanish x/y, world diameter, scale
    int4 bounds; // effect clip left/right, stored width/height
    uint4 material; // texture masks, palette base, optional override
};
precise float2 sum2(float a,float b) {
    precise float s=a+b,v=s-a;return float2(s,(a-(s-v))+(b-v));
}
precise float2 add2(float2 a,float2 b) {precise float2 s=sum2(a.x,b.x);return sum2(s.x,s.y+a.y+b.y);}
precise float2 product2(float a,float b) {
    precise float ca=4097*a,cb=4097*b,ah=ca-(ca-a),bh=cb-(cb-b),al=a-ah,bl=b-bh;
    precise float p=a*b;return float2(p,((ah*bh-p)+ah*bl+al*bh)+al*bl);
}
precise float2 multiply2(float2 a,float2 b) {precise float2 p=product2(a.x,b.x);return sum2(p.x,p.y+a.x*b.y+a.y*b.x+a.y*b.y);}
precise float2 divide2(float2 a,float2 b) {precise float q=a.x/b.x;precise float2 r=add2(a,-multiply2(float2(q,0),b));return sum2(q,(r.x+r.y)/b.x);}
bool negative(float2 v) {return v.x<0 || (v.x==0 && v.y<0);}
int trunc2(float2 v) {
    int base=int(floor(v.x));float2 remainder=add2(v,float2(-float(base),0));
    if(negative(remainder)) {--base;remainder=add2(remainder,float2(1,0));}
    else if(!negative(add2(remainder,float2(-1,0)))) {++base;remainder=add2(remainder,float2(-1,0));}
    return base+(negative(v) && (remainder.x!=0 || remainder.y!=0)?1:0);
}
int round2(float2 v) {return trunc2(add2(v,float2(negative(v)?-0.5:0.5,0)));}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=uint(bounds.w)) return;
    Command c=(Command)0;
    float2 depth=float2(camera_hi.z,camera_lo.z);
    if(!negative(add2(depth,float2(-128,0))) && view_hi.z>0) {
        float2 focal=float2(camera_hi.w,camera_lo.w);
        int dimension=clamp(trunc2(divide2(multiply2(float2(view_hi.z,view_lo.z),focal),depth)),0,240);
        if(dimension>0) {
            int scale=int(view_hi.w);
            int left=round2(float2(view_hi.x,view_lo.x))+trunc2(divide2(multiply2(float2(camera_hi.x,camera_lo.x),focal),depth))-dimension/2;
            int top=round2(float2(view_hi.y,view_lo.y))+trunc2(divide2(multiply2(float2(camera_hi.y,camera_lo.y),focal),depth))-dimension/2;
            int first=left,last=left+dimension;
            if(bounds.y>bounds.x) {first=max(first,bounds.x);last=min(last,bounds.y);}
            if(first<last && int(id.x)>=top*scale && int(id.x)<(top+dimension)*scale) {
                c.left=first*scale;c.right=last*scale;c.top=int(id.x);c.bottom=c.top+1;
                c.tag=1;c.textured=2;c.u_mask=material.x;c.v_mask=material.y;c.colour_base=material.z;
                c.u=left*scale;c.v=top*scale;c.du=dimension;c.dv=scale;c.scroll_x=material.w;
            }
        }
    }
    spans[id.x]=c;
}
