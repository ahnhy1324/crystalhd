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

int main(int argc,char **argv) {
    try {
        need(argc==4||argc==6||argc==7,"usage: h264-stop-kick G0_DIRECTORY RESULT_DIRECTORY reference|none|prefix|picture|prefed-picture|prefed-pair|input-idle-100|input-idle-1000|input-hold-1000|input-idle-multi|input-idle-empty|matrix|matrix-idle-100|matrix-idle-1000 [BOUNDARY PRIVATE_COUNT [raw|prefed|adjacent]]");
        std::string input=argv[1],output=argv[2],mode=argv[3];
        const bool matrix_idle=mode=="matrix-idle-100"||mode=="matrix-idle-1000";
        const unsigned matrix_idle_ms=mode=="matrix-idle-1000"?1000:100;
        const bool matrix=mode=="matrix"||matrix_idle;
        need(matrix==(argc>=6)&&!(argc==7&&!matrix_idle),"matrix requires boundary/count; transport control is input-idle only");
        const std::string transport=argc==7?argv[6]:"raw";
        need(transport=="raw"||transport=="prefed"||transport=="adjacent","research transport control");
        const bool early_prefeed=transport=="prefed";
        const bool adjacent_tags=transport=="adjacent";
        const unsigned prefix_count=matrix?small_unsigned(argv[4]):17;
        const unsigned matrix_private=matrix?small_unsigned(argv[5]):0;
        need(!matrix||((prefix_count==12||prefix_count==15||prefix_count==18)&&matrix_private<=5),
             "matrix uses independently ordered boundaries and at most5 private pictures");
        need(!matrix_idle||(prefix_count==18&&(matrix_private==0||matrix_private==4||matrix_private==5)),
             "input-only continuation uses frozen boundary18 with0/4/5 private pictures");
        need(!adjacent_tags||matrix_private==4,"four distinct2ms private tags fit between original18/19");
        need(!early_prefeed||matrix_private!=0,"early real prefix control requires private input");
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
        API api;Decoder decoder(api);decoder.start();
        auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(12);
        auto bounded=[&]{need(std::chrono::steady_clock::now()<deadline,"12s session deadline");};
        std::vector<Frame> frames;std::vector<uint64_t> real_order;
        unsigned real=0,private_outputs=0,unknown_outputs=0,leases_acquired=0,leases_released=0;
        unsigned accepted_real=0,accepted_private=0;
        uint32_t ready=0;bool eos=false,flush=false;
        const char *phase="prefix";
        auto last_input=std::chrono::steady_clock::now();
        const HANDLE original_handle=decoder.handle;
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
                struct Lease {
                    API &api;HANDLE handle;unsigned &released;bool live=true;
                    void finish(){auto rc=api.DtsReleaseOutputBuffs(handle,nullptr,FALSE);live=false;good("DtsReleaseOutputBuffs",rc);++released;}
                    ~Lease(){if(live&&api.DtsReleaseOutputBuffs(handle,nullptr,FALSE)==BC_STS_SUCCESS)++released;}}
                    lease{api,decoder.handle,leases_released};
                if(out.PicInfo.flags&VDEC_FLAG_EOS){need(flush,"no early EOS");lease.finish();continue;}
                need((out.PoutFlags&BC_POUT_FLAGS_PIB_VALID)&&out.Ybuff&&
                     out.PicInfo.width==640&&out.PicInfo.height==360&&
                     !(out.PicInfo.flags&VDEC_FLAG_INTERLACED_SRC)&&
                     uint64_t(out.YBuffDoneSz)*4>=kBytes,"progressive YUYV extent");
                uint64_t token=out.PicInfo.timeStamp;
                need(std::none_of(frames.begin(),frames.end(),[&](const Frame &f){return f.token==token;}),"duplicate output token");
                Frame frame{token,out.PicInfo.flags,{out.Ybuff,out.Ybuff+kBytes}};
                const bool is_private=private_expected&&token>=private_base&&
                    (token-private_base)%kPrivateStep==0&&(token-private_base)/kPrivateStep<private_expected;
                if(is_private) {
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
            }
            uint8_t end=0;good("DtsIsEndOfStream",api.DtsIsEndOfStream(decoder.handle,&end));eos=end!=0;
        };
        auto send=[&](const std::vector<uint8_t> &data,uint64_t token) {
            need(decoder.handle==original_handle,"same original decoder handle");
            const size_t reserve=data.size()+32*(data.size()/60000+3)+128;
            while(api.DtsTxFreeSize(decoder.handle)<reserve){bounded();receive();usleep(1000);}
            auto padded=data;padded.resize(data.size()+64,0);
            good("DtsProcInput",api.DtsProcInput(decoder.handle,padded.data(),data.size(),token,FALSE));
            last_input=std::chrono::steady_clock::now();
            receive();
        };
        auto wait=[&](unsigned milliseconds) {
            auto until=std::chrono::steady_clock::now()+std::chrono::milliseconds(milliseconds);
            do {receive();bounded();usleep(1000);} while(std::chrono::steady_clock::now()<until);
        };
        const auto phase_start=std::chrono::steady_clock::now();
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
            send(source[i].data,source[i].token);
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
                send(prefix,private_base+i*kPrivateStep);send(units[i],0);
                ++accepted_private;
            }
        } else if(mode=="picture") send(auxiliary,kPrivate);
        // The prefed control binds actual AU19 before the input-idle gap.
        // Its original timestamp is unchanged; no AU19 VCL bytes are sent
        // until resume, when the payload consumes this prefix with timestamp0.
        // Late/repeated timestamp controls did not repair the measured raw case.
        if(private_expected||mode=="prefix")send(prefix,early_prefeed?source[prefix_count].token:0);
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
        const bool g2=!reference&&!idle&&real==prefix_count&&private_outputs==private_expected&&!unknown_outputs&&!eos;
        printf("{\"type\":\"boundary\",\"mode\":\"%s\",\"real\":%u,\"private\":%u,\"eos\":%s,\"g2\":%s}\n",
               mode.c_str(),real,private_outputs,eos?"true":"false",g2?"true":"false");fflush(stdout);
        if(matrix_idle) {
            census("after-private-settle");
            // Separate ordinary input-pause experiment, explicitly requested
            // after G2 failed. This does not establish a STOP/drain boundary.
            need(!eos&&!unknown_outputs,"live channel before input-only pause");
            if(private_expected==5)
                need(real==18&&private_outputs==(early_prefeed?4U:3U),"reproduce declared real/private pre-EOS state");
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
                send(source[i].data,early_prefeed&&i==prefix_count?0:source[i].token);
                ++accepted_real;
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
        if(matrix_idle)census("after-terminal-eos");
        need(leases_acquired==leases_released,"all public RX leases released before close");
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
               eos?"true":"false",full_order?"true":"false",reference?"null":"true",unknown_outputs,terminal_complete?"true":"false");
        return (reference&&terminal_complete)||((g2||idle||matrix_idle)&&full_order&&terminal_complete)?0:2;
    } catch(const std::exception &error) {fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
