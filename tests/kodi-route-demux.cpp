/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual InputStream callbacks versus container packets; no video decoding,
 * libcrystalhd, GStreamer pipeline, device or speaker access. */
#include "../tools/kodi-route-probe/probe.cpp"
extern "C" {
#include <libavcodec/avcodec.h>
}
static std::string fixture;
static std::vector<unsigned> expected_color;
static void quiet_log(KODI_ADDON_BACKEND_HDL, int, const char*) {}
static DEMUX_PACKET* allocate_packet(void*, int size) {
  auto* packet = new DEMUX_PACKET{};
  packet->pData = static_cast<uint8_t*>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
  return packet;
}
static void free_packet(void*, DEMUX_PACKET* packet) {
  av_free(packet->pData); delete packet;
}
static void demux() {
  AddonToKodiFuncTable_Addon global_callbacks{}; global_callbacks.addon_log_msg = quiet_log;
  AddonGlobalInterface global{}; global.toKodi = &global_callbacks;
  kodi::addon::CPrivateBase::m_interface = &global;
  KODI_ADDON_INSTANCE_FUNC functions{};
  KODI_ADDON_INSTANCE_INFO metadata{}; metadata.type = ADDON_INSTANCE_INPUTSTREAM;
  metadata.version = "3.4.0"; metadata.id = "test";
  KodiToAddonFuncTable_InputStream callbacks{};
  AddonToKodiFuncTable_InputStream host{};
  host.allocate_demux_packet = allocate_packet; host.free_demux_packet = free_packet;
  AddonInstance_InputStream input{}; input.toAddon = &callbacks; input.toKodi = &host;
  KODI_ADDON_INSTANCE_STRUCT instance{};
  instance.info = &metadata; instance.functions = &functions; instance.inputstream = &input;
  {
    kodi::addon::IInstanceInfo info(&instance); ProbeStream stream(info);
    INPUTSTREAM_PROPERTY props{}; props.m_strURL = fixture.c_str();
    g_assert_true(callbacks.open(&input, &props));
    std::vector<unsigned> ids; g_assert_true(stream.GetStreamIds(ids));
    g_assert_cmpuint(ids.size(), ==, 2); g_assert_cmpuint(ids[0], ==, 0); g_assert_cmpuint(ids[1], ==, 1);
    kodi::addon::InputstreamInfo audio;
    g_assert_true(stream.GetStream(1, audio));
    const std::string codec_name = audio.GetCodecName();
    g_assert_cmpstr(codec_name.c_str(), ==, "aac");
    g_assert_cmpuint(audio.GetFeatures() & INPUTSTREAM_FEATURE_DECODE, ==, 0);
    INPUTSTREAM_TIMES times{}; g_assert_true(callbacks.get_times(&input, &times));
    AVFormatContext* reference = nullptr;
    g_assert_cmpint(avformat_open_input(&reference, fixture.c_str(), av_find_input_format("mov"), nullptr), ==, 0);
    const int v = av_find_best_stream(reference, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    kodi::addon::InputstreamInfo video_info; g_assert_true(stream.GetStream(0, video_info));
    const auto* color = reference->streams[v]->codecpar;
    if (!expected_color.empty()) {
      g_assert_cmpuint(video_info.GetColorSpace(), ==, expected_color[0]);
      g_assert_cmpuint(video_info.GetColorPrimaries(), ==, expected_color[1]);
      g_assert_cmpuint(video_info.GetColorTransferCharacteristic(), ==, expected_color[2]);
      g_assert_cmpuint(video_info.GetColorRange(), ==, expected_color[3]);
    } else if (color->color_primaries != AVCOL_PRI_UNSPECIFIED)
      g_assert_cmpuint(video_info.GetColorPrimaries(), ==, color->color_primaries);
    if (expected_color.empty() && color->color_trc != AVCOL_TRC_UNSPECIFIED)
      g_assert_cmpuint(video_info.GetColorTransferCharacteristic(), ==, color->color_trc);
    AVPacket* original = av_packet_alloc(); unsigned counts[2]{}; int64_t last_end = 0;
    auto compare = [&] {
      DEMUX_PACKET* packet = stream.DemuxRead(); g_assert_nonnull(packet);
      g_assert_cmpint(packet->iSize, ==, original->size);
      g_assert_cmpint(std::memcmp(packet->pData, original->data, original->size), ==, 0);
      const AVStream* s = reference->streams[original->stream_index];
      const unsigned id = s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? 0 : 1;
      g_assert_cmpint(packet->iStreamId, ==, id); ++counts[id];
      const AVRational to{1, STREAM_TIME_BASE};
      g_assert_cmpfloat(packet->pts, ==, av_rescale_q(original->pts, s->time_base, to));
      g_assert_cmpfloat(packet->dts, ==, av_rescale_q(original->dts, s->time_base, to));
      g_assert_cmpfloat(packet->duration, ==, av_rescale_q(original->duration, s->time_base, to));
      last_end = std::max(last_end, av_rescale_q(original->pts + original->duration, s->time_base, to));
      free_packet(nullptr, packet);
    };
    while (av_read_frame(reference, original) >= 0) { compare(); av_packet_unref(original); }
    g_assert_null(stream.DemuxRead()); g_assert_cmpuint(counts[0], >, 0); g_assert_cmpuint(counts[1], >, 0);
    g_assert_cmpfloat(times.ptsEnd, ==, last_end);
    double start = 0; g_assert_true(stream.DemuxSeekTime(500, true, start));
    g_assert_cmpfloat(start, ==, 500000);
    const int video = av_find_best_stream(reference, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int64_t seek = av_rescale_q(500000, AVRational{1, AV_TIME_BASE}, reference->streams[video]->time_base);
    g_assert_cmpint(av_seek_frame(reference, video, seek, AVSEEK_FLAG_BACKWARD), >=, 0);
    g_assert_cmpint(av_read_frame(reference, original), >=, 0); compare();
    av_packet_free(&original); avformat_close_input(&reference); stream.Close(); stream.Close();
  }
  kodi::addon::CPrivateBase::m_interface = nullptr;
}
int main(int argc, char** argv) {
  if ((argc != 2 && argc != 6) || argv[1][0] != '/') return 2;
  for (int i = 2; i < argc; ++i) expected_color.push_back(std::stoul(argv[i]));
  fixture = argv[1]; int test_argc = 1; g_test_init(&test_argc, &argv, nullptr);
  g_test_add_func("/kodi/demux/audio-video-timebases-seek", demux);
  const int result = g_test_run(); gst_deinit(); return result;
}
