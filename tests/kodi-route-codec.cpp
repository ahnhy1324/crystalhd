/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal SDK host, no player/device: run our real callback implementation.
 * In particular the targeted host drains until VC_EOF even after VC_ERROR. */
#include "../tools/kodi-route-probe/probe.cpp"

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

int main(int argc, char** argv) {
  g_test_init(&argc, &argv, nullptr);
  g_test_add_func("/kodi/codec/inactive-error-drain-reset", callback_lifetime);
  return g_test_run();
}
