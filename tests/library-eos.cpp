/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the actual EOS, PES packetization and input-ring implementation.
 * --wrap=txBufPush injects an error at one complete packet enqueue; successful
 * enqueues use the real ring. Detection tests run the actual TX loop against
 * bounded status/DMA stubs and the public output APIs against a fake FETCH.
 * No device, firmware, shared-memory setup or OS worker thread is started.
 */
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <time.h>
#include <vector>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_fwcmds.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
/* Existing exported packetizer helper; not declared in the public API. */
BC_STATUS DtsAlignSendData(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
static unsigned checks, failures, fail_at, calls;
static unsigned stop_after, generation_after;
static DTS_LIB_CONTEXT *observed_context;
static BC_STATUS injected_error = BC_STS_IO_ERROR;
static std::vector<std::vector<uint8_t> > packets;
static struct {
    bool allow_status, allow_output, marker, run_tx, timeout_during_dma;
    bool dma_in_progress, progress_each_status;
    unsigned status_calls, status_limit, fetch_calls, dma_calls, sleeps;
    unsigned status_error_call, suspend_call, dma_bytes;
    uint32_t clock_step_ms;
    uint64_t clock_ms;
    BC_STATUS fetch_status, dma_status;
    BC_DTS_STATS status;
    uint32_t marker_data[1 + (sizeof(BC_PIC_INFO_BLOCK) + 3) / 4];
} detection;

static void check(bool condition, const char *message)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

extern "C" BC_STATUS __real_txBufPush(pTXBUFFER, uint8_t *, uint32_t);
extern "C" BC_STATUS __wrap_txBufPush(pTXBUFFER ring, uint8_t *bytes, uint32_t size)
{
    packets.emplace_back(bytes, bytes + size);
    if (++calls == fail_at)
        return injected_error;
    const BC_STATUS status = __real_txBufPush(ring, bytes, size);
    /* Deterministic stop between accepted fragments, not a failed enqueue.
     * Models the next packet observing cancellation after prior progress.
     */
    if (calls == stop_after)
        observed_context->State = BC_DEC_STATE_STOP;
    if (status == BC_STS_SUCCESS && calls == generation_after) {
        DtsLock(observed_context);
        observed_context->eosDrainGeneration++;
        DtsUnLock(observed_context);
    }
    return status;
}

extern "C" int __wrap_ioctl(int fd, unsigned long code, ...)
{
    if (detection.allow_output && fd == 99 && code == BCM_IOC_FETCH_RXBUFF) {
        va_list args;
        va_start(args, code);
        BC_IOCTL_DATA *data = va_arg(args, BC_IOCTL_DATA *);
        va_end(args);
        ++detection.fetch_calls;
        data->RetSts = detection.fetch_status;
        if (detection.marker) {
            data->RetSts = BC_STS_SUCCESS;
            data->u.DecOutData.Flags = COMP_FLAG_DATA_VALID;
            BC_DEC_YUV_BUFFS &buffer = data->u.DecOutData.OutPutBuffs;
            buffer.YuvBuff = reinterpret_cast<uint8_t *>(detection.marker_data);
            buffer.YuvBuffSz = sizeof(detection.marker_data);
            buffer.YBuffDoneSz = sizeof(detection.marker_data) / 4;
        }
        return 0;
    }
    std::fputs("unexpected ioctl in hardware-free EOS test\n", stderr);
    std::abort();
}

extern "C" int __wrap_usleep(useconds_t)
{
    if (!detection.run_tx || ++detection.sleeps > 1000)
        std::abort();
    return 0;
}

extern "C" int __wrap_clock_gettime(clockid_t clock_id, struct timespec *value)
{
    if (!detection.run_tx || clock_id != CLOCK_MONOTONIC || !value)
        std::abort();
    detection.clock_ms += detection.clock_step_ms;
    value->tv_sec = detection.clock_ms / 1000;
    value->tv_nsec = (detection.clock_ms % 1000) * 1000000;
    return 0;
}

BC_STATUS DtsGetDrvStat(HANDLE device, BC_DTS_STATS *status)
{
    if (!detection.allow_status || device != observed_context ||
        ++detection.status_calls > detection.status_limit)
        std::abort();
    *status = detection.status;
    if (detection.progress_each_status) {
        DtsLock(observed_context);
        observed_context->outputProgress++;
        DtsUnLock(observed_context);
    }
    if (detection.suspend_call == detection.status_calls) {
        status->pwr_state_change = BC_HW_SUSPEND;
        detection.clock_ms += 2000;
    }
    if (detection.run_tx && detection.status_calls == detection.status_limit)
        observed_context->txThreadExit = true;
    if (detection.status_error_call == detection.status_calls) {
        detection.clock_ms += 2000;
        return BC_STS_IO_ERROR;
    }
    return BC_STS_SUCCESS;
}

BC_STATUS DtsTxDmaText(HANDLE device, uint8_t *, uint32_t size, uint32_t *, uint8_t)
{
    if (!detection.run_tx || device != observed_context)
        std::abort();
    ++detection.dma_calls;
    detection.dma_bytes += size;
    detection.dma_in_progress = true;
    if (detection.timeout_during_dma) {
        /* txBufPop already removed these bytes, but the DMA has not returned.
         * A concurrent output timeout cannot regard that empty ring as EOS.
         * Calling here makes that interleaving deterministic without threads.
         */
        check(observed_context->circBuf.busySize == 0,
              "DMA-gap fixture reaches empty ring before transfer completes");
        BC_DTS_PROC_OUT output = {};
        const BC_STATUS result = DtsProcOutput(device, 1, &output);
        uint8_t eos = 0;
        check(result == BC_STS_NO_DATA && DtsIsEndOfStream(device, &eos) == BC_STS_SUCCESS,
              "DMA-gap timeout retains the public output/query status");
        check(!eos && !(output.PicInfo.flags & VDEC_FLAG_LAST_PICTURE),
              "FLEA timeout cannot complete EOS while popped TX DMA is in flight");
    }
    detection.dma_in_progress = false;
    return detection.dma_status;
}

/* The TX loop's suspend-recovery branch must never run in these tests. Wrap
 * only those cross-TU recovery entry points, not the loop or EOS decisions.
 */
extern "C" BC_STATUS __wrap_DtsSetupHardware(HANDLE, BOOL) { std::abort(); }
extern "C" BC_STATUS __wrap_DtsOpenDecoder(HANDLE, uint32_t) { std::abort(); }
extern "C" BC_STATUS __wrap_DtsStartDecoder(HANDLE) { std::abort(); }
extern "C" BC_STATUS __wrap_DtsStartCapture(HANDLE) { std::abort(); }
BC_STATUS DtsSetCoreClock(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsPushFwToFlea(HANDLE, char *) { std::abort(); }
BC_STATUS DtsPushAuthFwToLink(HANDLE, char *) { std::abort(); }
BC_STATUS DtsFWInitialize(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWActivateDecoder(HANDLE) { std::abort(); }
BC_STATUS DtsFWStartVideo(HANDLE, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)
{ std::abort(); }
BC_STATUS DtsFWOpenChannel(HANDLE, uint32_t, uint32_t) { std::abort(); }
BC_STATUS DtsFWSetVideoInput(HANDLE) { std::abort(); }
BC_STATUS DtsSetProgressive(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetVideoClock(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetTSMode(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsCopyRawDataToOutBuff(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12ToYV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyNV12(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsCopyFormat(DTS_LIB_CONTEXT *, BC_DTS_PROC_OUT *, BC_DTS_PROC_OUT *)
{ std::abort(); }
BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool) { std::abort(); }
void DumpInputSampleToFile(uint8_t *, uint32_t) { std::abort(); }

struct Fixture {
    bc_dil_glob_s globals;
    DTS_LIB_CONTEXT context;
    BC_IOCTL_DATA output_data;
    Fixture(uint32_t device, uint32_t subtype)
    {
        std::memset(&globals, 0, sizeof(globals));
        std::memset(&context, 0, sizeof(context));
        std::memset(&output_data, 0, sizeof(output_data));
        std::memset(&detection, 0, sizeof(detection));
        detection.fetch_status = BC_STS_TIMEOUT;
        detection.dma_status = BC_STS_SUCCESS;
        detection.status.drvFLL = BC_RX_LIST_CNT;
        bc_dil_glob_ptr = &globals;
        observed_context = &context;
        context.Sig = LIB_CTX_SIG;
        context.ProcessID = getpid();
        context.State = BC_DEC_STATE_START;
        context.DevId = device;
        context.DevHandle = 99; // Only the rejecting/mocked ioctl wrapper sees it.
        context.FixFlags = DTS_LOAD_FILE_PLAY_FW;
        context.MpoolCnt = BC_RX_LIST_CNT;
        context.pOutData = &output_data;
        context.VidParams.MediaSubType = static_cast<BC_MEDIA_SUBTYPE>(subtype);
        context.VidParams.StreamType = BC_STREAM_TYPE_PES;
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&context.thLock, &attr);
        pthread_mutexattr_destroy(&attr);
        if (!context.alignBuf ||
            txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
            std::abort();
        calls = 0;
        generation_after = 0;
        packets.clear();
    }
    ~Fixture()
    {
        DtsReleasePESConverter(&context);
        std::free(context.VidParams.pMetaData);
        txBufFree(&context.circBuf);
        pthread_mutex_destroy(&context.thLock);
        std::free(context.alignBuf);
        bc_dil_glob_ptr = nullptr;
    }
};

static bool clean_metadata(const DTS_LIB_CONTEXT &context)
{
    const PES_CONVERT_PARAMS &pes = context.PESConvParams;
    return !pes.m_bPESExtField && !pes.m_pPESExtField &&
           !pes.m_bPESPrivData && !pes.m_pPESPrivData &&
           !pes.m_bStuffing && !pes.m_nStuffingBytes;
}

static BC_STATUS send(Fixture &fixture, unsigned mode, bool public_api)
{
    return public_api ? DtsFlushInput(&fixture.context, mode)
                      : DtsSendEOS(&fixture.context, mode);
}

static void test_h264_pes_oracle()
{
    const std::vector<uint8_t> payload = {0, 0, 1, 0x65, 0xaa};
    {
        Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
        check(DtsAlignSendData(&fixture.context,
                               const_cast<uint8_t *>(payload.data()),
                               payload.size(), 66, false) == BC_STS_SUCCESS,
              "H.264 PTS oracle accepts one Annex-B payload");
        const std::vector<uint8_t> expected = {
            0, 0, 1, 0xe0, 0, 13, 0x81, 0x80, 5,
            0x21, 0, 1, 0, 0x85,
            0, 0, 1, 0x65, 0xaa,
        };
        check(packets.size() == 1 && packets[0] == expected,
              "H.264 PTS oracle freezes the exact PES header and marker bits");
    }
    {
        Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
        check(DtsAlignSendData(&fixture.context,
                               const_cast<uint8_t *>(payload.data()),
                               payload.size(), 0, false) == BC_STS_SUCCESS,
              "H.264 no-PTS oracle accepts one Annex-B payload");
        const std::vector<uint8_t> expected = {
            0, 0, 1, 0xe0, 0, 8, 0x81, 0, 0,
            0, 0, 1, 0x65, 0xaa,
        };
        check(packets.size() == 1 && packets[0] == expected,
              "H.264 no-PTS oracle freezes the exact nine-byte PES header");
    }
    {
        Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
        const uint8_t expected_pts[] = {0x2f, 0xff, 0xff, 0xff, 0xff};
        check(DtsAlignSendData(&fixture.context,
                               const_cast<uint8_t *>(payload.data()),
                               payload.size(), UINT64_C(0x1ffffffff), false) ==
                  BC_STS_SUCCESS,
              "H.264 maximum 33-bit PTS is accepted");
        check(packets.size() == 1 && packets[0].size() == payload.size() + 14 &&
                  std::equal(packets[0].begin() + 9, packets[0].begin() + 14,
                             expected_pts),
              "H.264 maximum 33-bit PTS retains every marker bit");
    }

    for (bool pts : {false, true}) {
        const size_t maximum = pts ? 65512U : 65517U;
        for (size_t extra : {size_t{0}, size_t{1}}) {
            Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
            std::vector<uint8_t> source(maximum + extra);
            for (size_t i = 0; i < source.size(); ++i)
                source[i] = static_cast<uint8_t>(i * 131U + 17U);
            check(DtsAlignSendData(&fixture.context, source.data(), source.size(),
                                   pts ? 66 : 0, false) == BC_STS_SUCCESS,
                  "H.264 boundary oracle accepts maximum and split payloads");
            check(packets.size() == 1 + extra && packets[0].size() == 65526 &&
                      packets[0][4] == 0xff && packets[0][5] == 0xf0,
                  "H.264 boundary oracle freezes the 0xfff0 PES length cap");
            size_t consumed = 0;
            for (size_t n = 0; n < packets.size(); ++n) {
                const size_t header = 9U + packets[n][8];
                check((n == 0 && pts) == ((packets[n][7] & 0x80) != 0),
                      "only the first H.264 PES fragment carries PTS");
                check(std::equal(packets[n].begin() + header, packets[n].end(),
                                 source.begin() + consumed),
                      "H.264 PES fragments retain source bytes without padding");
                consumed += packets[n].size() - header;
            }
            check(consumed == source.size(),
                  "H.264 PES boundary split consumes the source exactly once");
        }
    }
}

static void test_packets(uint32_t device, uint32_t subtype, unsigned mode)
{
    fail_at = 0;
    Fixture fixture(device, subtype);
    check(send(fixture, mode, false) == BC_STS_SUCCESS,
          "successful EOS returns success");
    const unsigned expected_calls = device == BC_PCI_DEVID_FLEA && mode == 0 ? 4 : 1;
    check(calls == expected_calls, "preserve codec/chip EOS packet count");
    check(fixture.context.bEOSCheck == (mode == 0),
          "only completed mode0 EOS arms completion checking");
    check(!fixture.context.bEOS, "submission alone does not claim completed EOS");
    check(clean_metadata(fixture.context), "successful EOS clears temporary PES metadata");

    size_t total = 0;
    for (const auto &packet : packets) {
        check(packet.size() >= 9 && packet[0] == 0 && packet[1] == 0 &&
                  packet[2] == 1 && packet[3] == 0xe0,
              "EOS uses real PES packetization");
        check(packet.size() == 6U + (static_cast<unsigned>(packet[4]) << 8) + packet[5],
              "PES byte count matches header");
        total += packet.size();
    }
    check(total < 1024 && total == fixture.context.circBuf.busySize,
          "complete EOS fits reserved ring space and all packet bytes are queued");
    const std::vector<std::vector<uint8_t> > reference = packets;
    uint64_t signature = UINT64_C(14695981039346656037);
    for (const auto &packet : reference)
        for (uint8_t byte : packet)
            signature = (signature ^ byte) * UINT64_C(1099511628211);
    /* Successful packet fingerprints captured from the unchanged production
     * implementation. Cover EOS payloads, PES headers, extension/private
     * metadata, stuffing, timing marker, and chip-specific WMV3 bytes.
     */
    uint64_t expected_signature;
    if (device == BC_PCI_DEVID_FLEA && mode == 0)
        expected_signature = subtype == BC_MSUBTYPE_H264 ? UINT64_C(0x17cd22341ff27781)
            : subtype == BC_MSUBTYPE_MPEG2VIDEO ? UINT64_C(0x041d7cf93026bb49)
            : UINT64_C(0xc24508523c5b493b);
    else if (device == BC_PCI_DEVID_LINK && subtype == BC_MSUBTYPE_WMV3)
        expected_signature = UINT64_C(0x4d47ab12c1f5fdb8);
    else
        expected_signature = subtype == BC_MSUBTYPE_H264 ? UINT64_C(0xc9a0371573ed76b8)
            : subtype == BC_MSUBTYPE_MPEG2VIDEO ? UINT64_C(0x4ed2a7fde1262c74)
            : UINT64_C(0x24352af17dc9a696);
    check(signature == expected_signature, "successful EOS packet bytes match pre-fix baseline");
    if (device == BC_PCI_DEVID_FLEA && subtype == BC_MSUBTYPE_H264 && mode == 0) {
        const std::vector<uint8_t> eos = {
            0, 0, 1, 0xe0, 0, 0x0b, 0x81, 0, 0,
            0, 0, 1, 0x0a, 0, 0, 1, 0x0a,
        };
        const std::vector<uint8_t> marker_header = {
            0, 0, 1, 0xe0, 0, 0xb2, 0x81, 1, 0x14, 0x80,
            'B', 'R', 'C', 'M',
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0xff, 0xff, 0xff,
        };
        check(packets.size() == 4 && packets[0] == eos &&
                  packets[2] == eos && packets[3] == eos,
              "FLEA H.264 mode-0 EOS freezes the E-M-E-E packet order");
        check(packets[1].size() == 184 &&
                  std::equal(marker_header.begin(), marker_header.end(),
                             packets[1].begin()),
              "FLEA H.264 timing marker freezes its PES private-data header");
        if (packets[1].size() == 184) {
            const uint8_t *body = packets[1].data() + marker_header.size();
            bool body_matches = true;
            for (size_t offset = 0; offset < 155; ++offset) {
                uint8_t expected = offset >= 37 ? 0xff : 0;
                if (offset == 4)
                    expected = 0x0c;
                else if (offset == 13 || offset == 14 ||
                         (offset >= 17 && offset <= 28))
                    expected = 0xff;
                else if (offset == 16)
                    expected = 0x01;
                else if (offset == 36)
                    expected = 0xbc;
                if (body[offset] != expected) {
                    body_matches = false;
                    break;
                }
            }
            check(body_matches,
                  "FLEA H.264 timing marker freezes all 155 body bytes");
        }
    }
    {
        Fixture public_success(device, subtype);
        check(send(public_success, mode, true) == BC_STS_SUCCESS && packets == reference,
              "public EOS API preserves successful packet bytes");
        check(public_success.context.bEOSCheck == (mode == 0) &&
                  clean_metadata(public_success.context),
              "public EOS API preserves successful arming and metadata cleanup");
    }

    /* Inject each possible enqueue failure. No later packet may be accepted
     * after an earlier failure, and partial EOS must not arm the heuristic.
     * Compare the attempted prefix against the successful production packets.
     */
    for (bool public_api : {false, true}) {
        for (unsigned fragment = 1; fragment <= expected_calls; ++fragment) {
            fail_at = fragment;
            Fixture failing(device, subtype);
            /* Retrying a previously armed mode0 must disarm on failure. */
            failing.context.bEOSCheck = mode == 0;
            const BC_STATUS status = send(failing, mode, public_api);
            std::printf("EOS device=%u subtype=%u mode=%u public=%d failure=%u "
                        "error=%d status=%d calls=%u armed=%d\n", device, subtype, mode,
                        public_api, fragment, injected_error, status, calls, failing.context.bEOSCheck);
            check(status == injected_error, "EOS preserves the failed packet status");
            check(calls == fragment, "EOS stops at the first failed packet");
            check(!failing.context.bEOSCheck && !failing.context.bEOS,
                  "failed EOS cannot arm or claim completion");
            check(clean_metadata(failing.context), "failed EOS clears temporary PES metadata");
            size_t accepted = 0;
            for (unsigned index = 0; index < fragment; ++index) {
                check(index < packets.size() && packets[index] == reference[index],
                      "failure does not alter bytes of the attempted EOS prefix");
                if (index + 1 < fragment)
                    accepted += reference[index].size();
            }
            check(failing.context.circBuf.busySize == accepted,
                  "failed EOS queues exactly the accepted prefix");
        }
    }
}

static void test_mode5_preserves_prior_check()
{
    for (bool public_api : {false, true}) {
        for (bool fail : {false, true}) {
            fail_at = fail ? 1 : 0;
            Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_WVC1);
            fixture.context.bEOSCheck = true;
            fixture.context.eosTxComplete = true;
            const uint64_t generation = fixture.context.eosDrainGeneration;
            check(send(fixture, 5, public_api) == (fail ? injected_error : BC_STS_SUCCESS),
                  "mode5 preserves submission status with a prior completion check");
            check(fixture.context.bEOSCheck && !fixture.context.bEOS,
                  "mode5 does not change the existing EOS-check state");
            check(!fixture.context.eosTxComplete &&
                      fixture.context.eosDrainGeneration == generation + 1,
                  "mode5 extends the TX fence and invalidates stale completion");
            check(clean_metadata(fixture.context), "mode5 always clears temporary PES metadata");
        }
    }
}

static void configure_softrave(Fixture &fixture, uint32_t subtype);

static void test_drain_generation_change()
{
    for (unsigned mode : {0U, 5U}) {
        for (bool public_api : {false, true}) {
            fail_at = stop_after = 0;
            Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_DIVX);
            configure_softrave(fixture, BC_MSUBTYPE_DIVX);
            fixture.context.bEOSCheck = mode == 5;
            fixture.context.eosTxComplete = true;
            const uint64_t generation = fixture.context.eosDrainGeneration;
            generation_after = 1;
            check(send(fixture, mode, public_api) == BC_STS_IO_USER_ABORT,
                  "an EOS drain cannot commit across a newer TX generation");
            check(fixture.context.eosDrainGeneration == generation + 2,
                  "generation change remains owned by the newer TX epoch");
            check(!fixture.context.eosTxComplete && !fixture.context.bEOS,
                  "a stale drain cannot certify or claim EOS completion");
            check(fixture.context.bEOSCheck == (mode == 5),
                  "generation abort preserves mode5 and leaves mode0 disarmed");
            check(clean_metadata(fixture.context),
                  "generation-aborted EOS clears temporary PES metadata");
        }
    }
}

static void configure_softrave(Fixture &fixture, uint32_t subtype)
{
    std::vector<uint8_t> metadata(4, 0);
    if (subtype == BC_MSUBTYPE_DIVX)
        metadata = {0,0,1,0xb0,5,0,0,1,0xb5,0x89,0x13,0,0,1,0,
            0,0,1,0x20,0,0xc4,0x8d,0x88,0,0xf5,0x14,4,0x2d,0x14,0x63,
            0,0,1,0xb2,'L','a','v','c','6','1','.','1','9','.','1','0','1'};
    BC_INPUT_FORMAT format = {};
    format.mSubtype = static_cast<BC_MEDIA_SUBTYPE>(subtype);
    format.width = subtype == BC_MSUBTYPE_DIVX ? 640 : 720;
    format.height = subtype == BC_MSUBTYPE_DIVX ? 360 : 576;
    format.Progressive = TRUE;
    format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
    format.pMetaData = metadata.data(); format.metaDataSz = metadata.size();
    check(DtsSetInputFormat(&fixture.context, &format) == BC_STS_SUCCESS &&
              fixture.context.PESConvParams.m_bSoftRave &&
              fixture.context.VidParams.StreamType == BC_STREAM_TYPE_PES,
          "public FLEA input setup enables SoftRave PES framing");
}

static std::vector<std::vector<uint8_t> > divx_eos_packets(unsigned mode)
{
    const std::vector<uint8_t> timestamped_end = {
        0,0,1,0xe0,0,16,0x81,0x80,5, 0x21,0,1,0,1,
        0,0,1,0xb1,0,0,1,0xb1};
    const std::vector<uint8_t> plain_end = {
        0,0,1,0xe0,0,11,0x81,0,0, 0,0,1,0xb1,0,0,1,0xb1};
    if (mode == 5) return {timestamped_end};
    // Independent PES/private-data oracle: 155-byte timing marker, 20 optional
    // header bytes (private-data flag, BRCM+12 zeros, 3 stuffing bytes), no PTS.
    std::vector<uint8_t> marker(184, 0xff);
    const uint8_t header[] = {0,0,1,0xe0,0,178,0x81,1,20,0x80,'B','R','C','M'};
    std::memcpy(marker.data(), header, sizeof(header));
    std::memset(marker.data()+14, 0, 12);
    uint8_t *body = marker.data()+29;
    std::memset(body, 0, 13); body[4] = 0x0c;
    body[13] = body[14] = 0xff; body[15] = 0; body[16] = 1;
    std::memset(body+29, 0, 7); body[36] = 0xbc;
    return {timestamped_end, marker, plain_end, plain_end};
}

static void test_configured_softrave_eos(uint32_t subtype)
{
    // Exercise the configured packetizer and public drain together.
    for (unsigned mode : {0U, 5U}) {
        const unsigned count = mode == 0 ? 4 : 1;
        for (bool public_api : {false, true}) {
            for (unsigned failure = 0; failure <= count + 1; ++failure) {
                Fixture fixture(BC_PCI_DEVID_FLEA, subtype);
                configure_softrave(fixture, subtype);
                const auto expected_divx = divx_eos_packets(mode);

                fail_at = stop_after = 0;
                uint8_t picture[4] = {0, 0, 1, static_cast<uint8_t>(subtype == BC_MSUBTYPE_DIVX ? 0xb6 : 0x0d)};
                check(DtsAlignSendData(&fixture.context, picture, sizeof(picture), 0, FALSE) ==
                          BC_STS_SUCCESS && packets.size() == 1 && (packets[0][7] & 0x80),
                      "ordinary SoftRave timestamp zero retains its required PTS field");
                const std::vector<uint8_t> ordinary = packets[0];
                check(txBufFlush(&fixture.context.circBuf) == BC_STS_SUCCESS,
                      "configured fixture empties only its own ordinary packet");
                packets.clear();
                calls = 0;
                // Last case cancels after one genuinely accepted packet.
                const bool canceled = failure == count + 1;
                fail_at = canceled ? 0 : failure;
                stop_after = canceled ? 1 : 0;
                fixture.context.bEOSCheck = true;
                const BC_STATUS result = send(fixture, mode, public_api);
                const BC_STATUS expected = canceled && count > 1 ? BC_STS_IO_USER_ABORT
                    : !canceled && failure ? injected_error : BC_STS_SUCCESS;
                check(result == expected, "configured SoftRave EOS preserves enqueue/cancellation status");
                check(calls == (canceled ? 1 : failure ? failure : count),
                      "configured SoftRave EOS stops at the first incomplete fragment");
                check(fixture.context.PESConvParams.m_bSoftRave && clean_metadata(fixture.context),
                      "EOS always restores ordinary SoftRave state and clears temporary metadata");
                check(fixture.context.bEOSCheck == (mode == 5 || result == BC_STS_SUCCESS),
                      "configured SoftRave preserves mode5 and successful-only mode0 arming");
                check(!fixture.context.bEOS, "EOS submission never manufactures firmware completion");
                uint64_t signature = UINT64_C(14695981039346656037);
                size_t bytes = 0;
                for (size_t n = 0; n < packets.size(); ++n) {
                    const auto &packet = packets[n];
                    const bool tail_boundary = subtype == BC_MSUBTYPE_DIVX && n == 0;
                    check(packet.size() >= 9 && (packet[7] & 0xc0) ==
                              (tail_boundary ? 0x80 : 0),
                          "only the first DIVX sequence end carries the tail PTS");
                    for (uint8_t byte : packet)
                        signature = (signature ^ byte) * UINT64_C(1099511628211);
                    bytes += packet.size();
                }
                if (failure == 0) {
                    check(subtype == BC_MSUBTYPE_DIVX ? packets == expected_divx :
                          signature == (mode == 0 ? UINT64_C(0xc24508523c5b493b)
                                                   : UINT64_C(0x24352af17dc9a696)),
                          "configured EOS matches the codec packet oracle");
                    check(bytes < 1024, "configured SoftRave EOS fits the whole-call reserve");
                    std::printf("SOFTRAVE subtype=%u mode=%u public=%d bytes=%zu signature=%016llx\n",
                                subtype, mode, public_api, bytes, static_cast<unsigned long long>(signature));
                }
                if (subtype == BC_MSUBTYPE_DIVX) {
                    size_t accepted = 0, complete = 0;
                    for (size_t n = 0; n < expected_divx.size(); ++n) {
                        complete += expected_divx[n].size();
                        if (n < packets.size()) {
                            check(packets[n] == expected_divx[n], "partial DIVX EOS retains exact attempted packet bytes");
                            if (canceled || !failure || n + 1 < failure)
                                accepted += packets[n].size();
                        }
                    }
                    check(fixture.context.circBuf.busySize == accepted,
                          "partial DIVX EOS retains only genuinely accepted ring bytes");
                    fixture.context.State = BC_DEC_STATE_START;
                    fail_at = stop_after = calls = 0; packets.clear();
                    check(send(fixture, mode, public_api) == BC_STS_SUCCESS && packets == expected_divx,
                          "DIVX EOS retry submits the full original control sequence");
                    check(fixture.context.circBuf.busySize == accepted + complete,
                          "DIVX retry preserves the accepted prefix and appends exactly one full sequence");
                    check(fixture.context.PESConvParams.m_bSoftRave && clean_metadata(fixture.context) &&
                          fixture.context.bEOSCheck && !fixture.context.bEOS,
                          "DIVX retry restores packetizer state without claiming completed EOS");
                }

                // A failed or canceled drain must not alter subsequent input.
                fixture.context.State = BC_DEC_STATE_START;
                fail_at = stop_after = calls = 0;
                txBufFlush(&fixture.context.circBuf);
                packets.clear();
                check(DtsAlignSendData(&fixture.context, picture, sizeof(picture), 0, FALSE) ==
                          BC_STS_SUCCESS && packets.size() == 1 && packets[0] == ordinary,
                      "ordinary zero-PTS packet bytes are unchanged after every EOS exit");
            }
        }
    }
}

static void test_canceled_packet()
{
    fail_at = 1;
    injected_error = BC_STS_IO_USER_ABORT;
    Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
    uint8_t payload[8] = {0, 0, 1, 0x0a, 0, 0, 1, 0x0a};
    const BC_STATUS status = DtsAlignSendData(&fixture.context, payload, sizeof(payload), 0, false);
    check(status == BC_STS_IO_USER_ABORT, "packetizer cannot report canceled input as accepted");
    check(calls == 1 && fixture.context.circBuf.busySize == 0,
          "canceled packet queues no bytes and is not retried");
    std::printf("ABORT packet status=%d calls=%u queued=%u\n", status, calls,
                fixture.context.circBuf.busySize);
    test_packets(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264, 0);
    injected_error = BC_STS_IO_ERROR;
}

static void test_stop_between_fragments()
{
    for (bool public_api : {false, true}) {
        fail_at = 0;
        stop_after = 1;
        Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
        const BC_STATUS status = send(fixture, 0, public_api);
        check(status == BC_STS_IO_USER_ABORT && calls == 1 && !fixture.context.bEOSCheck,
              "stop between EOS fragments cannot acknowledge unsent remaining packets");
        check(packets.size() == 1 && fixture.context.circBuf.busySize == packets[0].size(),
              "stopped EOS retains only its genuinely accepted prefix");
        check(clean_metadata(fixture.context), "stopped EOS clears temporary PES metadata");
        stop_after = 0;
    }

    /* The same packetizer serves ordinary input. A large access unit spans
     * multiple PES packets, and cancellation must not acknowledge its suffix.
     */
    fail_at = 0;
    stop_after = 1;
    Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
    std::vector<uint8_t> payload(100000, 0x5a);
    const BC_STATUS status = DtsAlignSendData(&fixture.context, payload.data(),
                                            payload.size(), 0, false);
    check(status == BC_STS_IO_USER_ABORT && calls == 1,
          "stop between PES fragments reports incomplete ordinary input");
    check(packets.size() == 1 && packets[0].size() < payload.size() &&
              fixture.context.circBuf.busySize == packets[0].size(),
          "ordinary canceled input queues only its first actual PES packet");
    stop_after = 0;
}

static void test_packetizer_boundaries()
{
    fail_at = stop_after = 0;
    for (unsigned state : {static_cast<unsigned>(BC_DEC_STATE_CLOSE),
                           static_cast<unsigned>(BC_DEC_STATE_STOP),
                           static_cast<unsigned>(BC_DEC_STATE_FLUSH)}) {
        Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
        fixture.context.State = state;
        uint8_t payload[8] = {};
        check(DtsAlignSendData(&fixture.context, payload, sizeof(payload), 0, false)
                  == BC_STS_IO_USER_ABORT && calls == 0,
              "inactive packetizer cannot acknowledge bytes without entering send loop");
        check(DtsAlignSendData(&fixture.context, payload, 0, 0, false) == BC_STS_SUCCESS && calls == 0,
              "zero-length packetizer call retains its prior success semantics");
    }

    Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
    std::vector<uint8_t> payload(100000, 0x5a);
    check(DtsAlignSendData(&fixture.context, payload.data(), payload.size(), 0, false)
              == BC_STS_SUCCESS && calls == 2,
          "uncanceled multi-PES input still completes successfully");
    size_t consumed = 0;
    for (const auto &packet : packets) {
        const size_t header_size = 9U + packet[8];
        const size_t data_size = packet.size() - header_size;
        check(consumed + data_size <= payload.size() &&
                  std::memcmp(packet.data() + header_size, payload.data() + consumed, data_size) == 0,
              "uncanceled multi-PES input retains every original payload byte");
        consumed += data_size;
    }
    check(consumed == payload.size() && fixture.context.circBuf.busySize == payload.size() + 18,
          "uncanceled multi-PES input has exactly two original PES headers");
}

static void test_invalid_state()
{
    for (unsigned mode : {0U, 5U}) {
        for (unsigned state : {static_cast<unsigned>(BC_DEC_STATE_CLOSE),
                               static_cast<unsigned>(BC_DEC_STATE_STOP),
                               static_cast<unsigned>(BC_DEC_STATE_FLUSH)}) {
            fail_at = 0;
            Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
            fixture.context.State = state;
            const BC_STATUS expected = state == BC_DEC_STATE_CLOSE
                ? BC_STS_DEC_NOT_OPEN : BC_STS_DEC_NOT_STARTED;
            check(DtsFlushInput(&fixture.context, mode) == expected,
                  "public EOS rejects unopened/stopped/flushing decoder");
            check(calls == 0 && !fixture.context.bEOSCheck,
                  "invalid-state EOS cannot queue or arm anything");
        }
    }
}

static uint8_t query_eos(Fixture &fixture)
{
    uint8_t eos = 0xff;
    check(DtsIsEndOfStream(&fixture.context, &eos) == BC_STS_SUCCESS,
          "EOS query succeeds for a valid context");
    return eos;
}

static void test_idle_detection(uint32_t device, unsigned scenario)
{
    fail_at = stop_after = 0;
    Fixture fixture(device, BC_MSUBTYPE_H264);
    check(send(fixture, 0, true) == BC_STS_SUCCESS,
          "detection fixture queues the actual public EOS packets");
    const uint32_t queued = fixture.context.circBuf.busySize;
    detection.allow_status = detection.run_tx = true;
    detection.status_limit = BC_EOS_PIC_COUNT + 3;
    // 0: TX blocked; 1: RX ready; 2: empty/no marker; 3: failed DMA;
    // 4: timeout between the real ring pop and successful DMA completion.
    if (scenario == 1 || scenario == 2) {
        std::vector<uint8_t> transmitted(queued);
        check(txBufPop(&fixture.context.circBuf, transmitted.data(), queued) == BC_STS_SUCCESS,
              "idle fixture removes exactly the previously submitted EOS bytes");
    }
    if (scenario == 1)
        detection.status.drvRLL = 3;
    if (scenario == 3 || scenario == 4)
        detection.status.DrvcpbEmptySize = CIRC_TX_BUF_SIZE;
    if (scenario == 3)
        detection.dma_status = BC_STS_IO_ERROR;
    if (scenario == 4) {
        detection.allow_output = true;
        detection.timeout_during_dma = true;
    }
    txThreadProc(&fixture.context);
    detection.run_tx = false;
    const bool eos = query_eos(fixture) != 0;
    std::printf("DETECT device=%u scenario=%u polls=%u queued=%u DMA=%u/%u EOS=%d\n",
                device, scenario, detection.status_calls, fixture.context.circBuf.busySize,
                detection.dma_calls, detection.dma_bytes, eos);
    check(detection.status_calls == detection.status_limit,
          "actual TX worker runs a bounded complete idle-poll sequence");
    check(!detection.dma_in_progress, "mock DMA always returns before worker completion");
    check(fixture.context.circBuf.busySize == (scenario == 0 ? queued : 0),
          "TX scenario retains/removes exactly the expected ring bytes");
    check(detection.dma_calls == (scenario >= 3 ? 1U : 0U),
          "TX scenario reaches only its expected real-pop/mock-DMA path");
    if (device == BC_PCI_DEVID_FLEA)
        check(!eos, "FLEA capture silence without a firmware marker cannot claim EOS");
    else
        check(eos, "LINK retains its existing marker-less idle EOS fallback");
}

static void test_flea_mpeg4_idle_fallback(unsigned scenario)
{
    fail_at = stop_after = 0;
    Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_DIVX);
    configure_softrave(fixture, BC_MSUBTYPE_DIVX);
    check(send(fixture, 0, true) == BC_STS_SUCCESS,
          "MPEG-4 fallback fixture submits the complete production drain");
    const uint64_t fence = fixture.context.eosTxFence;
    detection.allow_status = detection.run_tx = true;
    detection.status.DrvcpbEmptySize = CIRC_TX_BUF_SIZE;
    detection.clock_step_ms = 100;
    detection.status_limit = 20;
    // 0: eligible; 1: RLL; 2: active RX/FLL short; 3: leased output;
    // 4: failed TX DMA; 5: output progress; 6: under one second;
    // 7: status-error gap; 8: suspend gap; 9: in-flight output timeout;
    // 10..12: non-idle output lifetime with pending zero; 13: cancellation only.
    switch (scenario) {
    case 1:
        detection.status.drvRLL = 1;
        break;
    case 2:
        detection.status.drvFLL = BC_RX_LIST_CNT - 1;
        break;
    case 3:
        fixture.context.ProcOutPending = 1;
        break;
    case 4:
        detection.dma_status = BC_STS_IO_ERROR;
        break;
    case 5:
        detection.progress_each_status = true;
        break;
    case 6:
        detection.status_limit = 8;
        break;
    case 7:
        detection.status_error_call = 5;
        detection.status_limit = 10;
        break;
    case 8:
        detection.suspend_call = 5;
        detection.status_limit = 10;
        break;
    case 9:
        detection.allow_output = true;
        detection.timeout_during_dma = true;
        break;
    case 10:
        fixture.context.outputPhase = DTS_OUTPUT_ACTIVE;
        break;
    case 11:
        fixture.context.outputPhase = DTS_OUTPUT_RETURNED;
        break;
    case 12:
        fixture.context.outputPhase = DTS_OUTPUT_RETIRE_ONLY;
        break;
    case 13:
        fixture.context.CancelWaiting = true;
        break;
    default:
        break;
    }
    txThreadProc(&fixture.context);
    detection.run_tx = false;
    const bool eos = query_eos(fixture) != 0;
    const bool expected = scenario == 0 || scenario == 9;
    std::printf("FLEA-MPEG4 scenario=%u polls=%u DMA=%u/%u retired=%llu fence=%llu EOS=%d\n",
                scenario, detection.status_calls, detection.dma_calls, detection.dma_bytes,
                static_cast<unsigned long long>(fixture.context.txBytesRetired),
                static_cast<unsigned long long>(fence), eos);
    check(detection.status_calls == detection.status_limit && detection.dma_calls == 1,
          "MPEG-4 fallback fixture runs one complete fenced TX DMA");
    check(fixture.context.txBytesRetired >= fence,
          "MPEG-4 fallback observes retirement through the captured fence");
    check(eos == expected,
          expected ? "only a fully idle MPEG-4 fence completes fallback EOS"
                   : "unsafe MPEG-4 state cannot complete fallback EOS");
    if (scenario >= 10 && scenario <= 12)
        check(fixture.context.ProcOutPending == 0 &&
                  fixture.context.outputPhase != DTS_OUTPUT_IDLE,
              "non-idle whole-call lifetime cannot be inferred from pending zero");
    if (scenario == 13)
        check(fixture.context.ProcOutPending == 0 &&
                  fixture.context.outputPhase == DTS_OUTPUT_IDLE && fixture.context.CancelWaiting,
              "an owned cancellation alone prevents idle EOS publication");
    if (scenario == 4)
        check(fixture.context.txDmaFault && !fixture.context.eosTxComplete,
              "failed DMA poisons the session and invalidates fence completion");
}

static void test_new_input_disarms_flea_fallback()
{
    fail_at = stop_after = 0;
    Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_DIVX);
    configure_softrave(fixture, BC_MSUBTYPE_DIVX);
    check(send(fixture, 0, true) == BC_STS_SUCCESS,
          "new-input fixture starts from an armed MPEG-4 drain");
    fixture.context.eosTxComplete = true;
    const uint64_t generation = fixture.context.eosDrainGeneration;
    uint8_t picture[] = {0, 0, 1, 0xb6, 0};
    check(DtsProcInput(&fixture.context, picture, sizeof(picture), 10000, FALSE) ==
              BC_STS_SUCCESS,
          "ordinary MPEG-4 input is accepted after a drain request");
    check(!fixture.context.bEOSCheck && !fixture.context.eosTxComplete &&
              fixture.context.eosDrainGeneration == generation + 1,
          "new input disarms the old EOS fence and advances its generation");
}

static void test_timeout_detection(uint32_t device, bool no_copy, uint32_t timeout)
{
    fail_at = stop_after = 0;
    Fixture fixture(device, BC_MSUBTYPE_H264);
    check(send(fixture, 0, true) == BC_STS_SUCCESS,
          "timeout fixture arms EOS only through actual successful packet submission");
    const uint32_t queued = fixture.context.circBuf.busySize;
    detection.allow_output = true;
    const unsigned attempts = timeout == 0 ? BC_EOS_PIC_COUNT + 1 : 1;
    BC_DTS_PROC_OUT output = {};
    bool last_picture_seen = false;
    for (unsigned attempt = 0; attempt < attempts; ++attempt) {
        output = {};
        const BC_STATUS result = no_copy ? DtsProcOutputNoCopy(&fixture.context, timeout, &output)
                                        : DtsProcOutput(&fixture.context, timeout, &output);
        const BC_STATUS expected = no_copy && timeout != 0 ? BC_STS_TIMEOUT : BC_STS_NO_DATA;
        check(result == expected, "timeouts preserve existing copy/NoCopy public status");
        last_picture_seen |= (output.PicInfo.flags & VDEC_FLAG_LAST_PICTURE) != 0;
        check(fixture.context.ProcOutPending == 0,
              "failed fetch releases only its own admission and leaves no output lease");
    }
    const bool eos = query_eos(fixture) != 0;
    std::printf("TIMEOUT device=%u no-copy=%d wait=%u calls=%u queued=%u EOS=%d flags=%x\n",
                device, no_copy, timeout, detection.fetch_calls, queued, eos,
                output.PicInfo.flags);
    check(detection.fetch_calls == attempts && fixture.context.circBuf.busySize == queued,
          "output timeout neither submits input nor consumes pending EOS bytes");
    if (device == BC_PCI_DEVID_FLEA || no_copy) {
        check(!eos && !last_picture_seen,
              "FLEA/NoCopy timeout does not fabricate completed EOS or last picture");
    } else {
        check(eos && last_picture_seen,
              "LINK copy API retains its existing timeout EOS fallback");
    }
}

static void test_firmware_eos(uint32_t subtype = BC_MSUBTYPE_H264)
{
    for (bool armed : {false, true}) {
        for (bool no_copy : {false, true}) {
            fail_at = stop_after = 0;
            Fixture fixture(BC_PCI_DEVID_FLEA, subtype);
            if (subtype == BC_MSUBTYPE_DIVX) configure_softrave(fixture, subtype);
            fixture.context.bEOSCheck = armed;
            detection.allow_output = detection.marker = true;
            BC_PIC_INFO_BLOCK picture = {};
            picture.flags = VDEC_FLAG_EOS;
            detection.marker_data[0] = BC_EOS_DETECTED;
            std::memcpy(detection.marker_data + 1, &picture, sizeof(picture));
            BC_DTS_PROC_OUT output = {};
            const BC_STATUS result = no_copy ? DtsProcOutputNoCopy(&fixture.context, 0, &output)
                                            : DtsProcOutput(&fixture.context, 0, &output);
            check(result == BC_STS_NO_DATA && (output.PicInfo.flags & VDEC_FLAG_EOS),
                  "actual embedded firmware EOS remains a NO_DATA marker, not a picture");
            check(query_eos(fixture) && fixture.context.ProcOutPending == 0,
                  "firmware marker completes EOS without leaking fetch admission");
            check(detection.fetch_calls == 1,
                  "firmware marker is consumed once without a replacement-buffer ioctl");
        }
        Fixture fixture(BC_PCI_DEVID_FLEA, subtype);
        if (subtype == BC_MSUBTYPE_DIVX) configure_softrave(fixture, subtype);
        fixture.context.bEOSCheck = armed;
        detection.allow_status = true;
        detection.status_limit = 1;
        detection.status.eosDetected = 1;
        detection.status.drvRLL = 2;
        BC_DTS_STATUS status = {};
        check(DtsGetDriverStatus(&fixture.context, &status) == BC_STS_SUCCESS &&
                  status.ReadyListCount == 2 && query_eos(fixture),
              "genuine driver EOS is preserved even while the caller still has ready output");
        std::printf("FIRMWARE armed=%d public marker/status paths preserved\n", armed);
    }
}

static void test_link_repeat_compatibility()
{
    Fixture fixture(BC_PCI_DEVID_LINK, BC_MSUBTYPE_H264);
    fixture.context.bEOSCheck = true;
    fixture.context.VidParams.Progressive = true;
    fixture.context.LastPicNum = 10;
    fixture.context.LastSessNum = 1;
    BC_DTS_PROC_OUT output = {};
    output.PicInfo.picture_number = 10;
    output.PicInfo.sess_num = 1;
    for (unsigned count = 0; count < BC_EOS_PIC_COUNT; ++count)
        check(DtsCheckRptPic(&fixture.context, &output),
              "LINK repeated-picture detection remains active");
    check(query_eos(fixture) && (output.PicInfo.flags & VDEC_FLAG_LAST_PICTURE),
          "LINK retains repeated-picture EOS and LAST_PICTURE compatibility");
}

int main()
{
    test_h264_pes_oracle();
    for (uint32_t device : {BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK}) {
        for (uint32_t subtype : {BC_MSUBTYPE_H264, BC_MSUBTYPE_MPEG2VIDEO,
                                 BC_MSUBTYPE_VC1, BC_MSUBTYPE_WVC1, BC_MSUBTYPE_WMV3}) {
            for (unsigned mode : {0U, 5U})
                test_packets(device, subtype, mode);
        }
    }
    test_invalid_state();
    test_mode5_preserves_prior_check();
    test_drain_generation_change();
    test_configured_softrave_eos(BC_MSUBTYPE_WMV3);
    for (BC_STATUS error : {BC_STS_IO_ERROR, BC_STS_INSUFF_RES, BC_STS_IO_USER_ABORT}) {
        injected_error = error;
        test_configured_softrave_eos(BC_MSUBTYPE_DIVX);
    }
    test_canceled_packet();
    test_stop_between_fragments();
    test_packetizer_boundaries();
    for (unsigned scenario = 0; scenario < 5; ++scenario)
        test_idle_detection(BC_PCI_DEVID_FLEA, scenario);
    test_idle_detection(BC_PCI_DEVID_LINK, 2);
    for (unsigned scenario = 0; scenario < 14; ++scenario)
        test_flea_mpeg4_idle_fallback(scenario);
    test_new_input_disarms_flea_fallback();
    for (uint32_t device : {BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK})
        for (bool no_copy : {false, true})
            for (uint32_t timeout : {0U, 1U})
                test_timeout_detection(device, no_copy, timeout);
    test_firmware_eos();
    test_firmware_eos(BC_MSUBTYPE_DIVX);
    test_link_repeat_compatibility();
    if (failures) {
        std::fprintf(stderr, "%u EOS checks failed\n", failures);
        return 1;
    }
    std::printf("PASS: %u production EOS submission/detection checks, firmware markers and LINK compatibility\n", checks);
    return 0;
}
