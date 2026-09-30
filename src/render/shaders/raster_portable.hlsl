struct Command {
    int left,top,right,bottom;
    uint even,odd,dither,tag;
    uint texture_offset,u_mask,v_mask,colour_base;
    int u,v,du,dv;
    float4 surface;
    uint has_surface,textured,scroll_x,scroll_y;
};
StructuredBuffer<Command> commands:register(t0,space0);
StructuredBuffer<uint> rows:register(t1,space0);
StructuredBuffer<uint> indices:register(t2,space0);
ByteAddressBuffer texels:register(t3,space0);
StructuredBuffer<uint> back_pixels:register(t4,space0);
StructuredBuffer<float4> back_surfaces:register(t5,space0);
StructuredBuffer<float4> geometry_planes:register(t6,space0);
StructuredBuffer<float> back_depth:register(t7,space0);
RWStructuredBuffer<uint> pixels:register(u0,space1);
RWStructuredBuffer<float4> surfaces:register(u1,space1);
RWStructuredBuffer<float> geometry_depth:register(u2,space1);
cbuffer Settings:register(b0,space2) {
    uint width,height,want_surface,reserved;
    uint has_back,has_back_surface,take_surface,padding;
    uint texel_bytes,reserved1,reserved2,reserved3;
    uint want_depth,plane_count,has_back_depth,depth_padding;
    float4 depth_projection; // focal x/y, center x/y in output pixels.
};
// Source wave arithmetic uses signed 16-bit wrapping before both phase steps.
int waveShift(int x,int offset,uint frame) {
    int phase=(int(uint(offset+x)<<16))>>16;
    phase=(int(uint((phase>>1)+int(frame&15U)-1)<<16))>>16;
    static const int sine[16]={0,1,2,3,3,3,2,1,0,-1,-2,-3,-3,-3,-2,-1};
    return sine[uint(phase)&15U];
}
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID) {
    if(id.x>=width || id.y>=height) return;
    bool have_pixel=false,have_surface=take_surface==0;
    uint packed=0;
    float4 surface=float4(0,0,1,0);
    float depth=0;
    uint pixel_owner=0,surface_owner=0;
    bool row_spans=(reserved&0x80000000U)!=0;
    bool wave_rows=row_spans && (padding&1U)!=0;
    int wave_delta=wave_rows?waveShift(int(id.x),int((padding>>1)&65535U),(padding>>17)&15U):0;
    bool sparse=!row_spans && (reserved&1U)!=0;
    bool tiled_spans=row_spans && (padding&0x80000000U)!=0;
    // Last covering command wins, as in the source painter. Surface metadata
    // survives later non-surface lines/sprites, matching SurfaceBuffer::set.
    uint row=id.y*((width+63)/64)+id.x/64;
    uint begin=row_spans?0:rows[row];
    if(tiled_spans) begin=row*((reserved&0x3fffffffU)+1)+1;
    for(uint cursor=tiled_spans?begin+indices[begin-1]:row_spans?(reserved&0x3fffffffU)*(wave_rows?2U:1U):rows[row+1];cursor>begin;) {
        --cursor;
        bool wave_candidate=wave_rows && (cursor&1U)!=0;
        int source_y=int(id.y)-(wave_candidate?wave_delta:0);
        if(source_y<0 || source_y>=int(height)) continue;
        uint polygon=wave_rows?cursor/2U:cursor;
        uint command_index=tiled_spans?indices[cursor]:row_spans?polygon*height+uint(source_y):indices[cursor+(sparse?height*((width+63)/64):0)];
        uint owner=command_index+1;
        // Sparse atomic scatter is unordered. Once both independent owners
        // outrank this command, neither coverage nor texture transparency
        // can change the result; avoid fetching its material altogether.
        if(sparse && have_pixel && owner<=pixel_owner && have_surface
            && (take_surface==0 || owner<=surface_owner)) continue;
        Command c=commands[command_index];
        if(wave_rows) {
            bool wave=c.textured==0 && (c.scroll_y&2U)!=0;
            if(wave!=wave_candidate) continue;
        }
        if(int(id.x)<c.left || int(id.x)>=c.right) continue;
        if(c.textured==0 && (c.scroll_y&1U)!=0 && int(id.x)!=c.u && int(id.x)!=c.v) continue;
        if(c.textured==0 && (c.scroll_y&4U)!=0) {
            uint bytes=texel_bytes;
            if(c.u_mask==0 || (c.u_mask&3U)!=0 || (c.texture_offset&3U)!=0
                || uint(source_y)>=c.v_mask || id.x/32>=c.u_mask/4 || c.texture_offset>bytes) continue;
            if(c.v_mask>(bytes-c.texture_offset)/c.u_mask) continue;
            uint bits=texels.Load(c.texture_offset+uint(source_y)*c.u_mask+(id.x/32)*4);
            if((bits&(1U<<(id.x&31)))==0) continue;
        }
        uint dither_scale=max(1U,c.scroll_x);
        uint colour=c.dither && (((id.x/dither_scale)^(id.y/dither_scale))&1)!=0?c.odd:c.even;
        if(c.textured!=0) {
            uint dx=id.x-uint(c.left);
            uint u,v;
            if(c.textured==1) {
                u=((((uint(c.u)+uint(c.du)*dx)&65535)>>8)+c.scroll_x)&c.u_mask;
                v=((((uint(c.v)+uint(c.dv)*dx)&65535)>>8)+c.scroll_y)&c.v_mask;
            } else if(c.textured==2) {
                u=((id.x-uint(c.u))/uint(c.dv))*(c.u_mask+1)/uint(c.du);
                v=((id.y-uint(c.v))/uint(c.dv))*(c.v_mask+1)/uint(c.du);
            } else {
                u=((uint(c.u)+uint(c.du)*(dx/uint(c.dv)))&65535)>>8;
                v=((uint(c.v)+uint(c.du)*((id.y-uint(c.top))/uint(c.dv)))&65535)>>8;
                if(u>c.u_mask || v>c.v_mask) continue;
            }
            uint offset=c.texture_offset+v*(c.u_mask+1)+u;
            uint texel=(texels.Load(offset&~3U)>>((offset&3U)*8))&255;
            if(texel==0) continue;
            colour=(c.colour_base+texel)&255;
            if(c.textured==2 && (c.scroll_x&256)!=0) colour=c.scroll_x&255;
        }
        if(!have_pixel || (sparse && owner>pixel_owner)) {
            packed=(packed&0xffff0000U)|colour|(c.tag<<8);have_pixel=true;pixel_owner=owner;
            if(want_depth!=0) {
                depth=0;
                uint plane_id=c.has_surface>>1;
                // Screen-space warps do not describe a pinhole-projected plane.
                // Mark them unknown rather than inventing plausible geometry.
                if(plane_id>0 && plane_id<=plane_count && !wave_rows
                    && !(c.textured==0 && (c.scroll_y&4U)!=0)) {
                    float4 plane=geometry_planes[plane_id-1];
                    float3 ray=float3((float2(id.xy)+.5-depth_projection.zw)/depth_projection.xy,1);
                    float denominator=dot(plane.xyz,ray);
                    float candidate=plane.w/denominator;
                    if(abs(denominator)>1e-8 && isfinite(candidate) && candidate>0) depth=candidate;
                }
            }
        }
        if(take_surface!=0 && c.has_surface!=0 && (!have_surface || (sparse && owner>surface_owner))) {
            packed=(packed&65535U)|(colour<<16)|(1U<<24);surface=c.surface;have_surface=true;surface_owner=owner;
        }
        if(!sparse && have_pixel && have_surface) break;
    }
    if((reserved&0x40000000U)!=0 && have_pixel) packed|=0x04000000U;
    if(has_back!=0) {
        uint back=back_pixels[id.y*width+id.x];
        if(!have_pixel) packed=(packed&0x01ff0000U)|(back&0x0c00ffffU);
        if(want_depth!=0 && !have_pixel && has_back_depth!=0) depth=back_depth[id.y*width+id.x];
        if((packed&0x01000000U)==0 && has_back_surface!=0 && (back&0x01000000U)!=0) {
            packed=(packed&0x0c00ffffU)|(back&0x01ff0000U);
            surface=back_surfaces[id.y*width+id.x];
        }
    }
    pixels[id.y*width+id.x]=packed;
    if(want_surface!=0) surfaces[id.y*width+id.x]=surface;
    if(want_depth!=0) geometry_depth[id.y*width+id.x]=depth;
}
