/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the actual EOS, PES packetization and input-ring implementation.
 * --wrap=txBufPush injects an error at one complete packet enqueue; successful
 * enqueues use the real ring. Detection tests run the actual TX loop against
 * bounded status/DMA stubs and the public output APIs against a fake FETCH.
 * No device, firmware, shared-memory setup or OS worker thread is started.
 */
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
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
static unsigned failures, fail_at, calls;
static unsigned stop_after;
static DTS_LIB_CONTEXT *observed_context;
static BC_STATUS injected_error = BC_STS_IO_ERROR;
static std::vector<std::vector<uint8_t> > packets;
static struct {
    bool allow_status, allow_output, marker, run_tx, timeout_during_dma;
    bool dma_in_progress;
    unsigned status_calls, status_limit, fetch_calls, dma_calls, sleeps;
    unsigned dma_bytes;
    BC_STATUS fetch_status, dma_status;
    BC_DTS_STATS status;
    uint32_t marker_data[1 + (sizeof(BC_PIC_INFO_BLOCK) + 3) / 4];
} detection;

static void check(bool condition, const char *message)
{
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

BC_STATUS DtsGetDrvStat(HANDLE device, BC_DTS_STATS *status)
{
    if (!detection.allow_status || device != observed_context ||
        ++detection.status_calls > detection.status_limit)
        std::abort();
    *status = detection.status;
    if (detection.run_tx && detection.status_calls == detection.status_limit)
        observed_context->txThreadExit = true;
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
        bc_dil_glob_ptr = &globals;
        observed_context = &context;
        context.Sig = LIB_CTX_SIG;
        context.ProcessID = getpid();
        context.State = BC_DEC_STATE_START;
        context.DevId = device;
        context.DevHandle = 99; // Only the rejecting/mocked ioctl wrapper sees it.
        context.FixFlags = DTS_LOAD_FILE_PLAY_FW;
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
            check(send(fixture, 5, public_api) == (fail ? injected_error : BC_STS_SUCCESS),
                  "mode5 preserves submission status with a prior completion check");
            check(fixture.context.bEOSCheck && !fixture.context.bEOS,
                  "mode5 does not change the existing EOS-check state");
            check(clean_metadata(fixture.context), "mode5 always clears temporary PES metadata");
        }
    }
}

static void test_configured_wmv3_eos()
{
    // The original BCM70015 import (813af6d) explicitly omitted PTS during
    // SoftRave EOS. Unlike a zeroed synthetic context, real WMV3 setup enables
    // SoftRave and adds PTS even for ordinary input timestamp zero. Exercise
    // the actual setup, packetizer and public drain together; firmware marker
    // recognition still requires the optional hardware drain probe.
    for (unsigned mode : {0U, 5U}) {
        const unsigned count = mode == 0 ? 4 : 1;
        for (bool public_api : {false, true}) {
            for (unsigned failure = 0; failure <= count + 1; ++failure) {
                Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_WMV3);
                BC_INPUT_FORMAT format = {};
                uint8_t metadata[4] = {};
                format.mSubtype = BC_MSUBTYPE_WMV3;
                format.width = 720;
                format.height = 576;
                format.Progressive = TRUE;
                format.OptFlags = 0x80000000U | vdecFrameRate59_94 | 0x40U;
                format.pMetaData = metadata;
                format.metaDataSz = sizeof(metadata);
                check(DtsSetInputFormat(&fixture.context, &format) == BC_STS_SUCCESS &&
                          fixture.context.PESConvParams.m_bSoftRave &&
                          fixture.context.VidParams.StreamType == BC_STREAM_TYPE_PES,
                      "actual FLEA WMV3 format setup enables SoftRave PES input");

                fail_at = stop_after = 0;
                uint8_t picture[4] = {0, 0, 1, 0x0d};
                check(DtsAlignSendData(&fixture.context, picture, sizeof(picture), 0, FALSE) ==
                          BC_STS_SUCCESS && packets.size() == 1 && (packets[0][7] & 0x80),
                      "ordinary WMV3 timestamp zero retains its required PTS field");
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
                check(result == expected, "configured WMV3 EOS preserves enqueue/cancellation status");
                check(calls == (canceled ? 1 : failure ? failure : count),
                      "configured WMV3 EOS stops at the first incomplete fragment");
                check(fixture.context.PESConvParams.m_bSoftRave && clean_metadata(fixture.context),
                      "EOS always restores ordinary SoftRave state and clears temporary metadata");
                check(fixture.context.bEOSCheck == (mode == 5 || result == BC_STS_SUCCESS),
                      "configured WMV3 preserves mode5 and successful-only mode0 arming");
                uint64_t signature = UINT64_C(14695981039346656037);
                size_t bytes = 0;
                for (const auto &packet : packets) {
                    check(packet.size() >= 9 && (packet[7] & 0xc0) == 0,
                          "SoftRave EOS control packets omit ordinary-picture PTS/DTS");
                    for (uint8_t byte : packet)
                        signature = (signature ^ byte) * UINT64_C(1099511628211);
                    bytes += packet.size();
                }
                if (failure == 0) {
                    check(signature == (mode == 0 ? UINT64_C(0xc24508523c5b493b)
                                                   : UINT64_C(0x24352af17dc9a696)),
                          "configured WMV3 EOS matches the historical no-PTS marker bytes");
                    check(bytes < 1024, "configured WMV3 EOS fits the whole-call reserve");
                    std::printf("SOFTRAVE mode=%u public=%d bytes=%zu signature=%016llx\n",
                                mode, public_api, bytes, static_cast<unsigned long long>(signature));
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

static void test_firmware_eos()
{
    for (bool armed : {false, true}) {
        for (bool no_copy : {false, true}) {
            fail_at = stop_after = 0;
            Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
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
        Fixture fixture(BC_PCI_DEVID_FLEA, BC_MSUBTYPE_H264);
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
    for (uint32_t device : {BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK}) {
        for (uint32_t subtype : {BC_MSUBTYPE_H264, BC_MSUBTYPE_MPEG2VIDEO,
                                 BC_MSUBTYPE_VC1, BC_MSUBTYPE_WVC1, BC_MSUBTYPE_WMV3}) {
            for (unsigned mode : {0U, 5U})
                test_packets(device, subtype, mode);
        }
    }
    test_invalid_state();
    test_mode5_preserves_prior_check();
    test_configured_wmv3_eos();
    test_canceled_packet();
    test_stop_between_fragments();
    test_packetizer_boundaries();
    for (unsigned scenario = 0; scenario < 5; ++scenario)
        test_idle_detection(BC_PCI_DEVID_FLEA, scenario);
    test_idle_detection(BC_PCI_DEVID_LINK, 2);
    for (uint32_t device : {BC_PCI_DEVID_FLEA, BC_PCI_DEVID_LINK})
        for (bool no_copy : {false, true})
            for (uint32_t timeout : {0U, 1U})
                test_timeout_detection(device, no_copy, timeout);
    test_firmware_eos();
    test_link_repeat_compatibility();
    if (failures) {
        std::fprintf(stderr, "%u EOS checks failed\n", failures);
        return 1;
    }
    std::puts("PASS: production EOS submission/detection, firmware markers and LINK compatibility");
    return 0;
}
