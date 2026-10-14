// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual DtsProcInput/PES/parser/ring regression; no decoder/device is opened.
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include "7411d.h"
#include "libcrystalhd_if.h"
#include "libcrystalhd_int_if.h"
#include "libcrystalhd_priv.h"
#include "libcrystalhd_fwcmds.h"

extern bc_dil_glob_s *bc_dil_glob_ptr;
static unsigned failures;
static std::vector<std::vector<uint8_t> > packets;
static bool fail_cache_allocation;
// Explicit CPU-only fault injection. Off by default for all legacy framing
// cases; no ioctl/worker/device is reachable. Attempts are not acceptance.
static struct {
    DTS_LIB_CONTEXT *context = nullptr;
    bool consume_after_push = false;
    bool allow_metadata_retry = false;
    unsigned waits = 0, metadata_retries = 0, cancel_after_push = 0;
    unsigned pause_after_push = 0, recovery_after_push = 0;
    unsigned failure_attempt = 0;
    BC_STATUS first_marker_status = BC_STS_SUCCESS;
    BC_STATUS failure_status = BC_STS_SUCCESS;
    std::vector<uint32_t> attempts;
    std::vector<std::vector<uint8_t> > accepted;
    std::vector<uint8_t> consumed;
} transport;
static void check(bool value, const char *message)
{
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
extern "C" BC_STATUS __real_txBufPush(pTXBUFFER, uint8_t *, uint32_t);
extern "C" int __real_posix_memalign(void **, size_t, size_t);
extern "C" int __wrap_posix_memalign(void **memory, size_t alignment, size_t size)
{
    if (fail_cache_allocation) {
        fail_cache_allocation = false;
        return ENOMEM;
    }
    return __real_posix_memalign(memory, alignment, size);
}
static void consume_ring(pTXBUFFER ring)
{
    const uint32_t size = ring->busySize;
    if (!size) return;
    std::vector<uint8_t> output(size);
    if (txBufPop(ring, output.data(), size) != BC_STS_SUCCESS) std::abort();
    transport.consumed.insert(transport.consumed.end(), output.begin(), output.end());
}
extern "C" BC_STATUS __wrap_txBufPush(pTXBUFFER ring, uint8_t *bytes, uint32_t size)
{
    if (transport.context) {
        if (ring != &transport.context->circBuf) std::abort();
        transport.attempts.push_back(size);
        if (transport.attempts.size() == 1 && transport.first_marker_status != BC_STS_SUCCESS)
            return transport.first_marker_status; // Rejected marker: NO ring push.
        if (transport.attempts.size() == transport.failure_attempt &&
            transport.failure_status != BC_STS_SUCCESS)
            return transport.failure_status;
        const BC_STATUS status = __real_txBufPush(ring, bytes, size);
        if (status == BC_STS_SUCCESS) {
            transport.accepted.emplace_back(bytes, bytes + size);
            if (transport.consume_after_push) consume_ring(ring);
            if (transport.cancel_after_push == transport.accepted.size())
                transport.context->State = BC_DEC_STATE_STOP;
            if (transport.pause_after_push == transport.accepted.size())
                transport.context->State = BC_DEC_STATE_PAUSE;
            if (transport.recovery_after_push == transport.accepted.size())
                transport.context->recoveryPending = true;
        }
        return status;
    }
    packets.emplace_back(bytes, bytes + size);
    return __real_txBufPush(ring, bytes, size);
}
extern "C" int __wrap_ioctl(int, unsigned long, ...) { std::abort(); }
extern "C" int __wrap_usleep(useconds_t duration)
{
    if (!transport.context) std::abort();
    if (duration == 2000 && transport.allow_metadata_retry) {
        ++transport.metadata_retries; return 0;
    }
    if (duration != 5000) std::abort();
    ++transport.waits;
    transport.context->State = BC_DEC_STATE_STOP;
    // A formerly impossible >ring enqueue exits via the real admission check,
    // not a timeout, actual sleep or fabricated transport success.
    return 0;
}
// This internal implementation helper has C++ linkage (not declared in the
// public C header); exercise its actual production symbol directly.
BC_STATUS DtsAlignSendData(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL);
BC_STATUS DtsFWDecFlushChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWPauseVideo(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWCloseChannel(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWStopVideo(HANDLE, uint32_t, bool) { std::abort(); }
BC_STATUS DtsSetCoreClock(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsPushFwToFlea(HANDLE, char *) { std::abort(); }
BC_STATUS DtsPushAuthFwToLink(HANDLE, char *) { std::abort(); }
BC_STATUS DtsFWInitialize(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsFWActivateDecoder(HANDLE) { std::abort(); }
BC_STATUS DtsFWStartVideo(HANDLE, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) { std::abort(); }
BC_STATUS DtsFWOpenChannel(HANDLE, uint32_t, uint32_t) { std::abort(); }
BC_STATUS DtsFWSetVideoInput(HANDLE) { std::abort(); }
BC_STATUS DtsSetProgressive(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetVideoClock(HANDLE, uint32_t) { std::abort(); }
BC_STATUS DtsSetTSMode(HANDLE, uint32_t) { std::abort(); }
void DumpInputSampleToFile(uint8_t *, uint32_t) { std::abort(); }

using Bytes = std::vector<uint8_t>;
// Synthetic NAL payloads exercise framing only, not decoder conformance.
static const Bytes sps = {0x67, 0x64, 0x00, 0x1f};
static const Bytes pps = {0x68, 0xee, 0x3c};
static const Bytes idr = {0x65, 0x88, 0x84, 0x11, 0x22, 0x33, 0x44, 0x55};
static void append(Bytes &bytes, const Bytes &nal, unsigned length_size)
{
    if (!length_size) bytes.insert(bytes.end(), {0, 0, 0, 1});
    else for (unsigned i = length_size; i; --i)
        bytes.push_back(static_cast<uint8_t>(nal.size() >> ((i - 1) * 8)));
    bytes.insert(bytes.end(), nal.begin(), nal.end());
}
static Bytes access_unit(unsigned length_size, bool headers)
{
    Bytes bytes;
    if (headers) { append(bytes, sps, length_size); append(bytes, pps, length_size); }
    append(bytes, idr, length_size);
    return bytes;
}

struct Fixture {
    bc_dil_glob_s globals = {};
    DTS_LIB_CONTEXT context = {};
    DTS_INPUT_MDATA metadata_pool[8] = {};
    Fixture(unsigned length_size, const Bytes *wmv_metadata = nullptr,
            BC_MEDIA_SUBTYPE subtype = BC_MSUBTYPE_AVC1,
            bool include_metadata = true)
    {
        bc_dil_glob_ptr = &globals;
        context.Sig = LIB_CTX_SIG;
        context.ProcessID = getpid();
        context.DevId = BC_PCI_DEVID_FLEA;
        context.State = BC_DEC_STATE_START;
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        if (pthread_mutex_init(&context.thLock, &attr) != 0)
            std::abort();
        pthread_mutexattr_destroy(&attr);
        context.MdataPoolPtr = metadata_pool;
        context.MDPendHead = context.MDPendTail =
            DTS_MDATA_PEND_LINK((&context));
        for (DTS_INPUT_MDATA &entry : metadata_pool) {
            entry.flink = context.MDFreeHead;
            context.MDFreeHead = &entry;
        }
        context.alignBuf = static_cast<uint8_t *>(std::malloc(ALIGN_BUF_SIZE));
        if (!context.alignBuf || txBufInit(&context.circBuf, CIRC_TX_BUF_SIZE) != BC_STS_SUCCESS)
            std::abort();
        Bytes metadata;
        if (wmv_metadata) metadata = *wmv_metadata;
        else if (include_metadata) {
            append(metadata, sps, 0); append(metadata, pps, 0);
        }
        BC_INPUT_FORMAT format = {};
        format.mSubtype = wmv_metadata ? BC_MSUBTYPE_WMV3 : subtype;
        format.width = 640; format.height = 360; format.Progressive = true;
        format.startCodeSz = length_size;
        format.OptFlags = 0x80000001; // Legacy client flags, without the single-thread flag.
        format.pMetaData = metadata.data(); format.metaDataSz = metadata.size();
        check(DtsSetInputFormat(&context, &format) == BC_STS_SUCCESS, "configure actual input converter");
        if ((include_metadata || wmv_metadata) &&
            (wmv_metadata || subtype == BC_MSUBTYPE_AVC1))
            check(context.PESConvParams.m_bAddSpsPps &&
                  context.PESConvParams.m_bIsAdd_SCode_CodeIn &&
                  !context.SingleThreadedAppMode &&
                  context.VidParams.StreamType == BC_STREAM_TYPE_PES,
                  "fixture exercises pending metadata before conversion in PES mode");
        else if (!include_metadata)
            check(!context.PESConvParams.m_pSpsPpsBuf &&
                  context.PESConvParams.m_iSpsPpsLen == 0,
                  "metadata-empty H.264 starts without a recovery cache");
        packets.clear();
    }
    ~Fixture()
    {
        DtsReleasePESConverter(&context);
        std::free(context.VidParams.pMetaData);
        std::free(context.alignBuf);
        txBufFree(&context.circBuf);
        pthread_mutex_destroy(&context.thLock);
        bc_dil_glob_ptr = nullptr;
    }
    BC_STATUS send(Bytes input)
    {
        const uint32_t size = input.size();
        input.resize(input.size() + 16, 0); // Defensive padding for the legacy converter.
        return DtsProcInput(&context, input.data(), size, 666671, false);
    }
};

static uint32_t recovery_replay_bytes(Fixture &fixture)
{
    uint64_t generation = 0;
    BC_STATUS recovery_status = BC_STS_ERROR;
    uint8_t active = 0;
    uint32_t bytes = UINT32_MAX;
    check(DtsGetRecoveryStatus(&fixture.context, &generation,
                               &recovery_status, &active, &bytes) ==
              BC_STS_SUCCESS && recovery_status == BC_STS_SUCCESS && !active,
          "atomic recovery snapshot succeeds for an idle input fixture");
    return bytes;
}

static Bytes parameter_sets(const Bytes &one_sps = sps,
                            const Bytes &one_pps = pps)
{
    Bytes bytes;
    append(bytes, one_sps, 0);
    append(bytes, one_pps, 0);
    return bytes;
}

static uint64_t pts(const Bytes &packet)
{
    if (packet.size() < 14 || !(packet[7] & 0x80)) return UINT64_MAX;
    const uint8_t *p = packet.data() + 9;
    return (static_cast<uint64_t>((p[0] >> 1) & 7) << 30) |
        (static_cast<uint64_t>(p[1]) << 22) | (static_cast<uint64_t>(p[2] >> 1) << 15) |
        (static_cast<uint64_t>(p[3]) << 7) | (p[4] >> 1);
}
static Bytes payload(const Bytes &packet)
{
    if (packet.size() < 9 || static_cast<size_t>(9 + packet[8]) > packet.size()) return {};
    return Bytes(packet.begin() + 9 + packet[8], packet.end());
}

static void Framing()
{
    for (unsigned length : {1U, 2U, 4U}) {
        Fixture f(length);
        const Bytes input = access_unit(length, true);
        check(DtsCheckSpsPps(&f.context, const_cast<uint8_t *>(input.data()), input.size()),
              "length-prefixed in-band SPS is recognized before conversion");
        check(f.send(input) == BC_STS_SUCCESS, "AVC1 access unit is accepted");
        check(packets.size() == 1, "in-band AVC1 parameter sets suppress redundant untimestamped metadata PES");
        if (!packets.empty()) {
            check(pts(packets.front()) == 66, "first actual PES retains caller's scaled positive timestamp");
            check(payload(packets.front()) == access_unit(0, true), "one PES carries exact converted AU, without duplicate headers");
        }
        check(!f.context.PESConvParams.m_bAddSpsPps, "first-input metadata handling completes normally");
    }
    {
        Fixture f(4);
        check(f.send(access_unit(4, false)) == BC_STS_SUCCESS, "AVC1 without in-band SPS remains accepted");
        check(packets.size() == 2, "missing in-band SPS retains separate stored metadata injection");
        if (packets.size() == 2) {
            Bytes metadata; append(metadata, sps, 0); append(metadata, pps, 0);
            check(pts(packets[0]) == UINT64_MAX && payload(packets[0]) == metadata,
                  "separate metadata retains original untimestamped bytes");
            check(pts(packets[1]) == 66 && payload(packets[1]) == access_unit(0, false),
                  "picture retains timestamp and exact payload after necessary metadata");
        }
    }
    {
        Fixture f(4);
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS, "already Annex-B AVC1 compatibility remains accepted");
        check(packets.size() == 1 && pts(packets.front()) == 66 &&
              payload(packets.front()) == access_unit(0, true), "Annex-B input keeps one timestamped AU");
        check(!f.context.PESConvParams.m_bIsAdd_SCode_CodeIn, "existing Annex-B converter autodetection remains intact");
    }
}

static void RecoverySingleThreadConfig()
{
	{
		Fixture f(2, nullptr, BC_MSUBTYPE_H264, false);
		check(f.send(access_unit(2, true)) == BC_STS_SUCCESS &&
		      f.context.PESConvParams.m_iSpsPpsLen == parameter_sets().size() &&
		      !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,
		                   parameter_sets().data(), parameter_sets().size()),
		      "accepted length-prefixed H.264 caches its original complete AU");
	}
	{
		static const Bytes newer_sps = {0x67, 0x42, 0x80, 0x2a, 0x01};
		static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
		for (const bool update_sps : {false, true}) {
			Fixture f(0, nullptr, BC_MSUBTYPE_H264);
			Bytes changed;
			append(changed, update_sps ? newer_sps : newer_pps, 0);
			append(changed, idr, 0);
			check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 2 &&
			      payload(packets[0]) == parameter_sets() &&
			      payload(packets[1]) == changed,
			      "ordinary parameter-set delta receives its preceding complete format state");
			check(!f.context.PESConvParams.m_pSpsPpsBuf &&
			      recovery_replay_bytes(f) == 0,
			      "accepted ordinary partial update invalidates obsolete format metadata");
		}
	}
	{
		static const Bytes newer_sps = {0x67, 0x42, 0x80, 0x2a, 0x01};
		static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
		Fixture f(0, nullptr, BC_MSUBTYPE_H264);
		Bytes changed = parameter_sets(newer_sps, newer_pps);
		append(changed, idr, 0);
		check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 1 &&
		      payload(packets[0]) == changed,
		      "ordinary complete update suppresses obsolete format metadata");
		const Bytes expected = parameter_sets(newer_sps, newer_pps);
		check(f.context.PESConvParams.m_iSpsPpsLen == expected.size() &&
		      !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,
		                   expected.data(), expected.size()),
		      "ordinary complete update replaces the persistent format cache");
	}
    {
        Fixture f(0, nullptr, BC_MSUBTYPE_H264);
        f.context.SingleThreadedAppMode = 1;
        f.context.PESConvParams.m_bAddSpsPps = false;
        check(recovery_replay_bytes(f) == parameter_sets().size(),
              "atomic snapshot reports the complete H.264 cache before recovery");
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
              f.context.recoveryInjectSpsPps,
              "recovery arms cached AVC configuration for single-thread mode");
        check(recovery_replay_bytes(f) == parameter_sets().size(),
              "atomic snapshot reports exact armed H.264 replay bytes");
        packets.clear();
        check(f.send(access_unit(0, false)) == BC_STS_SUCCESS,
              "single-thread post-recovery IDR-only AU is accepted");
        check(packets.size() == 2,
              "recovery injects cached AVC configuration before IDR-only AU");
        if (packets.size() == 2) {
            Bytes metadata; append(metadata, sps, 0); append(metadata, pps, 0);
            check(pts(packets[0]) == UINT64_MAX && payload(packets[0]) == metadata &&
                  pts(packets[1]) == 66 &&
                  payload(packets[1]) == access_unit(0, false),
                  "recovery sends exact cached SPS/PPS before timestamped AU");
        }
        check(!f.context.recoveryInjectSpsPps,
              "successful cached configuration enqueue consumes recovery latch once");
        check(recovery_replay_bytes(f) == parameter_sets().size(),
              "successful one-shot replay preserves the reported cache capability");
    }
    {
        Fixture f(0, nullptr, BC_MSUBTYPE_H264);
        f.context.SingleThreadedAppMode = 1;
        f.context.PESConvParams.m_bAddSpsPps = false;
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
              "second recovery state reset succeeds");
        packets.clear();
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS &&
              packets.size() == 1 &&
              payload(packets[0]) == access_unit(0, true),
              "in-band SPS/PPS suppresses duplicate recovery injection");
        check(!f.context.recoveryInjectSpsPps,
              "successful in-band configuration consumes recovery latch");
    }
    {
        Fixture f(0, nullptr, BC_MSUBTYPE_H264);
        f.context.SingleThreadedAppMode = 1;
        f.context.PESConvParams.m_bAddSpsPps = false;
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
              "fault-injected recovery state reset succeeds");
        transport.context = &f.context;
        transport.consume_after_push = false;
        transport.allow_metadata_retry = false;
        transport.first_marker_status = BC_STS_IO_ERROR;
        transport.attempts.clear();
        transport.accepted.clear();
        Bytes input = access_unit(0, false);
        const BC_STATUS failed_injection = f.send(input);
        check(failed_injection == BC_STS_IO_ERROR &&
              transport.attempts.size() == 1 &&
              transport.accepted.empty(),
              "failed cached configuration enqueue stops before picture AU");
        check(f.context.recoveryInjectSpsPps &&
              f.context.PESConvParams.m_bAddSpsPps,
              "failed cached configuration enqueue retains recovery latch");
        check(recovery_replay_bytes(f) == parameter_sets().size(),
              "failed injection keeps exact replay reservation armed");
        transport.context = nullptr;
        transport.first_marker_status = BC_STS_SUCCESS;
    }
    {
        Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
        f.context.SingleThreadedAppMode = 1;
        Bytes padded_au = {0};
        const Bytes complete_au = access_unit(0, true);
        padded_au.insert(padded_au.end(), complete_au.begin(), complete_au.end());
        check(f.send(padded_au) == BC_STS_SUCCESS &&
              f.context.PESConvParams.m_iSpsPpsLen == parameter_sets().size() &&
              !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,
                           parameter_sets().data(), parameter_sets().size()),
              "metadata-empty byte-stream accepts leading_zero_8bits and caches the first complete in-band SPS/PPS pair");
        check(recovery_replay_bytes(f) == parameter_sets().size(),
              "ordinary in-band headers publish the persistent cache capability");
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
              recovery_replay_bytes(f) == parameter_sets().size(),
              "recovery arms the exact canonical in-band cache size");
        packets.clear();
        check(f.send(access_unit(0, false)) == BC_STS_SUCCESS &&
              packets.size() == 2 && payload(packets[0]) == parameter_sets() &&
              payload(packets[1]) == access_unit(0, false),
              "metadata-empty H.264 replays cached in-band headers before an IDR-only AU");
        check(!f.context.recoveryInjectSpsPps &&
              recovery_replay_bytes(f) == parameter_sets().size(),
              "one-shot in-band replay is consumed while cache availability remains visible");
    }
    {
        for (const bool repeat_sps : {false, true}) {
            Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
            f.context.SingleThreadedAppMode = 1;
            check(f.send(access_unit(0, true)) == BC_STS_SUCCESS,
                  "partial-repeat fixture first builds a complete cache");
            check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
                  "partial-repeat fixture arms cached replay");
            Bytes partial;
            append(partial, repeat_sps ? sps : pps, 0);
            append(partial, idr, 0);
            packets.clear();
            check(f.send(partial) == BC_STS_SUCCESS && packets.size() == 2 &&
                  payload(packets[0]) == parameter_sets() &&
                  payload(packets[1]) == partial,
                  "partial SPS/PPS repeat cannot suppress complete recovery replay");
            check(recovery_replay_bytes(f) == parameter_sets().size(),
                  "identical partial SPS/PPS repeat preserves complete recovery cache");
        }
    }
    {
        static const Bytes newer_sps = {0x67, 0x42, 0x80, 0x2a, 0x01};
        static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
        for (const bool update_sps : {false, true}) {
            for (const bool fail_allocation : {false, true}) {
                Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
                f.context.SingleThreadedAppMode = 1;
                check(f.send(access_unit(0, true)) == BC_STS_SUCCESS &&
                      DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
                      "post-recovery partial-update fixture arms a complete cache");
                Bytes changed;
                append(changed, update_sps ? newer_sps : newer_pps, 0);
                append(changed, idr, 0);
                packets.clear();
                fail_cache_allocation = fail_allocation;
                check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 2 &&
                      payload(packets[0]) == parameter_sets() &&
                      payload(packets[1]) == changed,
                      "post-recovery partial update receives complete old state before its delta");
                check(!f.context.recoveryInjectSpsPps &&
                      !f.context.PESConvParams.m_pSpsPpsBuf &&
                      recovery_replay_bytes(f) == 0,
                      "post-recovery partial update cannot advertise stale bytes afterward");
            }
        }
    }
    {
        static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
        Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
        f.context.SingleThreadedAppMode = 1;
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS &&
              DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
              "failed partial replay fixture arms a complete cache");
        Bytes changed;
        append(changed, newer_pps, 0);
        append(changed, idr, 0);
        transport.context = &f.context;
        transport.first_marker_status = BC_STS_IO_ERROR;
        transport.attempts.clear();
        transport.accepted.clear();
        check(f.send(changed) == BC_STS_IO_ERROR &&
              transport.attempts.size() == 1 && transport.accepted.empty(),
              "failed old-cache enqueue stops before the changed partial AU");
        check(f.context.recoveryInjectSpsPps &&
              f.context.PESConvParams.m_bAddSpsPps &&
              recovery_replay_bytes(f) == parameter_sets().size(),
              "failed old-cache enqueue preserves complete recovery state");
        transport.context = nullptr;
        transport.first_marker_status = BC_STS_SUCCESS;
        packets.clear();
        check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 2 &&
              payload(packets[0]) == parameter_sets() &&
              payload(packets[1]) == changed,
              "partial retry preserves old complete configuration before its delta");
        check(!f.context.recoveryInjectSpsPps &&
              !f.context.PESConvParams.m_bAddSpsPps &&
              recovery_replay_bytes(f) == 0,
              "successful partial retry consumes the latch and invalidates old cache");
    }
    {
        static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
        Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
        f.context.SingleThreadedAppMode = 1;
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS &&
              DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
              "second-enqueue failure fixture arms a complete cache");
        Bytes changed;
        append(changed, newer_pps, 0);
        append(changed, idr, 0);
        transport.context = &f.context;
        transport.first_marker_status = BC_STS_SUCCESS;
        transport.failure_attempt = 2;
        transport.failure_status = BC_STS_IO_ERROR;
        transport.attempts.clear();
        transport.accepted.clear();
        check(f.send(changed) == BC_STS_IO_ERROR &&
              transport.attempts.size() == 2 &&
              transport.accepted.size() == 1,
              "changed partial failure occurs after old recovery state was queued");
        check(!f.context.recoveryInjectSpsPps &&
              !f.context.PESConvParams.m_bAddSpsPps &&
              recovery_replay_bytes(f) == parameter_sets().size(),
              "failed changed AU retains the old cache for a later recovery");
        transport.context = nullptr;
        transport.failure_attempt = 0;
        transport.failure_status = BC_STS_SUCCESS;
        check(DtsRecoveryFlushInputRing(&f.context) == BC_STS_SUCCESS &&
              DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
              f.context.recoveryInjectSpsPps &&
              f.context.PESConvParams.m_bAddSpsPps,
              "ring loss after the failed AU re-arms the retained complete cache");
        packets.clear();
        check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 2 &&
              payload(packets[0]) == parameter_sets() &&
              payload(packets[1]) == changed,
              "recovered changed partial retry preserves complete-before-delta order");
        check(recovery_replay_bytes(f) == 0,
              "accepted changed partial invalidates the old cache exactly once");
    }
    {
        static const Bytes newer_sps = {0x67, 0x42, 0x80, 0x2a, 0x01};
        static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
        Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
        f.context.SingleThreadedAppMode = 1;
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS &&
              DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
              "failed complete replay fixture arms a complete cache");
        Bytes changed = parameter_sets(newer_sps, newer_pps);
        append(changed, idr, 0);
        transport.context = &f.context;
        transport.first_marker_status = BC_STS_IO_ERROR;
        transport.attempts.clear();
        transport.accepted.clear();
        check(f.send(changed) == BC_STS_IO_ERROR &&
              transport.attempts.size() == 1 && transport.accepted.empty(),
              "failed in-band complete AU reports its enqueue error");
        check(f.context.recoveryInjectSpsPps &&
              f.context.PESConvParams.m_bAddSpsPps,
              "failed complete AU retains both recovery retry latches");
        transport.context = nullptr;
        transport.first_marker_status = BC_STS_SUCCESS;
        packets.clear();
        check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 1 &&
              payload(packets[0]) == changed,
              "complete AU retry suppresses obsolete recovery metadata");
        const Bytes expected = parameter_sets(newer_sps, newer_pps);
        check(!f.context.recoveryInjectSpsPps &&
              !f.context.PESConvParams.m_bAddSpsPps &&
              f.context.PESConvParams.m_iSpsPpsLen == expected.size() &&
              !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,
                           expected.data(), expected.size()),
              "successful complete AU retry consumes latches and keeps its cache");
    }
    {
        static const Bytes newer_sps = {0x67, 0x42, 0x80, 0x2a, 0x01};
        static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
        for (const bool update_sps : {false, true}) {
            Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
            f.context.SingleThreadedAppMode = 1;
            check(f.send(access_unit(0, true)) == BC_STS_SUCCESS,
                  "partial-update fixture first builds a complete cache");
            Bytes changed;
            append(changed, update_sps ? newer_sps : newer_pps, 0);
            append(changed, idr, 0);
            packets.clear();
            check(f.send(changed) == BC_STS_SUCCESS && packets.size() == 1,
                  "a changed partial parameter-set AU remains accepted without stale injection");
            check(!f.context.PESConvParams.m_pSpsPpsBuf &&
                  recovery_replay_bytes(f) == 0,
                  "changed SPS-only and PPS-only updates invalidate obsolete recovery bytes");
            check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
                  !f.context.recoveryInjectSpsPps,
                  "recovery cannot arm an invalidated partial-update cache");
            packets.clear();
            check(f.send(access_unit(0, false)) == BC_STS_SUCCESS &&
                  packets.size() == 1,
                  "a direct client never receives stale headers after partial-update recovery");
        }

        Fixture allocation(0, nullptr, BC_MSUBTYPE_H264, false);
        allocation.context.SingleThreadedAppMode = 1;
        check(allocation.send(access_unit(0, true)) == BC_STS_SUCCESS,
              "allocation-failure fixture first builds a complete cache");
        Bytes changed_complete = parameter_sets(newer_sps, newer_pps);
        append(changed_complete, idr, 0);
        fail_cache_allocation = true;
        check(allocation.send(changed_complete) == BC_STS_SUCCESS &&
              recovery_replay_bytes(allocation) == 0,
              "a failed changed-header allocation cannot advertise the obsolete cache");
    }
    {
        static const Bytes newer_sps = {0x67, 0x42, 0x80, 0x2a, 0x01};
        static const Bytes newer_pps = {0x68, 0xce, 0x06, 0xe2};
        Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
        f.context.SingleThreadedAppMode = 1;
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS,
              "replacement fixture caches its first parameter sets");
        Bytes newer_au = parameter_sets(newer_sps, newer_pps);
        append(newer_au, idr, 0);
        check(f.send(newer_au) == BC_STS_SUCCESS,
              "a later AU with a complete changed pair is accepted");
        const Bytes newer_headers = parameter_sets(newer_sps, newer_pps);
        check(f.context.PESConvParams.m_iSpsPpsLen == newer_headers.size() &&
              !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,
                           newer_headers.data(), newer_headers.size()),
              "later complete in-band SPS/PPS transactionally replaces the cache");
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
              recovery_replay_bytes(f) == newer_headers.size(),
              "changed cache publishes its exact new recovery reservation");
        packets.clear();
        check(f.send(access_unit(0, false)) == BC_STS_SUCCESS &&
              packets.size() == 2 && payload(packets[0]) == newer_headers,
              "recovery replays the latest complete SPS/PPS pair");
    }
    {
        Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
        BC_INPUT_FORMAT format = {};
        Bytes junk = {0x12, 0x34, 0x56, 0x78};
        format.mSubtype = BC_MSUBTYPE_H264;
        format.width = 640;
        format.height = 360;
        format.Progressive = true;
        format.OptFlags = 0x80000001;
        format.pMetaData = junk.data();
        format.metaDataSz = junk.size();
        check(DtsSetInputFormat(&f.context, &format) == BC_STS_SUCCESS &&
              !f.context.PESConvParams.m_pSpsPpsBuf &&
              f.context.PESConvParams.m_iSpsPpsLen == 0,
              "arbitrary H.264 metadata remains legacy-compatible and is not cached");
        Bytes only_sps;
        append(only_sps, sps, 0);
        format.pMetaData = only_sps.data();
        format.metaDataSz = only_sps.size();
        check(DtsSetInputFormat(&f.context, &format) == BC_STS_SUCCESS &&
              !f.context.PESConvParams.m_pSpsPpsBuf &&
              f.context.PESConvParams.m_iSpsPpsLen == 0,
              "partial H.264 metadata stays accepted but cannot prove a replay cache");
        f.context.SingleThreadedAppMode = 1;
        check(f.send(access_unit(0, true)) == BC_STS_SUCCESS &&
              f.context.PESConvParams.m_iSpsPpsLen == parameter_sets().size(),
              "later complete in-band headers fill a partial-metadata cache");
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
              f.context.recoveryInjectSpsPps,
              "complete in-band cache arms before an in-place format change");
        format.pMetaData = junk.data();
        format.metaDataSz = junk.size();
        check(DtsSetInputFormat(&f.context, &format) == BC_STS_SUCCESS &&
              !f.context.recoveryInjectSpsPps &&
              recovery_replay_bytes(f) == 0,
              "successful format replacement clears the prior converter replay latch");
    }
}

static void NestedInputAcrossLifecycleOwner()
{
    Fixture f(0, nullptr, BC_MSUBTYPE_H264, false);
    std::mutex mutex;
    std::condition_variable condition;
    bool owned = false, release = false;
    BC_STATUS owner_status = BC_STS_ERROR;
    check(DtsBeginInputCall(&f.context) == BC_STS_SUCCESS,
          "outer input call owns its reentrant admission before lifecycle overlap");
    std::thread owner([&] {
        owner_status = DtsAcquireControl(&f.context, DTS_CONTROL_LIFECYCLE);
        {
            std::lock_guard<std::mutex> lock(mutex);
            owned = true;
            condition.notify_all();
        }
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return release; });
        if (owner_status == BC_STS_SUCCESS)
            DtsReleaseControl(&f.context);
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        condition.wait(lock, [&] { return owned; });
    }
    const Bytes bytes = {0x41, 0x42, 0x43, 0x44};
    packets.clear();
    check(owner_status == BC_STS_SUCCESS &&
          DtsAlignSendData(&f.context, const_cast<uint8_t *>(bytes.data()),
                           bytes.size(), 0, false) == BC_STS_SUCCESS &&
          packets.size() == 1 && payload(packets[0]) == bytes,
          "same-thread nested input completes exactly once across a foreign ordinary lifecycle lease");
    f.context.txQuiescing = true;
    const size_t accepted = packets.size();
    check(DtsAlignSendData(&f.context, const_cast<uint8_t *>(bytes.data()),
                           bytes.size(), 0, false) == BC_STS_IO_USER_ABORT &&
          packets.size() == accepted,
          "destructive TX admission barrier still aborts nested input without a partial retry");
    f.context.txQuiescing = false;
    DtsFinishInputCall(&f.context);
    {
        std::lock_guard<std::mutex> lock(mutex);
        release = true;
        condition.notify_all();
    }
    owner.join();
    check(f.context.inputCalls == 0 && f.context.controlOwner == DTS_CONTROL_NONE,
          "nested input and foreign lifecycle owners each retire exactly once");
}

static void DetectorBounds()
{
    Fixture f(4);
    for (const Bytes &input : {Bytes{}, Bytes{0}, Bytes{0, 0}, Bytes{0, 0, 0},
            Bytes{0, 0, 0, 0}, Bytes{0xff, 0xff, 0xff, 0xff, 0x67},
            Bytes{0, 0, 0, 4, 0x67, 0x11, 0x22}, Bytes{0, 0, 0, 1},
            Bytes{0, 0, 1}, access_unit(4, false), access_unit(0, false)}) {
        // Exact-sized vectors: sanitizers detect any detector overread.
        check(!DtsCheckSpsPps(&f.context, const_cast<uint8_t *>(input.data()), input.size()),
              "missing/truncated/oversized SPS framing does not claim headers are present");
    }
    check(!DtsCheckSpsPps(&f.context, nullptr, 17), "null input is not dereferenced");
    Bytes input = access_unit(4, true);
    const Bytes before = input;
    const PES_CONVERT_PARAMS state = f.context.PESConvParams;
    check(DtsCheckSpsPps(&f.context, input.data(), input.size()), "valid exact-sized AVC1 header buffer is recognized");
    check(input == before && !std::memcmp(&state, &f.context.PESConvParams, sizeof state),
          "SPS detection does not mutate bytes or conversion state");
    f.context.PESConvParams.m_lStartCodeDataSize = 3;
    check(!DtsCheckSpsPps(&f.context, input.data(), input.size()), "continuation bytes are not assumed to start a new length-prefixed NAL");
    f.context.PESConvParams.m_lStartCodeDataSize = 0;
    // The low byte of this valid 4-byte NAL length looks like an SPS type
    // after a three-byte Annex-B prefix; it is actually an ordinary IDR.
    Bytes long_idr(263, 0x55); long_idr[0] = 0x65;
    Bytes length_collision; append(length_collision, long_idr, 4);
    check(!DtsCheckSpsPps(&f.context, length_collision.data(), length_collision.size()),
          "0x00000107 NAL length is not mistaken for an Annex-B SPS marker");
    Bytes preceded; append(preceded, Bytes{0x09, 0xf0}, 4); append(preceded, sps, 4);
    check(DtsCheckSpsPps(&f.context, preceded.data(), preceded.size()),
          "SPS after another bounded length-prefixed NAL is found");
    f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
    Bytes annex_three = {0, 0, 1, 0x67, 0x64, 0, 0x1f};
    check(DtsCheckSpsPps(&f.context, annex_three.data(), annex_three.size()),
          "already-selected Annex-B mode retains three-byte start codes");
    check(!DtsCheckSpsPps(&f.context, input.data(), input.size()),
          "Annex-B mode does not reinterpret a length-prefixed input");
    f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = true;
    for (unsigned length : {0U, 5U, UINT32_MAX}) {
        f.context.VidParams.StartCodeSz = length;
        check(!DtsCheckSpsPps(&f.context, input.data(), input.size()), "invalid configured length width is bounded");
    }
}

static void AnnexBScannerBounds()
{
    Fixture f(4);
    f.context.VidParams.MediaSubType = BC_MSUBTYPE_H264;
    f.context.VidParams.StreamType = BC_STREAM_TYPE_ES;
    f.context.PESConvParams.m_bAddSpsPps = false;
    f.context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
    DTS_INPUT_MDATA metadata[32] = {};
    f.context.MdataPoolPtr = metadata;
    f.context.MDPendHead = f.context.MDPendTail = DTS_MDATA_PEND_LINK((&f.context));
    f.context.MDFreeHead = nullptr;
    for (DTS_INPUT_MDATA &entry : metadata) {
        entry.flink = f.context.MDFreeHead;
        f.context.MDFreeHead = &entry;
    }
    auto scan = [&](const Bytes &input, bool first, NALU_t &nalu) {
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = first;
        uint8_t *exact = new uint8_t[input.size()];
        if (!input.empty()) std::memcpy(exact, input.data(), input.size());
        const int consumed = DtsGetNaluType(&f.context, exact, input.size(), &nalu, false);
        check(input.empty() || !std::memcmp(exact, input.data(), input.size()),
              "NAL scanning leaves exact-sized caller bytes unchanged");
        delete[] exact;
        return consumed;
    };
    auto send = [&](const Bytes &input) {
        packets.clear();
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = true;
        uint8_t *exact = new uint8_t[input.size()];
        std::memcpy(exact, input.data(), input.size());
        const BC_STATUS status = DtsProcInput(&f.context, exact, input.size(), 666671, false);
        check(!std::memcmp(exact, input.data(), input.size()), "ES transport preserves caller bytes");
        delete[] exact;
        return status;
    };
    auto timestamp_packet = [&](const Bytes &packet) {
        const uint32_t sequence = f.context.InMdataTag & DTS_MDATA_MAX_TAG;
        const Bytes expected = {0, 0, 1, 0xbd, 7, 0x40,
            static_cast<uint8_t>(sequence), static_cast<uint8_t>(sequence >> 8), 0x0a, 0, 0, 0};
        return packet == expected && f.context.MDPendTail != DTS_MDATA_PEND_LINK((&f.context)) &&
            f.context.MDPendTail->appTimeStamp == 66;
    };
    for (const Bytes &input : {Bytes{}, Bytes{0}, Bytes{0, 0}, Bytes{0, 0, 0},
            Bytes{0, 0, 0, 0}, Bytes{1}, Bytes{0, 1, 0x65}, Bytes{0, 0, 1},
            Bytes{0, 0, 0, 1}, Bytes{0, 0, 1, 0, 0, 1, 0x65},
            Bytes{0, 0, 1, 0, 0, 0, 1, 0x65}}) {
        NALU_t nalu = {};
        check(scan(input, true, nalu) < 0, "empty/truncated/adjacent prefixes have no NAL header");
        check(f.context.PESConvParams.m_bIsFirstByteStreamNALU,
              "a rejected NAL does not consume first-byte-stream state");
    }
    NALU_t nalu = {};
    check(DtsGetNaluType(&f.context, nullptr, 1, &nalu, false) < 0 &&
          DtsGetNaluType(&f.context, nullptr, 1, &nalu, true) < 0,
          "both NAL scan modes reject null input");
    uint8_t header = 0x65;
    check(DtsGetNaluType(&f.context, &header, 0, &nalu, true) < 0 &&
          DtsGetNaluType(&f.context, &header, UINT32_MAX, &nalu, false) < 0 &&
          DtsGetNaluType(nullptr, &header, 1, &nalu, true) < 0 &&
          DtsGetNaluType(&f.context, &header, 1, nullptr, true) < 0,
          "NAL scan rejects empty, oversized and invalid arguments before reading");
    check(DtsGetNaluType(&f.context, &header, 1, &nalu, true) == 1 &&
          nalu.Len == 1 && nalu.NalUnitType == NALU_TYPE_IDR,
          "sync-marker-free mode retains its one-byte NAL behavior");
    const Bytes leading = {0, 0, 0, 0, 1, 0x65};
    check(scan(leading, true, nalu) == 6 && nalu.StartcodePrefixLen == 4 && nalu.Len == 1,
          "first NAL permits leading_zero_8bits without changing payload length");
    check(scan(leading, false, nalu) < 0, "subsequent NAL retains its leading-zero restriction");
    const Bytes mixed = {0, 0, 1, 9, 0xf0, 0, 0, 0, 0, 0, 1, 0x65, 0x88};
    check(scan(mixed, true, nalu) == 7 && nalu.StartcodePrefixLen == 3 && nalu.Len == 2 &&
          nalu.NalUnitType == NALU_TYPE_AUD, "mixed prefixes exclude trailing zeros from NAL length");
    const Bytes remainder(mixed.begin() + 7, mixed.end());
    check(scan(remainder, false, nalu) == 6 && nalu.StartcodePrefixLen == 4 && nalu.Len == 2 &&
          nalu.NalUnitType == NALU_TYPE_IDR, "next scan begins at the complete four-byte prefix");

    // Extension payloads below test framing only, not MVC bitstream conformance.
    for (uint8_t type : {uint8_t(9), uint8_t(14), uint8_t(15), uint8_t(20), uint8_t(31)}) {
        const Bytes unknown = {0, 0, 1, type, 0xf0};
        uint32_t offset = 123;
        int nal_type = 123;
        uint8_t *exact = new uint8_t[unknown.size()];
        std::memcpy(exact, unknown.data(), unknown.size());
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = true;
        check(DtsParseAVC(&f.context, exact, unknown.size(), &offset, false, &nal_type) == BC_STS_ERROR &&
              nal_type == -1, "terminal unrecognized NAL has no timestamp boundary");
        delete[] exact;
        const uint32_t previous_tag = f.context.InMdataTag;
        check(send(unknown) == BC_STS_SUCCESS && packets.size() == 1 && packets[0] == unknown &&
              f.context.InMdataTag == previous_tag,
              "terminal unrecognized NAL preserves exact ES bytes without timestamp metadata");
        Bytes picture = unknown;
        append(picture, idr, 0);
        check(send(picture) == BC_STS_SUCCESS && packets.size() == 2 && timestamp_packet(packets[0]) &&
              packets[1] == picture,
              "unknown-to-IDR input keeps the whole ES payload after its timestamp metadata");
        for (const Bytes &parameter : {sps, pps, Bytes{0x06, 0x80}}) {
            Bytes combined = unknown;
            append(combined, parameter, 0);
            const size_t boundary = combined.size();
            append(combined, idr, 0);
            const Bytes prefix(combined.begin(), combined.begin() + boundary);
            const Bytes tail(combined.begin() + boundary, combined.end());
            check(send(combined) == BC_STS_SUCCESS && packets.size() == 3 &&
                  packets[0] == prefix && timestamp_packet(packets[1]) && packets[2] == tail,
                  "parameter-set/SEI cumulative split preserves prefix and timestamped remainder");
        }
    }
    for (uint8_t type : {uint8_t(1), uint8_t(5)}) {
        Bytes picture = {0, 0, 1, 9, 0xf0, 0, 0, 1, type, 0x88};
        uint32_t offset = 0;
        f.context.PESConvParams.m_bIsFirstByteStreamNALU = true;
        check(DtsFindIDR(&f.context, picture.data(), picture.size(), &offset) == BC_STS_SUCCESS &&
              offset == picture.size(), "IDR search preserves legacy slice and IDR acceptance");
    }
    // All short combinations exercise start-code look-behind and terminal bounds.
    const uint8_t alphabet[] = {0, 1, 9, 0x65};
    size_t combinations = 1;
    for (size_t size = 0; size <= 8; ++size, combinations *= 4) {
        for (size_t value = 0; value < combinations; ++value) {
            Bytes input(size);
            size_t digits = value;
            for (uint8_t &byte : input) { byte = alphabet[digits & 3]; digits >>= 2; }
            const int consumed = scan(input, true, nalu);
            check(consumed < 0 || (static_cast<size_t>(consumed) <= size && nalu.Len > 0 &&
                  nalu.Len + nalu.StartcodePrefixLen <= static_cast<uint32_t>(consumed)),
                  "short NAL scan returns only an in-range, nonempty extent");
        }
    }
}

static void WmvBFrameMetadata()
{
    // Canonical four-byte STRUCT_C. MAXBFRAMES is a three-bit count,
    // not a boolean whose only true representation is seven. The synthetic
    // picture bodies exercise framing only, not decoder conformance.
    for (unsigned maximum = 0; maximum < 8; ++maximum) {
        for (unsigned range = 0; range < 2; ++range) {
            for (unsigned interpolation = 0; interpolation < 2; ++interpolation) {
                const Bytes metadata = {0x4b, 0xf1, 0x0a,
                    static_cast<uint8_t>((range << 7) | (maximum << 4) |
                                         (interpolation << 1) | 1)};
                Fixture f(4, &metadata);
                check(f.context.PESConvParams.m_bMaxbFrames == (maximum != 0),
                      "every nonzero MAXBFRAMES count enables B-picture syntax");
                const unsigned first_bit = 7 - (2 + range + interpolation);
                const Bytes intra = {static_cast<uint8_t>(maximum ? 1U << (first_bit - 1) : 0),
                                      0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
                const Bytes predicted = {static_cast<uint8_t>(1U << first_bit),
                                          0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
                const Bytes bidirectional = {0, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb};
                const Bytes sequence = {0, 0, 1, 0x0f, 2, 0x80, 1, 0x68,
                                        metadata[0], metadata[1], metadata[2], metadata[3]};
                auto submit = [&](const Bytes &picture, bool is_intra) {
                    packets.clear();
                    check(f.send(picture) == BC_STS_SUCCESS, "WMV3 input is accepted");
                    check(packets.size() == (is_intra ? 2U : 1U),
                          "only actual WMV3 I pictures inject sequence metadata");
                    if (is_intra && packets.size() == 2)
                        check(payload(packets.front()) == sequence,
                              "I picture retains exact STRUCT_C and visible dimensions");
                    Bytes expected = {0, 0, 1, 0x0d};
                    expected.insert(expected.end(), picture.begin(), picture.end());
                    check(!packets.empty() && pts(packets.back()) == 66 &&
                          payload(packets.back()) == expected,
                          "WMV3 picture bytes and caller timestamp remain unchanged");
                };
                submit(intra, true);
                submit(predicted, false);
                if (maximum) submit(bidirectional, false);
                submit(intra, true);
            }
        }
    }
}

enum class TransportKind { FleaEs, LinkPes, FleaPes };

struct TransportFixture : Fixture {
    DTS_INPUT_MDATA metadata[8] = {};
    explicit TransportFixture(TransportKind kind) :
            Fixture(0, nullptr, BC_MSUBTYPE_H264) {
        if (kind == TransportKind::FleaEs) {
            // Deliberate synthetic ES path; real Flea configuration below is
            // exercised separately and must remain PES.
            context.DevId = BC_PCI_DEVID_FLEA;
            context.VidParams.StreamType = BC_STREAM_TYPE_ES;
        } else if (kind == TransportKind::LinkPes) {
            context.DevId = BC_PCI_DEVID_LINK;
            context.VidParams.StreamType = BC_STREAM_TYPE_PES;
        } else {
            check(context.DevId == BC_PCI_DEVID_FLEA &&
                      context.VidParams.StreamType == BC_STREAM_TYPE_PES,
                  "actual Flea H.264 configuration selects PES transport");
        }
        context.PESConvParams.m_bAddSpsPps = false;
        context.PESConvParams.m_bIsAdd_SCode_CodeIn = false;
        context.MdataPoolPtr = metadata;
        context.MDPendHead = context.MDPendTail = DTS_MDATA_PEND_LINK((&context));
        context.MDFreeHead = nullptr;
        for (DTS_INPUT_MDATA &entry : metadata) {
            entry.flink = context.MDFreeHead; context.MDFreeHead = &entry;
        }
        transport.context = &context;
        transport.consume_after_push = transport.allow_metadata_retry = false;
        transport.waits = transport.metadata_retries = transport.cancel_after_push = 0;
        transport.pause_after_push = transport.recovery_after_push = 0;
        transport.first_marker_status = BC_STS_SUCCESS;
        transport.failure_attempt = 0;
        transport.failure_status = BC_STS_SUCCESS;
        transport.attempts.clear(); transport.accepted.clear(); transport.consumed.clear();
    }
    explicit TransportFixture(bool link_pes = false) :
            TransportFixture(link_pes ? TransportKind::LinkPes :
                                        TransportKind::FleaEs) {}
    ~TransportFixture() {
        transport.context = nullptr;
        DtsClrPendMdataList(&context);
    }
    unsigned free_metadata() const {
        const DTS_INPUT_MDATA *at = context.MDFreeHead;
        unsigned count = 0;
        while (at && count < 9) { ++count; at = at->flink; }
        return count;
    }
    bool no_pending_metadata() {
        return context.MDPendHead == DTS_MDATA_PEND_LINK((&context)) &&
               context.MDPendTail == DTS_MDATA_PEND_LINK((&context));
    }
};

static Bytes transport_bytes(uint32_t size)
{
    Bytes bytes(size);
    for (uint32_t i = 0; i < size; ++i) bytes[i] = static_cast<uint8_t>((i*37U+(i>>8)*13U+91U)%251U);
    return bytes;
}
static Bytes accepted_bytes()
{
    Bytes all;
    for (const Bytes &part : transport.accepted) all.insert(all.end(), part.begin(), part.end());
    return all;
}
static std::vector<uint32_t> expected_chunks(const uint8_t *data, uint32_t size)
{
    std::vector<uint32_t> chunks;
    while (size) {
        uint32_t chunk = size < ALIGN_BUF_SIZE ? size : ALIGN_BUF_SIZE;
        const unsigned odd = reinterpret_cast<uintptr_t>(data) % 4;
        // Preserve the historical unaligned first-copy footprint; the new
        // invariant is that subsequent aligned chunks are also bounded.
        if (odd && size > ALIGN_BUF_SIZE) chunk = ALIGN_BUF_SIZE-odd;
        chunks.push_back(chunk); data += chunk; size -= chunk;
    }
    return chunks;
}

static Bytes accepted_after(size_t first)
{
    Bytes all;
    for (size_t i = first; i < transport.accepted.size(); ++i)
        all.insert(all.end(), transport.accepted[i].begin(), transport.accepted[i].end());
    return all;
}

struct PesCapture {
    bool valid = true;
    Bytes payload;
    std::vector<uint64_t> timestamps;
    uint64_t bytes = 0;
};

static PesCapture captured_pes(size_t first = 0)
{
    PesCapture capture;
    for (size_t i = first; i < transport.accepted.size(); ++i) {
        const Bytes &packet = transport.accepted[i];
        capture.bytes += packet.size();
        if (packet.size() < sizeof(b_pes_header) || packet[0] != 0 ||
            packet[1] != 0 || packet[2] != 1 || packet[3] != 0xe0) {
            capture.valid = false;
            continue;
        }
        const uint32_t packet_length =
            (static_cast<uint32_t>(packet[4]) << 8) | packet[5];
        const size_t payload_offset = sizeof(b_pes_header) + packet[8];
        if (packet.size() != packet_length + 6U ||
            payload_offset > packet.size()) {
            capture.valid = false;
            continue;
        }
        if (packet[7] & 0x80)
            capture.timestamps.push_back(pts(packet));
        capture.payload.insert(capture.payload.end(),
                               packet.begin() + payload_offset, packet.end());
    }
    return capture;
}

static uint64_t expected_pes_bytes(uint32_t payload_bytes,
                                   uint32_t first_optional_bytes)
{
    if (!payload_bytes) return 0;
    const uint64_t first_payload =
        static_cast<uint32_t>(MAX_RE_PES_BOUND) - 3U - first_optional_bytes;
    const uint64_t later_payload =
        static_cast<uint32_t>(MAX_RE_PES_BOUND) - 3U;
    uint64_t packets = 1;
    if (payload_bytes > first_payload) {
        const uint64_t rest = payload_bytes - first_payload;
        packets += (rest + later_payload - 1U) / later_payload;
    }
    return payload_bytes + first_optional_bytes +
           packets * sizeof(b_pes_header);
}

static void FragmentedH264Input()
{
    Bytes logical = parameter_sets();
    append(logical, Bytes(DTS_INPUT_FRAGMENT_MAX_SIZE + 137U, 0x5a), 0);
    // Turn the generated final NAL into an IDR while retaining a deterministic
    // payload whose 512 KiB boundary lies strictly inside that NAL.
    logical[parameter_sets().size() + 4] = 0x65;
    const uint32_t first_size = DTS_INPUT_FRAGMENT_MAX_SIZE;
    const uint32_t last_size = logical.size() - first_size;

    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   666671, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "fragment FIRST accepts one bounded H.264 prefix");
        check(f.context.inputFragmentActive &&
              !f.context.inputFragmentRawPermit,
              "FIRST retains only the logical-AU latch between calls");
        const size_t accepted = transport.accepted.size();
        check(DtsProcInput(&f.context, logical.data(), 8, 666672, false) ==
                  BC_STS_BUSY &&
              DtsAlignSendData(&f.context, logical.data(), 8, 0, false) ==
                  BC_STS_BUSY &&
              DtsSendData(&f.context, logical.data(), 8, 0, false) ==
                  BC_STS_BUSY &&
              DtsSendEOS(&f.context, 0) == BC_STS_BUSY &&
              transport.accepted.size() == accepted,
              "legacy, raw and EOS producers cannot interleave an open fragment");
        BC_STATUS foreign_proc = BC_STS_SUCCESS;
        BC_STATUS foreign_align = BC_STS_SUCCESS;
        BC_STATUS foreign_send = BC_STS_SUCCESS;
        std::thread foreign([&] {
            foreign_proc = DtsProcInput(&f.context, logical.data(), 8,
                                        666672, false);
            foreign_align = DtsAlignSendData(&f.context, logical.data(), 8,
                                             0, false);
            foreign_send = DtsSendData(&f.context, logical.data(), 8, 0,
                                       false);
        });
        foreign.join();
        check(foreign_proc == BC_STS_BUSY && foreign_align == BC_STS_BUSY &&
                  foreign_send == BC_STS_BUSY &&
                  transport.accepted.size() == accepted,
              "legacy and raw producers on another thread cannot interleave an open fragment");
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_SUCCESS &&
              !f.context.inputFragmentActive,
              "LAST completes and releases the logical-AU latch");
        check(!transport.accepted.empty() &&
              transport.accepted.front().size() == sizeof(BC_SEQ_HDR_FORMAT) &&
              accepted_after(1) == logical,
              "mid-NAL split preserves every logical AU byte after one timestamp marker");
        check(f.context.PESConvParams.m_iSpsPpsLen == parameter_sets().size() &&
              !std::memcmp(f.context.PESConvParams.m_pSpsPpsBuf,
                           parameter_sets().data(), parameter_sets().size()),
              "FIRST performs H.264 recovery-cache bookkeeping exactly once");
        check(f.free_metadata() == 7 && !f.no_pending_metadata(),
              "fragmented AU owns exactly one timestamp metadata record");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   666671, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "stall fixture accepts FIRST before capacity disappears");
        transport.consume_after_push = false;
        Bytes full(CIRC_TX_BUF_SIZE, 0xa5);
        check(__real_txBufPush(&f.context.circBuf, full.data(), full.size()) ==
                  BC_STS_SUCCESS,
              "stall fixture fills the real ring after FIRST");
        const size_t attempts = transport.attempts.size();
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_BUSY &&
              f.context.inputFragmentActive && transport.waits == 0 &&
              transport.attempts.size() == attempts,
              "continuation with no capacity returns BUSY without a byte or wait");
        consume_ring(&f.context.circBuf);
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_SUCCESS &&
              !f.context.inputFragmentActive,
              "stalled LAST retries after output-side capacity returns");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_INV_ARG,
              "continuation without FIRST is rejected");
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   666671, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "reset fixture opens one fragmented AU");
        f.context.inputFragmentMdata = DtsAllocMdata(&f.context);
        f.context.inputFragmentRawPermit = true;
        f.context.inputFragmentThread = pthread_self();
        const unsigned free_with_reservation = f.free_metadata();
        check(f.context.inputFragmentMdata != nullptr,
              "reset fixture models a FIRST timestamp owner reserved in flight");
        check(DtsRecoveryFlushInputRing(&f.context) == BC_STS_SUCCESS &&
              !f.context.inputFragmentActive &&
              !f.context.inputFragmentRawPermit &&
              !f.context.inputFragmentMdata &&
              f.free_metadata() == free_with_reservation + 1 &&
              DtsProcInputFragment(&f.context,
                                   logical.data() + first_size, last_size, 0,
                                   false, DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_INV_ARG,
              "recovery reset discards the fragment identity and rejects its tail");
        uint8_t byte = 0;
        check(DtsProcInputFragment(&f.context, &byte,
                                   DTS_INPUT_FRAGMENT_MAX_SIZE + 1U, 1,
                                   false, DTS_INPUT_FRAGMENT_FIRST |
                                                  DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_INV_ARG,
              "one byte above the public fragment bound is rejected before access");
    }
    for (const uint32_t shortage : {1U, 0U}) {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
              f.context.recoveryInjectSpsPps,
              "capacity fixture arms the cached H.264 recovery prefix");
        Bytes idr_only;
        append(idr_only, idr, 0);
        const uint32_t required = idr_only.size() + sizeof(BC_SEQ_HDR_FORMAT) +
                                  parameter_sets().size();
        Bytes fill(CIRC_TX_BUF_SIZE - required + shortage, 0x39);
        check(__real_txBufPush(&f.context.circBuf, fill.data(), fill.size()) ==
                  BC_STS_SUCCESS,
              "capacity fixture establishes an exact recovery-prefix boundary");
        const BC_STATUS status = DtsProcInputFragment(
            &f.context, idr_only.data(), idr_only.size(), 666671, false,
            DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST);
        if (shortage) {
            check(status == BC_STS_BUSY && transport.attempts.empty() &&
                  !f.context.inputFragmentActive && transport.waits == 0,
                  "one byte short reserves neither recovery prefix, marker nor input");
        } else {
            check(status == BC_STS_SUCCESS && transport.waits == 0 &&
                  !f.context.inputFragmentActive &&
                  transport.accepted.size() == 3 &&
                  transport.accepted[0] == parameter_sets() &&
                  transport.accepted[1].size() == sizeof(BC_SEQ_HDR_FORMAT) &&
                  transport.accepted[2] == idr_only,
                  "exact free-space boundary admits prefix, one marker and raw input");
        }
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
                  f.context.recoveryInjectSpsPps,
              "metadata fixture arms the cached H.264 recovery prefix");
        f.context.MDFreeHead = nullptr;
        const uint32_t ring_busy = f.context.circBuf.busySize;
        const uint64_t enqueued = f.context.txBytesEnqueued;
        const uint32_t metadata_tag = f.context.InMdataTag;
        const bool add_config = f.context.PESConvParams.m_bAddSpsPps;
        const bool inject_config = f.context.recoveryInjectSpsPps;
        const uint8_t *const config = f.context.PESConvParams.m_pSpsPpsBuf;
        const uint32_t config_size = f.context.PESConvParams.m_iSpsPpsLen;
        Bytes config_copy(config, config + config_size);
        Bytes idr_only;
        append(idr_only, idr, 0);
        check(DtsProcInputFragment(
                  &f.context, idr_only.data(), idr_only.size(), 666671,
                  false, DTS_INPUT_FRAGMENT_FIRST |
                             DTS_INPUT_FRAGMENT_LAST) == BC_STS_BUSY &&
                  transport.attempts.empty() && transport.waits == 0 &&
                  f.context.circBuf.busySize == ring_busy &&
                  f.context.txBytesEnqueued == enqueued &&
                  f.context.InMdataTag == metadata_tag &&
                  !f.context.inputFragmentActive &&
                  !f.context.inputFragmentMdata &&
                  f.context.PESConvParams.m_bAddSpsPps == add_config &&
                  f.context.recoveryInjectSpsPps == inject_config &&
                  f.context.PESConvParams.m_pSpsPpsBuf == config &&
                  f.context.PESConvParams.m_iSpsPpsLen == config_size &&
                  !std::memcmp(config, config_copy.data(), config_size) &&
                  !f.context.MDFreeHead && f.no_pending_metadata(),
              "metadata exhaustion returns zero-byte BUSY before recovery prefix and preserves all owners/latches");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS,
              "late-cancel fixture arms recovery prefix injection");
        Bytes idr_only;
        append(idr_only, idr, 0);
        transport.failure_attempt = 2;
        transport.failure_status = BC_STS_BUSY;
        check(DtsProcInputFragment(
                  &f.context, idr_only.data(), idr_only.size(), 666671,
                  false, DTS_INPUT_FRAGMENT_FIRST |
                             DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_IO_USER_ABORT &&
                  transport.accepted.size() == 1 &&
                  transport.accepted[0] == parameter_sets() &&
                  f.context.inputFragmentActive &&
                  !f.context.inputFragmentRawPermit &&
                  !f.context.inputFragmentMdata,
              "a late packetizer BUSY after a prefix is terminal cancellation, never zero-byte retryable BUSY");
        check(DtsRecoveryFlushInputRing(&f.context) == BC_STS_SUCCESS &&
                  !f.context.inputFragmentActive,
              "recovery teardown retires the terminal partial fragment latch");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        Bytes idr_only;
        append(idr_only, idr, 0);
        transport.consume_after_push = true;
        transport.recovery_after_push = 1;
        check(DtsProcInputFragment(
                  &f.context, idr_only.data(), idr_only.size(), 0, false,
                  DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_IO_USER_ABORT &&
                  transport.accepted.size() == 1 &&
                  transport.accepted[0] == idr_only &&
                  !f.context.inputFragmentActive && transport.waits == 0,
              "recovery published after the final enqueue is terminal, never a retryable BUSY after accepted bytes");
        f.context.recoveryPending = false;
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   0, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "pause fixture admits FIRST and publishes its fragment latch");
        check(DtsPauseDecoder(&f.context) == BC_STS_BUSY &&
                  f.context.State == BC_DEC_STATE_START &&
                  f.context.inputFragmentActive,
              "pause cannot change decoder state during an admitted fragmented AU");
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_SUCCESS &&
                  !f.context.inputFragmentActive,
              "rejected pause leaves the bounded continuation usable");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        Bytes storage(DTS_INPUT_FRAGMENT_MAX_SIZE + 1U, 0x5a);
        uint8_t *unaligned = storage.data() + 1;
        unaligned[0] = unaligned[1] = unaligned[2] = 0;
        unaligned[3] = 1;
        unaligned[4] = 0x65;
        transport.consume_after_push = true;
        transport.pause_after_push = 1;
        check(DtsProcInputFragment(
                  &f.context, unaligned, DTS_INPUT_FRAGMENT_MAX_SIZE,
                  666671, false,
                  DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_IO_USER_ABORT &&
                  transport.accepted.size() == 1 &&
                  transport.accepted[0].size() == sizeof(BC_SEQ_HDR_FORMAT) &&
                  transport.waits == 0 && f.context.inputFragmentActive,
              "a PAUSE race after admission cancels promptly without entering the legacy pause wait loop");
        f.context.State = BC_DEC_STATE_START;
        check(DtsRecoveryFlushInputRing(&f.context) == BC_STS_SUCCESS &&
                  !f.context.inputFragmentActive,
              "flush retires the terminal latch after a raced pause");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        Bytes first;
        append(first, sps, 0);
        first.insert(first.end(), {0, 0, 0, 1, pps[0], pps[1]});
        Bytes tail = {pps[2]};
        append(tail, idr, 0);
        transport.consume_after_push = true;
        check(f.context.PESConvParams.m_iSpsPpsLen == parameter_sets().size(),
              "truncated-PPS fixture begins with a complete older recovery cache");
        check(DtsProcInputFragment(&f.context, first.data(), first.size(), 0,
                                   false, DTS_INPUT_FRAGMENT_FIRST) ==
                  BC_STS_SUCCESS &&
                  !f.context.PESConvParams.m_pSpsPpsBuf &&
                  f.context.PESConvParams.m_iSpsPpsLen == 0,
              "complete SPS plus a PPS cut inside FIRST invalidates rather than publishing truncated recovery bytes");
        check(DtsProcInputFragment(&f.context, tail.data(), tail.size(), 0,
                                   false, DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_SUCCESS &&
                  !f.context.PESConvParams.m_pSpsPpsBuf &&
                  f.context.PESConvParams.m_iSpsPpsLen == 0,
              "raw PPS continuation cannot retroactively establish recovery state");
        first.insert(first.end(), tail.begin(), tail.end());
        check(accepted_bytes() == first,
              "truncated-PPS fail-closed handling preserves every transport byte");
    }
    {
        TransportFixture f;
        f.context.SingleThreadedAppMode = 1;
        std::free(f.context.PESConvParams.m_pSpsPpsBuf);
        f.context.PESConvParams.m_pSpsPpsBuf = nullptr;
        f.context.PESConvParams.m_iSpsPpsLen = 0;
        f.context.PESConvParams.m_bAddSpsPps = false;
        f.context.recoveryInjectSpsPps = false;
        Bytes first(DTS_INPUT_FRAGMENT_MAX_SIZE, 0x5a);
        first[0] = first[1] = first[2] = 0;
        first[3] = 1;
        first[4] = 0x67;
        Bytes tail(47, 0x33);
        append(tail, pps, 0);
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, first.data(), first.size(), 0,
                                   false, DTS_INPUT_FRAGMENT_FIRST) ==
                  BC_STS_SUCCESS &&
                  !f.context.PESConvParams.m_pSpsPpsBuf &&
                  f.context.PESConvParams.m_iSpsPpsLen == 0,
              "SPS cut at FIRST boundary cannot establish a complete recovery cache");
        check(DtsProcInputFragment(&f.context, tail.data(), tail.size(), 0,
                                   false, DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_SUCCESS &&
                  !f.context.PESConvParams.m_pSpsPpsBuf &&
                  f.context.PESConvParams.m_iSpsPpsLen == 0,
              "raw continuation PPS is not combined with an incomplete FIRST cache");
        Bytes combined = first;
        combined.insert(combined.end(), tail.begin(), tail.end());
        check(accepted_bytes() == combined,
              "cross-boundary configuration test still preserves transport bytes exactly");
    }
}

static void FleaPesFragmentedH264Input()
{
    Bytes logical = parameter_sets();
    append(logical, Bytes(DTS_INPUT_FRAGMENT_MAX_SIZE + 137U, 0x5a), 0);
    logical[parameter_sets().size() + 4] = 0x65;
    const uint32_t first_size = DTS_INPUT_FRAGMENT_MAX_SIZE;
    const uint32_t last_size = logical.size() - first_size;
    const uint64_t first_required = expected_pes_bytes(first_size, 5);
    const uint64_t last_required = expected_pes_bytes(last_size, 0);
    check(first_required == 524374U,
          "512 KiB timestamped Flea fragment has nine exact PES packets");

    {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        const unsigned metadata_before = f.free_metadata();
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   666671, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "actual Flea PES accepts a timestamped 512 KiB FIRST");
        const size_t first_packets = transport.accepted.size();
        const PesCapture first = captured_pes();
        check(first.valid && first_packets == 9 &&
                  first.bytes == first_required &&
                  first.payload == Bytes(logical.begin(),
                                         logical.begin() + first_size) &&
                  first.timestamps.size() == 1 &&
                  first.timestamps[0] == 66,
              "Flea FIRST PES reconstructs exact mid-NAL bytes and one scaled PTS");

        const size_t accepted = transport.accepted.size();
        BC_INPUT_FORMAT replacement = {};
        replacement.mSubtype = BC_MSUBTYPE_H264;
        replacement.width = 640;
        replacement.height = 360;
        replacement.Progressive = true;
        replacement.startCodeSz = 4;
        BC_STATUS foreign_align = BC_STS_SUCCESS;
        std::thread foreign([&] {
            foreign_align = DtsAlignSendData(&f.context, logical.data(), 8,
                                             0, false);
        });
        foreign.join();
        check(DtsProcInput(&f.context, logical.data(), 8, 666672, false) ==
                  BC_STS_BUSY &&
              DtsSendEOS(&f.context, 0) == BC_STS_BUSY &&
              foreign_align == BC_STS_BUSY &&
              DtsPauseDecoder(&f.context) == BC_STS_BUSY &&
              DtsSetupHardware(&f.context, false) == BC_STS_BUSY &&
              DtsSetVideoParams(&f.context, BC_VID_ALGO_H264, false, false,
                                true, 0) == BC_STS_BUSY &&
              DtsSetInputFormat(&f.context, &replacement) == BC_STS_BUSY &&
              transport.accepted.size() == accepted &&
              f.context.VidParams.StreamType == BC_STREAM_TYPE_PES &&
              f.context.VidParams.MediaSubType == BC_MSUBTYPE_H264,
              "Flea PES fragment excludes producers, pause and reconfiguration");

        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_SUCCESS,
              "actual Flea PES accepts the raw mid-NAL continuation");
        const PesCapture whole = captured_pes();
        check(whole.valid && whole.payload == logical &&
                  whole.timestamps.size() == 1 &&
                  whole.timestamps[0] == 66 &&
                  whole.bytes == first_required + last_required &&
                  !f.context.inputFragmentActive,
              "all Flea PES fragments reconstruct one byte-exact logical AU");
        check(f.free_metadata() == metadata_before && f.no_pending_metadata(),
              "Flea inline PTS consumes no SPES metadata owner");
    }

    {
        static const uint32_t observed_size = 1450169U;
        Bytes observed = parameter_sets();
        append(observed,
               Bytes(observed_size - observed.size() - 4U, 0x5a), 0);
        observed[parameter_sets().size() + 4U] = 0x65;
        check(observed.size() == observed_size,
              "native-sized Flea fixture matches the observed 1,450,169-byte AU");
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        transport.consume_after_push = true;
        const uint32_t chunk = DTS_INPUT_FRAGMENT_MAX_SIZE;
        check(DtsProcInputFragment(&f.context, observed.data(), chunk,
                                   666671, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "native-sized Flea AU accepts its bounded FIRST");
        BC_STATUS middle_status = BC_STS_ERROR;
        std::thread continuation([&] {
            middle_status = DtsProcInputFragment(
                &f.context, observed.data() + chunk, chunk, 0, false, 0);
        });
        continuation.join();
        const uint32_t tail_size = observed.size() - 2U * chunk;
        check(middle_status == BC_STS_SUCCESS &&
                  DtsProcInputFragment(&f.context,
                                       observed.data() + 2U * chunk,
                                       tail_size, 0, false,
                                       DTS_INPUT_FRAGMENT_LAST) ==
                      BC_STS_SUCCESS,
              "serialized continuation may change thread and completes the native-sized AU");
        const PesCapture capture = captured_pes();
        const uint64_t expected = expected_pes_bytes(chunk, 5) +
                                  expected_pes_bytes(chunk, 0) +
                                  expected_pes_bytes(tail_size, 0);
        check(capture.valid && capture.payload == observed &&
                  capture.timestamps.size() == 1 &&
                  capture.timestamps[0] == 66 && capture.bytes == expected &&
                  !f.context.inputFragmentActive,
              "three Flea fragments reconstruct the observed large AU with one PTS");
    }

    for (unsigned alignment = 0; alignment < 4; ++alignment) {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        Bytes storage(132U + alignment, 0x3c);
        uint8_t *input = storage.data() + alignment;
        const uint32_t size = storage.size() - alignment;
        input[0] = input[1] = input[2] = 0;
        input[3] = 1;
        input[4] = 0x65;
        transport.consume_after_push = true;
        check(DtsProcInputFragment(
                  &f.context, input, size, 666671, false,
                  DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST) ==
                  BC_STS_SUCCESS,
              "Flea PES accepts every caller-buffer alignment");
        const PesCapture capture = captured_pes();
        check(capture.valid && capture.payload == Bytes(input, input + size) &&
                  capture.timestamps.size() == 1,
              "caller alignment does not change Flea PES payload or PTS");
    }

    {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   666671, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS,
              "Flea capacity fixture admits FIRST before continuation stall");
        transport.accepted.clear();
        transport.attempts.clear();
        transport.consume_after_push = false;
        Bytes fill(CIRC_TX_BUF_SIZE - last_required + 1U, 0x39);
        check(__real_txBufPush(&f.context.circBuf, fill.data(), fill.size()) ==
                  BC_STS_SUCCESS,
              "Flea continuation fixture leaves exactly one PES byte short");
        const uint32_t ring_busy = f.context.circBuf.busySize;
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_BUSY &&
                  transport.attempts.empty() && transport.waits == 0 &&
                  f.context.circBuf.busySize == ring_busy &&
                  f.context.inputFragmentActive,
              "one-byte-short Flea continuation returns zero-byte BUSY");
        consume_ring(&f.context.circBuf);
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data() + first_size,
                                   last_size, 0, false,
                                   DTS_INPUT_FRAGMENT_LAST) == BC_STS_SUCCESS,
              "Flea continuation retries after exact PES capacity returns");
        const PesCapture tail = captured_pes();
        check(tail.valid && tail.payload ==
                  Bytes(logical.begin() + first_size, logical.end()) &&
                  tail.timestamps.empty() && tail.bytes == last_required,
              "Flea continuation adds no timestamp and preserves its payload");
    }

    for (const uint32_t shortage : {1U, 0U}) {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        uint8_t private_data[16] = {};
        uint8_t extension_data[2] = {0x31, 0x32};
        f.context.PESConvParams.m_bPESPrivData = true;
        f.context.PESConvParams.m_pPESPrivData = private_data;
        f.context.PESConvParams.m_bPESExtField = true;
        f.context.PESConvParams.m_nPESExtLen = sizeof(extension_data);
        f.context.PESConvParams.m_pPESExtField = extension_data;
        f.context.PESConvParams.m_bStuffing = true;
        f.context.PESConvParams.m_nStuffingBytes = 3;
        Bytes input(200000, 0x4b);
        input[0] = input[1] = input[2] = 0;
        input[3] = 1;
        input[4] = 0x65;
        const uint32_t optional = 5U + 1U + 16U + 3U + 3U;
        const uint64_t required = expected_pes_bytes(input.size(), optional);
        Bytes fill(CIRC_TX_BUF_SIZE - required + shortage, 0x72);
        check(__real_txBufPush(&f.context.circBuf, fill.data(), fill.size()) ==
                  BC_STS_SUCCESS,
              "Flea optional-header fixture establishes exact ring boundary");
        f.context.MDFreeHead = nullptr;
        const uint32_t ring_busy = f.context.circBuf.busySize;
        const BC_STATUS status = DtsProcInputFragment(
            &f.context, input.data(), input.size(), 666671, false,
            DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST);
        if (shortage) {
            check(status == BC_STS_BUSY && transport.attempts.empty() &&
                      transport.waits == 0 &&
                      f.context.circBuf.busySize == ring_busy &&
                      !f.context.inputFragmentActive,
                  "all PES overhead is reserved before a one-byte-short FIRST");
        } else {
            const PesCapture capture = captured_pes();
            check(status == BC_STS_SUCCESS && capture.valid &&
                      capture.payload == input &&
                      capture.timestamps.size() == 1 &&
                      capture.timestamps[0] == 66 &&
                      capture.bytes == required && transport.waits == 0 &&
                      f.no_pending_metadata(),
                  "exact capacity includes PTS/private/ext/stuffing without SPES metadata");
        }
    }

    for (const uint32_t stuffing : {112U, 113U}) {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        uint8_t extension_data[127] = {};
        f.context.PESConvParams.m_bPESExtField = true;
        f.context.PESConvParams.m_nPESExtLen = sizeof(extension_data);
        f.context.PESConvParams.m_pPESExtField = extension_data;
        f.context.PESConvParams.m_bStuffing = true;
        f.context.PESConvParams.m_nStuffingBytes = stuffing;
        Bytes input = access_unit(0, false);
        transport.consume_after_push = true;
        const BC_STATUS status = DtsProcInputFragment(
            &f.context, input.data(), input.size(), 666671, false,
            DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST);
        if (stuffing == 112U) {
            const PesCapture capture = captured_pes();
            check(status == BC_STS_SUCCESS && capture.valid &&
                      transport.accepted.size() == 1 &&
                      transport.accepted[0][8] == 246U &&
                      capture.payload == input &&
                      capture.timestamps.size() == 1,
                  "largest representable PES optional header is admitted exactly");
        } else {
            check(status == BC_STS_INV_ARG && transport.attempts.empty() &&
                      transport.waits == 0 && !f.context.inputFragmentActive,
                  "PES optional header that would wrap its byte cursor is rejected before input");
        }
    }

    for (const uint32_t shortage : {1U, 0U}) {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        check(DtsRecoveryRestoreInputState(&f.context) == BC_STS_SUCCESS &&
                  f.context.recoveryInjectSpsPps,
              "Flea recovery fixture arms cached Annex-B configuration");
        Bytes idr_only;
        append(idr_only, idr, 0);
        const uint64_t required =
            expected_pes_bytes(parameter_sets().size(), 0) +
            expected_pes_bytes(idr_only.size(), 5);
        Bytes fill(CIRC_TX_BUF_SIZE - required + shortage, 0x19);
        check(__real_txBufPush(&f.context.circBuf, fill.data(), fill.size()) ==
                  BC_STS_SUCCESS,
              "Flea recovery fixture establishes exact prefix plus AU capacity");
        f.context.MDFreeHead = nullptr;
        const uint32_t ring_busy = f.context.circBuf.busySize;
        const BC_STATUS status = DtsProcInputFragment(
            &f.context, idr_only.data(), idr_only.size(), 666671, false,
            DTS_INPUT_FRAGMENT_FIRST | DTS_INPUT_FRAGMENT_LAST);
        if (shortage) {
            check(status == BC_STS_BUSY && transport.attempts.empty() &&
                      f.context.circBuf.busySize == ring_busy &&
                      f.context.recoveryInjectSpsPps &&
                      !f.context.inputFragmentActive,
                  "Flea recovery prefix and timestamped AU are atomically preflighted");
        } else {
            Bytes expected = parameter_sets();
            expected.insert(expected.end(), idr_only.begin(), idr_only.end());
            const PesCapture capture = captured_pes();
            check(status == BC_STS_SUCCESS && capture.valid &&
                      capture.payload == expected &&
                      capture.timestamps.size() == 1 &&
                      capture.timestamps[0] == 66 &&
                      capture.bytes == required && f.no_pending_metadata(),
                  "Flea exact boundary carries recovery prefix and one AU PTS");
        }
    }

    {
        TransportFixture f(TransportKind::FleaPes);
        f.context.SingleThreadedAppMode = 1;
        transport.consume_after_push = true;
        check(DtsProcInputFragment(&f.context, logical.data(), first_size,
                                   0, false,
                                   DTS_INPUT_FRAGMENT_FIRST) == BC_STS_SUCCESS &&
                  f.context.inputFragmentActive,
              "Flea lifecycle fixture opens a fragmented PES AU");
        check(DtsRecoveryFlushInputRing(&f.context) == BC_STS_SUCCESS &&
                  !f.context.inputFragmentActive &&
                  DtsProcInputFragment(&f.context,
                                       logical.data() + first_size,
                                       last_size, 0, false,
                                       DTS_INPUT_FRAGMENT_LAST) ==
                      BC_STS_INV_ARG,
              "recovery teardown aborts Flea PES and rejects its stale tail");
    }
}

static void BoundedEsChunks()
{
    for (uint32_t size : {ALIGN_BUF_SIZE-1U, uint32_t(ALIGN_BUF_SIZE), ALIGN_BUF_SIZE+1U,
                          ALIGN_BUF_SIZE+3U, CIRC_TX_BUF_SIZE-1U, uint32_t(CIRC_TX_BUF_SIZE),
                          CIRC_TX_BUF_SIZE+1U, 2U*CIRC_TX_BUF_SIZE-1U,
                          2U*CIRC_TX_BUF_SIZE, 2U*CIRC_TX_BUF_SIZE+17U}) {
        for (unsigned offset = 0; offset < 4; ++offset) {
            TransportFixture f;
            transport.consume_after_push = true;
            // Keep this fault-injection fixture explicitly finite, including
            // under 32-bit fortified memcpy range analysis.
            if (size > 3U*CIRC_TX_BUF_SIZE) std::abort();
            const Bytes expected = transport_bytes(size);
            Bytes storage(size+8, 0xd7);
            uint8_t *aligned = storage.data() + ((4-reinterpret_cast<uintptr_t>(storage.data())%4)%4);
            uint8_t *data = aligned+offset;
            std::memcpy(data, expected.data(), size);
            const Bytes original = storage;
            const BC_STATUS status = DtsAlignSendData(&f.context, data, size, 0, false);
            check(status == BC_STS_SUCCESS, "large aligned/unaligned ES accepts complete input");
            check(transport.waits == 0, "bounded ES never waits for an impossible enqueue");
            check(transport.attempts == expected_chunks(data,size), "exact ordered ES chunks bounded at512KiB");
            check(accepted_bytes() == expected && transport.consumed == expected,
                  "actual real ring pushes/pops preserve every ES byte in order");
            check(storage == original, "ES chunk alignment never changes caller samples or canaries");
            check(f.context.txBytesEnqueued == size && f.context.circBuf.busySize == 0 &&
                  f.context.circBuf.freeSize == CIRC_TX_BUF_SIZE, "whole acceptance counters and empty ring agree");
            std::printf("ES chunks: bytes=%u alignment=%u status=%u waits=%u attempts=%zu accepted=%zu\n",
                        size,offset,status,transport.waits,transport.attempts.size(),accepted_bytes().size());
        }
    }
}

static void EsRingPressureAndCancellation()
{
    for (uint32_t existing : {ALIGN_BUF_SIZE-5U,CIRC_TX_BUF_SIZE-16U,uint32_t(CIRC_TX_BUF_SIZE)}) {
        TransportFixture f;
        const Bytes prefix = transport_bytes(existing),expected = transport_bytes(CIRC_TX_BUF_SIZE+19U);
        check(__real_txBufPush(&f.context.circBuf,const_cast<uint8_t *>(prefix.data()),existing)==BC_STS_SUCCESS,
              "real ring prefill establishes near/full capacity case");
        transport.consume_after_push = true;
        Bytes input = expected;
        const BC_STATUS status = DtsAlignSendData(&f.context,input.data(),input.size(),0,false);
        if (existing > ALIGN_BUF_SIZE) {
            check(status==BC_STS_IO_USER_ABORT && transport.waits==1 && transport.attempts.empty() &&
                  transport.accepted.empty() && f.context.txBytesEnqueued==0 &&
                  f.context.circBuf.busySize==existing,"near-full/full ring first wait cancels without accepting a prefix");
            continue;
        }
        Bytes complete = prefix; complete.insert(complete.end(),expected.begin(),expected.end());
        check(status == BC_STS_SUCCESS && transport.waits == 0,
              "near-capacity ring makes bounded progress without an impossible wait");
        check(accepted_bytes() == expected && transport.consumed == complete &&
              f.context.txBytesEnqueued == expected.size(), "ring wrap preserves prefilling and complete new byte ordering");
    }
    {
        TransportFixture f; Bytes existing(CIRC_TX_BUF_SIZE,0x35),input(ALIGN_BUF_SIZE+1U,0x67);
        check(__real_txBufPush(&f.context.circBuf,existing.data(),existing.size())==BC_STS_SUCCESS,"full real ring before cancellation");
        check(DtsAlignSendData(&f.context,input.data(),input.size(),0,false)==BC_STS_IO_USER_ABORT &&
              transport.waits==1 && transport.attempts.empty() && transport.accepted.empty() &&
              f.context.txBytesEnqueued==0 && f.context.circBuf.busySize==existing.size(),
              "first5mswait cancellation rejects all unaccepted bytes without timeout/hang");
    }
    {
        TransportFixture f; Bytes input = transport_bytes(2U*CIRC_TX_BUF_SIZE+17U);
        transport.consume_after_push = true; transport.cancel_after_push = 1;
        check(DtsAlignSendData(&f.context,input.data(),input.size(),0,false)==BC_STS_IO_USER_ABORT,
              "stop after one accepted chunk never reports whole input success");
        check(transport.waits==0 && transport.accepted.size()==1 &&
              transport.accepted[0]==Bytes(input.begin(),input.begin()+ALIGN_BUF_SIZE) &&
              f.context.txBytesEnqueued==ALIGN_BUF_SIZE,"canceled prefix is exactly one bounded accepted chunk");
        const size_t attempts = transport.attempts.size();
        check(DtsAlignSendData(&f.context,input.data(),input.size(),0,false)==BC_STS_IO_USER_ABORT &&
              transport.attempts.size()==attempts,"already stopped input makes no further push");
    }
    for (uint32_t size : {CIRC_TX_BUF_SIZE+1U,UINT32_MAX}) {
        TransportFixture f; uint8_t byte=0x41;
        check(DtsSendData(&f.context,&byte,size,0,false)==BC_STS_INSUFF_RES && transport.waits==0 &&
              transport.attempts.empty() && transport.accepted.empty() && f.context.txBytesEnqueued==0,
              "direct greater-than-ring enqueue rejects INSUFF_RES before waiting or reading input");
    }
    for (bool quiescing : {false,true}) {
        TransportFixture f; uint8_t byte=0x41;
        if (quiescing) f.context.txQuiescing=true; else f.context.State=BC_DEC_STATE_STOP;
        check(DtsSendData(&f.context,&byte,UINT32_MAX,0,false)==BC_STS_IO_USER_ABORT &&
              transport.waits==0 && transport.attempts.empty(),"closed admission takes precedence over oversized capacity rejection");
    }
}

static void SpesAdmissionFailures()
{
    for (bool link_pes : {false,true}) {
        for (BC_STATUS injected : {BC_STS_BUSY,BC_STS_INSUFF_RES,BC_STS_IO_ERROR,
                                   BC_STS_IO_USER_ABORT,BC_STS_INV_ARG,BC_STS_TIMEOUT}) {
            TransportFixture f(link_pes);
            transport.first_marker_status=injected;
            Bytes input=transport_bytes(32);
            const BC_STATUS actual=DtsAlignSendData(&f.context,input.data(),input.size(),66,false);
            check(actual==injected,"first rejected SPES marker status is not overwritten by ES/LinkPES payload");
            check(transport.attempts.size()==1 && transport.attempts[0]==sizeof(BC_SEQ_HDR_FORMAT) &&
                  transport.accepted.empty() && f.context.txBytesEnqueued==0 &&
                  f.context.circBuf.busySize==0,"failed marker has exactly one unaccepted attempt and no payload bytes");
            check(f.free_metadata()==8 && f.no_pending_metadata() && transport.waits==0 &&
                  transport.metadata_retries==0,"failed marker restores free metadata without pending ownership");
            std::printf("SPES failure: link-pes=%u injected=%u actual=%u attempts=%zu accepted=%zu\n",
                        link_pes,injected,actual,transport.attempts.size(),accepted_bytes().size());
        }
        {
            TransportFixture f(link_pes);
            f.context.MDFreeHead=nullptr; // Valid pool, genuinely no free/pending entry.
            transport.allow_metadata_retry=true;
            Bytes input=transport_bytes(32);
            const BC_STATUS actual=DtsAlignSendData(&f.context,input.data(),input.size(),66,false);
            check(actual==BC_STS_BUSY && transport.metadata_retries==20 && transport.waits==0 &&
                  transport.attempts.empty() && transport.accepted.empty() && f.context.txBytesEnqueued==0,
                  "actual empty metadata pool exhausts20retry attempts and refuses all payload");
            check(!f.context.MDFreeHead && f.no_pending_metadata(),"empty metadata pool does not invent/leak an owner");
            std::printf("SPES empty pool: link-pes=%u actual=%u retries=%u push-attempts=%zu\n",
                        link_pes,actual,transport.metadata_retries,transport.attempts.size());
        }
        {
            TransportFixture f(link_pes); Bytes input=transport_bytes(32);
            check(DtsAlignSendData(&f.context,input.data(),input.size(),66,false)==BC_STS_SUCCESS &&
                  transport.attempts.size()==2 && transport.accepted.size()==2,
                  "normal positive timestamp accepts marker followed by one payload packet");
            if (transport.accepted.size()==2) {
                const uint32_t sequence=f.context.InMdataTag&DTS_MDATA_MAX_TAG;
                const Bytes marker={0,0,1,0xbd,7,0x40,static_cast<uint8_t>(sequence),
                    static_cast<uint8_t>(sequence>>8),0x0a,0,0,0};
                check(transport.accepted[0]==marker &&
                      (link_pes ? payload(transport.accepted[1]) : transport.accepted[1])==input,
                      "normal marker bytes and ordered ES/LinkPES payload remain exact");
                check(f.free_metadata()==7 && !f.no_pending_metadata() &&
                      f.context.MDPendHead==f.context.MDPendTail && f.context.MDPendHead->appTimeStamp==66,
                      "accepted marker holds exactly one correctly timestamped metadata owner");
                check(DtsClrPendMdataList(&f.context)==BC_STS_SUCCESS && f.free_metadata()==8 &&
                      f.no_pending_metadata(),"normal pending marker returns safely to free pool");
            }
        }
    }
}

static void TransportAdmission()
{
    FragmentedH264Input(); FleaPesFragmentedH264Input(); BoundedEsChunks();
    EsRingPressureAndCancellation(); SpesAdmissionFailures();
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--framing") &&
                     std::strcmp(argv[1], "--transport-only"))) return 2;
    if (argc == 2 && !std::strcmp(argv[1], "--transport-only")) {
        TransportAdmission();
        if (failures) { std::fprintf(stderr,"%u transport checks failed\n",failures); return 1; }
        std::puts("PASS: CPU fault injection, bounded real ES ring progress and exact SPES failure admission");
        return 0;
    }
    Framing();
    RecoverySingleThreadConfig();
    NestedInputAcrossLifecycleOwner();
    WmvBFrameMetadata();
    AnnexBScannerBounds();
    // --framing omits the separate SPS detector regression.
    if (argc == 1) { DetectorBounds(); TransportAdmission(); }
    if (failures) { std::fprintf(stderr, "%u input checks failed\n", failures); return 1; }
    std::puts("PASS: actual AVC1/WMV3 input timestamps, framing, SPS detection and bounded Annex-B scanning");
}
