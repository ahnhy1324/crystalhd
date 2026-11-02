// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise the real plugin in NULL only: no Worker, X11, DRM or GPU is opened.
#include <limits>
#include "../filters/gst/field-output/gstcrystalhdfield.cpp"

static GstCrystalhdField *element() {
    auto *sink=static_cast<GstCrystalhdField *>(g_object_new(gst_crystalhd_field_get_type(),nullptr));
    gst_object_ref_sink(sink);
    g_assert_null(sink->state->worker.get());
    return sink;
}

static GstCaps *caps(GstVideoFormat format=GST_VIDEO_FORMAT_YUY2,
                     unsigned width=1280,unsigned height=720,
                     GstVideoColorMatrix matrix=GST_VIDEO_COLOR_MATRIX_BT709) {
    GstVideoInfo info;gst_video_info_init(&info);
    g_assert_true(gst_video_info_set_format(&info,format,width,height));
    info.fps_n=25;info.fps_d=1;info.par_n=1;info.par_d=1;
    info.colorimetry.range=GST_VIDEO_COLOR_RANGE_16_235;
    info.colorimetry.matrix=matrix;
    info.colorimetry.transfer=GST_VIDEO_TRANSFER_BT709;
    info.colorimetry.primaries=GST_VIDEO_COLOR_PRIMARIES_BT709;
    return gst_video_info_to_caps(&info);
}

static void test_caps() {
    auto *sink=element();
    for(auto matrix:{GST_VIDEO_COLOR_MATRIX_BT601,GST_VIDEO_COLOR_MATRIX_BT709}) {
        auto *value=caps(GST_VIDEO_FORMAT_YUY2,1920,1088,matrix);
        g_assert_true(set_caps(GST_BASE_SINK(sink),value));gst_caps_unref(value);
        g_assert_cmpint(sink->state->info.width,==,1920);
        g_assert_cmpint(sink->state->info.colorimetry.matrix,==,matrix);
    }
    g_assert_null(sink->state->worker.get());gst_object_unref(sink);
}

static void test_caps_rejection() {
    auto *sink=element();auto *valid=caps();
    g_assert_true(set_caps(GST_BASE_SINK(sink),valid));gst_caps_unref(valid);
    for(unsigned mode=0;mode<6;mode++) {
        auto *value=caps(mode==0?GST_VIDEO_FORMAT_NV12:GST_VIDEO_FORMAT_YUY2,
                         mode==1?641:mode==2?1922:1280,mode==3?719:720);
        if(mode==4)gst_caps_set_simple(value,"colorimetry",G_TYPE_STRING,"1:3:5:1",nullptr);
        if(mode==5)gst_caps_set_simple(value,"framerate",GST_TYPE_FRACTION,0,1,nullptr);
        g_assert_false(set_caps(GST_BASE_SINK(sink),value));gst_caps_unref(value);
        g_assert_cmpint(sink->state->info.width,==,1280);
        g_assert_cmpint(sink->state->info.height,==,720);
    }
    g_assert_null(sink->state->worker.get());gst_object_unref(sink);
}

static void test_nv12_caps() {
    auto *sink=element();
    for(auto matrix:{GST_VIDEO_COLOR_MATRIX_BT601,GST_VIDEO_COLOR_MATRIX_BT709})
      for(const char *site:{"jpeg","mpeg2"})for(const char *mode:{"progressive","interleaved"}) {
        auto *value=caps(GST_VIDEO_FORMAT_NV12,1280,720,matrix);
        gst_caps_set_simple(value,"chroma-site",G_TYPE_STRING,site,"interlace-mode",G_TYPE_STRING,mode,nullptr);
        g_assert_true(set_caps(GST_BASE_SINK(sink),value));gst_caps_unref(value);
        g_assert_cmpint(GST_VIDEO_INFO_FORMAT(&sink->state->info),==,GST_VIDEO_FORMAT_NV12);
      }
    for(unsigned n=0;n<5;n++) {
        auto *value=caps(GST_VIDEO_FORMAT_NV12,1280,n==0?718:720);
        if(n!=1)gst_caps_set_simple(value,"chroma-site",G_TYPE_STRING,n==2?"dv":"mpeg2",nullptr);
        if(n==3)gst_caps_set_simple(value,"interlace-mode",G_TYPE_STRING,"mixed",nullptr);
        if(n==4)gst_caps_set_simple(value,"colorimetry",G_TYPE_STRING,"1:3:5:1",nullptr);
        g_assert_false(set_caps(GST_BASE_SINK(sink),value));gst_caps_unref(value);
        g_assert_cmpint(sink->state->info.height,==,720);
      }
    g_assert_null(sink->state->worker.get());gst_object_unref(sink);
}
static void test_plane_mapping() {
    std::array<guint8,96> bytes{};GstVideoFrame frame{};
    frame.map[0].data=bytes.data();frame.map[0].size=64;
    frame.map[1].data=bytes.data()+64;frame.map[1].size=32;
    frame.data[0]=bytes.data();frame.data[1]=bytes.data()+64;
    g_assert_cmpuint(crystalhd_field::mapped_plane_available(frame,0),==,64);
    g_assert_cmpuint(crystalhd_field::mapped_plane_available(frame,1),==,32);
    frame.map[0].size=80;
    g_assert_cmpuint(crystalhd_field::mapped_plane_available(frame,1),==,32);
    frame.data[1]=bytes.data()+96;
    bool rejected=false;try{crystalhd_field::mapped_plane_available(frame,1);}catch(const std::exception &){rejected=true;}
    g_assert_true(rejected);
}

static void test_times() {
    auto *sink=element();auto *value=caps();
    g_assert_true(set_caps(GST_BASE_SINK(sink),value));gst_caps_unref(value);
    auto *buffer=gst_buffer_new();GstClockTime first=0,last=0;
    GST_BUFFER_PTS(buffer)=10*GST_MSECOND;
    GST_BUFFER_DURATION(buffer)=20*GST_MSECOND;
    get_times(GST_BASE_SINK(sink),buffer,&first,&last);
    g_assert_cmpuint(first,==,10*GST_MSECOND);g_assert_cmpuint(last,==,30*GST_MSECOND);
    GST_BUFFER_DURATION(buffer)=GST_CLOCK_TIME_NONE;
    get_times(GST_BASE_SINK(sink),buffer,&first,&last);
    g_assert_cmpuint(last,==,50*GST_MSECOND);
    GST_BUFFER_PTS(buffer)=GST_CLOCK_TIME_NONE;
    get_times(GST_BASE_SINK(sink),buffer,&first,&last);
    g_assert_cmpuint(first,==,GST_CLOCK_TIME_NONE);g_assert_cmpuint(last,==,GST_CLOCK_TIME_NONE);
    GST_BUFFER_PTS(buffer)=GST_CLOCK_TIME_NONE-2;GST_BUFFER_DURATION(buffer)=3;
    get_times(GST_BASE_SINK(sink),buffer,&first,&last);
    g_assert_cmpuint(last,==,GST_CLOCK_TIME_NONE);
    gst_buffer_unref(buffer);gst_object_unref(sink);
}

static void test_target_clock() {
    Command command;command.base=100;command.latency=5;command.offset=10;
    GstClockTime target=0;
    g_assert_true(Worker::target_time(command,20,target));g_assert_cmpuint(target,==,135);
    command.offset=-50;
    g_assert_true(Worker::target_time(command,20,target));g_assert_cmpuint(target,==,100);
    command.offset=std::numeric_limits<gint64>::min();
    g_assert_true(Worker::target_time(command,20,target));g_assert_cmpuint(target,==,100);
    g_assert_false(Worker::target_time(command,GST_CLOCK_TIME_NONE,target));
    command.offset=0;command.base=GST_CLOCK_TIME_NONE-1;command.latency=0;
    g_assert_false(Worker::target_time(command,1,target));
    command.base=0;command.latency=GST_CLOCK_TIME_NONE-1;
    g_assert_false(Worker::target_time(command,1,target));
}

static void test_command_ownership() {
    auto *buffer=gst_buffer_new();
    auto *clock=GST_CLOCK(g_object_new(GST_TYPE_SYSTEM_CLOCK,nullptr));gst_object_ref_sink(clock);
    const auto buffers=GST_MINI_OBJECT_REFCOUNT_VALUE(buffer);
    const auto clocks=GST_OBJECT_REFCOUNT_VALUE(clock);
    {
        Command command;command.buffer=gst_buffer_ref(buffer);
        command.clock=GST_CLOCK(gst_object_ref(clock));
        g_assert_cmpint(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer),==,buffers+1);
        g_assert_cmpint(GST_OBJECT_REFCOUNT_VALUE(clock),==,clocks+1);
    }
    g_assert_cmpint(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer),==,buffers);
    g_assert_cmpint(GST_OBJECT_REFCOUNT_VALUE(clock),==,clocks);
    gst_buffer_unref(buffer);gst_object_unref(clock);
}

static void test_output_validation() {
    auto *sink=element();auto *base=GST_BASE_SINK(sink);
    auto *callbacks=GST_BASE_SINK_GET_CLASS(base);
    auto *buffer=gst_buffer_new();
    for(auto callback:{callbacks->render,callbacks->preroll}) {
        gst_segment_init(&base->segment,GST_FORMAT_TIME);
        GST_BUFFER_PTS(buffer)=GST_CLOCK_TIME_NONE;
        g_assert_cmpint(callback(base,buffer),==,GST_FLOW_NOT_NEGOTIATED);
        GST_BUFFER_PTS(buffer)=0;
        gst_segment_init(&base->segment,GST_FORMAT_BYTES);
        g_assert_cmpint(callback(base,buffer),==,GST_FLOW_NOT_NEGOTIATED);
        gst_segment_init(&base->segment,GST_FORMAT_TIME);
        for(double rate:{0.0,-1.0}) {
            base->segment.rate=rate;
            g_assert_cmpint(callback(base,buffer),==,GST_FLOW_NOT_NEGOTIATED);
        }
        base->segment.rate=1.0;
        g_assert_cmpint(callback(base,buffer),==,GST_FLOW_FLUSHING);
        g_assert_cmpint(GST_MINI_OBJECT_REFCOUNT_VALUE(buffer),==,1);
        g_assert_null(sink->state->worker.get());
    }
    gst_buffer_unref(buffer);gst_object_unref(sink);
}

static void test_null_lifetime() {
    auto *sink=element();GstStructure *stats=nullptr;
    g_object_get(sink,"field-stats",&stats,nullptr);guint64 count=1;
    for(const char *name:{"pictures","fields","failures","images","retained-buffers"}) {
        g_assert_true(gst_structure_get_uint64(stats,name,&count));g_assert_cmpuint(count,==,0);
    }
    gst_structure_free(stats);
    set_window(GST_VIDEO_OVERLAY(sink),123);expose(GST_VIDEO_OVERLAY(sink));
    g_assert_cmpuint(sink->state->window,==,123);g_assert_null(sink->state->worker.get());
    g_assert_false(gst_base_sink_is_qos_enabled(GST_BASE_SINK(sink)));
    g_assert_false(gst_base_sink_is_last_sample_enabled(GST_BASE_SINK(sink)));
    g_assert_cmpuint(gst_base_sink_get_render_delay(GST_BASE_SINK(sink)),==,15*GST_MSECOND);
    g_assert_cmpint(gst_base_sink_get_max_lateness(GST_BASE_SINK(sink)),==,20*GST_MSECOND);
    gst_object_unref(sink);
}

int main(int argc,char **argv) {
    gst_init(&argc,&argv);g_test_init(&argc,&argv,nullptr);
    g_test_add_func("/field/caps",test_caps);
    g_test_add_func("/field/caps-rejection",test_caps_rejection);
    g_test_add_func("/field/nv12-caps",test_nv12_caps);
    g_test_add_func("/field/plane-mapping",test_plane_mapping);
    g_test_add_func("/field/times",test_times);
    g_test_add_func("/field/clock",test_target_clock);
    g_test_add_func("/field/command-ownership",test_command_ownership);
    g_test_add_func("/field/output-validation",test_output_validation);
    g_test_add_func("/field/null-lifetime",test_null_lifetime);
    const int result=g_test_run();gst_deinit();return result;
}
