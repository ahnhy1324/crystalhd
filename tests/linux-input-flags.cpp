// SPDX-License-Identifier: LGPL-2.1-or-later
// Observer controls only. The actual device/kernel call is replaced by a leaf.
#define ISSUE690_UNIT
#include "../tools/research/linux-input-flags/probe.cpp"
#include "libcrystalhd_fwcmds.h"
#include "libcrystalhd_priv.h"

static unsigned checks, failures, native_calls;
static BOOL seen_flag;
static uint8_t source_bytes[] = {0,0,0,1,0x65,0x42};
static BC_STATUS output_status = BC_STS_SUCCESS, release_status = BC_STS_SUCCESS;
static BC_DTS_PROC_OUT scripted_output;
static bool initialize_control;
static void Check(bool value, const char *message) {
    ++checks;
    if (!value) { ++failures; std::fprintf(stderr, "FAIL %s\n", message); }
}
static BC_STATUS InputLeaf(HANDLE, uint8_t *data, uint32_t size, uint64_t token, BOOL flag) {
    Check(size == sizeof(source_bytes) && !std::memcmp(data, source_bytes, size) && token == Token(0,0),
          "same authored bytes and timestamp reach real-call seam");
    seen_flag = flag; return BC_STS_SUCCESS;
}
static BC_STATUS OutputLeaf(HANDLE, uint32_t, BC_DTS_PROC_OUT *out) {
    *out = scripted_output; return output_status;
}
static BC_STATUS ReleaseLeaf(HANDLE, PVOID, BOOL) { return release_status; }
static int IoctlLeaf(int, unsigned long command, void *pointer) {
    ++native_calls;
    auto *io = static_cast<BC_IOCTL_DATA *>(pointer);
    if (initialize_control) {
        const auto *init = reinterpret_cast<const C011CmdInit *>(io->u.fwCmd.cmd);
        Check(command == BCM_IOC_FW_CMD && io->IoctlDataSz == 0 &&
              init->command == eCMD_C011_INIT && init->sequence == 1 &&
              init->memSizeMBytes == 64 && init->inputClkFreq == 200000000 &&
              init->uartBaudRate == 38400 && init->initArcs == (C011_STREAM_ARC|C011_VDEC_ARC),
              "unmodified actual initialization envelope with legacy header0 reaches syscall seam");
        io->RetSts=BC_STS_SUCCESS;
        io->u.fwCmd.rsp[0]=init->command; io->u.fwCmd.rsp[1]=init->sequence; io->u.fwCmd.rsp[2]=0;
        return 0;
    }
    if (command == BCM_IOC_PROC_INPUT) {
        Check(io->u.ProcInput.Encrypted == 0 && io->u.ProcInput.BuffSz == sizeof(source_bytes) &&
              !std::memcmp(io->u.ProcInput.pDmaBuff, source_bytes, sizeof(source_bytes)),
              "unmodified clear ioctl payload reaches syscall seam");
        io->u.ProcInput.DramOffset = 0x123400;
    }
    io->RetSts = BC_STS_IO_ERROR; errno = EIO; return -1;
}
int main() {
    using namespace issue690;
    Check(!Prepare("relative"), "relative output directory refused before any device call");
    input_call=InputLeaf; output_call=OutputLeaf; release_call=ReleaseLeaf; ioctl_call=IoctlLeaf;
    events=std::tmpfile(); public_file=std::tmpfile(); tx_file=std::tmpfile();
    Check(events && public_file && tx_file, "temporary evidence files");
    Input input; Packet p; p.data.assign(source_bytes,source_bytes+sizeof(source_bytes));
    p.size=sizeof(source_bytes); input.packets.push_back(p); admitted=&input;
    HANDLE owner=reinterpret_cast<HANDLE>(0x1234);
    active=true;
    {
        // Execute the unmodified library's initialize -> pool -> DtsDrvCmd
        // path, including this diagnostic ioctl observer. Only syscall is a seam.
        DTS_LIB_CONTEXT context={}; BC_IOCTL_DATA pooled={};
        context.Sig=LIB_CTX_SIG; context.ProcessID=getpid(); context.DevHandle=99;
        context.DevId=BC_PCI_DEVID_FLEA; context.pIoDataFreeHd=&pooled;
        pthread_mutexattr_t attr; pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock,&attr); pthread_mutexattr_destroy(&attr);
        initialize_control=true;
        Check(DtsFWInitialize(&context,0)==BC_STS_SUCCESS && native_calls==1 && !failed,
              "actual normal initialization passes observer without rewriting header0");
        Check(pooled.IoctlDataSz==0 && context.pIoDataFreeHd==&pooled && !pooled.next,
              "actual initialization returns original zero-header envelope to pool");
        initialize_control=false; native_calls=0;
        pthread_mutex_destroy(&context.thLock);
    }
    for (unsigned value : {0U,1U}) {
        flag=value; inputs=0;
        Check(__wrap_DtsProcInput(owner,source_bytes,sizeof(source_bytes),Token(0,0),FALSE)==BC_STS_SUCCESS &&
              seen_flag==BOOL(value) && inputs==1, "public0/1 argument is sole input change");
    }
    inputs=0; failed=false;
    Check(__wrap_DtsProcInput(owner,source_bytes,sizeof(source_bytes),Token(0,0),TRUE)==BC_STS_ERROR &&
          failed && inputs==0, "unexpected incoming flag rejected");
    failed=false;
    BC_IOCTL_DATA io={}; io.IoctlDataSz=sizeof(io); io.u.ProcInput.pDmaBuff=source_bytes;
    io.u.ProcInput.BuffSz=sizeof(source_bytes);
    Check(ioctl(99,BCM_IOC_PROC_INPUT,&io)==-1 && errno==EIO && io.RetSts==BC_STS_IO_ERROR &&
          io.u.ProcInput.DramOffset==0x123400 && native_calls==1 && tx_bytes==sizeof(source_bytes),
          "actual failure errno/status/offset and full TX copy retained");
    io.IoctlDataSz=0;
    Check(ioctl(99,BCM_IOC_PROC_INPUT,&io)==-1 && errno==EINVAL && native_calls==1 && failed,
          "invalid TX envelope size still rejected before syscall");
    io.IoctlDataSz=sizeof(io); failed=false;
    io.u.ProcInput.Encrypted=1;
    Check(ioctl(99,BCM_IOC_PROC_INPUT,&io)==-1 && native_calls==1 && failed,
          "unexpected direct TX bit1 never sent");
    failed=false; io.u.ProcInput.Encrypted=0; io.u.ProcInput.Mapped=1;
    Check(ioctl(99,BCM_IOC_PROC_INPUT,&io)==-1 && native_calls==1 && failed,
          "unexpected mapped transfer never sent");
    failed=false; io.u.ProcInput.Mapped=0; tx_bytes=capture_limit;
    Check(ioctl(99,BCM_IOC_PROC_INPUT,&io)==-1 && native_calls==1 && failed,
          "bounded observer overflow stops before syscall");
    std::vector<uint8_t> image(width*height*2U,0x81);
    scripted_output={}; scripted_output.Ybuff=image.data(); scripted_output.YBuffDoneSz=image.size()/4;
    scripted_output.b422Mode=OUTPUT_MODE422_YUY2; scripted_output.PoutFlags=BC_POUT_FLAGS_PIB_VALID;
    scripted_output.PicInfo.width=width; scripted_output.PicInfo.height=height;
    scripted_output.PicInfo.timeStamp=Token(0,0);
    failed=false; interrupted=0;
    BC_DTS_PROC_OUT out={};
    pixels.file=std::tmpfile(); pixels.expected=expected; pixels.frame_bytes=image.size();
    pixels.total_bytes=image.size()*expected;
    Check(__wrap_DtsProcOutputNoCopy(owner,0,&out)==BC_STS_SUCCESS && acquired && pending_pixels &&
          pending.pixels==image, "plain validated payload copied while leased");
    image[0]=0x19;
    Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE)==BC_STS_SUCCESS && !acquired &&
          outputs==1 && pixels.written==width*height*2U, "owned copy written after one successful release");
    std::rewind(pixels.file);
    Check(std::fgetc(pixels.file)==0x81, "capture does not retain borrowed output pointer");
    scripted_output.PoutFlags|=BC_POUT_FLAGS_ENCRYPTED;
    scripted_output.Ybuff=reinterpret_cast<uint8_t *>(1);
    Check(__wrap_DtsProcOutputNoCopy(owner,0,&out)==BC_STS_SUCCESS && acquired && !pending_pixels && failed,
          "encrypted flag refuses pixel access before invalid pointer");
    Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE)==BC_STS_SUCCESS && !acquired && outputs==1,
          "rejected successful output still releases exactly once");
    failed=false; interrupted=0; scripted_output.PoutFlags=BC_POUT_FLAGS_PIB_VALID;
    scripted_output.Ybuff=image.data(); release_status=BC_STS_IO_ERROR;
    __wrap_DtsProcOutputNoCopy(owner,0,&out);
    Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE)==BC_STS_IO_ERROR && !acquired && outputs==1 && failed,
          "failed release neither writes nor retries ownership");
    failed=false; output_status=BC_STS_NO_DATA;
    Check(__wrap_DtsProcOutputNoCopy(owner,0,&out)==BC_STS_NO_DATA && !acquired && !failed,
          "unsuccessful fetch creates no release lease");
    active=false;
    Check(CloseFiles(), "evidence closes cleanly");
    std::printf("Issue690 observer controls: %u checks, %u failures; hardware=NOT_RUN\n",checks,failures);
    return failures ? 1 : 0;
}
