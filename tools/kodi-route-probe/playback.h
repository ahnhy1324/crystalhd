/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "native.h"

namespace crystalhd_kodi {
/* Private playback lifecycle, NOT a V4L2 ioctl implementation. Kodi's video
 * callbacks serialize this facade; InputProgress separates finite EOF and
 * private admission pause from Kodi's demux-speed notifications. The
 * qualified Gst decoder still owns the actual
 * accepted-input native-token -> source-PTS registry and all RX leases.
 * No second FIFO/POC timestamp guess, private picture, or new copy is added.
 */
class PlaybackDecoder {
public:
  using Submit = NativeDecoder::Submit;
  using Result = NativeDecoder::Result;
  enum class State { Closed, Running, Paused, Draining, Ended, Failed };
  struct Census {
    uint64_t generation = 0, queued = 0, returned = 0;
    uint64_t pauses = 0, resumes = 0, terminal_eos = 0;
  };
private:
  NativeDecoder backend;
  State state = State::Closed;
  Census census;
  uint64_t next_generation = 0;
  std::string error;
  bool Start(bool opened) {
    error.clear();
    if (!opened) { state = State::Failed; return false; }
    census = {}; census.generation = ++next_generation;
    state = State::Running; return true;
  }
  GstSample* Fail(Result& result, const char* why, GstSample* sample = nullptr) {
    if (sample) gst_sample_unref(sample);
    error = why; state = State::Failed; result = Result::Error; return nullptr;
  }
public:
  bool Open(unsigned width, unsigned height, unsigned fps_n, unsigned fps_d,
            const uint8_t* extra, size_t extra_size) {
    Close(); return Start(backend.Open(width, height, fps_n, fps_d, extra, extra_size));
  }
  bool SetPaused(bool paused) {
    if (paused && state == State::Running) {
      state = State::Paused; ++census.pauses;
    } else if (!paused && state == State::Paused) {
      state = State::Running; ++census.resumes;
    }
    // Pausing an already terminal stream never reopens it or sends more EOS.
    return state == State::Running || state == State::Paused;
  }
  Submit Push(const uint8_t* data, size_t size, double pts, double dts, double duration) {
    if (state == State::Paused) return Submit::Retry; // consumed nothing
    if (state != State::Running) return Submit::Error;
    const Submit result = backend.Push(data, size, pts, dts, duration);
    if (result == Submit::Accepted) ++census.queued;
    else if (result == Submit::Error) state = State::Failed;
    return result;
  }
  GstSample* Next(bool complete_demux_input, Result& result) {
    result = Result::Error;
    if (state == State::Failed || state == State::Closed) return nullptr;
    if (state == State::Ended) { result = Result::Eof; return nullptr; }
    // A pause/DRAIN is never EOF. The caller must prove finite demux EOF and
    // delivery of every demuxed packet. Defer even that terminal request while
    // input is paused; output servicing itself remains allowed.
    if (complete_demux_input && state == State::Running) {
      state = State::Draining; ++census.terminal_eos;
    }
    GstSample* sample = backend.Next(state == State::Draining, result);
    if (sample) {
      if (result != Result::Picture || census.returned >= census.queued)
        return Fail(result, "picture without an accepted source AU", sample);
      ++census.returned; return sample;
    }
    if (result == Result::Error) state = State::Failed;
    else if (result == Result::Eof) {
      if (state != State::Draining || census.returned != census.queued)
        return Fail(result, "terminal EOS before all accepted originals returned");
      state = State::Ended;
    }
    return nullptr;
  }
  void Close() { backend.Close(); state = State::Closed; }
  State Current() const { return state; }
  const Census& Counts() const { return census; }
  const std::string& Error() const { return error.empty() ? backend.Error() : error; }
#ifdef CRYSTALHD_KODI_TEST
  bool OpenRawTest(unsigned width, unsigned height) {
    Close(); return Start(backend.OpenRawTest(width, height));
  }
#endif
};
}
