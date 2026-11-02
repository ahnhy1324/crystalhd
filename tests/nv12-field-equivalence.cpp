// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in real-GPU layout control: independently materialize source field rows.
// Identical rendering holds sampling constant; not absolute chroma conformance,
// A/V, endurance or a leak-free GL verdict.
#include "field-view-gpu.h"
struct Planes {std::vector<uint8_t> y,uv;};
static Planes source(unsigned w,unsigned h,unsigned generation) {
    Planes p;p.y.resize(size_t(w)*h);p.uv.resize(size_t(w)*h/2);
    for(unsigned row=0;row<h;++row)for(unsigned x=0;x<w;++x)
        p.y[size_t(row)*w+x]=24+(x*7+row*11+generation*37)%208;
    // Dense per-pair/per-row chroma; opposite field rows differ independently.
    for(unsigned row=0;row<h/2;++row)for(unsigned pair=0;pair<w/2;++pair) {
        p.uv[size_t(row)*w+2*pair]=32+(pair*17+(row/2)*29+generation*31+(row&1)*83)%192;
        p.uv[size_t(row)*w+2*pair+1]=32+(pair*23+(row/2)*13+generation*47+(row&1)*107)%192;
    }
    return p;
}
static Planes materialize(const Planes &original,unsigned w,unsigned h,unsigned parity,bool wrong_uv=false) {
    Planes p;p.y.resize(size_t(w)*h/2);p.uv.resize(size_t(w)*h/4);
    for(unsigned row=0;row<h/2;++row)
        std::copy_n(original.y.data()+size_t(2*row+parity)*w,w,p.y.data()+size_t(row)*w);
    for(unsigned row=0;row<h/4;++row)
        std::copy_n(original.uv.data()+size_t(2*row+(wrong_uv?1-parity:parity))*w,w,p.uv.data()+size_t(row)*w);
    return p;
}
static void write(GbmAllocation &allocation,const Planes &planes) {
    allocation.write_nv12(planes.y.data(),allocation.width,planes.y.size(),
                          planes.uv.data(),allocation.width,planes.uv.size());
}
static std::vector<uint8_t> render(Renderer &renderer,const Image &image,unsigned parity) {
    // Same shader/normalized Bob coordinates; only the source layout changes.
    renderer.draw_to(image,true,parity,renderer.picture.fbo,0,0,
                     renderer.picture.w,renderer.picture.h,false,false);
    std::vector<uint8_t> p(size_t(renderer.picture.w)*renderer.picture.h*4);
    glReadPixels(0,0,renderer.picture.w,renderer.picture.h,GL_RGBA,GL_UNSIGNED_BYTE,p.data());
    need(glGetError()==GL_NO_ERROR,"equivalence RGB readback");return p;
}
struct Difference {unsigned maximum=0;size_t over2=0,alpha=0;};
static Difference compare(const std::vector<uint8_t> &a,const std::vector<uint8_t> &b) {
    need(a.size()==b.size()&&a.size()%4==0,"equivalence image extents");Difference d;
    for(size_t i=0;i<a.size();i+=4) {
        for(unsigned c=0;c<3;++c) {
            const unsigned error=std::abs(int(a[i+c])-int(b[i+c]));
            d.maximum=std::max(d.maximum,error);d.over2+=error>2;
        }
        d.alpha+=a[i+3]!=255||b[i+3]!=255;
    }
    return d;
}
int main() {
 try {
    setvbuf(stdout,nullptr,_IOLBF,0);need(temperature()<75000,"thermal admission");
    yuy2_gpu::GPU gpu("/dev/dri/renderD128");unsigned comparisons=0;bool sentinel=false;
    const std::array<unsigned,4> geometries[]={{640,360,640,360},{720,576,854,480},{1280,720,1920,1080}};
    for(auto geometry:geometries) {
        const auto [w,h,dw,dh]=geometry;
        GbmAllocation original(gpu,w,h,DRM_FORMAT_NV12),extracted(gpu,w,h/2,DRM_FORMAT_NV12);
        Renderer renderer(w,h,dw,dh);
        for(unsigned generation=0;generation<2;++generation) {
            const auto bytes=source(w,h,generation);write(original,bytes);
            for(bool sd:{false,true})for(bool mpeg2:{false,true})for(unsigned parity=0;parity<2;++parity) {
                const auto matrix=sd?EGL_ITU_REC601_EXT:EGL_ITU_REC709_EXT;
                const auto site=mpeg2?EGL_YUV_CHROMA_SITING_0_EXT:EGL_YUV_CHROMA_SITING_0_5_EXT;
                Image field(gpu,original,true,parity,matrix,false,DRM_FORMAT_NV12,original.uv_offset,original.uv_pitch,site);
                need(field.texture,"production NV12 field import");const auto actual=render(renderer,field,parity);
                write(extracted,materialize(bytes,w,h,parity));
                {
                    Image ordinary(gpu,extracted,false,0,matrix,false,DRM_FORMAT_NV12,extracted.uv_offset,extracted.uv_pitch,site);
                    need(ordinary.texture,"materialized progressive NV12 import");
                    const auto d=compare(actual,render(renderer,ordinary,parity));
                    printf("{\"type\":\"nv12-field-equivalence\",\"source\":[%u,%u],\"output\":[%u,%u],\"generation\":%u,\"matrix\":%u,\"site\":\"%s\",\"parity\":%u,\"pixels\":%zu,\"max_error\":%u,\"components_over2\":%zu,\"nonopaque\":%zu}\n",
                        w,h,dw,dh,generation,sd?601:709,mpeg2?"mpeg2":"jpeg",parity,actual.size()/4,d.maximum,d.over2,d.alpha);
                    need(!d.over2&&!d.alpha,"all-pixel materialized-field equivalence tolerance2");++comparisons;
                }
                if(!sentinel) {
                    write(extracted,materialize(bytes,w,h,parity,true));
                    Image wrong(gpu,extracted,false,0,matrix,false,DRM_FORMAT_NV12,extracted.uv_offset,extracted.uv_pitch,site);
                    need(wrong.texture,"negative UV-parity control import");
                    const auto d=compare(actual,render(renderer,wrong,parity));
                    printf("{\"type\":\"nv12-wrong-UV-parity\",\"components_over2\":%zu,\"max_error\":%u}\n",d.over2,d.maximum);
                    need(d.over2>1000,"opposite UV-field sentinel must differ");sentinel=true;
                }
            }
        }
    }
    need(comparisons==48&&sentinel,"complete NV12 equivalence matrix");
    puts("PASS NV12 materialized-field equivalence:48 all-pixel comparisons and wrong-UV sentinel; absolute chroma siting NOT_QUALIFIED");
 }catch(const std::exception &e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
