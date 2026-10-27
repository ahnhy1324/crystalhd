/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <atomic>
#include <cstdint>

namespace crystalhd_kodi {
/* InputStream owns demux EOF; VideoCodec's DRAIN flag only requests available
 * pictures and is also used during ordinary input starvation. A finite FIFO
 * can be ended only after every demuxed video packet reached this codec. */
struct InputProgress {
  std::atomic<uint64_t> epoch{1}, packets{0};
  std::atomic<bool> eof{false}, failed{false}, closed{false};
  std::atomic<bool> pause_input{false}; // explicit private admission control
  std::atomic<int> demux_speed{1000};
  void SetSpeed(int speed) {
    // Kodi reports presentation/demux speed independently of already queued
    // codec packets. Blocking those packets can prevent the host from ever
    // processing Resume. A speed notification is not our pause_input API.
    demux_speed.store(speed);
  }
  void Seek() {
    eof.store(false); packets.store(0); failed.store(false);
    epoch.fetch_add(1);
  }
  bool Final(uint64_t decoder_epoch, uint64_t accepted) const {
    return epoch.load() == decoder_epoch && eof.load() &&
           packets.load() == accepted && epoch.load() == decoder_epoch;
  }
};
}
