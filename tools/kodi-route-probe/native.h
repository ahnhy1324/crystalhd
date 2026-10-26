/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <string>
#include "packed.h"

namespace crystalhd_kodi {
/* Reuse the project's qualified decoder for CPB admission, NoCopy leases,
 * cancellation and recovery. This first adapter copies RX -> Gst -> Kodi;
 * it does not implement a new DIL caller or claim zero-copy. */
class NativeDecoder {
  GstElement* pipeline = nullptr;
  GstAppSrc* input = nullptr;   // borrowed until Close()
  GstAppSink* output = nullptr;
  GstBus* bus = nullptr;
  bool draining = false, failed = false;
  std::string error;
  bool CheckError();
  bool Build(GstCaps* caps, bool raw_test);
public:
  enum class Submit { Accepted, Retry, Error };
  enum class Result { Picture, NeedInput, Wait, Eof, Error };
  NativeDecoder() = default;
  NativeDecoder(const NativeDecoder&) = delete;
  NativeDecoder& operator=(const NativeDecoder&) = delete;
  ~NativeDecoder() { Close(); }
  bool Open(unsigned width, unsigned height, unsigned fps_n, unsigned fps_d,
            const uint8_t* extra, size_t extra_size);
  Submit Push(const uint8_t* data, size_t size, double pts, double dts,
              double duration);
  GstSample* Next(bool drain, Result& result);
  void Close();
  const std::string& Error() const { return error; }
#ifdef CRYSTALHD_KODI_TEST
  /* Test executable only: real appsrc/appsink transport with raw pixels,
   * never a hardware decoder or a production decoder selection option. */
  bool OpenRawTest(unsigned width, unsigned height);
  guint64 QueuedTest() const { return input ? gst_app_src_get_current_level_buffers(input) : 0; }
#endif
};
}
