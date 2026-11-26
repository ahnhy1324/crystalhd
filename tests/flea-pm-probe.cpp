// SPDX-License-Identifier: LGPL-2.1-or-later
// Actual diagnostic wrappers; all device-facing functions are leaf seams.
#define FINITE_PM_UNIT
#include "../tools/research/flea-pm-finite/probe.cpp"

static unsigned checks, failures, released, colours, pause_order;
static BC_STATUS output_status, release_status, pause_status, resume_status, input_status;
static BC_DTS_PROC_OUT picture;
static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
static BC_STATUS Colour(HANDLE, BC_OUTPUT_FORMAT format)
{
    ++colours;
    Check(format == OUTPUT_MODE422_YUY2, "only ordinary YUY2 reaches setter");
    return BC_STS_SUCCESS;
}
static BC_STATUS Fetch(HANDLE, uint32_t, BC_DTS_PROC_OUT *out)
{ *out = picture; return output_status; }
static BC_STATUS Release(HANDLE, PVOID, BOOL)
{ ++released; return release_status; }
static BC_STATUS Pause(HANDLE, uint32_t operation)
{
    using namespace finite_pm;
    Check(released == 1 && !acquired && released_pictures == 1 && !pixels.written,
          "pause sequence follows first actual release before owned file write");
    Check(operation == (pause_order++ ? 0U : 1U), "ON then OFF exactly once");
    return operation ? pause_status : resume_status;
}
static BC_STATUS InputLeaf(HANDLE, uint8_t *, uint32_t, uint64_t, BOOL encrypted)
{
    Check(!encrypted, "ordinary clear public argument is preserved");
    return input_status;
}
static void Reset()
{
    using namespace finite_pm;
    active = true; host_only = failed = acquired = pending_pixels = pause_done = false;
    interrupted = 0;
    inputs = outputs = released_pictures = setter_calls = pause_calls = 0;
    released = colours = pause_order = 0; submitted_bytes = 0;
    output_status = release_status = pause_status = resume_status = BC_STS_SUCCESS;
    input_status = BC_STS_IO_ERROR;
    input_tokens.clear();
    // Output-only cases supply one already successful input ownership token.
    input_tokens.insert(Token(0,0));
    events = std::tmpfile(); input_file = std::tmpfile(); pixels.file = std::tmpfile();
    Check(events && input_file && pixels.file, "exclusive host test evidence files");
    pixels.frame_bytes = uint64_t(width)*height*2U; pixels.total_bytes = pixels.frame_bytes*expected;
    pixels.expected = expected; pixels.frames = 0; pixels.written = 0;
    colour_call = Colour; output_call = Fetch; release_call = Release; pause_call = Pause;
    input_call = InputLeaf;
}
static void Finish()
{
    using namespace finite_pm;
    active = false;
    Check(CloseFiles(), "observer files close");
    Check(std::fclose(pixels.file) == 0, "pixel test file closes");
    pixels.file = nullptr;
}
int main()
{
    using namespace finite_pm;
    Check(!Prepare("relative"), "relative evidence directory refused without a device");
    HANDLE owner = reinterpret_cast<HANDLE>(0x1234);
    std::vector<uint8_t> image(width*height*2U, 0x81);
    picture = {}; picture.Ybuff = image.data(); picture.YBuffDoneSz = image.size()/4;
    picture.b422Mode = OUTPUT_MODE422_YUY2; picture.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
    picture.PicInfo.width = width; picture.PicInfo.height = height;
    picture.PicInfo.timeStamp = Token(0,0);
    for (unsigned mode : {0U, 1U}) {
        Reset(); host_only = mode != 0;
        Check(__wrap_DtsSetColorSpace(owner, OUTPUT_MODE422_YUY2) == BC_STS_SUCCESS &&
              setter_calls == 1 && colours == (mode ? 0U : 1U),
              "reference executes setter; host leaves existing cache untouched");
        BC_DTS_PROC_OUT out = {};
        Check(__wrap_DtsProcOutputNoCopy(owner,0,&out) == BC_STS_SUCCESS && acquired &&
              pending_pixels && !pause_calls, "fetch only acquires and copies the output");
        image[0] = 0x19;
        Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE) == BC_STS_SUCCESS &&
              released == 1 && outputs == 1 && pause_calls == 2 && !failed && !acquired,
              "first real release executes the single pair and writes owned pixels");
        std::rewind(pixels.file);
        Check(std::fgetc(pixels.file) == 0x81, "capture retains owned pre-release bytes");
        std::fseek(pixels.file,0,SEEK_END); image[0] = 0x81;
        picture.PicInfo.timeStamp = Token(0,1); input_tokens.insert(Token(0,1));
        __wrap_DtsProcOutputNoCopy(owner,0,&out);
        __wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE);
        Check(outputs == 2 && released == 2 && pause_calls == 2 && !failed,
              "later frames never add pause commands");
        Finish();
        picture.PicInfo.timeStamp = Token(0,0);
    }
    for (unsigned fault : {0U,1U}) {
        Reset();
        if (fault) resume_status = BC_STS_IO_ERROR; else pause_status = BC_STS_IO_ERROR;
        BC_DTS_PROC_OUT out = {};
        __wrap_DtsProcOutputNoCopy(owner,0,&out);
        Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE) == BC_STS_SUCCESS &&
              pause_calls == 2 && pause_order == 2 && failed && interrupted && outputs == 1 &&
              paused_status == pause_status && resumed_status == resume_status,
              "ON failure still attempts OFF once; actual statuses and first copy survive");
        Finish();
    }
    Reset(); release_status = BC_STS_IO_ERROR;
    BC_DTS_PROC_OUT out = {};
    __wrap_DtsProcOutputNoCopy(owner,0,&out);
    Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE) == BC_STS_IO_ERROR &&
          released == 1 && !outputs && !pause_calls && !acquired && failed,
          "failed release causes no pause, write or automatic retry");
    Finish();
    Reset(); picture.PoutFlags |= BC_POUT_FLAGS_ENCRYPTED;
    picture.Ybuff = reinterpret_cast<uint8_t *>(1);
    Check(__wrap_DtsProcOutputNoCopy(owner,0,&out) == BC_STS_SUCCESS &&
          acquired && !pending_pixels && failed, "encrypted output rejected before pointer access");
    Check(__wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE) == BC_STS_SUCCESS &&
          released == 1 && !pause_calls && !outputs, "invalid successful fetch still releases once");
    Finish();
    for (unsigned invalid : {0U,1U,2U}) {
        Reset(); picture.Ybuff = image.data(); picture.PoutFlags = BC_POUT_FLAGS_PIB_VALID;
        picture.PicInfo.timeStamp = Token(0,0);
        if (invalid == 0) picture.PicInfo.timeStamp = Token(0,17);
        if (invalid == 1) input_tokens.erase(Token(0,0)); // Already consumed duplicate token.
        if (invalid == 2) picture.PoutFlags = 0;
        __wrap_DtsProcOutputNoCopy(owner,0,&out);
        Check(failed && acquired && !pending_pixels,
              "unknown/consumed token or missing PIB never qualifies a first output");
        __wrap_DtsReleaseOutputBuffs(owner,nullptr,FALSE);
        Check(released == 1 && !pause_calls && !outputs,
              "invalid first output releases once without exercising pause");
        Finish();
    }
    Reset(); output_status = BC_STS_NO_DATA;
    Check(__wrap_DtsProcOutputNoCopy(owner,0,&out) == BC_STS_NO_DATA && !acquired &&
          !failed && !pause_calls, "no-data return creates no lease or pause");
    uint8_t bytes[] = {0,0,1,0,0x42};
    Input input; Packet packet; packet.data.assign(bytes,bytes+sizeof(bytes));
    packet.size = sizeof(bytes); input.packets.push_back(packet); admitted = &input;
    Check(__wrap_DtsProcInput(owner,bytes,sizeof(bytes),Token(0,0),FALSE) == BC_STS_IO_ERROR &&
          inputs == 1 && submitted_bytes == sizeof(bytes), "source bytes/token and actual input failure retained");
    input_tokens.clear(); inputs = 0; input_status = BC_STS_SUCCESS;
    Check(__wrap_DtsProcInput(owner,bytes,sizeof(bytes),Token(0,0),FALSE) == BC_STS_SUCCESS &&
          input_tokens.count(Token(0,0)) == 1, "actual successful submit creates output token ownership");
    inputs = 0;
    Check(__wrap_DtsProcInput(owner,bytes,sizeof(bytes),Token(0,1),FALSE) == BC_STS_ERROR &&
          failed && !inputs, "changed source token refused before public input");
    Finish();
    std::printf("Finite PM probe controls: %u checks, %u failures; hardware=NOT_RUN\n", checks, failures);
    return failures ? 1 : 0;
}
