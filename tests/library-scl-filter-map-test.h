// SPDX-License-Identifier: LGPL-2.1-or-later
// Hardware-free literal oracle for the optional native SCL register observer.
#ifndef CRYSTALHD_LIBRARY_SCL_FILTER_MAP_TEST_H
#define CRYSTALHD_LIBRARY_SCL_FILTER_MAP_TEST_H

static uint32_t SclFilterMapTestAddress(unsigned field)
{
    static const uint32_t controls[23] = {
        0x540800, 0x540804, 0x540808, 0x54080c, 0x540810, 0x540814,
        0x540818, 0x54081c, 0x540820, 0x540824, 0x540828, 0x54082c,
        0x540830, 0x540834, 0x540838, 0x54083c, 0x540840, 0x540844,
        0x540848, 0x54084c, 0x540850, 0x540854, 0x5408a4
    };
    if (field < 23) return controls[field];
    if (field < 55) return 0x540900 + (field - 23) * 4;
    if (field < 87) return 0x540980 + (field - 55) * 4;
    if (field < 151) return 0x540a00 + (field - 87) * 4;
    if (field < 215) return 0x540b00 + (field - 151) * 4;
    return field == 215 ? 0x540800 : 0;
}

static uint32_t SclFilterMapTestMask(unsigned field)
{
    static const uint32_t controls[23] = {
        0xffff, 0xe, 0x1f7, 0xff, 0x07ff07ff, 0x07ff07ff,
        0x07ff07ff, 0x07ff07ff, 0x003f0000, 0xfffffff8,
        0x03fffff8, 0x001fc000, 0xffffc000, 0xffffc000,
        0xffffffff, 0x7fffffff, 0x3ffffffc, 0x3ffffffc,
        0x07ff0000, 0x07ff0000, 0x07ff0000, 1, 0xff
    };
    return field < 23 ? controls[field] : field < 215 ? 0x3ffc3ffc : field == 215 ? 0xffff : 0;
}

struct SclFilterMapTestFixture {
    uint32_t raw[432] = {};
    unsigned calls = 0, fail_at = 432;
    BC_STATUS api_status = BC_STS_ERROR;
    bool valid = true;
    SclFilterMapTestFixture() {
        static const uint32_t controls[23] = {
            0x80, 0xc, 5, 4, 0x02800168, 0, 0x02800168, 0x014000b4,
            0x00010000, 0x800, 0x1000, 0x4000, 0x4000, 0x8000,
            0x12345678, 0x1000, 4, 8, 0x00100000, 0x00200000, 0x00300000, 1, 0
        };
        for (unsigned field = 0; field < 216; ++field) {
            uint32_t value = field < 23 ? controls[field] : field == 215 ? 0x80 :
                (((field * 37) & 0xfff) << 18) | (((field * 73 + 1) & 0xfff) << 2);
            raw[field] = raw[field + 216] = value;
        }
    }
    static BC_STATUS Read(HANDLE handle, uint32_t address, uint32_t *value) {
        auto *fixture = static_cast<SclFilterMapTestFixture *>(handle);
        const unsigned index = fixture->calls++;
        if (index >= 432 || !value) { fixture->valid = false; return BC_STS_ERROR; }
        fixture->valid &= address == SclFilterMapTestAddress(index % 216);
        *value = index == fixture->fail_at ? 0xdeadbeefU : fixture->raw[index];
        return index == fixture->fail_at ? fixture->api_status : BC_STS_SUCCESS;
    }
};

template<class Check> static void SclFilterMapSelfTest(const Check &check)
{
    for (unsigned field = 0; field < 216; ++field) {
        check(SclFilterMapAddress(field) == SclFilterMapTestAddress(field) &&
              SclFilterMapMask(field) == SclFilterMapTestMask(field),
              "SCL filter map every control/bank/boundary address and mask matches independent literals");
        check(SclFilterMapScalarFailure(field, 0xffffffffU) == SclFilterMapFailure::Unavailable,
              "SCL filter map all-ones is unavailable even for the full-width phase field");
    }
    check(SclFilterMapAddress(216) == 0 && SclFilterMapMask(216) == 0 &&
          SclFilterMapScalarFailure(216, 0) == SclFilterMapFailure::Argument,
          "SCL filter map out-of-range scalar lookup is not another register");
    for (uint32_t value = 0; value < 4096; ++value) {
        const uint32_t other = 4095 - value;
        const uint32_t raw = (value << 18) | (other << 2);
        check(SclFilterMapEven(raw) == value && SclFilterMapOdd(raw) == other &&
              SclFilterMapEven(raw | 0xc003c003U) == value &&
              SclFilterMapOdd(raw | 0xc003c003U) == other,
              "SCL filter map unsigned 12-bit fields use shifts18/2, not contiguous halfwords or signed inference");
    }
    {
        SclFilterMapTestFixture fixture;
        struct { uint32_t before = 0x12345678; SclFilterMapSnapshot value;
                 uint32_t after = 0x87654321; } guarded;
        check(ReadSclFilterMap(&fixture, &guarded.value, SclFilterMapTestFixture::Read) &&
              fixture.valid && fixture.calls == 432 && guarded.value.reads == 432 &&
              guarded.value.measured == 432 && guarded.value.Complete() && guarded.value.Stable() &&
              !guarded.value.StatusObserved() && guarded.value.failure == SclFilterMapFailure::None &&
              guarded.value.status == BC_STS_SUCCESS && guarded.before == 0x12345678 && guarded.after == 0x87654321 &&
              !std::memcmp(guarded.value.raw, fixture.raw, sizeof(fixture.raw)),
              "SCL filter map exact432-read complete literal stream preserves raw tuples and canaries");
        SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture; fixture.calls = 0;
        check(observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false) &&
              fixture.valid && fixture.calls == 432 && observer.attempted && !observer.failed && !observer.fatal &&
              observer.Finish(true, false) && !observer.Finish(false, false),
              "SCL filter map successful native first-output observation requires independent native success");
        for (unsigned frame : {2U, 90U, 179U, 180U})
            check(observer.AfterDelivered(&fixture, frame, true, true, SclFilterMapTestFixture::Read, false) &&
                  fixture.calls == 432, "SCL filter map later owned outputs cannot retarget or repeat the snapshot");
    }
    for (unsigned position = 0; position < 432; ++position) {
        for (int code = -1; code <= BC_STS_PWR_MGMT; ++code) {
            if (code == BC_STS_SUCCESS) continue;
            SclFilterMapTestFixture fixture; fixture.fail_at = position;
            fixture.api_status = static_cast<BC_STATUS>(code);
            struct { uint32_t before = 0x12345678; SclFilterMapSnapshot value;
                     uint32_t after = 0x87654321; } guarded;
            check(!ReadSclFilterMap(&fixture, &guarded.value, SclFilterMapTestFixture::Read) &&
                  fixture.valid && fixture.calls == position + 1 && guarded.value.reads == position + 1 &&
                  guarded.value.measured == position && guarded.value.failure == SclFilterMapFailure::Read &&
                  guarded.value.status == code && !guarded.value.Complete() && !guarded.value.Stable() &&
                  guarded.before == 0x12345678 && guarded.after == 0x87654321,
                  "SCL filter map every27 API status at every432 read stops on the exact literal prefix");
            bool unread_zero = true;
            for (unsigned unread = position; unread < 432; ++unread)
                unread_zero &= guarded.value.raw[unread / 216][unread % 216] == 0;
            check(unread_zero, "SCL filter map failed API poison and later unmeasured values are not published");
            fixture.calls = 0;
            SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture;
            check(!observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false) &&
                  observer.failed && observer.fatal && observer.attempted && fixture.calls == position + 1 &&
                  observer.snapshot.status == code && observer.snapshot.failure == SclFilterMapFailure::Read &&
                  !observer.Finish(true, false), "SCL filter map API failure is sticky, including last closing revision");
            SclFilterMapTestFixture other;
            check(!observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false) &&
                  !observer.AfterDelivered(&fixture, 2, true, true, SclFilterMapTestFixture::Read, false) &&
                  !observer.AfterDelivered(&other, 1, true, true, SclFilterMapTestFixture::Read, false) &&
                  fixture.calls == position + 1 && other.calls == 0,
                  "SCL filter map failed observer cannot retry, switch owner or make later custom I/O");
        }
        const unsigned field = position % 216;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (SclFilterMapTestMask(field) & (1U << bit)) continue;
            SclFilterMapTestFixture fixture; fixture.raw[position] |= 1U << bit;
            SclFilterMapSnapshot snapshot;
            const auto expected = field == 0 || field == 215 ? SclFilterMapFailure::Revision : SclFilterMapFailure::Reserved;
            check(!ReadSclFilterMap(&fixture, &snapshot, SclFilterMapTestFixture::Read) && fixture.valid &&
                  fixture.calls == position + 1 && snapshot.measured == position + 1 && snapshot.failure == expected,
                  "SCL filter map every reserved bit in either pass fails without a later read");
        }
        for (uint32_t raw : {0xffffffffU, 0U, 1U, 0xffffU, 0x81U}) {
            if (raw != 0xffffffffU && field != 0 && field != 215) continue;
            SclFilterMapTestFixture fixture; fixture.raw[position] = raw;
            SclFilterMapSnapshot snapshot;
            check(!ReadSclFilterMap(&fixture, &snapshot, SclFilterMapTestFixture::Read) && fixture.valid &&
                  fixture.calls == position + 1 && snapshot.measured == position + 1 &&
                  snapshot.failure == (raw == 0xffffffffU ? SclFilterMapFailure::Unavailable : SclFilterMapFailure::Revision),
                  "SCL filter map all-ones and wrong board revisions reject at each measured position");
        }
    }
    for (unsigned field = 1; field < 215; ++field) {
        const uint32_t bit = SclFilterMapTestMask(field) & (0U - SclFilterMapTestMask(field));
        SclFilterMapTestFixture fixture; fixture.raw[field + 216] ^= bit;
        SclFilterMapSnapshot snapshot;
        check(ReadSclFilterMap(&fixture, &snapshot, SclFilterMapTestFixture::Read) && fixture.valid &&
              fixture.calls == 432 && snapshot.Complete() && !snapshot.Stable() &&
              snapshot.failure == SclFilterMapFailure::None &&
              snapshot.raw[0][field] == fixture.raw[field] && snapshot.raw[1][field] == fixture.raw[field + 216],
              "SCL filter map complete unequal legal tuples are retained as observations without stable retries");
        fixture.calls = 0;
        SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture;
        check(observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false) &&
              observer.failed && !observer.fatal && observer.snapshot.failure == SclFilterMapFailure::Unstable &&
              fixture.calls == 432 && !observer.Finish(true, false) &&
              observer.AfterDelivered(&fixture, 2, true, true, SclFilterMapTestFixture::Read, false) && fixture.calls == 432,
              "SCL filter map tuple drift retains diagnostic FAIL while allowing ordinary later native delivery");
    }
    for (unsigned flags = 1; flags < 256; ++flags) {
        for (unsigned where = 0; where < 3; ++where) {
            SclFilterMapTestFixture fixture;
            if (where != 1) fixture.raw[22] = flags;
            if (where != 0) fixture.raw[238] = flags;
            SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture;
            check(observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false) &&
                  fixture.valid && fixture.calls == 432 && observer.snapshot.Complete() &&
                  observer.snapshot.StatusObserved() && observer.failed && !observer.fatal &&
                  observer.snapshot.raw[0][22] == (where == 1 ? 0U : flags) &&
                  observer.snapshot.raw[1][22] == (where == 0 ? 0U : flags) &&
                  !observer.Finish(true, false) &&
                  observer.AfterDelivered(&fixture, 180, true, true, SclFilterMapTestFixture::Read, false) && fixture.calls == 432,
                  "SCL filter map every defined STATUS combination completes both tuples and stays FAIL through native EOS");
        }
    }
    for (bool maximum : {false, true}) {
        SclFilterMapTestFixture fixture;
        for (unsigned field = 1; field < 215; ++field) {
            uint32_t raw = maximum ? SclFilterMapTestMask(field) : 0;
            if (raw == 0xffffffffU) --raw;
            fixture.raw[field] = fixture.raw[field + 216] = raw;
        }
        SclFilterMapSnapshot snapshot;
        check(ReadSclFilterMap(&fixture, &snapshot, SclFilterMapTestFixture::Read) && fixture.valid &&
              fixture.calls == 432 && snapshot.Complete() && snapshot.Stable(),
              "SCL filter map reserved-clean zero/extreme modes, offsets and coefficients stay raw rather than guessed defaults");
    }
    {
        SclFilterMapTestFixture fixture; SclFilterMapObserver disabled;
        check(disabled.AfterDelivered(nullptr, 0, false, false, nullptr, false) &&
              disabled.Finish(true, false) && fixture.calls == 0 && !disabled.attempted,
              "SCL filter map default-disabled path is a pure zero-I/O no-op");
        check(!ReadSclFilterMap(&fixture, nullptr, SclFilterMapTestFixture::Read) && fixture.calls == 0,
              "SCL filter map null destination never invokes a reader");
        SclFilterMapSnapshot snapshot;
        check(!ReadSclFilterMap(&fixture, &snapshot, nullptr) && fixture.calls == 0 &&
              snapshot.failure == SclFilterMapFailure::Argument, "SCL filter map null reader is an argument failure");
        check(!ReadSclFilterMap(nullptr, &snapshot, SclFilterMapTestFixture::Read) && fixture.calls == 0 &&
              snapshot.reads == 0 && snapshot.measured == 0 && snapshot.failure == SclFilterMapFailure::Argument,
              "SCL filter map null handle is an argument failure before any callback");
        for (unsigned fault = 0; fault < 7; ++fault) {
            SclFilterMapObserver observer; observer.enabled = true;
            SclFilterMapTestFixture other; observer.owner = fault == 0 ? nullptr : &fixture;
            const auto failure = fault == 3 ? SclFilterMapFailure::Argument : fault >= 6 ?
                SclFilterMapFailure::Order : SclFilterMapFailure::Admission;
            check(!observer.AfterDelivered(fault == 1 ? nullptr : fault == 2 ? &other : &fixture,
                  fault == 6 ? 2 : 1, fault != 4, fault != 5,
                  fault == 3 ? nullptr : SclFilterMapTestFixture::Read, false) && observer.failed && observer.fatal &&
                  observer.snapshot.failure == failure && fixture.calls == 0 && other.calls == 0 &&
                  !observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false),
                  "SCL filter map missing/mismatched owner, reader, release, owned write and first ordinal latch before I/O");
        }
        for (unsigned frame : {0U, 2U, 179U, 180U}) {
            SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture;
            check(!observer.AfterDelivered(&fixture, frame, true, true, SclFilterMapTestFixture::Read, false) &&
                  observer.failed && observer.fatal && observer.snapshot.failure == SclFilterMapFailure::Order && fixture.calls == 0,
                  "SCL filter map no format-change/premature/later frame may become the first observation");
        }
        for (unsigned fault = 0; fault < 5; ++fault) {
            SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture; fixture.calls = 0;
            check(observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, false),
                  "SCL filter map post-attempt admission-fault setup");
            SclFilterMapTestFixture other;
            check(!observer.AfterDelivered(fault == 0 ? &other : &fixture, fault == 1 ? 1 : 2,
                  fault != 2, fault != 3, fault == 4 ? nullptr : SclFilterMapTestFixture::Read, false) &&
                  observer.failed && observer.fatal && fixture.calls == 432 && other.calls == 0 && !observer.Finish(true, false),
                  "SCL filter map duplicate or lost later delivery admission never erases failure or rereads");
        }
    }
    for (unsigned failure_at : {0U, 431U, 432U}) {
        FILE *record = std::tmpfile(); const int saved_stdout = dup(STDOUT_FILENO);
        std::fflush(stdout);
        const bool redirected = record && saved_stdout >= 0 && dup2(fileno(record), STDOUT_FILENO) >= 0;
        bool result_matched = false;
        if (redirected) {
            SclFilterMapTestFixture fixture; fixture.fail_at = failure_at;
            SclFilterMapObserver observer; observer.enabled = true; observer.owner = &fixture;
            const bool collected = observer.AfterDelivered(&fixture, 1, true, true, SclFilterMapTestFixture::Read, true);
            result_matched = collected == (failure_at == 432) && fixture.valid &&
                fixture.calls == (failure_at == 432 ? 432U : failure_at + 1) && observer.snapshot.measured == failure_at;
            std::fflush(stdout);
        }
        const bool restored = saved_stdout >= 0 && dup2(saved_stdout, STDOUT_FILENO) >= 0;
        if (saved_stdout >= 0) close(saved_stdout);
        std::string text;
        if (record) {
            std::rewind(record); char chunk[1024]; size_t bytes = 0;
            while ((bytes = std::fread(chunk, 1, sizeof(chunk), record)) && text.size() < 131072)
                text.append(chunk, bytes);
            std::fclose(record);
        }
        unsigned rows = 0;
        for (size_t position = 0; (position = text.find("SCL filter raw:", position)) != std::string::npos; ++position)
            ++rows;
        check(redirected && restored && result_matched && !text.empty() && text.size() < 131072 && rows == 432 &&
              text.find("deadbeef") == std::string::npos && (failure_at == 432 ?
                (text.find("NOT-READ") == std::string::npos && text.find("raw-stable=yes") != std::string::npos) :
                (text.find("NOT-READ") != std::string::npos && text.find("raw-stable=yes") == std::string::npos)),
              "SCL filter map actual reporter marks unread first/final words rather than publishing API poison or stability");
        if (failure_at == 432) {
            for (const char *boundary : {
                    "field=23 address=00540900 bank=VY phase=0 even-tap=0",
                    "field=54 address=0054097c bank=VY phase=7 even-tap=6",
                    "field=55 address=00540980 bank=VC phase=0 even-tap=0",
                    "field=86 address=005409fc bank=VC phase=7 even-tap=6",
                    "field=87 address=00540a00 bank=HY phase=0 even-tap=0",
                    "field=150 address=00540afc bank=HY phase=7 even-tap=14",
                    "field=151 address=00540b00 bank=HC phase=0 even-tap=0",
                    "field=214 address=00540bfc bank=HC phase=7 even-tap=14"})
                check(text.find(boundary) != std::string::npos,
                      "SCL filter map complete reporter labels exact physical bank/phase/tap boundaries without address following");
        }
    }
    {
        const std::vector<const char *> valid = {"probe", "--hardware", "fixture", "180", "30", "1",
            "--scaler-test", "320", "--observe-scl-filter-map", "--capture-yuy2", "new"};
        Options admitted;
        check(ParseArguments(valid, &admitted) && admitted.observe_scl_filter_map && NeedsRawIo(admitted) &&
              !NeedsRawIo(Options{}), "SCL filter map explicitly requires capability before fixture/capture/device access");
        for (const char *width : {"0", "320", "640"}) {
            auto arguments = valid; arguments[7] = width; Options options;
            check(ParseArguments(arguments, &options) && options.observe_scl_filter_map,
                  "SCL filter map admitted native/identity/half-width controls remain explicit");
        }
        for (unsigned field = 0; field < 11; ++field) {
            auto arguments = valid;
            if (field == 0) arguments[1] = "--preflight";
            if (field == 1) arguments[3] = "179";
            if (field == 2) arguments[5] = "2";
            if (field == 3) arguments[7] = "480";
            if (field == 4) arguments[9] = "--capture-uyvy";
            if (field == 5) arguments.resize(9);
            if (field == 6) arguments.insert(arguments.begin() + 8, "--mpeg1-via-mpeg2");
            if (field == 7) arguments.insert(arguments.begin() + 8, "--h263-via-divx");
            if (field == 8) arguments.insert(arguments.begin() + 8, "--open-only");
            if (field == 9) arguments.insert(arguments.begin() + 8, "--observe-scl-filter-map");
            if (field == 10) { arguments.erase(arguments.begin() + 8); arguments.push_back("--observe-scl-filter-map"); }
            Options invalid;
            check(!ParseArguments(arguments, &invalid), "SCL filter map rejects nonnative/repeated/noncapture/misplaced scope");
        }
        for (const auto &mixed : std::vector<std::vector<const char *>>{
                {"--observe-chroma"}, {"--observe-scl-config"}, {"--observe-scl-view", "2"},
                {"--observe-mfd-config"}, {"--inject-mfd-colour", "a"}, {"--scl-status-test", "observe"}})
            for (unsigned where : {8U, 9U}) {
                auto arguments = valid; arguments.insert(arguments.begin() + where, mixed.begin(), mixed.end());
                Options invalid;
                check(!ParseArguments(arguments, &invalid), "SCL filter map every existing diagnostic mixture/order is rejected");
            }
        Options invalid;
        check(!ParseArguments({"probe", "--self-test", "--observe-scl-filter-map"}, &invalid),
              "SCL filter map cannot hide a hardware observer inside self-test");
        Input native; native.codec = AV_CODEC_ID_MPEG2VIDEO; native.subtype = BC_MSUBTYPE_MPEG2VIDEO;
        native.progressive = true; native.width = 640; native.height = 360; native.packets.resize(180);
        check(SclInputAdmitted(admitted, native), "SCL filter map admits only its measured native input profile");
        for (unsigned field = 0; field < 7; ++field) {
            Options changed = admitted; Input altered = native;
            if (field == 0) altered.codec = AV_CODEC_ID_H264;
            if (field == 1) altered.subtype = BC_MSUBTYPE_H264;
            if (field == 2) altered.progressive = false;
            if (field == 3) altered.width = 638;
            if (field == 4) altered.height = 358;
            if (field == 5) altered.packets.pop_back();
            if (field == 6) changed.expected = 179;
            check(!SclInputAdmitted(changed, altered) && SclInputAdmitted(Options{}, altered),
                  "SCL filter map codec/subtype/progressive/geometry/count admission is distinct from unchanged default behavior");
        }
    }
}

#endif
