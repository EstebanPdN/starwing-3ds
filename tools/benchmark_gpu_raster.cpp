#include "starfox/render/gpu_raster.hpp"
#include <chrono>
#include <iostream>
#include <string_view>
int main(int argc,char** argv) {
    using namespace starfox::render;
    if(argc==2 && std::string_view(argv[1])=="--binning-only") {
        // Measure the actual source-command distribution before selecting a
        // GPU binning representation. No GPU submission or window creation.
        starfox::assets::Shape shape;
        shape.vertices={{-90,-60,0},{-80,60,25},{90,65,0},{80,-65,-20}};
        shape.colour_words={0x006c};shape.faces={{-1,0,{0,0,127},{0,1,2,3}}};
        for(unsigned scale:{1U,2U,4U}) {
            Framebuffer frame(400,224,scale);RasterCommands batch;
            batch.reset(frame.stored_width(),frame.stored_height());frame.record_to(&batch);
            RenderSettings settings;settings.render_scale=scale;SoftwareRenderer renderer(settings);
            for(unsigned object=0;object<32;++object) {
                RenderPose pose;pose.z=100+object*13;pose.x=int(object%8)*15-50;
                pose.vanish_x=200;pose.vanish_y=112;renderer.draw(shape,pose,frame,false);
            }
            frame.record_to(nullptr);std::vector<double> samples;
            for(unsigned repeat=0;repeat<11;++repeat) {
                const auto start=std::chrono::steady_clock::now();
                for(unsigned run=0;run<32;++run) batch.bin_rows();
                const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/32;
                if(repeat>=2) samples.push_back(us);
            }
            std::sort(samples.begin(),samples.end());
            const auto tiles=std::uint64_t((batch.width()+63)/64)*batch.height();
            std::cout<<"binning scale="<<scale<<" commands="<<batch.commands.size()
                <<" tiles="<<tiles<<" references="<<batch.indices.size()
                <<" csr_bytes="<<(batch.rows.size()+batch.indices.size())*4
                <<" dense_bitset_bytes="<<tiles*((batch.commands.size()+31)/32)*4
                <<" median_us="<<samples[samples.size()/2]<<'\n';
        }
        return 0;
    }
    if(argc==2 && std::string_view(argv[1])=="--geometry-only") {
        // Isolate transformation/projection cost; no faces, pixels or GPU
        // submission. This is not an in-game frame-rate benchmark.
        starfox::assets::Shape mesh;
        for(int i=0;i<256;++i) mesh.vertices.push_back({i%31-15,i%23-11,i%17-8});
        for(unsigned scale:{1U,2U,4U}) for(bool fractional:{false,true}) {
            RenderSettings config;config.render_scale=scale;SoftwareRenderer renderer(config);
            Framebuffer frame(224,192,scale);
            RenderPose pose;pose.z=500;pose.use_rotation_matrix=true;
            pose.rotation_matrix={32767,0,0,0,32767,0,0,0,32767};
            pose.subpixel_projection=fractional;
            std::vector<double> samples;
            for(unsigned sample=0;sample<11;++sample) {
                const auto start=std::chrono::steady_clock::now();
                for(unsigned i=0;i<2000;++i) {pose.x=int(i%41)-20;renderer.draw(mesh,pose,frame,false);}
                const auto elapsed=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/2000;
                if(sample>=2) samples.push_back(elapsed);
            }
            std::sort(samples.begin(),samples.end());
            std::cout<<"geometry vertices=256 scale="<<scale<<" fractional="<<fractional<<" median_us="<<samples[samples.size()/2]<<'\n';
        }
        return 0;
    }
    for(unsigned scale:{1U,2U,4U}) {
        Framebuffer foreground(9,7,scale),layer(9,7,scale);
        foreground.begin_write_coverage();foreground.set(2,3,0);
        if(std::count(foreground.write_coverage().begin(),foreground.write_coverage().end(),1)!=int(scale*scale)) return 20;
        foreground.begin_write_coverage();layer.set(1,2,17);
        composite_transparent_layer(layer,foreground,{});
        if(std::count(foreground.write_coverage().begin(),foreground.write_coverage().end(),1)!=int(scale*scale)) return 21;
        foreground.begin_write_coverage();foreground.clear();
        if(std::count(foreground.write_coverage().begin(),foreground.write_coverage().end(),1)!=int(foreground.pixels().size())) return 22;
        foreground.begin_write_coverage();foreground.copy_pixels_from(layer);
        if(std::count(foreground.write_coverage().begin(),foreground.write_coverage().end(),1)!=int(foreground.pixels().size())) return 23;
        foreground.begin_write_coverage();foreground.end_write_coverage();foreground.clear();
        if(std::count(foreground.write_coverage().begin(),foreground.write_coverage().end(),1)!=0) return 24;
    }
    starfox::assets::Shape shape;
    shape.vertices={{-90,-60,0},{-80,60,25},{90,65,0},{80,-65,-20}};
    shape.colour_words={0x0009,0x006c,0x4000};
    shape.faces={{-1,1,{0,0,127},{0,1,2,3}},{-1,1,{0,0,127},{3,2,1,0}},
        {-1,0,{0,0,127},{0,2}}};
    starfox::assets::TextureImage texture;
    texture.descriptor=0x4000;texture.u_mask=texture.v_mask=7;
    texture.coordinates={{{0,0},{0,7},{7,7},{7,0}}};texture.texels.resize(64);
    for(unsigned i=0;i<64;++i) texture.texels[i]=std::uint8_t(i%7);
    shape.textures.push_back(texture);
    const auto faces=shape.faces;
    {
        RasterCommands edges;edges.reset(8,2);RasterCommand edge;
        edge.left=0;edge.right=8;edge.top=0;edge.bottom=1;edge.even=7;edge.reserved1=1;edge.u=-1;edge.v=7;edges.add(edge);
        edge.top=1;edge.bottom=2;edge.u=2;edge.v=5;edges.add(edge);
        Framebuffer expected(8,2),actual(8,2);expected.set(7,0,7);expected.set(2,1,7);expected.set(5,1,7);
        replay_raster_commands(edges,actual,nullptr);
        if(actual.pixels()!=expected.pixels()) throw std::runtime_error("Edge-only replay coverage failed");
    }
    {
        RasterCommands masked;masked.reset(67,3);masked.texels.resize(40);
        RasterCommand c;c.right=67;c.bottom=3;c.even=0;c.odd=9;c.dither=1;
        c.tag=std::uint32_t(PixelLayer::three_d);c.reserved1=4;c.texture_offset=4;c.u_mask=12;c.v_mask=3;
        c.has_surface=1;c.surface={0,0,1,123};
        Framebuffer expected(67,3),actual(67,3);
        SurfaceBuffer mask_surfaces(67,3);
        const auto check_mask_surfaces=[&] {
            for(unsigned y=0;y<3;++y) for(unsigned x=0;x<67;++x) {
                const auto& s=mask_surfaces.samples()[y*67+x];
                const bool covered=x==0 || x==31 || x==32 || x==66;
                if(s.valid!=covered || (covered && (s.palette_index!=(((x^y)&1)?9:0)
                    || s.normal_x!=0 || s.normal_y!=0 || s.normal_z!=1 || s.depth!=123)))
                    throw std::runtime_error("Masked solid surface ownership mismatch");
            }
        };
        expected.enable_layer_tags(true);actual.enable_layer_tags(true);
        for(unsigned y=0;y<3;++y) for(unsigned x:{0U,31U,32U,66U}) {
            masked.texels[4+y*12+x/8]|=std::uint8_t(1U<<(x&7));
            expected.set_stored(x,y,((x^y)&1)?9:0,PixelLayer::three_d);
        }
        masked.add(c);
        replay_raster_commands(masked,actual,&mask_surfaces);check_mask_surfaces();
        if(actual.pixels()!=expected.pixels() || actual.layer_tags()!=expected.layer_tags())
            throw std::runtime_error("Masked solid replay lost gaps, zero writes or word boundaries");
        for(unsigned invalid=0;invalid<5;++invalid) {
            auto bad=c;
            if(invalid==0) bad.u_mask=0;
            if(invalid==1) bad.u_mask=2;
            if(invalid==2) bad.texture_offset=1;
            if(invalid==3) bad.v_mask=4;
            if(invalid==4) bad.texture_offset=0xfffffffcU;
            masked.commands={bad};expected.clear();
            replay_raster_commands(masked,actual,nullptr);
            if(actual.pixels()!=expected.pixels() || actual.layer_tags()!=expected.layer_tags())
                throw std::runtime_error("Invalid solid mask replay was not rejected");
        }
    }
    for(unsigned width:{224U,257U}) for(unsigned scale:{1U,2U,4U}) for(unsigned mode=0;mode<14;++mode) for(bool textured:{false,true}) {
        shape.faces=faces;
        Framebuffer reference(width,193,scale),recorded(width,193,scale),replayed(width,193,scale);
        for(auto* frame:{&reference,&recorded,&replayed}) frame->enable_layer_tags(true);
        SurfaceBuffer expected(width*scale,193*scale),actual(width*scale,193*scale),software(width*scale,193*scale);
        RenderSettings settings;settings.render_scale=scale;SoftwareRenderer renderer(settings);
        RenderPose pose;pose.z=mode==9?40:280;pose.vanish_x=128;pose.texture_scroll_x=-3;pose.texture_scroll_y=260;
        pose.wireframe_mode=mode<3?mode:0;pose.wobble_mode=mode>=6?mode-6:0;
        pose.cel_mode=mode==4;pose.wave_mode=mode==5;pose.wave_offset=-17;
        pose.continuous_geometry=mode==8;shape.faces[0].colour_id=shape.faces[1].colour_id=textured?2:1;
        pose.use_rotation_matrix=mode==7 || mode==8;
        pose.rotation_matrix={32767,0,0,0,32767,0,0,0,32767};
        if(mode==10) shape.faces={{-1,2,{0,0,127},{0},true}};
        if(mode>=11) {
            pose.simple_scaled_sprite=true;pose.simple_sprite_colour=2;pose.simple_sprite_world_size=80;
            if(mode==12) pose.palette_override=203;
            if(mode==13) {pose.effect_clip_left=110;pose.effect_clip_right=140;}
        }
        RasterCommands batch;batch.reset(recorded.stored_width(),recorded.stored_height());recorded.record_to(&batch);
        renderer.draw(shape,pose,reference,false,&expected);
        renderer.draw(shape,pose,recorded,false,&actual);
        if(mode==7 && !textured) {
            std::vector<unsigned> row_writes(recorded.stored_height());
            for(const auto& command:batch.commands)
                for(int row=std::max(0,command.top);row<std::min(int(row_writes.size()),command.bottom);++row)
                    ++row_writes[std::size_t(row)];
            if(std::none_of(row_writes.begin(),row_writes.end(),[&](unsigned writes){return writes>shape.faces.size();}))
                throw std::runtime_error("Repeated-row wobble fixture no longer exercises multiple spans per face-row");
        }
        // A later non-surface write must not erase underlying metadata.
        reference.set(127,96,3);recorded.set(127,96,3);
        recorded.record_to(nullptr);
        replay_raster_commands(batch,replayed,&software);
        if(reference.pixels()!=replayed.pixels() || reference.layer_tags()!=replayed.layer_tags()) {
            std::cerr<<"CPU command parity failed scale="<<scale<<" mode="<<mode<<" textured="<<textured<<'\n';return 2;
        }
        std::vector<const SurfaceBuffer*> comparisons{&software};
        for(std::size_t i=0;i<expected.samples().size();++i) for(const auto* samples:comparisons) {
            const auto& a=expected.samples()[i];const auto& b=samples->samples()[i];
            if(a.valid!=b.valid || a.palette_index!=b.palette_index || a.normal_x!=b.normal_x || a.normal_y!=b.normal_y
                || a.normal_z!=b.normal_z || a.depth!=b.depth) {std::cerr<<"Surface parity failed\n";return 5;}
        }
    }
    shape.faces=faces;
    std::cout<<"168 native raster command replay cases match source renderer exactly\n";
}
