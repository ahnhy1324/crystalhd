/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include "native.h"
#include <memory>

namespace crystalhd_kodi {
class V4l2Decoder {
  struct Session;
  std::shared_ptr<Session> session;
  std::string error;
public:
  using Submit = NativeDecoder::Submit;
  using Result = NativeDecoder::Result;
  ~V4l2Decoder() { Close(); }
  bool Open(unsigned width, unsigned height, unsigned fps_n, unsigned fps_d,
            const uint8_t* extra, size_t extra_size);
  Submit Push(const uint8_t* data, size_t size, double pts, double dts, double duration);
  GstSample* Next(bool final_input, Result& result);
  void Close();
  const std::string& Error() const { return error; }
};
}
