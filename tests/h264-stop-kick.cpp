// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded #533 legacy-channel experiment, not a production drain policy.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>
extern "C" {
#include "bc_dts_types.h"
#include "bc_dts_defs.h"
#include "libcrystalhd_if.h"
}

static void need(bool good, const std::string &message) {
    if (!good) throw std::runtime_error(message);
}
static void good(const char *name, BC_STATUS result) {
    need(result==BC_STS_SUCCESS,std::string(name)+" status="+std::to_string(result));
}
static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream stream(path,std::ios::binary);
    need(bool(stream),"open input "+path);
    std::vector<uint8_t> result((std::istreambuf_iterator<char>(stream)),{});
    need(result.size()<=16U*1024U*1024U,"bounded input"); return result;
}
static uint64_t integer(const std::vector<uint8_t> &data,size_t &cursor,unsigned bytes) {
    need(bytes<=8 && cursor<=data.size() && bytes<=data.size()-cursor,"container integer");
    uint64_t value=0;
    for (unsigned i=0;i<bytes;++i) value|=uint64_t(data[cursor++])<<(8*i);
    return value;
}
struct Packet { uint64_t token; int64_t pts; std::vector<uint8_t> data; };
static std::vector<Packet> packets(const std::string &directory) {
    auto data=read_file(directory+"/original20.packets");
    need(data.size()>=12 && !memcmp(data.data(),"CHDKICK1",8),"container identity");
    size_t cursor=8; need(integer(data,cursor,4)==20,"exact20 fixture");
    std::vector<Packet> result;
    for (unsigned i=0;i<20;++i) {
        size_t count=integer(data,cursor,4);
        uint64_t token=integer(data,cursor,8); int64_t pts=integer(data,cursor,8);
        need(token==uint64_t(i+1)*100000 && count && count<=1024*1024 &&
             cursor<=data.size() && count<=data.size()-cursor,"packet identity/extent");
        result.push_back({token,pts,{data.begin()+cursor,data.begin()+cursor+count}});
        cursor+=count;
    }
    need(cursor==data.size(),"container trailing data"); return result;
}

#define FUNCTIONS(X) \
    X(DtsDeviceOpen) X(DtsDeviceClose) X(DtsSetInputFormat) X(DtsOpenDecoder) \
    X(DtsSetColorSpace) X(DtsStartDecoder) X(DtsStartCapture) X(DtsStopDecoder) \
    X(DtsCloseDecoder) X(DtsProcInput) X(DtsTxFreeSize) X(DtsGetDriverStatus) \
    X(DtsProcOutputNoCopy) X(DtsReleaseOutputBuffs) X(DtsIsEndOfStream) \
    X(DtsFlushInput) X(DtsCrystalHDVersion)
struct API {
    void *library;
#define MEMBER(name) decltype(&::name) name=nullptr;
    FUNCTIONS(MEMBER)
#undef MEMBER
    API() {
        const char *path=getenv("CRYSTALHD_LEGACY_LIB");
        library=dlopen(path?path:"libcrystalhd.so.3",RTLD_NOW|RTLD_LOCAL);
        need(library,"legacy library");
#define RESOLVE(name) name=reinterpret_cast<decltype(name)>(dlsym(library,#name)); need(name,#name);
        FUNCTIONS(RESOLVE)
#undef RESOLVE
    }
    ~API() { if(library) dlclose(library); }
};
struct Decoder {
    API &api; HANDLE handle=nullptr; bool opened=false,started=false;
    explicit Decoder(API &a):api(a) {
        const uint32_t mode=DTS_PLAYBACK_MODE|DTS_LOAD_FILE_PLAY_FW|DTS_SKIP_TX_CHK_CPB|
            DTS_PLAYBACK_DROP_RPT_MODE|DTS_SINGLE_THREADED_MODE|
            DTS_DFLT_RESOLUTION(vdecRESOLUTION_1080p23_976);
        good("DtsDeviceOpen",api.DtsDeviceOpen(&handle,mode));
        need(handle!=nullptr,"non-null decoder handle");
    }
    void start() {
        BC_INFO_CRYSTAL version{};
        good("DtsCrystalHDVersion",api.DtsCrystalHDVersion(handle,&version));
        need(version.device==1,"BCM70015 only");
        BC_INPUT_FORMAT format{};
        format.Progressive=TRUE; format.OptFlags=0x80000000U|vdecFrameRate59_94|0x40U;
        format.mSubtype=BC_MSUBTYPE_H264; format.width=640;format.height=360;format.startCodeSz=4;
        good("DtsSetInputFormat",api.DtsSetInputFormat(handle,&format));
        good("DtsOpenDecoder",api.DtsOpenDecoder(handle,BC_STREAM_TYPE_ES));opened=true;
        good("DtsSetColorSpace",api.DtsSetColorSpace(handle,OUTPUT_MODE422_YUY2));
        good("DtsStartDecoder",api.DtsStartDecoder(handle));started=true;
        good("DtsStartCapture",api.DtsStartCapture(handle));
    }
    void finish() {
        if(started){auto status=api.DtsStopDecoder(handle);started=false;good("DtsStopDecoder",status);}
        if(opened){auto status=api.DtsCloseDecoder(handle);opened=false;good("DtsCloseDecoder",status);}
        if(handle){auto saved=handle;handle=nullptr;good("DtsDeviceClose",api.DtsDeviceClose(saved));}
    }
    ~Decoder() {
        if(started) api.DtsStopDecoder(handle);
        if(opened) api.DtsCloseDecoder(handle);
        if(handle) api.DtsDeviceClose(handle);
    }
};
struct Frame { uint64_t token; uint32_t flags; std::vector<uint8_t> pixels; };
constexpr size_t kBytes=640U*360U*2U;
constexpr uint64_t kPrivate=(UINT64_C(1)<<40)+100000;

int main(int argc,char **argv) {
    try {
        need(argc==4,"usage: h264-stop-kick G0_DIRECTORY RESULT_DIRECTORY reference|none|prefix|picture");
        std::string input=argv[1],output=argv[2],mode=argv[3];
        need(mode=="reference"||mode=="none"||mode=="prefix"||mode=="picture","mode");
        const bool reference=mode=="reference";
        const auto source=packets(input);
        auto prefix=read_file(input+"/prefix.h264"),auxiliary=read_file(input+"/kick1.h264");
        need(prefix.size()==170 && auxiliary.size()>0 && auxiliary.size()<=16384,"bounded G0 payloads");
        std::map<uint64_t,Frame> baseline;
        if(!reference) {
            auto data=read_file(output+"/native-reference.bin");size_t cursor=0;
            need(data.size()==20*(12+kBytes),"reference extent");
            while(cursor<data.size()) {
                uint64_t token=integer(data,cursor,8);uint32_t flags=integer(data,cursor,4);
                Frame f{token,flags,{data.begin()+cursor,data.begin()+cursor+kBytes}};cursor+=kBytes;
                need(baseline.emplace(token,std::move(f)).second,"unique reference token");
            }
        }
        API api;Decoder decoder(api);decoder.start();
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        auto bounded=[&]{need(std::chrono::steady_clock::now()<deadline,"12s session deadline");};
        std::vector<Frame> frames;std::vector<uint64_t> real_order;
        unsigned real=0,private_outputs=0;uint32_t ready=0;bool eos=false,flush=false;
        auto receive=[&]{
            for(unsigned attempt=0;attempt<32;++attempt) {
                bounded();BC_DTS_STATUS status{};
                good("DtsGetDriverStatus",api.DtsGetDriverStatus(decoder.handle,&status));
                ready=status.ReadyListCount;
                if(!ready) break;
                BC_DTS_PROC_OUT out{};auto result=api.DtsProcOutputNoCopy(decoder.handle,0,&out);
                if(result==BC_STS_FMT_CHANGE) continue;
                if(result==BC_STS_NO_DATA||result==BC_STS_BUSY||result==BC_STS_TIMEOUT) break;
                good("DtsProcOutputNoCopy",result);
                struct Lease {
                    API &api;HANDLE handle;bool live=true;
                    void finish(){auto rc=api.DtsReleaseOutputBuffs(handle,nullptr,FALSE);live=false;good("DtsReleaseOutputBuffs",rc);}
                    ~Lease(){if(live)api.DtsReleaseOutputBuffs(handle,nullptr,FALSE);}}
                    lease{api,decoder.handle};
                if(out.PicInfo.flags&VDEC_FLAG_EOS){need(flush,"no early EOS");lease.finish();continue;}
                need((out.PoutFlags&BC_POUT_FLAGS_PIB_VALID)&&out.Ybuff&&
                     out.PicInfo.width==640&&out.PicInfo.height==360&&
                     !(out.PicInfo.flags&VDEC_FLAG_INTERLACED_SRC)&&
                     uint64_t(out.YBuffDoneSz)*4>=kBytes,"progressive YUYV extent");
                uint64_t token=out.PicInfo.timeStamp;
                need(std::none_of(frames.begin(),frames.end(),[&](const Frame &f){return f.token==token;}),"duplicate output token");
                Frame frame{token,out.PicInfo.flags,{out.Ybuff,out.Ybuff+kBytes}};
                if(token==kPrivate) {
                    need(mode=="picture"&&private_outputs++==0,"private identity");
                    need(std::all_of(frame.pixels.begin(),frame.pixels.end(),[](uint8_t v){return v==128;}),"private DC128 pixels");
                } else {
                    need(std::any_of(source.begin(),source.end(),[&](const Packet &p){return p.token==token;}),"unknown/ghost output");
                    if(!reference) need(baseline.at(token).pixels==frame.pixels,"original pixels changed");
                    ++real;real_order.push_back(token);
                }
                printf("{\"type\":\"picture\",\"mode\":\"%s\",\"token\":%llu,\"private\":%s,\"before_eos\":%s,\"flags\":%u}\n",
                    mode.c_str(),static_cast<unsigned long long>(token),token==kPrivate?"true":"false",flush?"false":"true",frame.flags);
                fflush(stdout);frames.push_back(std::move(frame));
                lease.finish();
            }
            uint8_t end=0;good("DtsIsEndOfStream",api.DtsIsEndOfStream(decoder.handle,&end));eos=end!=0;
        };
        auto send=[&](const std::vector<uint8_t> &data,uint64_t token) {
            const size_t reserve=data.size()+32*(data.size()/60000+3)+128;
            while(api.DtsTxFreeSize(decoder.handle)<reserve){bounded();receive();usleep(1000);}
            auto padded=data;padded.resize(data.size()+64,0);
            good("DtsProcInput",api.DtsProcInput(decoder.handle,padded.data(),data.size(),token,FALSE));
            receive();
        };
        auto wait=[&](unsigned milliseconds) {
            auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
            do {receive();bounded();usleep(1000);} while(std::chrono::steady_clock::now()<until);
        };
        for(unsigned i=0;i<(reference?20:17);++i) send(source[i].data,source[i].token);
        if(mode=="picture") send(auxiliary,kPrivate);
        if(mode=="picture"||mode=="prefix") send(prefix,0); // Untimed delimiter; no invented next-public identity.
        wait(reference?30:600);
        const unsigned boundary_real=real,boundary_private=private_outputs;
        const bool g2=!reference&&real==17&&private_outputs==(mode=="picture"?1U:0U)&&!eos;
        printf("{\"type\":\"boundary\",\"mode\":\"%s\",\"real\":%u,\"private\":%u,\"eos\":%s,\"g2\":%s}\n",
               mode.c_str(),real,private_outputs,eos?"true":"false",g2?"true":"false");fflush(stdout);
        if(g2) for(unsigned i=17;i<20;++i) send(source[i].data,source[i].token);
        // Final EOS is cleanup after the measured boundary. It is never used
        // to turn an incomplete G2 boundary into successful STOP/START.
        need(!eos,"no EOS before terminal cleanup");good("DtsFlushInput",api.DtsFlushInput(decoder.handle,0));flush=true;
        const unsigned expected=reference||g2?20:17;
        while(!eos||ready||real!=expected||private_outputs!=(mode=="picture"?1U:0U)){
            bounded();receive();usleep(1000);
        }
        std::vector<Packet> sorted=source;
        std::stable_sort(sorted.begin(),sorted.end(),[](const Packet &a,const Packet &b){return a.pts<b.pts;});
        std::vector<uint64_t> expected_order;for(const auto &packet:sorted) expected_order.push_back(packet.token);
        const bool full_order=real_order==expected_order;
        if(reference) {
            need(full_order,"reference display order");
            int fd=open((output+"/native-reference.bin").c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
            need(fd>=0,"exclusive reference output");
            for(const auto &frame:frames) {
                std::vector<uint8_t> record;
                for(unsigned b=0;b<8;++b) record.push_back(frame.token>>(8*b));
                for(unsigned b=0;b<4;++b) record.push_back(frame.flags>>(8*b));
                record.insert(record.end(),frame.pixels.begin(),frame.pixels.end());
                size_t offset=0;while(offset<record.size()) {ssize_t n=write(fd,record.data()+offset,record.size()-offset);need(n>0,"reference write");offset+=n;}
            }
            need(close(fd)==0,"reference close");
        }
        decoder.finish();
        printf("{\"type\":\"result\",\"mode\":\"%s\",\"boundary_real\":%u,\"boundary_private\":%u,"
               "\"g2\":%s,\"continuation_attempted\":%s,\"terminal_real\":%u,\"terminal_private\":%u,"
               "\"eos\":%s,\"full_reference_order\":%s,\"pixels_match_reference\":%s}\n",mode.c_str(),
               boundary_real,boundary_private,g2?"true":"false",g2?"true":"false",real,private_outputs,
               eos?"true":"false",full_order?"true":"false",reference?"null":"true");
        return reference||(g2&&full_order)?0:2;
    } catch(const std::exception &error) {fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
