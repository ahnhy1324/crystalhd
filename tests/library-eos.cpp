/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Exercise the actual EOS, PES packetization and input-ring implementation.
 * --wrap=txBufPush injects an error at one complete packet enqueue; successful
 * enqueues use the real ring. Firmware and ioctl entry points abort if reached.
 * No device, firmware, shared-memory setup or worker thread is started.
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

extern "C" int __wrap_ioctl(int, unsigned long, ...)
{
    std::fputs("unexpected ioctl in hardware-free EOS test\n", stderr);
    std::abort();
}
BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool) { std::abort(); }
void DumpInputSampleToFile(uint8_t *, uint32_t) { std::abort(); }

struct Fixture {
    bc_dil_glob_s globals;
    DTS_LIB_CONTEXT context;
    Fixture(uint32_t device, uint32_t subtype)
    {
        std::memset(&globals, 0, sizeof(globals));
        std::memset(&context, 0, sizeof(context));
        bc_dil_glob_ptr = &globals;
        observed_context = &context;
        context.Sig = LIB_CTX_SIG;
        context.ProcessID = getpid();
        context.State = BC_DEC_STATE_START;
        context.DevId = device;
        context.VidParams.MediaSubType = static_cast<BC_MEDIA_SUBTYPE>(subtype);
        context.VidParams.StreamType = BC_STREAM_TYPE_PES;
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        if (!context.alignBuf ||
            txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
            std::abort();
        calls = 0;
        packets.clear();
    }
    ~Fixture()
    {
        txBufFree(&context.circBuf);
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
    test_canceled_packet();
    test_stop_between_fragments();
    test_packetizer_boundaries();
    if (failures) {
        std::fprintf(stderr, "%u EOS checks failed\n", failures);
        return 1;
    }
    std::puts("PASS: production EOS packet errors, public status, metadata and byte prefixes");
    return 0;
}
