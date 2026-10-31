/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Explicit, private MPEG-1 test input adapter; not an installed plugin.
 * Reuses the own GstVideoDecoder with a distinct GType and rank-NONE factory.
 * Ordinary crystalhddec caps/codec admission remain unchanged.
 *
 * External caps stay video/mpeg,mpegversion=1. Only the internal DIL subtype
 * is MPEG2VIDEO, whose unchanged setter selects BC_VID_ALGO_MPEG2 == 1.
 * This is library-drain's named --mpeg1-via-mpeg2 research route, not selector5.
 * No compressed bytes, caps, PTS, queues, QoS or deadlines are rewritten.
 *
 * Link this file, mpeg1-natural-playback.c and gstcrystalhd-finish-trace.c
 * against the exact operator-pinned DIL; do not rebuild/change that library.
 */
#include <gst/gst.h>
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd-codecs.h"

G_STATIC_ASSERT(BC_VID_ALGO_MPEG2 == 1);

static gboolean
research_mpeg1_codec_from_caps(GstCaps *caps, CrystalHdCodec *codec)
{
  const GstStructure *s;
  const gchar *interlace;
  gboolean parsed, systemstream;
  gint version, width, height;
  CrystalHdCodec ordinary;

  if (codec == NULL)
    return FALSE;
  memset(codec, 0, sizeof(*codec));
  codec->subtype = BC_MSUBTYPE_INVALID;
  if (caps == NULL || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1)
    return FALSE;
  /* Reference the ordinary reader but reject all its public codecs here:
   * this explicitly selected test factory admits MPEG-1 alone. */
  if (gst_crystalhd_codec_from_caps(caps, &ordinary))
    return FALSE;
  s = gst_caps_get_structure(caps, 0);
  interlace = gst_structure_get_string(s, "interlace-mode");
  if (!gst_structure_has_name(s, "video/mpeg") ||
      !gst_structure_get_int(s, "mpegversion", &version) || version != 1 ||
      !gst_structure_get_boolean(s, "systemstream", &systemstream) || systemstream ||
      !gst_structure_get_boolean(s, "parsed", &parsed) || !parsed ||
      !gst_structure_get_int(s, "width", &width) || width < 2 || width > 1920 ||
      !gst_structure_get_int(s, "height", &height) || height < 2 || height > 1088 ||
      (width & 1) || (height & 1) ||
      (interlace != NULL && !g_str_equal(interlace, "progressive")))
    return FALSE;

  memset(codec, 0, sizeof(*codec));
  codec->subtype = BC_MSUBTYPE_MPEG2VIDEO;
  /* No MPEG-2 profile/extension is invented. Existing damage predicates
   * require proven profile4/5 headers; unknown MPEG-1 state stays unknown. */
  return TRUE;
}

/* A separate GType prevents the installed public decoder from substituting
 * for the private factory, even if its registry metadata is visible. */
#define GST_CRYSTALHD_PRIVATE_MPEG1_RESEARCH 1
#define GstCrystalHdDec GstCrystalHdMpeg1ResearchDec
#define GstCrystalHdDecClass GstCrystalHdMpeg1ResearchDecClass
#define gst_crystalhd_dec gst_crystalhd_mpeg1_research_dec
#define gst_crystalhd_dec_get_type gst_crystalhd_mpeg1_research_dec_get_type
#define gst_crystalhd_dec_class_init gst_crystalhd_mpeg1_research_dec_class_init
#define gst_crystalhd_dec_init gst_crystalhd_mpeg1_research_dec_init
#define gst_crystalhd_dec_parent_class gst_crystalhd_mpeg1_research_dec_parent_class
#define gst_crystalhd_codec_from_caps research_mpeg1_codec_from_caps
#include "../filters/gst/gst-plugin-1.0/gstcrystalhd.c"
#undef gst_crystalhd_codec_from_caps
#undef gst_crystalhd_dec_parent_class
#undef gst_crystalhd_dec_init
#undef gst_crystalhd_dec_class_init
#undef gst_crystalhd_dec_get_type
#undef gst_crystalhd_dec
#undef GstCrystalHdDecClass
#undef GstCrystalHdDec
#undef GST_CRYSTALHD_PRIVATE_MPEG1_RESEARCH

gboolean
gst_crystalhd_mpeg1_research_register(void)
{
  const gchar *name = "crystalhdmpeg1researchdec";
  GType type = gst_crystalhd_mpeg1_research_dec_get_type();
  GstElementFactory *existing = gst_element_factory_find(name);
  if (existing != NULL) {
    gboolean same = gst_element_factory_get_element_type(existing) == type;
    gst_object_unref(existing);
    return same;
  }
  GST_DEBUG_CATEGORY_INIT(gst_crystalhd_debug, "crystalhdmpeg1research", 0,
                          "Private MPEG-1 algorithm-1 research input adapter");
  g_print("Research adapter: factory=%s rank=NONE source-mpegversion=1 "
          "transport-subtype=MPEG2VIDEO configured-algorithm=1 "
          "selector5-not-used public-support-unchanged=yes\n", name);
  return gst_element_register(NULL, name, GST_RANK_NONE, type);
}

/* No element is instantiated/driven to a state, and no DIL API is called.
 * The ordinary reader is the unmodified header function included before
 * this translation unit's private codec-admission remapping macro. */
gboolean
gst_crystalhd_mpeg1_research_self_test(void)
{
  static const gchar *const rejected[] = {
    "video/mpeg,mpegversion=2,systemstream=false,parsed=true,width=320,height=240,interlace-mode=progressive",
    "video/x-h264,stream-format=byte-stream,alignment=au,parsed=true,width=320,height=240",
    "video/mpeg,mpegversion=1,systemstream=true,parsed=true,width=320,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=false,width=320,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,width=320,height=240",
    "video/mpeg,mpegversion=1,parsed=true,width=320,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=0,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320,height=0",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=319,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320,height=239",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=1922,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320,height=1090",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=(string)320,height=240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320,height=(string)240",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320,height=240,interlace-mode=interleaved",
    "video/mpeg,mpegversion=1,systemstream=false,parsed=true,width=320,height=240,interlace-mode=mixed",
    "ANY", "EMPTY"
  };
  GstCaps *mpeg1 = gst_caps_from_string(
      "video/mpeg,mpegversion=1,systemstream=false,parsed=true,"
      "width=320,height=240,framerate=25/1,interlace-mode=progressive");
  GstCaps *original = gst_caps_copy(mpeg1);
  GstCaps *mpeg2 = gst_caps_from_string(rejected[0]);
  GstCaps *advertised = gst_static_pad_template_get_caps(&sink_template);
  CrystalHdCodec codec;
  gboolean ok = TRUE;
  GstElementFactory *factory;

#define REQUIRE_RESEARCH(condition, label) G_STMT_START { \
  if (!(condition)) { \
    g_printerr("MPEG-1 admission self-test failed: %s\n", (label)); \
    ok = FALSE; \
  } \
} G_STMT_END
  REQUIRE_RESEARCH(gst_crystalhd_mpeg1_research_register(), "private registration");
  factory = gst_element_factory_find("crystalhdmpeg1researchdec");
  REQUIRE_RESEARCH(factory != NULL, "exact factory exists");
  if (factory != NULL) {
    REQUIRE_RESEARCH(gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory)) ==
        GST_RANK_NONE, "rank NONE");
    REQUIRE_RESEARCH(gst_element_factory_get_element_type(factory) ==
        gst_crystalhd_mpeg1_research_dec_get_type(), "exact private GType");
    gst_object_unref(factory);
  }
  REQUIRE_RESEARCH(!gst_crystalhd_codec_from_caps(mpeg1, &codec),
                   "ordinary reader rejects MPEG-1");
  REQUIRE_RESEARCH(gst_crystalhd_codec_from_caps(mpeg2, &codec) &&
      codec.subtype == BC_MSUBTYPE_MPEG2VIDEO, "ordinary reader accepts MPEG-2");
  REQUIRE_RESEARCH(research_mpeg1_codec_from_caps(mpeg1, &codec) &&
      codec.subtype == BC_MSUBTYPE_MPEG2VIDEO, "private MPEG-1 algorithm-1 route");
  REQUIRE_RESEARCH(gst_caps_is_equal(mpeg1, original), "MPEG-1 caps unchanged");
  REQUIRE_RESEARCH(gst_caps_can_intersect(advertised, mpeg1) &&
      !gst_caps_can_intersect(advertised, mpeg2), "private advertised caps");
  for (guint i = 0; i < G_N_ELEMENTS(rejected); i++) {
    GstCaps *caps = gst_caps_from_string(rejected[i]);
    REQUIRE_RESEARCH(caps != NULL, "negative caps parse");
    REQUIRE_RESEARCH(!research_mpeg1_codec_from_caps(caps, &codec) &&
        codec.subtype == BC_MSUBTYPE_INVALID, rejected[i]);
    if (caps != NULL)
      gst_caps_unref(caps);
  }
  REQUIRE_RESEARCH(!research_mpeg1_codec_from_caps(NULL, &codec), "missing caps");
  REQUIRE_RESEARCH(!research_mpeg1_codec_from_caps(mpeg1, NULL), "missing destination");
  gst_caps_unref(advertised);
  gst_caps_unref(mpeg2);
  gst_caps_unref(original);
  gst_caps_unref(mpeg1);
#undef REQUIRE_RESEARCH
  g_print("MPEG-1 admission self-test: %s; ordinary reader/private route separate; "
          "caps immutable; %u negative caps; device calls=0\n",
          ok ? "PASS" : "FAIL", (guint)G_N_ELEMENTS(rejected));
  return ok;
}
