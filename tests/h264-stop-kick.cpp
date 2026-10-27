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
static unsigned small_unsigned(const char *text) {
    size_t used=0;
    const auto value=std::stoul(text,&used);
    need(used==strlen(text)&&value<=20,"bounded integer argument");
    return static_cast<unsigned>(value);
}
static std::vector<uint8_t> read_file(const std::string &path,size_t maximum=16U*1024U*1024U) {
    std::ifstream stream(path,std::ios::binary);
    need(bool(stream),"open input "+path);
    std::vector<uint8_t> result((std::istreambuf_iterator<char>(stream)),{});
    need(result.size()<=maximum,"bounded input"); return result;
}
static uint64_t integer(const std::vector<uint8_t> &data,size_t &cursor,unsigned bytes) {
    need(bytes<=8 && cursor<=data.size() && bytes<=data.size()-cursor,"container integer");
    uint64_t value=0;
    for (unsigned i=0;i<bytes;++i) value|=uint64_t(data[cursor++])<<(8*i);
    return value;
}
struct Packet { uint64_t token; int64_t pts; std::vector<uint8_t> data; };
static std::vector<Packet> packets(const std::string &directory,bool cycles=false) {
    auto data=read_file(directory+(cycles?"/original180.packets":"/original20.packets"));
    need(data.size()>=12 && !memcmp(data.data(),cycles?"CHDCYCL1":"CHDKICK1",8),"container identity");
    const unsigned count=cycles?180:20;
    size_t cursor=8; need(integer(data,cursor,4)==count,"exact fixture count");
    std::vector<Packet> result;
    for (unsigned i=0;i<count;++i) {
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
struct Attribution {
    unsigned matches=0;uint64_t original_token=0;bool flags=false,neutral=false;
    bool strict(const Frame &frame) const {
        return matches==1&&original_token==frame.token&&flags;
    }
};
// Pixel association diagnoses transport failures. It never supplies a returned
// identity: strict acceptance also requires the unchanged native token.
static Attribution attribute(const Frame &frame,const std::map<uint64_t,Frame> &baseline) {
    Attribution result;
    result.neutral=std::all_of(frame.pixels.begin(),frame.pixels.end(),[](uint8_t v){return v==128;});
    for(const auto &entry:baseline)if(entry.second.pixels==frame.pixels) {
        ++result.matches;result.original_token=entry.first;result.flags=entry.second.flags==frame.flags;
    }
    return result;
}
static bool has_vcl(const std::vector<uint8_t> &data) {
    for(size_t i=0;i+3<data.size();++i)
        if(!data[i]&&!data[i+1]&&data[i+2]==1&&
           (data[i+3]&31)>=1&&(data[i+3]&31)<=5)return true;
    return false;
}
static void write_frames(const std::string &path,const std::vector<Frame> &frames) {
    int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);
    need(fd>=0,"exclusive frame output");
    for(const auto &frame:frames) {
        std::vector<uint8_t> record;
        for(unsigned b=0;b<8;++b)record.push_back(frame.token>>(8*b));
        for(unsigned b=0;b<4;++b)record.push_back(frame.flags>>(8*b));
        record.insert(record.end(),frame.pixels.begin(),frame.pixels.end());
        size_t offset=0;
        while(offset<record.size()) {
            ssize_t n=write(fd,record.data()+offset,record.size()-offset);
            if(n<=0){close(fd);need(false,"frame write");}
            offset+=n;
        }
    }
    need(close(fd)==0,"frame close");
}
constexpr size_t kBytes=640U*360U*2U;
// Flea's legacy SPES path quantizes timestamps to 2ms (20000 in 100ns
// units). Use distinct exact round trips between original tokens17/18.
constexpr uint64_t kPrivate=1740000;
constexpr uint64_t kPrivateStep=20000;
static_assert(kPrivate%20000==0&&kPrivateStep%20000==0,"legacy timestamp granularity");
static std::vector<std::vector<uint8_t>> auxiliary_units(const std::vector<uint8_t> &data,unsigned expected) {
    std::vector<size_t> boundaries;
    for(size_t i=0;i+5<data.size();++i)
        if(!data[i]&&!data[i+1]&&!data[i+2]&&data[i+3]==1&&(data[i+4]&31)==9)
            boundaries.push_back(i);
    need(boundaries.size()==expected&&boundaries.front()==0,"private AUD unit count");
    boundaries.push_back(data.size());std::vector<std::vector<uint8_t>> result;
    for(unsigned i=0;i<expected;++i)result.emplace_back(data.begin()+boundaries[i],data.begin()+boundaries[i+1]);
    return result;
}

// Positive-candidate durability only. Input syntax is separately screened by
// h264-pause-cycles.py over each complete multi-insertion sequence. This does
// not issue STOP/START, drain at a pause, retag an output, or expose a decoder.
static int cycle_run(const std::string &input,const std::string &output,const std::string &mode,const char *text) {
    size_t used=0;const auto value=std::stoul(text,&used);
    need(used==strlen(text)&&(value==0||value==2||value==5||value==10||value==100),"bounded cycle stage");
    const unsigned cycles=static_cast<unsigned>(value);
    const bool reference=mode=="cycles-reference",dummy=mode=="cycles-next";
    need(reference==(cycles==0),"only uninterrupted oracle uses zero pauses");
    const auto source=packets(input,true);
    struct Plan {unsigned boundary;std::vector<std::vector<uint8_t>> units;};
    std::vector<Plan> plans;std::ifstream plan_file(input+"/cycles.tsv");unsigned boundary,count;
    need(bool(plan_file),"offline cycle plan");
    while(plan_file>>boundary>>count) {
        need(plans.size()<6&&count==5&&boundary>0&&boundary<=180&&
             (plans.empty()||plans.back().boundary<boundary),"six ordered independently qualified insertions");
        plans.push_back({boundary,auxiliary_units(read_file(input+"/private-"+std::to_string(boundary)+".h264"),5)});
    }
    need(plan_file.eof()&&plans.size()==6,"complete six-GOP plan");
    const auto prefix=read_file(input+"/prefix.h264");need(prefix.size()==170&&!has_vcl(prefix),"prefix without VCL");
    const unsigned repeats=reference?1:(cycles+5)/6,total=180*repeats,private_expected=dummy?5*cycles:0;
    const uint64_t private_base=uint64_t(total+1)*100000;
    std::map<uint64_t,Frame> baseline;
    if(!reference) {
        auto data=read_file(output+"/native-cycle-reference.bin",180*(12+kBytes));size_t cursor=0;
        need(data.size()==180*(12+kBytes),"complete native180 reference");
        while(cursor<data.size()) {
            uint64_t token=integer(data,cursor,8);uint32_t flags=integer(data,cursor,4);
            Frame f{token,flags,{data.begin()+cursor,data.begin()+cursor+kBytes}};cursor+=kBytes;
            need(baseline.emplace(token,std::move(f)).second,"distinct base oracle tokens");
        }
        for(const auto &p:source)need(baseline.count(p.token),"every original AU has a native oracle");
    }
    auto sorted=source;std::stable_sort(sorted.begin(),sorted.end(),[](const Packet &a,const Packet &b){return a.pts<b.pts;});
    std::vector<uint64_t> expected_order,actual_order;
    for(unsigned repeat=0;repeat<repeats;++repeat)for(const auto &p:sorted)expected_order.push_back(p.token+uint64_t(repeat)*180*100000);
    API api;Decoder decoder(api);decoder.start();const HANDLE owner=decoder.handle;
    const auto start=std::chrono::steady_clock::now();
    auto elapsed=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
    const unsigned watchdog_seconds=12+2*cycles+4*repeats;
    auto bounded=[&]{need(elapsed()<1000.0*watchdog_seconds,"bounded complete-cycle session watchdog");};
    std::vector<bool> real_seen(total),private_seen(private_expected);
    std::vector<double> accepted_at(total);
    std::vector<unsigned> per_cycle_private(cycles);
    std::vector<Frame> reference_frames;
    unsigned real=0,privates=0,accepted=0,accepted_private=0,rx_acquired=0,rx_released=0,calls=0,cycle=0;
    uint32_t ready=0;bool eos=false,flush=false;
    const char *phase="original-input";
    auto receive=[&]{
        for(unsigned attempt=0;attempt<32;++attempt) {
            bounded();BC_DTS_STATUS status{};good("DtsGetDriverStatus",api.DtsGetDriverStatus(owner,&status));
            ready=status.ReadyListCount;if(!ready)break;
            BC_DTS_PROC_OUT out{};const auto rc=api.DtsProcOutputNoCopy(owner,0,&out);
            if(rc==BC_STS_FMT_CHANGE)continue;
            if(rc==BC_STS_NO_DATA||rc==BC_STS_BUSY||rc==BC_STS_TIMEOUT)break;
            good("DtsProcOutputNoCopy",rc);++rx_acquired;
            struct Lease {
                API &api;HANDLE owner;unsigned &released;bool live=true;
                void finish(){const auto rc=api.DtsReleaseOutputBuffs(owner,nullptr,FALSE);live=false;good("DtsReleaseOutputBuffs",rc);++released;}
                ~Lease(){if(live&&api.DtsReleaseOutputBuffs(owner,nullptr,FALSE)==BC_STS_SUCCESS)++released;}
            } lease{api,owner,rx_released};
            const uint64_t token=out.PicInfo.timeStamp;
            printf("{\"type\":\"cycle-rx-acquire\",\"lease\":%u,\"token\":%llu,\"cycle\":%u,\"elapsed_ms\":%.3f}\n",rx_acquired,static_cast<unsigned long long>(token),cycle,elapsed());
            if(out.PicInfo.flags&VDEC_FLAG_EOS) {
                need(flush,"no EOS at a repeated pause");lease.finish();
                printf("{\"type\":\"cycle-rx-release\",\"lease\":%u,\"eos_marker\":true,\"elapsed_ms\":%.3f}\n",rx_released,elapsed());continue;
            }
            need((out.PoutFlags&BC_POUT_FLAGS_PIB_VALID)&&out.Ybuff&&out.PicInfo.width==640&&out.PicInfo.height==360&&
                 !(out.PicInfo.flags&VDEC_FLAG_INTERLACED_SRC)&&uint64_t(out.YBuffDoneSz)*4>=kBytes,"cycle progressive YUYV extent");
            const bool is_private=token>=private_base&&(token-private_base)%kPrivateStep==0&&(token-private_base)/kPrivateStep<private_expected;
            if(is_private) {
                const unsigned index=(token-private_base)/kPrivateStep;
                need(!private_seen[index]&&std::all_of(out.Ybuff,out.Ybuff+kBytes,[](uint8_t v){return v==128;}),"distinct private token and full DC128 image");
                private_seen[index]=true;++privates;++per_cycle_private[index/5];
                printf("{\"type\":\"cycle-picture\",\"token\":%llu,\"private_generation\":%u,\"private_index\":%u,\"flags\":%u,\"phase\":\"%s\",\"elapsed_ms\":%.3f}\n",
                    static_cast<unsigned long long>(token),index/5+1,index%5+1,out.PicInfo.flags,phase,elapsed());
            }else {
                need(token&&token%100000==0&&token/100000<=total,"actual original token without remapping");
                const unsigned index=token/100000-1;
                need(!real_seen[index]&&accepted_at[index]>0,"distinct accepted original identity");
                if(!reference) {
                    const auto &f=baseline.at(uint64_t(index%180+1)*100000);
                    need(f.flags==out.PicInfo.flags&&std::equal(f.pixels.begin(),f.pixels.end(),out.Ybuff),"complete original YUYV/flags equality");
                }else reference_frames.push_back({token,out.PicInfo.flags,{out.Ybuff,out.Ybuff+kBytes}});
                real_seen[index]=true;++real;actual_order.push_back(token);
                printf("{\"type\":\"cycle-picture\",\"token\":%llu,\"source_au\":%u,\"source_copy\":%u,\"flags\":%u,\"full_native_reference_match\":%s,\"phase\":\"%s\",\"accepted_to_output_ms\":%.3f,\"elapsed_ms\":%.3f}\n",
                    static_cast<unsigned long long>(token),index%180+1,index/180+1,out.PicInfo.flags,reference?"null":"true",phase,elapsed()-accepted_at[index],elapsed());
            }
            lease.finish();printf("{\"type\":\"cycle-rx-release\",\"lease\":%u,\"eos_marker\":false,\"elapsed_ms\":%.3f}\n",rx_released,elapsed());fflush(stdout);
        }
        uint8_t end=0;good("DtsIsEndOfStream",api.DtsIsEndOfStream(owner,&end));eos=end!=0;
    };
    auto send=[&](const std::vector<uint8_t> &data,uint64_t token,const char *kind,unsigned association) {
        need(decoder.handle==owner&&!eos,"unchanged live cycle channel");
        const size_t reserve=data.size()+32*(data.size()/60000+3)+128;
        while(api.DtsTxFreeSize(owner)<reserve){bounded();receive();usleep(1000);}
        auto padded=data;padded.resize(data.size()+64,0);
        good("DtsProcInput",api.DtsProcInput(owner,padded.data(),data.size(),token,FALSE));
        ++calls;
        if(!strcmp(kind,"original")){accepted_at.at(association-1)=elapsed();++accepted;}
        if(!strcmp(kind,"private-vcl"))++accepted_private;
        printf("{\"type\":\"cycle-input\",\"call\":%u,\"cycle\":%u,\"kind\":\"%s\",\"association\":%u,\"token\":%llu,\"bytes\":%zu,\"vcl_present\":%s,\"elapsed_ms\":%.3f}\n",
            calls,cycle,kind,association,static_cast<unsigned long long>(token),data.size(),has_vcl(data)?"true":"false",elapsed());fflush(stdout);receive();
    };
    auto wait=[&](unsigned ms){const double until=elapsed()+ms;do{receive();bounded();usleep(1000);}while(elapsed()<until);};
    auto census=[&](const char *checkpoint) {
        need(decoder.handle==owner&&rx_acquired==rx_released,"same channel; no held public lease at cycle checkpoint");
        printf("{\"type\":\"cycle-census\",\"cycle\":%u,\"checkpoint\":\"%s\",\"accepted_originals\":%u,\"output_originals\":%u,\"accepted_private\":%u,\"output_private\":%u,\"pending_private\":%u,\"current_private_outputs\":%u,\"rx_acquired\":%u,\"rx_released\":%u,\"same_handle\":true,\"eos\":%s,\"elapsed_ms\":%.3f}\n",
            cycle,checkpoint,accepted,real,accepted_private,privates,accepted_private-privates,cycle?per_cycle_private.at(cycle-1):0,rx_acquired,rx_released,eos?"true":"false",elapsed());fflush(stdout);
    };
    uint64_t prefed_token=0;
    for(unsigned repeat=0;repeat<repeats;++repeat)for(unsigned i=0;i<180;++i) {
        const unsigned ordinal=repeat*180+i+1;const uint64_t token=uint64_t(ordinal)*100000;
        phase="original-input";send(source[i].data,token==prefed_token?0:token,"original",ordinal);prefed_token=0;
        if(cycle>=cycles||std::none_of(plans.begin(),plans.end(),[&](const Plan &p){return p.boundary==i+1;}))continue;
        ++cycle;const auto &plan=*std::find_if(plans.begin(),plans.end(),[&](const Plan &p){return p.boundary==i+1;});
        census("before-private");phase="private-input";
        if(dummy) {
            for(unsigned j=0;j<5;++j) {
                send(prefix,private_base+uint64_t((cycle-1)*5+j)*kPrivateStep,"private-prefix",(cycle-1)*5+j+1);
                send(plan.units[j],0,"private-vcl",(cycle-1)*5+j+1);
            }
            prefed_token=uint64_t(ordinal+1)*100000;
            send(prefix,prefed_token,"next-original-prefix",ordinal+1);
        }
        census("after-prefix-alone");wait(600);need(!eos,"no EOS before repeated idle");census("before-idle");
        phase="input-idle";wait(100);need(!eos,"no EOS during repeated idle");census("after-idle");
    }
    need(cycle==cycles&&accepted==total&&accepted_private==private_expected&&!prefed_token,"all planned input/pauses complete before true end");
    phase="terminal-eos";good("DtsFlushInput",api.DtsFlushInput(owner,0));flush=true;
    while(!eos||ready){bounded();receive();usleep(1000);}
    census("after-terminal-eos");
    need(actual_order==expected_order&&real==total&&privates==private_expected&&rx_acquired==rx_released,"complete original order/identity and all private retirement");
    if(reference)write_frames(output+"/native-cycle-reference.bin",reference_frames);
    decoder.finish();
    printf("{\"type\":\"cycle-result\",\"mode\":\"%s\",\"cycles\":%u,\"repeats\":%u,\"originals\":%u,\"private\":%u,\"rx_acquired\":%u,\"rx_released\":%u,\"watchdog_seconds\":%u,\"same_channel\":true,\"strict_identity_pixels_flags_order\":true,\"eos_only_at_true_end\":true,\"wall_ms\":%.3f,\"pass\":true}\n",
        mode.c_str(),cycles,repeats,real,privates,rx_acquired,rx_released,watchdog_seconds,elapsed());
    return 0;
}

int main(int argc,char **argv) {
    try {
        if(argc==5&&(std::string(argv[3])=="cycles-reference"||std::string(argv[3])=="cycles-none"||std::string(argv[3])=="cycles-next"))
            return cycle_run(argv[1],argv[2],argv[3],argv[4]);
        need(argc==4||argc==6||argc==7||argc==8,"usage: h264-stop-kick G0_DIRECTORY RESULT_DIRECTORY MODE [BOUNDARY PRIVATE_COUNT [raw|prefed|adjacent]]; pts-idle|pts-preflight requires 18 5 next|previous|reserved-near|reserved-far|untimed zero|own");
        std::string input=argv[1],output=argv[2],mode=argv[3];
        const bool preflight=mode=="pts-preflight";
        const bool pts_audit=mode=="pts-idle"||preflight;
        const bool matrix_idle=mode=="matrix-idle-100"||mode=="matrix-idle-1000"||pts_audit;
        const unsigned matrix_idle_ms=mode=="matrix-idle-1000"?1000:100;
        const bool matrix=mode=="matrix"||matrix_idle;
        need(matrix==(argc>=6)&&!(argc==7&&!matrix_idle)&&pts_audit==(argc==8),"matrix/PTS argument shape");
        const std::string transport=argc>=7?argv[6]:"raw";
        const std::string resumed=pts_audit?argv[7]:"zero";
        need(pts_audit?(transport=="next"||transport=="previous"||transport=="reserved-near"||
             transport=="reserved-far"||transport=="untimed"):
             (transport=="raw"||transport=="prefed"||transport=="adjacent"),"research transport control");
        need(resumed=="zero"||resumed=="own","resumed payload token policy");
        const bool early_prefeed=transport=="prefed"||pts_audit;
        const bool adjacent_tags=transport=="adjacent";
        const unsigned prefix_count=matrix?small_unsigned(argv[4]):17;
        const unsigned matrix_private=matrix?small_unsigned(argv[5]):0;
        need(!matrix||((prefix_count==12||prefix_count==15||prefix_count==18)&&matrix_private<=5),
             "matrix uses independently ordered boundaries and at most5 private pictures");
        need(!matrix_idle||(prefix_count==18&&(matrix_private==0||matrix_private==4||matrix_private==5)),
             "input-only continuation uses frozen boundary18 with0/4/5 private pictures");
        need(!adjacent_tags||matrix_private==4,"four distinct2ms private tags fit between original18/19");
        need(!early_prefeed||matrix_private!=0,"early real prefix control requires private input");
        need(!pts_audit||(prefix_count==18&&matrix_private==5),"PTS audit freezes boundary18/private5");
        const bool idle=mode=="input-idle-100"||mode=="input-idle-1000"||
            mode=="input-hold-1000"||mode=="input-idle-multi"||mode=="input-idle-empty";
        const bool hold=mode=="input-hold-1000";
        const bool empty=mode=="input-idle-empty";
        const unsigned idle_ms=(mode=="input-idle-1000"||hold)?1000:100;
        need(mode=="reference"||mode=="none"||mode=="prefix"||mode=="picture"||
             mode=="prefed-picture"||mode=="prefed-pair"||idle||matrix,"mode");
        const bool reference=mode=="reference";
        const bool prefed=mode=="prefed-picture"||mode=="prefed-pair"||matrix;
        const unsigned private_expected=matrix?matrix_private:mode=="prefed-pair"?2:(mode=="picture"||prefed?1:0);
        const auto source=packets(input);
        // These are synthetic diagnostic identities, not media timestamps.
        // Keep every original token unchanged. Default private tags follow
        // all originals; the four-picture adjacent control tests tag order.
        const uint64_t private_base=matrix?(adjacent_tags?source[prefix_count-1].token+kPrivateStep:source.back().token+kPrivateStep):kPrivate;
        const uint64_t early_token=transport=="previous"?source[prefix_count-1].token:
            transport=="reserved-near"?source.back().token+6*kPrivateStep:
            transport=="reserved-far"?source.back().token+50*kPrivateStep:
            transport=="untimed"?0:early_prefeed?source[prefix_count].token:0;
        if(pts_audit) {
            need(early_token%kPrivateStep==0,"exact2ms early token");
            if(transport=="reserved-near"||transport=="reserved-far") {
                need(std::none_of(source.begin(),source.end(),[&](const Packet &p){return p.token==early_token;}),"reserved token versus originals");
                need(early_token<private_base||early_token>=private_base+private_expected*kPrivateStep,"reserved token versus private pictures");
            }
        }
        std::vector<uint8_t> prefix,auxiliary;
        if(!idle) {
            prefix=read_file(input+"/prefix.h264");
            if(!matrix||private_expected)
                auxiliary=read_file(input+"/kick"+std::to_string(private_expected?private_expected:1)+".h264");
            need(prefix.size()==170 && ((matrix&&!private_expected)||!auxiliary.empty()) && auxiliary.size()<=16384,"bounded G0 payloads");
        }
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
        if(pts_audit) {
            need(!has_vcl(prefix),"early prefix contains no original/private VCL");
            const auto units=auxiliary_units(auxiliary,private_expected);
            need(std::all_of(units.begin(),units.end(),has_vcl),"all five qualified private payloads have VCL");
            need(baseline.size()==20&&std::all_of(source.begin(),source.end(),[&](const Packet &p){return baseline.count(p.token)&&has_vcl(p.data);}),"complete original native oracle");
            if(preflight) {
                for(const auto &entry:baseline)need(attribute(entry.second,baseline).strict(entry.second),"unique original pixel/flag oracle");
                const Frame original=baseline.at(source[prefix_count].token);
                for(uint64_t token:{uint64_t(0),source[prefix_count-1].token,
                        source.back().token+6*kPrivateStep,source.back().token+50*kPrivateStep}) {
                    Frame wrong=original;wrong.token=token;const auto a=attribute(wrong,baseline);
                    need(a.matches==1&&a.original_token==original.token&&a.flags&&!a.strict(wrong),"wrong native token must stay strict FAIL");
                }
                Frame wrong=original;wrong.flags^=1;need(!attribute(wrong,baseline).strict(wrong),"wrong flags rejected");
                wrong=original;wrong.pixels[0]^=1;need(!attribute(wrong,baseline).strict(wrong),"changed pixels rejected");
                printf("{\"type\":\"pts-preflight\",\"policy\":\"%s\",\"resumed\":\"%s\",\"early_token\":%llu,\"original_au19_token\":%llu,\"device_opened\":false,\"pass\":true}\n",
                    transport.c_str(),resumed.c_str(),static_cast<unsigned long long>(early_token),static_cast<unsigned long long>(source[prefix_count].token));
                return 0;
            }
        }
        API api;Decoder decoder(api);decoder.start();
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        auto bounded=[&]{need(std::chrono::steady_clock::now()<deadline,"12s session deadline");};
        std::vector<Frame> frames;std::vector<uint64_t> real_order;
        std::vector<uint64_t> pixel_order;bool diagnostic_flags=true;
        unsigned real=0,private_outputs=0,unknown_outputs=0,leases_acquired=0,leases_released=0;
        unsigned accepted_real=0,accepted_private=0;
        uint32_t ready=0;bool eos=false,flush=false;
        const char *phase="prefix";
        auto last_input=std::chrono::steady_clock::now();
        const HANDLE original_handle=decoder.handle;
        const auto phase_start=std::chrono::steady_clock::now();
        auto elapsed=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-phase_start).count();};
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
                ++leases_acquired;
                if(pts_audit)printf("{\"type\":\"rx-acquire\",\"lease\":%u,\"token\":%llu,\"phase\":\"%s\",\"elapsed_ms\":%.3f}\n",
                    leases_acquired,static_cast<unsigned long long>(out.PicInfo.timeStamp),phase,elapsed());
                struct Lease {
                    API &api;HANDLE handle;unsigned &released;bool live=true;
                    void finish(){auto rc=api.DtsReleaseOutputBuffs(handle,nullptr,FALSE);live=false;good("DtsReleaseOutputBuffs",rc);++released;}
                    ~Lease(){if(live&&api.DtsReleaseOutputBuffs(handle,nullptr,FALSE)==BC_STS_SUCCESS)++released;}}
                    lease{api,decoder.handle,leases_released};
                if(out.PicInfo.flags&VDEC_FLAG_EOS){need(flush,"no early EOS");lease.finish();
                    if(pts_audit)printf("{\"type\":\"rx-release\",\"lease\":%u,\"eos_marker\":true,\"elapsed_ms\":%.3f}\n",leases_released,elapsed());
                    continue;}
                need((out.PoutFlags&BC_POUT_FLAGS_PIB_VALID)&&out.Ybuff&&
                     out.PicInfo.width==640&&out.PicInfo.height==360&&
                     !(out.PicInfo.flags&VDEC_FLAG_INTERLACED_SRC)&&
                     uint64_t(out.YBuffDoneSz)*4>=kBytes,"progressive YUYV extent");
                uint64_t token=out.PicInfo.timeStamp;
                const bool duplicate=std::any_of(frames.begin(),frames.end(),[&](const Frame &f){return f.token==token;});
                need(pts_audit||!duplicate,"duplicate output token");
                need(!pts_audit||frames.size()<25,"bounded original/private output extent");
                Frame frame{token,out.PicInfo.flags,{out.Ybuff,out.Ybuff+kBytes}};
                const bool is_private=private_expected&&token>=private_base&&
                    (token-private_base)%kPrivateStep==0&&(token-private_base)/kPrivateStep<private_expected;
                if(pts_audit) {
                    const auto a=attribute(frame,baseline);
                    const bool identity=!duplicate&&a.strict(frame)&&!is_private;
                    printf("{\"type\":\"pts-attribution\",\"index\":%zu,\"actual_token\":%llu,\"flags\":%u,\"private_token\":%s,\"duplicate_token\":%s,"
                           "\"pixel_matches\":%u,\"diagnostic_original_token\":%llu,\"diagnostic_flags_match\":%s,\"neutral_pixels\":%s,\"strict_original_identity\":%s,\"phase\":\"%s\",\"elapsed_ms\":%.3f}\n",
                           frames.size(),static_cast<unsigned long long>(token),frame.flags,is_private?"true":"false",duplicate?"true":"false",a.matches,
                           static_cast<unsigned long long>(a.original_token),a.flags?"true":"false",a.neutral?"true":"false",identity?"true":"false",phase,elapsed());
                    if(is_private) {
                        need(!duplicate&&a.neutral&&!a.matches&&private_outputs++<private_expected,"unambiguous private output attribution");
                    }else {
                        need(a.matches==1&&!a.neutral,"independent original pixel attribution");
                        pixel_order.push_back(a.original_token);diagnostic_flags=diagnostic_flags&&a.flags;
                        real_order.push_back(token);
                        if(identity)++real;else ++unknown_outputs;
                    }
                }else if(is_private) {
                    need(private_outputs++<private_expected,"private identity");
                    need(std::all_of(frame.pixels.begin(),frame.pixels.end(),[](uint8_t v){return v==128;}),"private DC128 pixels");
                } else {
                    const bool known=std::any_of(source.begin(),source.end(),[&](const Packet &p){return p.token==token;});
                    if(!known) {
                        const bool neutral=std::all_of(frame.pixels.begin(),frame.pixels.end(),[](uint8_t v){return v==128;});
                        unsigned matching_references=0;uint64_t matched_token=0;bool matched_flags=false;
                        for(const auto &entry:baseline)if(entry.second.pixels==frame.pixels) {
                            ++matching_references;matched_token=entry.first;matched_flags=entry.second.flags==frame.flags;
                        }
                        // Diagnostic only. Never use pixel matching to repair
                        // or accept the missing public timestamp identity.
                        printf("{\"type\":\"unmatched\",\"mode\":\"%s\",\"token\":%llu,\"neutral_pixels\":%s,\"flags\":%u,\"before_eos\":%s,"
                               "\"matching_references\":%u,\"matched_reference_token\":%llu,\"matched_reference_flags\":%s}\n",
                               mode.c_str(),static_cast<unsigned long long>(token),neutral?"true":"false",frame.flags,flush?"false":"true",
                               matching_references,static_cast<unsigned long long>(matched_token),matched_flags?"true":"false");
                        fflush(stdout);++unknown_outputs;lease.finish();
                        need(flush,"unknown/ghost output before terminal cleanup");
                        continue;
                    }
                    if(!reference) {
                        need(baseline.at(token).pixels==frame.pixels,"original pixels changed token="+std::to_string(token));
                        if(idle||matrix)need(baseline.at(token).flags==frame.flags,"original flags changed token="+std::to_string(token));
                    }
                    ++real;real_order.push_back(token);
                }
                printf("{\"type\":\"picture\",\"mode\":\"%s\",\"token\":%llu,\"private\":%s,\"before_eos\":%s,\"flags\":%u,\"phase\":\"%s\"}\n",
                    mode.c_str(),static_cast<unsigned long long>(token),is_private?"true":"false",flush?"false":"true",frame.flags,phase);
                fflush(stdout);frames.push_back(std::move(frame));
                lease.finish();
                if(pts_audit)printf("{\"type\":\"rx-release\",\"lease\":%u,\"eos_marker\":false,\"elapsed_ms\":%.3f}\n",leases_released,elapsed());
                fflush(stdout);
            }
            uint8_t end=0;good("DtsIsEndOfStream",api.DtsIsEndOfStream(decoder.handle,&end));eos=end!=0;
        };
        unsigned input_calls=0;
        auto send=[&](const std::vector<uint8_t> &data,uint64_t token,const char *kind="unspecified",unsigned association=0) {
            need(decoder.handle==original_handle,"same original decoder handle");
            const size_t reserve=data.size()+32*(data.size()/60000+3)+128;
            while(api.DtsTxFreeSize(decoder.handle)<reserve){bounded();receive();usleep(1000);}
            auto padded=data;padded.resize(data.size()+64,0);
            const auto result=api.DtsProcInput(decoder.handle,padded.data(),data.size(),token,FALSE);
            ++input_calls;
            if(pts_audit)printf("{\"type\":\"input-call\",\"call\":%u,\"kind\":\"%s\",\"association\":%u,\"bytes\":%zu,\"vcl_present\":%s,\"token\":%llu,\"status\":%u,\"phase\":\"%s\",\"elapsed_ms\":%.3f}\n",
                input_calls,kind,association,data.size(),has_vcl(data)?"true":"false",static_cast<unsigned long long>(token),unsigned(result),phase,elapsed());
            fflush(stdout);good("DtsProcInput",result);
            last_input=std::chrono::steady_clock::now();
            receive();
        };
        auto wait=[&](unsigned milliseconds) {
            auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
            do {receive();bounded();usleep(1000);} while(std::chrono::steady_clock::now()<until);
        };
        auto census=[&](const char *checkpoint) {
            need(decoder.handle==original_handle,"original channel at phase checkpoint");
            need(leases_acquired==leases_released,"no held public RX lease at phase checkpoint");
            printf("{\"type\":\"phase-census\",\"checkpoint\":\"%s\",\"accepted_real\":%u,\"accepted_private\":%u,"
                   "\"output_real\":%u,\"output_private\":%u,\"rx_acquired\":%u,\"rx_released\":%u,"
                   "\"same_handle\":true,\"eos\":%s,\"elapsed_ms\":%.3f}\n",checkpoint,accepted_real,accepted_private,
                   real,private_outputs,leases_acquired,leases_released,eos?"true":"false",
                   std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-phase_start).count());
            fflush(stdout);
        };
        if(matrix)printf("{\"type\":\"matrix-input\",\"accepted_originals\":%u,\"private_count\":%u,\"private_token_base\":%llu}\n",
            prefix_count,private_expected,static_cast<unsigned long long>(private_base));
        if(matrix_idle)printf("{\"type\":\"input-transport\",\"policy\":\"%s\"}\n",transport.c_str());
        for(unsigned i=0;i<(empty?0:reference?20:prefix_count);++i) {
            send(source[i].data,source[i].token,"original",i+1);
            ++accepted_real;
            if(mode=="input-idle-multi"&&(i==4||i==8||i==12)) {
                phase="intermediate-idle";const unsigned before=real;wait(100);
                need(!eos,"no EOS during intermediate input idle");
                printf("{\"type\":\"input-idle\",\"accepted\":%u,\"requested_ms\":100,\"real_before\":%u,\"real_after\":%u,\"actual_gap_ms\":%.3f}\n",
                    i+1,before,real,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-last_input).count());
                phase="prefix";
            }
        }
        if(matrix_idle){census("after-original-prefix");phase="private-input";}
        if(prefed&&private_expected) {
            const auto units=auxiliary_units(auxiliary,private_expected);
            for(unsigned i=0;i<private_expected;++i) {
                // The timestamped prefix belongs to the actual private AU
                // that follows. Its payload consumes that timestamp, exactly
                // as the established Prefix Kick contract requires.
                send(prefix,private_base+i*kPrivateStep,"private-prefix",i+1);send(units[i],0,"private-vcl",i+1);
                ++accepted_private;
            }
        } else if(mode=="picture") send(auxiliary,kPrivate);
        // The prefed control binds actual AU19 before the input-idle gap.
        // Its original timestamp is unchanged; no AU19 VCL bytes are sent
        // until resume, when the payload consumes this prefix with timestamp0.
        // Late/repeated timestamp controls did not repair the measured raw case.
        if(pts_audit)census("before-early-prefix");
        if(private_expected||mode=="prefix")send(prefix,early_token,"early-prefix",prefix_count+1);
        if(pts_audit)census("after-early-prefix-alone");
        const unsigned before_idle=real;
        if(idle)phase="input-idle";
        if(hold) {
            // Explicit host-output backpressure control: no receiving during
            // this bounded interval, then drain ready outputs before resume.
            const auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(idle_ms);
            do {bounded();usleep(1000);}while(std::chrono::steady_clock::now()<until);
            receive();
        }else wait(idle?idle_ms:reference?30:600);
        if(idle)printf("{\"type\":\"input-idle\",\"accepted\":%u,\"requested_ms\":%u,\"host_receive\":%s,\"real_before\":%u,\"real_after\":%u,\"actual_gap_ms\":%.3f}\n",
            empty?0:17,idle_ms,hold?"false":"true",before_idle,real,
            std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-last_input).count());
        const unsigned boundary_real=real,boundary_private=private_outputs;
        const bool g2=!pts_audit&&!reference&&!idle&&real==prefix_count&&private_outputs==private_expected&&!unknown_outputs&&!eos;
        printf("{\"type\":\"boundary\",\"mode\":\"%s\",\"real\":%u,\"private\":%u,\"eos\":%s,\"g2\":%s}\n",
               mode.c_str(),real,private_outputs,eos?"true":"false",g2?"true":"false");fflush(stdout);
        if(matrix_idle) {
            census("after-private-settle");
            // Separate ordinary input-pause experiment, explicitly requested
            // after G2 failed. This does not establish a STOP/drain boundary.
            need(!eos&&!unknown_outputs,"live channel before input-only pause");
            if(private_expected==5&&!pts_audit)
                need(real==18&&private_outputs==(early_prefeed?4U:3U),"reproduce declared real/private pre-EOS state");
            if(pts_audit)need(real==18&&private_outputs<=5,"all18 original identities before PTS-only pause");
            phase="input-idle";
            const auto pause_start=std::chrono::steady_clock::now();
            wait(matrix_idle_ms);
            need(!eos,"no EOS during dummy input-only pause");
            printf("{\"type\":\"dummy-input-idle\",\"requested_ms\":%u,\"observed_ms\":%.3f,\"input_gap_ms\":%.3f}\n",
                   matrix_idle_ms,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-pause_start).count(),
                   std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-last_input).count());
            census("after-input-idle");
        }
        if(g2||idle||matrix_idle) {
            need(!eos&&decoder.handle==original_handle,"live original channel at input resume");
            phase="continuation";
            for(unsigned i=empty?0:prefix_count;i<20;++i) {
                const uint64_t token=early_prefeed&&i==prefix_count&&resumed=="zero"?0:source[i].token;
                send(source[i].data,token,"original",i+1);
                ++accepted_real;
                if(pts_audit)census(i==prefix_count?"after-original-au19":"after-original-au20");
            }
            if(idle||matrix_idle)wait(30);
            if(matrix_idle)census("after-real-continuation");
        }
        // Final EOS is cleanup after the measured boundary. It is never used
        // to turn an incomplete G2 boundary into successful STOP/START.
        need(!eos,"no EOS before terminal cleanup");phase="terminal-eos";
        need(!matrix_idle||accepted_real==20,"terminal EOS only after all20 originals submitted");
        good("DtsFlushInput",api.DtsFlushInput(decoder.handle,0));flush=true;
        const unsigned expected=reference||g2||idle||matrix_idle?20:prefix_count;
        while(!eos||ready){
            bounded();receive();usleep(1000);
        }
        const bool terminal_complete=real==expected&&private_outputs==private_expected&&!unknown_outputs;
        std::vector<Packet> sorted=source;
        std::stable_sort(sorted.begin(),sorted.end(),[](const Packet &a,const Packet &b){return a.pts<b.pts;});
        std::vector<uint64_t> expected_order;for(const auto &packet:sorted) expected_order.push_back(packet.token);
        const bool full_order=real_order==expected_order;
        if(pts_audit) {
            printf("{\"type\":\"pts-audit-result\",\"early_policy\":\"%s\",\"early_token\":%llu,\"resumed_policy\":\"%s\",\"pixel_order_continuity\":%s,\"pixel_flags_match\":%s,\"strict_original_identity\":%s,\"observed_pictures\":%zu}\n",
                transport.c_str(),static_cast<unsigned long long>(early_token),resumed.c_str(),pixel_order==expected_order?"true":"false",diagnostic_flags?"true":"false",
                full_order&&terminal_complete?"true":"false",frames.size());
            write_frames(output+"/observed-output.bin",frames);
        }
        if(matrix_idle)census("after-terminal-eos");
        need(leases_acquired==leases_released,"all public RX leases released before close");
        if(reference) {
            need(full_order,"reference display order");
            write_frames(output+"/native-reference.bin",frames);
        }
        decoder.finish();
        if(matrix)printf("{\"type\":\"matrix-retirement\",\"leases_acquired\":%u,\"leases_released\":%u}\n",
            leases_acquired,leases_released);
        if(idle)printf("{\"type\":\"input-idle-result\",\"mode\":\"%s\",\"distinct_real\":%u,\"same_channel\":true,\"full_reference_order\":%s,\"full_pixels_and_flags_match\":true,\"leases_acquired\":%u,\"leases_released\":%u,\"terminal_eos\":%s,\"pass\":%s}\n",
            mode.c_str(),real,full_order?"true":"false",leases_acquired,leases_released,eos?"true":"false",
            full_order&&terminal_complete?"true":"false");
        printf("{\"type\":\"result\",\"mode\":\"%s\",\"boundary_real\":%u,\"boundary_private\":%u,"
               "\"g2\":%s,\"continuation_attempted\":%s,\"terminal_real\":%u,\"terminal_private\":%u,"
               "\"eos\":%s,\"full_reference_order\":%s,\"pixels_match_reference\":%s,"
               "\"unknown_outputs\":%u,\"terminal_complete\":%s}\n",mode.c_str(),
               boundary_real,boundary_private,g2?"true":"false",g2||idle||matrix_idle?"true":"false",real,private_outputs,
               eos?"true":"false",full_order?"true":"false",reference?"null":pts_audit?(pixel_order==expected_order?"true":"false"):"true",unknown_outputs,terminal_complete?"true":"false");
        return (reference&&terminal_complete)||((g2||idle||matrix_idle)&&full_order&&terminal_complete)?0:2;
    } catch(const std::exception &error) {fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
