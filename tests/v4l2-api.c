// SPDX-License-Identifier: GPL-2.0-or-later
/* Real standard-ioctl/MMAP smoke; usable from ILP32 or LP64 without FFmpeg. */
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned checks;

static void require(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", what, errno, strerror(errno));
        exit(EXIT_FAILURE);
    }
    checks++;
}

static int call(int fd, unsigned long request, void *arg)
{
    int rc;
    do { rc = ioctl(fd, request, arg); } while (rc < 0 && errno == EINTR);
    return rc;
}

static void checked(int fd, unsigned long request, void *arg, const char *what)
{
    require(call(fd, request, arg) == 0, what);
}

static void rejected(int fd, unsigned long request, void *arg, int error,
                     const char *what)
{
    int rc = call(fd, request, arg);
    require(rc == -1 && errno == error, what);
}

static struct v4l2_format format(int fd, enum v4l2_buf_type type)
{
    struct v4l2_format fmt = { .type = type };
    checked(fd, VIDIOC_G_FMT, &fmt, "G_FMT");
    require(fmt.fmt.pix.pixelformat == (type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
            V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_YUYV), "queue format");
    require(fmt.fmt.pix.field == V4L2_FIELD_NONE, "progressive queue field");
    require(fmt.fmt.pix.width && fmt.fmt.pix.height && fmt.fmt.pix.sizeimage,
            "nonzero queue geometry and size");
    return fmt;
}

static void enumerate(int fd, enum v4l2_buf_type type)
{
    struct v4l2_fmtdesc fmt = { .type = type };
    checked(fd, VIDIOC_ENUM_FMT, &fmt, "ENUM_FMT index 0");
    require(fmt.pixelformat == (type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
            V4L2_PIX_FMT_H264 : V4L2_PIX_FMT_YUYV), "sole advertised fourcc");
    require(fmt.flags == (type == V4L2_BUF_TYPE_VIDEO_OUTPUT ?
            V4L2_FMT_FLAG_COMPRESSED : 0), "narrow format flags");
    require(fmt.description[0] != '\0', "format description");
    fmt.index = 1;
    rejected(fd, VIDIOC_ENUM_FMT, &fmt, EINVAL, "no second format");
}

static void unsupported_memory(int fd, enum v4l2_buf_type type)
{
    const unsigned memories[] = { V4L2_MEMORY_USERPTR, V4L2_MEMORY_DMABUF };
    for (unsigned i = 0; i < sizeof(memories) / sizeof(memories[0]); i++) {
        struct v4l2_requestbuffers req = {
            .count = 2, .type = type, .memory = memories[i]
        };
        rejected(fd, VIDIOC_REQBUFS, &req, EINVAL, "unsupported memory rejected");
    }
}

static void buffers(int fd, enum v4l2_buf_type type)
{
    struct v4l2_format fmt = format(fd, type);
    struct v4l2_requestbuffers req = {
        .count = 2, .type = type, .memory = V4L2_MEMORY_MMAP
    };
    checked(fd, VIDIOC_REQBUFS, &req, "MMAP REQBUFS");
    require(req.count >= 2 && req.count <= 8, "bounded requested buffer count");
    require(req.capabilities & V4L2_BUF_CAP_SUPPORTS_MMAP, "MMAP supported");
    require(!(req.capabilities & (V4L2_BUF_CAP_SUPPORTS_USERPTR |
            V4L2_BUF_CAP_SUPPORTS_DMABUF | V4L2_BUF_CAP_SUPPORTS_REQUESTS)),
            "no unsupported memory/request capabilities");
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {
            .type = type, .memory = V4L2_MEMORY_MMAP, .index = i
        };
        checked(fd, VIDIOC_QUERYBUF, &buf, "QUERYBUF");
        require(buf.index == i && buf.type == type &&
                buf.memory == V4L2_MEMORY_MMAP, "QUERYBUF identity");
        require(buf.length >= fmt.fmt.pix.sizeimage && buf.length,
                "QUERYBUF allocation size");
        require(!(buf.flags & (V4L2_BUF_FLAG_QUEUED | V4L2_BUF_FLAG_DONE |
                V4L2_BUF_FLAG_ERROR)), "fresh buffer flags");
        require(buf.timestamp.tv_sec == 0 && buf.timestamp.tv_usec == 0,
                "fresh buffer timestamp ABI");
        unsigned char *map = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                 MAP_SHARED, fd, (off_t)buf.m.offset);
        require(map != MAP_FAILED, "MMAP via returned queue offset");
        map[0] = (unsigned char)(0x5aU + i);
        map[buf.length - 1] = (unsigned char)(0xa5U - i);
        require(map[0] == (unsigned char)(0x5aU + i) &&
                map[buf.length - 1] == (unsigned char)(0xa5U - i),
                "mapped allocation writable at both ends");
        require(munmap(map, buf.length) == 0, "munmap");
    }
    struct v4l2_buffer invalid = {
        .type = type, .memory = V4L2_MEMORY_MMAP, .index = req.count
    };
    rejected(fd, VIDIOC_QUERYBUF, &invalid, EINVAL, "out-of-range QUERYBUF");
    rejected(fd, VIDIOC_S_FMT, &fmt, EBUSY, "S_FMT with allocated queue");
    struct v4l2_exportbuffer exp = { .type = type, .index = 0 };
    rejected(fd, VIDIOC_EXPBUF, &exp, ENOTTY, "no DMABUF export ioctl");
}

static void release_buffers(int fd, enum v4l2_buf_type type)
{
    struct v4l2_requestbuffers req = { .type = type, .memory = V4L2_MEMORY_MMAP };
    checked(fd, VIDIOC_REQBUFS, &req, "release MMAP buffers");
    require(req.count == 0, "zero allocations after release");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        puts("usage: v4l2-api --device /dev/videoN\n"
             "Checks standard ioctls and MMAP only; no firmware, STREAMON or decode.");
        return 0;
    }
    if (argc != 3 || strcmp(argv[1], "--device") != 0) {
        fputs("usage: v4l2-api --device /dev/videoN\n", stderr);
        return 2;
    }
    printf("userspace_pointer_bits=%zu v4l2_buffer_size=%zu\n",
           sizeof(void *) * 8, sizeof(struct v4l2_buffer));
    int fd = open(argv[2], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    require(fd >= 0, "open explicitly selected video device");
    struct v4l2_capability cap = { 0 };
    checked(fd, VIDIOC_QUERYCAP, &cap, "QUERYCAP");
    require(strcmp((const char *)cap.driver, "crystalhd") == 0, "CrystalHD driver");
    require(strcmp((const char *)cap.card, "CrystalHD BCM70015 decoder") == 0,
            "BCM70015 card");
    require(strncmp((const char *)cap.bus_info, "PCI:", 4) == 0, "PCI bus identity");
    require(cap.capabilities & V4L2_CAP_DEVICE_CAPS, "device capability field");
    require(cap.device_caps == (V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING |
            V4L2_CAP_EXT_PIX_FORMAT),
            "single-plane M2M streaming only");
    struct v4l2_queryctrl minimum = { .id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE };
    checked(fd, VIDIOC_QUERYCTRL, &minimum, "minimum CAPTURE control");
    require(minimum.minimum == 2 && minimum.maximum == 2 &&
            minimum.default_value == 2 && (minimum.flags & V4L2_CTRL_FLAG_READ_ONLY),
            "fixed read-only minimum CAPTURE control");
    struct v4l2_control ctrl = { .id = minimum.id };
    checked(fd, VIDIOC_G_CTRL, &ctrl, "G_CTRL");
    require(ctrl.value == 2, "minimum CAPTURE value");
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    checked(fd, VIDIOC_SUBSCRIBE_EVENT, &sub, "subscribe source-change event");
    struct v4l2_event event = { 0 };
    int event_rc = call(fd, VIDIOC_DQEVENT, &event);
    require(event_rc == -1 && (errno == EAGAIN || errno == ENOENT),
            "no fabricated source event");
    checked(fd, VIDIOC_UNSUBSCRIBE_EVENT, &sub, "unsubscribe event");
    struct v4l2_decoder_cmd command = { .cmd = V4L2_DEC_CMD_STOP };
    checked(fd, VIDIOC_TRY_DECODER_CMD, &command, "TRY_DECODER_CMD");
    checked(fd, VIDIOC_DECODER_CMD, &command, "STOP on idle queues is a no-op");
    const enum v4l2_buf_type types[] = {
        V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_BUF_TYPE_VIDEO_CAPTURE
    };
    for (unsigned i = 0; i < 2; i++) {
        enumerate(fd, types[i]);
        unsupported_memory(fd, types[i]);
        struct v4l2_format fmt = format(fd, types[i]);
        checked(fd, VIDIOC_TRY_FMT, &fmt, "TRY_FMT");
        checked(fd, VIDIOC_S_FMT, &fmt, "S_FMT without allocations");
    }
    struct v4l2_format multi = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    rejected(fd, VIDIOC_G_FMT, &multi, EINVAL, "no multiplanar CAPTURE");
    multi.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rejected(fd, VIDIOC_G_FMT, &multi, EINVAL, "no multiplanar OUTPUT");
    buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    struct v4l2_format out = format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    rejected(fd, VIDIOC_S_FMT, &out, EBUSY, "OUTPUT format cannot resize allocated CAPTURE");
    buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    release_buffers(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    release_buffers(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    out = format(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    checked(fd, VIDIOC_S_FMT, &out, "format usable after allocation release");
    require(close(fd) == 0, "close");
    printf("PASS: %u real API checks; no decoding performed\n", checks);
    return 0;
}
