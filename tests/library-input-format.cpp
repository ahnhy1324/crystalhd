// SPDX-License-Identifier: LGPL-2.1-or-later
// Production input configuration and initial PES conversion, without hardware.
// Each case runs in a child so the unfixed NULL/short-metadata faults are bounded.
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
using Bytes = std::vector<uint8_t>;
static unsigned checks, failures;
static void Check(bool ok, const char *why) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); }
}
struct Allocation { void *pointer; bool live; };
static Allocation allocations[256];
static size_t allocation_count;
static bool record_allocations;
static int fail_malloc, fail_aligned;
static unsigned failed_allocations;
static void Record(void *p) {
    if (!record_allocations || !p) return;
    if (allocation_count == sizeof allocations / sizeof allocations[0]) std::abort();
    allocations[allocation_count++] = {p, true};
}
static size_t Live() {
    size_t count = 0;
    for (size_t n = 0; n < allocation_count; ++n) count += allocations[n].live;
    return count;
}
extern "C" void *__real_malloc(size_t);
extern "C" void __real_free(void *);
extern "C" int __real_posix_memalign(void **, size_t, size_t);
extern "C" void *__wrap_malloc(size_t size) {
    if (record_allocations && fail_malloc && --fail_malloc == 0) {
        ++failed_allocations; errno = ENOMEM; return nullptr;
    }
    void *p = __real_malloc(size); Record(p); return p;
}
extern "C" int __wrap_posix_memalign(void **p, size_t align, size_t size) {
    if (record_allocations && fail_aligned && --fail_aligned == 0) {
        ++failed_allocations; return ENOMEM; // POSIX leaves *p unchanged.
    }
    const int result = __real_posix_memalign(p, align, size);
    if (!result) Record(*p);
    return result;
}
extern "C" void __wrap_free(void *p) {
    for (size_t n = allocation_count; n; --n) {
        if (allocations[n-1].pointer == p && allocations[n-1].live) {
            allocations[n-1].live = false; break;
        }
    }
    __real_free(p);
}
extern "C" int __wrap_ioctl(int, unsigned long, ...) { std::abort(); }

static const Bytes wmv = {0x4b, 0xf1, 0x0a, 0x93};
static const Bytes avc = {0,0,0,1,0x67,0x64,0x20, 0,0,0,1,0x68,0xee};
static const Bytes advanced = {0,0,1,0x0f,0xca,0xfe,0,0,1,0x0e,0x12,0x34};
static BC_INPUT_FORMAT Format(uint32_t subtype, const Bytes &bytes) {
    BC_INPUT_FORMAT f = {};
    f.mSubtype = static_cast<BC_MEDIA_SUBTYPE>(subtype);
    f.width = 640; f.height = 360; f.Progressive = true;
    f.OptFlags = 0x80000001; f.startCodeSz = 4;
    f.pMetaData = bytes.empty() ? nullptr : const_cast<uint8_t *>(bytes.data());
    f.metaDataSz = bytes.size();
    return f;
}
struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    explicit Fixture(uint32_t device = BC_PCI_DEVID_FLEA) {
        bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG; context.ProcessID = getpid();
        context.DevId = device; context.State = BC_DEC_STATE_START;
        pthread_mutexattr_t attr;
        if (pthread_mutexattr_init(&attr) ||
            pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE) ||
            pthread_mutex_init(&context.thLock, &attr)) std::abort();
        pthread_mutexattr_destroy(&attr);
    }
    ~Fixture() {
        DtsReleasePESConverter(&context);
        std::free(context.VidParams.pMetaData);
        context.VidParams.pMetaData = nullptr;
        pthread_mutex_destroy(&context.thLock);
        bc_dil_glob_ptr = nullptr;
    }
    BC_STATUS Set(BC_INPUT_FORMAT *f) {
        record_allocations = true;
        const BC_STATUS result = DtsSetInputFormat(&context, f);
        record_allocations = false;
        return result;
    }
    void Valid() {
        auto f = Format(BC_MSUBTYPE_AVC1, avc);
        Check(Set(&f) == BC_STS_SUCCESS, "initial valid AVC1 configuration");
    }
};
struct Snapshot {
    DTS_VIDEO_PARAMS video;
    PES_CONVERT_PARAMS pes;
    uint32_t single, scaling, drop;
    Bytes metadata, headers, pending;
    size_t live;
    explicit Snapshot(const Fixture &f) : video(f.context.VidParams),
        pes(f.context.PESConvParams), single(f.context.SingleThreadedAppMode),
        scaling(f.context.EnableScaling), drop(f.context.bEnable720pDropHalf), live(Live()) {
        if (video.MetaDataSz) metadata.assign(video.pMetaData, video.pMetaData + video.MetaDataSz);
        if (pes.m_iSpsPpsLen) headers.assign(pes.m_pSpsPpsBuf, pes.m_pSpsPpsBuf + pes.m_iSpsPpsLen);
        if (pes.lPendBufferSize) pending.assign(pes.pStartcodePendBuff,pes.pStartcodePendBuff+pes.lPendBufferSize);
    }
    void Same(const Fixture &f) const {
        Check(!std::memcmp(&video, &f.context.VidParams, sizeof video), "failed setup preserves all video fields/owned pointer");
        Check(!std::memcmp(&pes, &f.context.PESConvParams, sizeof pes), "failed setup preserves converter fields/owned pointers");
        Check(single == f.context.SingleThreadedAppMode && scaling == f.context.EnableScaling &&
              drop == f.context.bEnable720pDropHalf, "failed setup preserves mode/scaling state");
        Check(Live() == live, "failed setup releases only staging allocations");
        // Do not dereference a freed baseline allocation after detecting mutation.
        if (f.context.VidParams.pMetaData == video.pMetaData && Live() == live)
            Check(metadata.empty() || !std::memcmp(metadata.data(), video.pMetaData, metadata.size()), "failed setup preserves metadata bytes");
        if (f.context.PESConvParams.m_pSpsPpsBuf == pes.m_pSpsPpsBuf && Live() == live)
            Check(headers.empty() || !std::memcmp(headers.data(), pes.m_pSpsPpsBuf, headers.size()), "failed setup preserves converter bytes");
        if (f.context.PESConvParams.pStartcodePendBuff == pes.pStartcodePendBuff && Live() == live)
            Check(pending.empty() || !std::memcmp(pending.data(),pes.pStartcodePendBuff,pending.size()),"failed setup preserves pending bytes");
    }
};
static void Rejected(BC_INPUT_FORMAT *bad) {
    Fixture f; f.Valid(); const Snapshot before(f);
    BC_INPUT_FORMAT input = {}; if (bad) input = *bad;
    Check(f.Set(bad) == BC_STS_INV_ARG, "malformed format is rejected"); before.Same(f);
    if (bad) Check(!std::memcmp(&input,bad,sizeof input), "failed setup does not mutate caller format");
}
static void NullFormat() { Rejected(nullptr); }
static void NullMetadata() {
    auto f = Format(BC_MSUBTYPE_AVC1, {}); f.metaDataSz = 9;
    Rejected(&f);
}
static void MissingWmv() {
    Fixture fresh; const Snapshot before(fresh);
    auto next=Format(BC_MSUBTYPE_WMV3, {});
    Check(fresh.Set(&next)==BC_STS_INV_ARG,"fresh WMV3 setup rejects absent STRUCT_C instead of dereferencing NULL");
    before.Same(fresh);
}
static void MissingWmvReplacement() { auto f=Format(BC_MSUBTYPE_WMV3,{}); Rejected(&f); }
static void ShortWmv() {
    for (size_t n = 1; n < 4; ++n) {
        Bytes bytes(wmv.begin(),wmv.begin()+n); auto f=Format(BC_MSUBTYPE_WMV3,bytes);
        Rejected(&f);
    }
}
static void AllocationFailure(bool aligned) {
    for (uint32_t subtype : {BC_MSUBTYPE_WMV3, BC_MSUBTYPE_AVC1, BC_MSUBTYPE_WVC1}) {
        Fixture f; f.Valid(); const Snapshot before(f);
        const Bytes &bytes = subtype == BC_MSUBTYPE_WMV3 ? wmv : subtype == BC_MSUBTYPE_AVC1 ? avc : advanced;
        auto next = Format(subtype,bytes); next.width=1280; next.height=720; next.OptFlags=0x80;
        const BC_INPUT_FORMAT caller = next;
        failed_allocations=0; fail_malloc=aligned?0:1; fail_aligned=aligned?1:0;
        Check(f.Set(&next)==BC_STS_INSUFF_RES, "allocation failure reaches public status");
        fail_malloc=fail_aligned=0;
        Check(failed_allocations==1, "requested real allocation seam was exercised");
        before.Same(f);
        Check(!std::memcmp(&caller,&next,sizeof next), "allocation failure leaves caller scaling unchanged");
    }
}
static void MallocFailure() { AllocationFailure(false); }
static void AlignedFailure() { AllocationFailure(true); }
static void ZeroMetadata() {
    for (uint32_t subtype : {BC_MSUBTYPE_H264,BC_MSUBTYPE_AVC1,BC_MSUBTYPE_MPEG2VIDEO,BC_MSUBTYPE_VC1}) {
        Fixture f; f.Valid(); auto next=Format(subtype,{});
        Check(f.Set(&next)==BC_STS_SUCCESS,"in-band codec permits zero metadata");
        Check(!f.context.VidParams.pMetaData && !f.context.VidParams.MetaDataSz,"zero metadata clears old owned metadata");
        Check(!f.context.PESConvParams.m_pSpsPpsBuf && !f.context.PESConvParams.m_iSpsPpsLen,"zero metadata clears previous converter headers");
        Check(Live()==0,"zero metadata frees replaced buffers");
    }
}
static void RepeatedConfiguration() {
    Fixture f;
    for (unsigned n=0;n<24;++n) {
        const Bytes &bytes = n%2 ? wmv : avc;
        auto next=Format(n%2 ? BC_MSUBTYPE_WMV3 : BC_MSUBTYPE_AVC1,bytes);
        Check(f.Set(&next)==BC_STS_SUCCESS,"repeated valid configuration succeeds");
        Check(f.context.VidParams.pMetaData!=next.pMetaData &&
              f.context.VidParams.MetaDataSz==bytes.size() &&
              !std::memcmp(f.context.VidParams.pMetaData,bytes.data(),bytes.size()),"metadata is an exact owned copy");
        Check(Live()==2,"reconfiguration retires previous metadata and PES header buffers");
    }
}
static void AliasedMetadata() {
    Fixture f; f.Valid();
    auto next=Format(BC_MSUBTYPE_AVC1,avc);
    next.pMetaData=f.context.VidParams.pMetaData;
    Check(f.Set(&next)==BC_STS_SUCCESS,"replacement can copy metadata already owned by the context");
    Check(f.context.VidParams.MetaDataSz==avc.size() &&
          !std::memcmp(f.context.VidParams.pMetaData,avc.data(),avc.size()),"alias replacement preserves original bytes before releasing old storage");
    Check(Live()==2,"alias replacement releases old metadata and converter");
}
static void PendingBufferOwnership() {
    Fixture f; f.Valid();
    record_allocations=true;
    f.context.PESConvParams.pStartcodePendBuff=static_cast<uint8_t *>(std::malloc(16));
    record_allocations=false;
    if(!f.context.PESConvParams.pStartcodePendBuff) std::abort();
    f.context.PESConvParams.lPendBufferSize=16;
    std::memset(f.context.PESConvParams.pStartcodePendBuff,0x55,16);
    const Snapshot before(f); auto next=Format(BC_MSUBTYPE_WMV3,wmv);
    fail_aligned=1;
    Check(f.Set(&next)==BC_STS_INSUFF_RES,"failed replacement retains active pending converter bytes");
    fail_aligned=0; before.Same(f);
    Check(f.Set(&next)==BC_STS_SUCCESS,"retry succeeds after converter allocation failure");
    Check(!f.context.PESConvParams.pStartcodePendBuff && !f.context.PESConvParams.lPendBufferSize,
          "successful new format resets pending converter bytes");
    Check(Live()==2,"successful replacement releases previous pending allocation too");
}
static void DirectConverterRetry() {
    Fixture f; f.Valid(); const Snapshot before(f);
    fail_aligned=1; record_allocations=true;
    Check(DtsSetPESConverter(&f.context)==BC_STS_INSUFF_RES,"direct converter exposes allocation failure");
    record_allocations=false; fail_aligned=0; before.Same(f);
    record_allocations=true;
    Check(DtsSetPESConverter(&f.context)==BC_STS_SUCCESS,"direct converter retry succeeds");
    record_allocations=false;
    Check(Live()==2,"direct converter retry retires previous header allocation");
    Check(f.context.PESConvParams.m_iSpsPpsLen==avc.size() &&
          !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,avc.data(),avc.size()),"direct retry preserves exact headers");
}
static void ValidCodecs() {
    for(uint32_t device:{BC_PCI_DEVID_LINK,BC_PCI_DEVID_FLEA}) {
        for(uint32_t subtype:{BC_MSUBTYPE_AVC1,BC_MSUBTYPE_H264,BC_MSUBTYPE_MPEG2VIDEO,
                             BC_MSUBTYPE_VC1,BC_MSUBTYPE_WVC1,BC_MSUBTYPE_WMVA,BC_MSUBTYPE_WMV3}) {
            Fixture f(device);
            const Bytes &bytes=subtype==BC_MSUBTYPE_WMV3?wmv:subtype==BC_MSUBTYPE_AVC1?avc:advanced;
            auto next=Format(subtype,bytes);
            Check(f.Set(&next)==BC_STS_SUCCESS,"existing codec configuration remains accepted");
            Check(f.context.VidParams.MediaSubType==subtype && f.context.VidParams.WidthInPixels==640 &&
                  f.context.VidParams.HeightInPixels==360 && f.context.VidParams.StartCodeSz==4,"input fields retain their meaning");
            const uint32_t algorithm=subtype==BC_MSUBTYPE_WMV3?BC_VID_ALGO_VC1MP:
                subtype==BC_MSUBTYPE_MPEG2VIDEO?BC_VID_ALGO_MPEG2:
                (subtype==BC_MSUBTYPE_H264 || subtype==BC_MSUBTYPE_AVC1)?BC_VID_ALGO_H264:BC_VID_ALGO_VC1;
            Check(f.context.VidParams.VideoAlgo==algorithm,"codec algorithm mapping is preserved");
            Check(f.context.VidParams.StreamType==((device==BC_PCI_DEVID_FLEA || subtype==BC_MSUBTYPE_WMV3)?BC_STREAM_TYPE_PES:BC_STREAM_TYPE_ES),"device-specific stream framing is preserved");
            auto &pes=f.context.PESConvParams;
            if(subtype==BC_MSUBTYPE_AVC1)
                Check(pes.m_iSpsPpsLen==avc.size() && !std::memcmp(pes.m_pSpsPpsBuf,avc.data(),avc.size()),"AVC headers unchanged");
            if(subtype==BC_MSUBTYPE_WVC1 || subtype==BC_MSUBTYPE_WMVA)
                Check(pes.m_iSpsPpsLen==advanced.size() && !std::memcmp(pes.m_pSpsPpsBuf,advanced.data(),advanced.size()),"Advanced sequence bytes unchanged");
            if(subtype==BC_MSUBTYPE_WMV3) {
                const size_t offset=device==BC_PCI_DEVID_FLEA?8:21;
                Check(pes.m_iSpsPpsLen==(device==BC_PCI_DEVID_FLEA?12U:32U) &&
                      !std::memcmp(pes.m_pSpsPpsBuf+offset,wmv.data(),4),"WMV3 metadata unchanged in original device-specific header");
                Check(pes.m_bMaxbFrames && pes.m_bRangered && pes.m_bFinterpFlag,"WMV3 syntax flags unchanged");
            }
        }
    }
}
static void MalformedAvc() {
    for(const Bytes &bytes:{Bytes{0},Bytes{0,0},Bytes{0,0,0},Bytes{0,0,1},Bytes{0,0,0,1},
        Bytes{0,7,0x67},Bytes{0,0},Bytes{0,1,0x67,0}}) {
        auto next=Format(BC_MSUBTYPE_AVC1,bytes); Rejected(&next);
    }
}
static void ManyNals() {
    Bytes input, expected;
    for(unsigned n=0;n<48;++n) {
        const uint8_t type=n%3==0?0x09:n%3==1?0x67:0x68;
        input.insert(input.end(),{0,0,1,type,static_cast<uint8_t>(0x40+n)});
        if(type!=0x09) expected.insert(expected.end(),{0,0,0,1,type,static_cast<uint8_t>(0x40+n)});
    }
    Fixture f; auto next=Format(BC_MSUBTYPE_AVC1,input);
    Check(f.Set(&next)==BC_STS_SUCCESS,"more than forty bounded NALs are supported");
    auto &p=f.context.PESConvParams;
    Check(p.m_iSpsPpsLen==expected.size() && p.m_pSpsPpsBuf &&
          !std::memcmp(p.m_pSpsPpsBuf,expected.data(),expected.size()),"only SPS/PPS retained in exact original order");
}
static void HeaderEncodings() {
    const Bytes expected={0,0,0,1,0x67,0x42,0,0,0,1,0x68,0xee};
    for(const Bytes &input:{Bytes{0,2,0x67,0x42,0,2,0x68,0xee},
        Bytes{0,0,1,0x67,0x42,0,0,0,1,0x68,0xee},
        Bytes{0,0,0,1,0x09,0xaa,0,0,0,1,0x67,0x42,0,0,0,1,0x68,0xee}}) {
        Fixture f; auto next=Format(BC_MSUBTYPE_AVC1,input);
        Check(f.Set(&next)==BC_STS_SUCCESS,"bounded metadata encodings accepted");
        const auto &p=f.context.PESConvParams;
        Check(p.m_iSpsPpsLen==expected.size() && p.m_pSpsPpsBuf &&
              !std::memcmp(p.m_pSpsPpsBuf,expected.data(),expected.size()),"normalized metadata retains exact SPS/PPS bytes");
    }
    Fixture f;
    const Bytes divx={0,0,1,0xb0,0x12,0,0,1,0xb5,0x34};
    const Bytes divx_expected={0,0,0,1,0xb0,0x12,0,0,0,1,0xb5,0x34};
    auto next=Format(BC_MSUBTYPE_DIVX,divx);
    Check(f.Set(&next)==BC_STS_SUCCESS,"legacy DivX metadata framing remains accepted");
    const auto &p=f.context.PESConvParams;
    Check(f.context.VidParams.VideoAlgo==BC_VID_ALGO_DIVX &&
          p.m_iSpsPpsLen==divx_expected.size() && p.m_pSpsPpsBuf &&
          !std::memcmp(p.m_pSpsPpsBuf,divx_expected.data(),divx_expected.size()),
          "DivX preserves every metadata unit, without AVC filtering");
}
static void GuardedConverter() {
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    for(size_t size=1;size<=3;++size) {
        auto *mapping=static_cast<uint8_t *>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(mapping==MAP_FAILED || mprotect(mapping+page,page,PROT_NONE)) std::abort();
        uint8_t *data=mapping+page-size; std::memset(data,0,size);
        Fixture f; f.context.VidParams.MediaSubType=BC_MSUBTYPE_AVC1;
        f.context.VidParams.pMetaData=data; f.context.VidParams.MetaDataSz=size;
        record_allocations=true;
        const BC_STATUS result=DtsSetPESConverter(&f.context);
        record_allocations=false;
        f.context.VidParams.pMetaData=nullptr; f.context.VidParams.MetaDataSz=0;
        Check(result==BC_STS_INV_ARG,"initial converter rejects guarded short AVC metadata before reading beyond it");
        munmap(mapping,page*2);
    }
}
static void WmvEncoderSuffix() {
    for(const Bytes &metadata:{Bytes{0x0f,0xf1,0x8a,0x01,0x40,0x0f},
                              Bytes{0x4b,0xf1,0x0a,0x93,0xaa,0xbb,0xcc,0xdd}}) {
        Fixture f; auto next=Format(BC_MSUBTYPE_WMV3,metadata);
        Check(f.Set(&next)==BC_STS_SUCCESS,"WMV3 accepts STRUCT_C plus trailing encoder bytes");
        Check(f.context.VidParams.MetaDataSz==metadata.size() &&
              f.context.VidParams.pMetaData!=metadata.data() &&
              !std::memcmp(f.context.VidParams.pMetaData,metadata.data(),metadata.size()),
              "all supplied metadata bytes are owned and preserved");
        const auto &p=f.context.PESConvParams;
        Check(p.m_iSpsPpsLen==12 && p.m_pSpsPpsBuf &&
              !std::memcmp(p.m_pSpsPpsBuf+8,metadata.data(),4),
              "WMV3 generated header uses only exact first four STRUCT_C bytes");
    }
}
static void GuardedWmv() {
    const size_t page=static_cast<size_t>(sysconf(_SC_PAGESIZE));
    for(bool unaligned:{false,true}) {
        auto *mapping=static_cast<uint8_t *>(mmap(nullptr,page*2,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0));
        if(mapping==MAP_FAILED || mprotect(mapping+page,page,PROT_NONE)) std::abort();
        // The guard-page view ends exactly at the inaccessible next page.
        // The heap view is deliberately unaligned and also ends at its ASan
        // allocation boundary; page-granular protection cannot provide both.
        uint8_t *allocation=unaligned?static_cast<uint8_t *>(std::malloc(5)):nullptr;
        if(unaligned && !allocation) std::abort();
        uint8_t *data=unaligned?allocation+1:mapping+page-4;
        std::memcpy(data,wmv.data(),4);
        {
            Fixture f; f.context.VidParams.MediaSubType=BC_MSUBTYPE_WMV3;
            f.context.VidParams.WidthInPixels=640; f.context.VidParams.HeightInPixels=360;
            f.context.VidParams.pMetaData=data; f.context.VidParams.MetaDataSz=4;
            record_allocations=true;
            const BC_STATUS result=DtsSetPESConverter(&f.context);
            record_allocations=false;
            f.context.VidParams.pMetaData=nullptr; f.context.VidParams.MetaDataSz=0;
            Check(result==BC_STS_SUCCESS,"initial converter accepts exact four-byte aligned/unaligned STRUCT_C");
            const auto &p=f.context.PESConvParams;
            Check(p.m_iSpsPpsLen==12 && p.m_pSpsPpsBuf &&
                  !std::memcmp(p.m_pSpsPpsBuf+8,wmv.data(),4),"bounded WMV3 conversion preserves metadata");
            Check(p.m_bMaxbFrames && p.m_bRangered && p.m_bFinterpFlag,"unaligned metadata preserves parsed flags");
        }
        std::free(allocation); munmap(mapping,page*2);
    }
}
int main() {
    const rlimit no_core={0,0};
    if(setrlimit(RLIMIT_CORE,&no_core)) return 2;
    struct Case { const char *name; void (*run)(); };
    const Case cases[]={{"NULL input format",NullFormat},{"nonzero NULL metadata",NullMetadata},
        {"WMV3 missing metadata",MissingWmv},{"WMV3 missing replacement metadata",MissingWmvReplacement},
        {"WMV3 short metadata",ShortWmv},
        {"malloc failure atomicity",MallocFailure},{"converter allocation failure atomicity",AlignedFailure},
        {"zero metadata replaces stale state",ZeroMetadata},{"repeated configuration ownership",RepeatedConfiguration},
        {"aliased metadata replacement",AliasedMetadata},{"pending converter buffer ownership",PendingBufferOwnership},
        {"direct converter failure and retry",DirectConverterRetry},
        {"valid codec preservation",ValidCodecs},{"malformed AVC framing",MalformedAvc},
        {"more than forty NALs",ManyNals},{"metadata framing/filtering",HeaderEncodings},
        {"guarded initial converter",GuardedConverter},{"WMV3 trailing encoder metadata",WmvEncoderSuffix},
        {"exact-boundary and unaligned WMV3 metadata",GuardedWmv}};
    unsigned failed_cases=0;
    for(const auto &test:cases) {
        std::fflush(nullptr); const pid_t child=fork();
        if(child<0) return 2;
        if(!child) {
            alarm(10); test.run(); Check(Live()==0,"fixture cleanup releases every tracked allocation");
            std::printf("%s: %u checks, %u failures\n",test.name,checks,failures);
            std::fflush(nullptr); std::exit(failures?1:0);
        }
        int status=0; if(waitpid(child,&status,0)!=child) return 2;
        const bool success=WIFEXITED(status) && WEXITSTATUS(status)==0;
        if(!success) {
            ++failed_cases;
            std::fprintf(stderr,"FAIL case %s: %s %d\n",test.name,WIFSIGNALED(status)?"signal":"exit",WIFSIGNALED(status)?WTERMSIG(status):WEXITSTATUS(status));
        }
    }
    std::printf("Input format: %zu isolated cases, %u failures (no hardware)\n",sizeof cases/sizeof cases[0],failed_cases);
    return failed_cases?1:0;
}
