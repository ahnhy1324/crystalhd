/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal SDK host, no player/device: run our real callback implementation.
 * In particular the targeted host drains until VC_EOF even after VC_ERROR. */
#include "../tools/kodi-route-probe/probe.cpp"
#include <array>

static void quiet_log(KODI_ADDON_BACKEND_HDL, int, const char*) {}

static void callback_lifetime() {
  AddonToKodiFuncTable_Addon global_callbacks{};
  global_callbacks.addon_log_msg = quiet_log;
  AddonGlobalInterface global{}; global.toKodi = &global_callbacks;
  kodi::addon::CPrivateBase::m_interface = &global;
  KODI_ADDON_INSTANCE_FUNC functions{};
  KODI_ADDON_INSTANCE_INFO metadata{}; metadata.type = ADDON_INSTANCE_VIDEOCODEC;
  KodiToAddonFuncTable_VideoCodec callbacks{};
  AddonToKodiFuncTable_VideoCodec host{};
  AddonInstance_VideoCodec video{}; video.toAddon = &callbacks; video.toKodi = &host;
  KODI_ADDON_INSTANCE_STRUCT instance{};
  instance.info = &metadata; instance.functions = &functions; instance.videocodec = &video;
  {
    kodi::addon::IInstanceInfo info(&instance);
    NativeCodec codec(info, 30, 1);
    VIDEOCODEC_PICTURE picture{}; picture.flags = VIDEOCODEC_PICTURE_FLAG_DRAIN;
    /* A second host Reset/destructor must not spin forever on VC_BUFFER. */
    g_assert_cmpint(callbacks.get_picture(&video, &picture), ==, VC_EOF);
    callbacks.reset(&video);
    g_assert_cmpint(callbacks.get_picture(&video, &picture), ==, VC_EOF);
    /* No init data: Start rejects geometry before Gst/plugin/device access.
     * Keep the normal decode error, but explicitly retire an error drain. */
    DEMUX_PACKET bad{};
    g_assert_true(callbacks.add_data(&video, &bad));
    picture.flags = 0;
    g_assert_cmpint(callbacks.get_picture(&video, &picture), ==, VC_ERROR);
    picture.flags = VIDEOCODEC_PICTURE_FLAG_DRAIN;
    g_assert_cmpint(callbacks.get_picture(&video, &picture), ==, VC_EOF);
    g_assert_null(picture.videoBufferHandle);
    callbacks.reset(&video);
    g_assert_cmpint(callbacks.get_picture(&video, &picture), ==, VC_EOF);
  }
  kodi::addon::CPrivateBase::m_interface = nullptr;
}

static std::array<uint8_t, 16> backing;
static bool get_buffer(void*, VIDEOCODEC_PICTURE* picture) {
  picture->decodedData = backing.data(); picture->videoBufferHandle = &backing;
  return true;
}
static void release_buffer(void*, void*) {}

static void temporary_drain() {
  AddonToKodiFuncTable_Addon global_callbacks{}; global_callbacks.addon_log_msg = quiet_log;
  AddonGlobalInterface global{}; global.toKodi = &global_callbacks;
  kodi::addon::CPrivateBase::m_interface = &global;
  KODI_ADDON_INSTANCE_FUNC functions{};
  KODI_ADDON_INSTANCE_INFO metadata{}; metadata.type = ADDON_INSTANCE_VIDEOCODEC;
  KodiToAddonFuncTable_VideoCodec callbacks{};
  AddonToKodiFuncTable_VideoCodec host{}; host.get_frame_buffer = get_buffer; host.release_frame_buffer = release_buffer;
  AddonInstance_VideoCodec video{}; video.toAddon = &callbacks; video.toKodi = &host;
  KODI_ADDON_INSTANCE_STRUCT instance{};
  instance.info = &metadata; instance.functions = &functions; instance.videocodec = &video;
  {
    auto progress = std::make_shared<crystalhd_kodi::InputProgress>();
    kodi::addon::IInstanceInfo info(&instance); NativeCodec codec(info, 30, 1, progress);
    g_assert_true(codec.OpenRawTest());
    /* The parent can already have demuxed EOF while Kodi still has a packet
     * in its video queue. That is not yet the last submitted codec input. */
    progress->packets.store(2); progress->eof.store(true);
    std::array<uint8_t, 16> input{}; input.fill(73);
    DEMUX_PACKET packet{}; packet.pData = input.data(); packet.iSize = input.size();
    packet.pts = packet.dts = 0; packet.duration = 33333;
    g_assert_true(callbacks.add_data(&video, &packet));
    auto pull = [&](VIDEOCODEC_RETVAL expected) {
      const auto until = g_get_monotonic_time() + 3000000;
      while (true) {
        VIDEOCODEC_PICTURE picture{}; picture.flags = VIDEOCODEC_PICTURE_FLAG_DRAIN;
        const auto result = callbacks.get_picture(&video, &picture);
        if (result == expected) return picture.pts;
        g_assert_true(result == VC_NONE || result == VC_BUFFER || result == VC_EOF);
        g_assert_cmpint(g_get_monotonic_time(), <, until);
      }
    };
    g_assert_cmpint(pull(VC_PICTURE), ==, 0);
    for (auto byte : backing) g_assert_cmpuint(byte, ==, 73);
    pull(VC_EOF); // temporary phase only; firmware/appsrc must stay open
    input.fill(129); packet.pts = packet.dts = 33333;
#ifdef CRYSTALHD_KODI_PLAYBACK
    progress->paused.store(true);
    g_assert_false(callbacks.add_data(&video, &packet)); // no packet consumed
    pull(VC_EOF); // end temporary host squeeze only, never media EOF
    progress->paused.store(false);
#endif
    g_assert_true(callbacks.add_data(&video, &packet));
    g_assert_cmpint(pull(VC_PICTURE), ==, 33333);
    for (auto byte : backing) g_assert_cmpuint(byte, ==, 129);
    pull(VC_EOF); // actual finite input is now complete
    callbacks.reset(&video);
  }
  kodi::addon::CPrivateBase::m_interface = nullptr;
}

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/kodi/codec/inactive-error-drain-reset", callback_lifetime);
  g_test_add_func("/kodi/codec/temporary-drain-then-more-input", temporary_drain);
  const int result = g_test_run(); gst_deinit(); return result;
}
