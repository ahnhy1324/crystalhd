// SPDX-License-Identifier: GPL-2.0-or-later
// Separate resource experiment. Existing pixel/contract qualification retains
// its full observers and acceptance criteria in v4l2-decode.cpp.
#define CRYSTALHD_TEST_GPU 1
#define main crystalhd_qualification_main
#include "v4l2-decode.cpp"
#undef main
#include <sys/resource.h>

static double bench_clock(clockid_t kind) {
    timespec t{}; require(!clock_gettime(kind, &t), "benchmark clock");
    return t.tv_sec + t.tv_nsec / 1e9;
}
static void until(double target) {
    timespec t{time_t(target), long((target - time_t(target)) * 1e9)};
    int rc;
    do { rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, nullptr); }
    while (rc == EINTR);
    require(!rc, "presentation cadence sleep");
}
class ResourceRenderer {
    yuy2_gpu::GPU gpu{"/dev/dri/renderD128"};
    yuy2_gpu::Picture picture;
    bool upload;
    std::vector<uint8_t> first, second;
public:
    double cpu = 0, wall = 0;
    uint64_t copies = 0;
    ResourceRenderer(unsigned w, unsigned h, bool staging)
        : picture(w, h, staging), upload(staging) {
        if (upload) { first.resize(size_t(w)*h*2); second.resize(first.size()); }
    }
    size_t staging_bytes() const { return first.size() + second.size(); }
    void present(int fd, const v4l2_buffer& b, const Queue::Mapping& map, unsigned stride) {
        const double c = bench_clock(CLOCK_PROCESS_CPUTIME_ID), t = bench_clock(CLOCK_MONOTONIC);
        if (upload) {
            for (unsigned y=0; y<picture.h; ++y)
                memcpy(first.data()+size_t(y)*picture.w*2,
                       static_cast<const uint8_t*>(map.address)+size_t(y)*stride, picture.w*2);
            memcpy(second.data(),first.data(),first.size()); copies += first.size()*2;
            glBindTexture(GL_TEXTURE_2D,picture.packed);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,picture.w/2,picture.h,GL_RGBA,GL_UNSIGNED_BYTE,second.data());
            picture.draw(false,picture.packed);
        } else {
            v4l2_exportbuffer exp{}; exp.type=V4L2_BUF_TYPE_VIDEO_CAPTURE;
            exp.index=b.index; exp.flags=O_CLOEXEC|O_RDONLY;
            checked(fd,VIDIOC_EXPBUF,&exp,"EXPBUF measured CAPTURE");
            try {
                yuy2_gpu::Imported image(gpu,exp.fd,picture.w,picture.h,stride);
                picture.draw(true,image.texture);
            } catch (...) { close(exp.fd); throw; }
            close(exp.fd);
        }
        // Imported image destruction and glFinish precede CAPTURE requeue.
        cpu += bench_clock(CLOCK_PROCESS_CPUTIME_ID)-c;
        wall += bench_clock(CLOCK_MONOTONIC)-t;
    }
};

static Input prepare_source(const std::string& path) {
    AVFormatContext* f=nullptr;
    require(avformat_open_input(&f,path.c_str(),av_find_input_format("mov"),nullptr)>=0,"open source descriptor");
    struct Cleanup { AVFormatContext*& f; ~Cleanup(){avformat_close_input(&f);} } cleanup{f};
    const int v=av_find_best_stream(f,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
    require(v>=0,"source video descriptor");
    const auto* p=f->streams[v]->codecpar;
    require(p->codec_id==AV_CODEC_ID_H264 &&
        (p->field_order==AV_FIELD_PROGRESSIVE || p->field_order==AV_FIELD_UNKNOWN),
        "resource fixture must be progressive H264");
    // Frozen fixtures are colour-qualified outside the measured process.
    // Report unspecified container descriptors without inferring them.
    // Never call find_stream_info: its probe
    // can invoke software decoding and contaminate whole-process CPU/RSS.
    Input input{{},unsigned(p->width),unsigned(p->height),0,{}};
    AVBSFContext* filter=nullptr;
    require(av_bsf_alloc(av_bsf_get_by_name("h264_mp4toannexb"),&filter)>=0,"resource AnnexB filter");
    struct FilterCleanup { AVBSFContext*& f; ~FilterCleanup(){av_bsf_free(&f);} } filter_cleanup{filter};
    require(avcodec_parameters_copy(filter->par_in,p)>=0,"resource codec descriptor copy");
    filter->time_base_in=f->streams[v]->time_base;
    require(av_bsf_init(filter)>=0,"resource AnnexB initialization");
    AVPacket* packet=av_packet_alloc(); require(packet,"source packet allocation");
    struct PacketCleanup { AVPacket*& p; ~PacketCleanup(){av_packet_free(&p);} } packet_cleanup{packet};
    size_t bytes=0;
    auto receive=[&](){
        for(;;) {
            int rc=av_bsf_receive_packet(filter,packet);
            if(rc==AVERROR_EOF || rc==AVERROR(EAGAIN))break;
            require(rc>=0 && packet->size>0 && packet->pts!=AV_NOPTS_VALUE && packet->pts>=0,
                "benchmark cadence requires actual source PTS/payload; no inferred timestamps");
            if(input.packets.empty())
                printf("RESOURCE_SOURCE container_matrix=%d primaries=%d transfer=%d range=%d render_matrix=bt709 render_range=limited decoder_open=0\n",
                    p->color_space,p->color_primaries,p->color_trc,p->color_range);
            bytes+=packet->size;require(bytes<=256U*1024*1024,"resource fixture byte bound");
            const int64_t pts=av_rescale_q(packet->pts,filter->time_base_out,AVRational{1,1000000});
            input.largest=std::max(input.largest,size_t(packet->size));
            input.packets.push_back({std::vector<unsigned char>(packet->data,packet->data+packet->size),uint64_t(pts),pts,bool(packet->flags&AV_PKT_FLAG_KEY)});
            av_packet_unref(packet);
        }
    };
    int rc;
    while ((rc=av_read_frame(f,packet))>=0) {
        if (packet->stream_index==v) {
            require(av_bsf_send_packet(filter,packet)>=0,"resource AnnexB input");receive();
        }
        av_packet_unref(packet);
    }
    require(rc==AVERROR_EOF,"source descriptor read failed");
    require(av_bsf_send_packet(filter,nullptr)>=0,"resource AnnexB flush");receive();
    return input;
}

int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IOLBF,0);
    try {
        std::string path,media,mode; bool paced=true,preflight=false; unsigned timeout=30;
        for (int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--help") { puts("usage: v4l2-resource-bench --input MP4 --mode decode|upload|direct [--device NODE] [--unpaced] [--preflight] [--timeout 30]\nSource-paced actual PTS by default; no timed pixel hashes, RGB readback, software video decoder or physical-display claim.");return 0; }
            if(arg=="--unpaced") {paced=false;continue;}
            if(arg=="--preflight") {preflight=true;continue;}
            require(i+1<argc,"missing value for "+arg); std::string value=argv[++i];
            if(arg=="--input") media=value;
            else if(arg=="--device") path=value;
            else if(arg=="--mode") mode=value;
            else if(arg=="--timeout") timeout=std::stoul(value);
            else throw std::runtime_error("unknown resource option "+arg);
        }
        require(!media.empty() && (mode=="decode"||mode=="upload"||mode=="direct") && timeout && timeout<=120,"provide bounded resource input/mode");
        Input input=prepare_source(media);
        require(input.packets.size()>1 && input.packets.size()<=2000 && input.width<=1920 && input.height<=1080,"bounded resource fixture geometry/count");
        std::vector<uint64_t> order; for(const auto& p:input.packets)order.push_back(p.timestamp);
        std::sort(order.begin(),order.end());
        require(std::adjacent_find(order.begin(),order.end())==order.end() && order.back()-order.front()<=60000000,"source PTS must be distinct and span at most60s");
        if(preflight) {printf("PREFLIGHT mode=%s width=%u height=%u frames=%zu source_span_us=%llu hardware=NOT_RUN\n",mode.c_str(),input.width,input.height,order.size(),(unsigned long long)(order.back()-order.front()));return 0;}
        std::unique_ptr<ResourceRenderer> renderer;
        if(mode!="decode")renderer=std::make_unique<ResourceRenderer>(input.width,input.height,mode=="upload");
        if(path.empty())path=discover();
        Device device(path);
        v4l2_capability caps{}; checked(device.fd,VIDIOC_QUERYCAP,&caps,"resource QUERYCAP");
        const unsigned cap=caps.capabilities&V4L2_CAP_DEVICE_CAPS?caps.device_caps:caps.capabilities;
        require((cap&(V4L2_CAP_VIDEO_M2M|V4L2_CAP_STREAMING))==(V4L2_CAP_VIDEO_M2M|V4L2_CAP_STREAMING),"real single-plane M2M streaming node required");
        v4l2_event_subscription sub{};sub.type=V4L2_EVENT_SOURCE_CHANGE;
        checked(device.fd,VIDIOC_SUBSCRIBE_EVENT,&sub,"resource SOURCE_CHANGE");
        Queue output(device.fd,V4L2_BUF_TYPE_VIDEO_OUTPUT),capture(device.fd,V4L2_BUF_TYPE_VIDEO_CAPTURE);
        v4l2_format fmt{};fmt.type=output.type;fmt.fmt.pix.width=input.width;fmt.fmt.pix.height=input.height;
        fmt.fmt.pix.pixelformat=V4L2_PIX_FMT_H264;fmt.fmt.pix.field=V4L2_FIELD_NONE;
        fmt.fmt.pix.sizeimage=std::max(size_t(2*1024*1024),input.largest);
        fmt.fmt.pix.colorspace=V4L2_COLORSPACE_REC709;fmt.fmt.pix.ycbcr_enc=V4L2_YCBCR_ENC_709;
        fmt.fmt.pix.quantization=V4L2_QUANTIZATION_LIM_RANGE;fmt.fmt.pix.xfer_func=V4L2_XFER_FUNC_709;fmt.fmt.pix.priv=V4L2_PIX_FMT_PRIV_MAGIC;
        checked(device.fd,VIDIOC_S_FMT,&fmt,"resource S_FMT");output.allocate(8);
        size_t next=0,completed=0,frames=0;bool stopped=false,last=false,configured=false;
        unsigned stride=0,sizeimage=0;uint64_t capture_bytes=0,output_bytes=0,payload_bytes=0;
        for(const auto& b:output.buffers)output_bytes+=b.length;
        auto feed=[&](){for(unsigned i=0;i<output.buffers.size() && next<input.packets.size();++i)if(!output.buffers[i].queued)output.queue(i,&input.packets[next++]);};
        feed(); const double phase_cpu=bench_clock(CLOCK_PROCESS_CPUTIME_ID),phase_start=bench_clock(CLOCK_MONOTONIC);
        output.start(); double first_ready=0,first_done=0,last_done=0,max_late=0;size_t late_period=0;
        const double period=double(order.back()-order.front())/1e6/(order.size()-1);
        while(!last || completed!=input.packets.size()) {
            require(bench_clock(CLOCK_MONOTONIC)-phase_start<timeout,"resource probe deadline");
            v4l2_event event{};
            while(call(device.fd,VIDIOC_DQEVENT,&event)==0) {
                require(event.type==V4L2_EVENT_SOURCE_CHANGE && !configured,"unexpected resource stream change");
                fmt={};fmt.type=capture.type;checked(device.fd,VIDIOC_G_FMT,&fmt,"resource G_FMT");
                require(fmt.fmt.pix.pixelformat==V4L2_PIX_FMT_YUYV && fmt.fmt.pix.field==V4L2_FIELD_NONE &&
                    fmt.fmt.pix.width==input.width && fmt.fmt.pix.height==input.height && fmt.fmt.pix.bytesperline>=input.width*2,"actual YUYV CAPTURE must equal source geometry");
                stride=fmt.fmt.pix.bytesperline;sizeimage=fmt.fmt.pix.sizeimage;
                capture.allocate(12);for(unsigned i=0;i<capture.buffers.size();++i){capture_bytes+=capture.buffers[i].length;capture.queue(i);}capture.start();configured=true;
                printf("RESOURCE_FORMAT width=%u height=%u stride=%u sizeimage=%u capture_buffers=%zu capture_alloc_bytes=%llu output_alloc_bytes=%llu\n",input.width,input.height,stride,sizeimage,capture.buffers.size(),(unsigned long long)capture_bytes,(unsigned long long)output_bytes);
            }
            require(errno==EAGAIN || errno==ENOENT,"resource DQEVENT error");
            v4l2_buffer b{};while(output.dequeue(b))++completed;feed();
            if(!stopped && completed==input.packets.size()) {v4l2_decoder_cmd cmd{};cmd.cmd=V4L2_DEC_CMD_STOP;checked(device.fd,VIDIOC_DECODER_CMD,&cmd,"resource true EOF STOP");stopped=true;}
            while(configured && !last && capture.dequeue(b)) {
                const auto& map=capture.buffers[b.index];require(b.bytesused<=map.length,"resource CAPTURE extent");
                if(b.bytesused) {
                    const uint64_t ts=uint64_t(b.timestamp.tv_sec)*1000000+b.timestamp.tv_usec;
                    require(frames<order.size() && ts==order[frames],"resource source PTS/order mismatch");
                    require(b.bytesused>=uint64_t(input.height-1)*stride+input.width*2 && b.field==V4L2_FIELD_NONE,"resource visible geometry/field extent");
                    const double ready=bench_clock(CLOCK_MONOTONIC);if(!frames)first_ready=ready;
                    const double target=first_ready+double(ts-order.front())/1e6;
                    require(!paced || target<phase_start+timeout,"source cadence exceeds probe deadline");
                    if(paced)until(target);
                    if(renderer)renderer->present(device.fd,b,map,stride);
                    last_done=bench_clock(CLOCK_MONOTONIC);if(!frames)first_done=last_done;
                    const double late=last_done-target;max_late=std::max(max_late,late);if(late>period)++late_period;
                    ++frames;payload_bytes+=b.bytesused;
                }
                last=b.flags&V4L2_BUF_FLAG_LAST;require(!last||stopped,"resource premature LAST");
                if(!last)capture.queue(b.index); // only after GPU completion/image destruction
            }
            if(!last || completed!=input.packets.size()) {pollfd p{device.fd,POLLIN|POLLOUT|POLLPRI|POLLERR,0};int rc;do{rc=poll(&p,1,10);}while(rc<0&&errno==EINTR);require(rc>=0,"resource poll");}
        }
        require(configured && stopped && last && frames==input.packets.size(),"resource finite census incomplete");
        v4l2_buffer after{};after.type=capture.type;after.memory=V4L2_MEMORY_MMAP;
        require(call(device.fd,VIDIOC_DQBUF,&after)<0&&errno==EPIPE,"resource genuine terminal EPIPE required");
        const double elapsed=bench_clock(CLOCK_MONOTONIC)-phase_start,cpu=bench_clock(CLOCK_PROCESS_CPUTIME_ID)-phase_cpu;
        // Fresh K4 negative check after genuine LAST, outside decode-loop timing.
        expect_decoder_command_errno(device.fd,V4L2_DEC_CMD_START,EOPNOTSUPP,"resource nonempty drained START");
        puts("UNSUPPORTED_START_PASS errno=EOPNOTSUPP nonempty=1 genuine_last=1");
        rusage usage{};require(!getrusage(RUSAGE_SELF,&usage),"resource getrusage");
        printf("RESOURCE_RESULT mode=%s paced=%u frames=%zu output_completed=%zu width=%u height=%u stride=%u capture_alloc_bytes=%llu output_alloc_bytes=%llu staging_bytes=%zu consumer_copy_bytes=%llu capture_bytesused_total=%llu active_yuyv_bytes=%llu phase_wall_s=%.9f phase_cpu_s=%.9f phase_cpu_percent_one_core=%.6f cpu_ms_per_frame=%.6f output_interval_fps=%.6f render_cpu_s=%.9f render_wall_s=%.9f max_complete_late_ms=%.6f late_over_period=%zu self_user_s=%.6f self_system_s=%.6f peak_rss_kib=%ld hashes=NOT_RUN physical_display=NOT_RUN last=1 epipe=1\n",
            mode.c_str(),paced?1:0,frames,completed,input.width,input.height,stride,(unsigned long long)capture_bytes,(unsigned long long)output_bytes,renderer?renderer->staging_bytes():0,(unsigned long long)(renderer?renderer->copies:0),(unsigned long long)payload_bytes,(unsigned long long)(uint64_t(input.width)*input.height*2*frames),elapsed,cpu,cpu*100/elapsed,cpu*1000/frames,(frames-1)/(last_done-first_done),renderer?renderer->cpu:0,renderer?renderer->wall:0,max_late*1000,late_period,usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6,usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6,usage.ru_maxrss);
        return 0;
    } catch(const std::exception& e) {fprintf(stderr,"RESOURCE_FAIL: %s\n",e.what());return 1;}
}
