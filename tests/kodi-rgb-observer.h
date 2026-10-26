/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Diagnostic target only. Relative files belong to its private working
 * directory. No file output is compiled into normal/native/trace targets. */
#pragma once
#include <cstdio>

static void observe_rgb_source(const VIDEOCODEC_PICTURE& picture, unsigned ordinal) {
  GChecksum* digest = g_checksum_new(G_CHECKSUM_SHA256);
  FILE* first = ordinal == 0 ? std::fopen("rgb-first.yuy2", "wb") : nullptr;
  for (unsigned y = 0; y < picture.height; ++y) {
    const auto* row = picture.decodedData + picture.planeOffsets[0] +
                      static_cast<size_t>(y) * picture.stride[0];
    g_checksum_update(digest, row, picture.width * 2);
    if (first && std::fwrite(row, picture.width * 2, 1, first) != 1) {
      std::fclose(first); first = nullptr;
    }
  }
  if (first) std::fclose(first);
  kodi::Log(ADDON_LOG_INFO, "CrystalHD-RGB-TEST ordinal=%u pts_us=%lld YUY2=%s",
            ordinal, static_cast<long long>(picture.pts), g_checksum_get_string(digest));
  g_checksum_free(digest);
}
