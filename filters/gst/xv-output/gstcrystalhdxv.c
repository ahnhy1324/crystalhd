/* SPDX-License-Identifier: MIT */
/* Optional Xv output. Completion retires X-server SHM use, not scanout.
 * Each element owns a connection, bounded images and an event reader. No
 * process exit, Xlib error handler, global display or firmware dependency. */
#define _GNU_SOURCE
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>
#include <gst/video/videooverlay.h>
#include <xcb/xcb.h>
#include <xcb/xcbext.h>
#include <xcb/shm.h>
#include <xcb/xv.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#define XV_UYVY UINT32_C(0x59565955)
#define XV_DEPTH 3
#define XV_MAX_IMAGES 12
#define XV_MAX_BYTES (128U * 1024U * 1024U)
#define XV_TIMEOUT_US G_USEC_PER_SEC
#define XV_MAX_CONTEXTS 4
#define XV_MAX_PIXELS (4096U * 2160U)

typedef struct XvContext XvContext;
typedef struct XvMemory XvMemory;
struct XvMemory {
  XvContext *context;
  XvMemory *next;
  int shmid;
  guint32 segment;
  guint width, height, pitch, offset, size;
  guint64 generation;
  guint8 *data;
  gboolean server_owned, dispose, context_owned;
};
typedef struct {
  XvMemory *memory;
  GstBuffer *buffer;
  guint32 sequence;
  guint64 generation, epoch;
  GstClockTime pts;
  gint64 submitted;
} XvFlight;
struct XvContext {
  gint references, stopping, flushing;
  GMutex lock;
  GCond changed;
  xcb_connection_t *connection;
  xcb_window_t window;
  xcb_gcontext_t gc;
  xcb_xv_port_t port;
  guint width, height, opcode, completion_type;
  xcb_atom_t delete_atom, colorspace_atom;
  gint original_colorspace, active_colorspace;
  gboolean own_window, failed, closed, detached, quarantined;
  const char *failure;
  int wake_fd;
  GThread *thread;
  GstBus *bus;
  XvMemory *images, *copies[XV_DEPTH];
  XvFlight flights[XV_DEPTH];
  guint image_count, image_bytes, pending;
  guint64 submitted, acknowledged, terminal, copied_bytes, generation, epoch;
  guint64 allocated, freed, stale, pool_submitted;
};
/* Bounds contexts retained after an unproved detach; no unbounded error loop. */
static gint live_contexts;

static XvContext *context_ref(XvContext *c)
{
  g_atomic_int_inc(&c->references);
  return c;
}
static void context_unref(XvContext *c)
{
  if (!g_atomic_int_dec_and_test(&c->references)) return;
  if (c->bus) gst_object_unref(c->bus);
  if (c->wake_fd >= 0) close(c->wake_fd);
  g_cond_clear(&c->changed);
  g_mutex_clear(&c->lock);
  g_atomic_int_add(&live_contexts, -1);
  g_free(c);
}
static void wake_context(XvContext *c)
{
  uint64_t one = 1;
  if (c->wake_fd >= 0) {
    ssize_t n;
    do n = write(c->wake_fd, &one, sizeof(one)); while (n < 0 && errno == EINTR);
    /* A full nonblocking eventfd already represents a pending wake. */
  }
}
/* Caller holds lock. No Gst unref or application callback under this lock. */
static gboolean context_error(XvContext *c, const char *reason)
{
  if (!c->failed) { c->failed = TRUE; c->failure = reason; }
  g_cond_broadcast(&c->changed);
  wake_context(c);
  return FALSE;
}
static gpointer reply_locked(XvContext *c, guint sequence)
{
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US;
  if (!c->connection || xcb_flush(c->connection) <= 0) {
    context_error(c, "X11 transport flush failed"); return NULL;
  }
  for (;;) {
    gpointer reply = NULL;
    xcb_generic_error_t *error = NULL;
    if (xcb_poll_for_reply(c->connection, sequence, &reply, &error)) {
      if (error || !reply) {
        free(error); free(reply);
        context_error(c, "X11 request was rejected"); return NULL;
      }
      return reply;
    }
    gint64 left = end - g_get_monotonic_time();
    if (xcb_connection_has_error(c->connection) || left <= 0) {
      xcb_discard_reply(c->connection, sequence);
      context_error(c, "X11 reply unavailable or timed out"); return NULL;
    }
    struct pollfd fds[2] = {
      {xcb_get_file_descriptor(c->connection), POLLIN, 0},
      {c->wake_fd, POLLIN, 0}};
    int result = poll(fds, 2, (int)((left + 999) / 1000));
    if (result < 0 && errno == EINTR) continue;
    if (result < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
      context_error(c, "X11 reply transport failed"); return NULL;
    }
    if (fds[1].revents & POLLIN) {
      uint64_t value;
      ssize_t ignored = read(c->wake_fd, &value, sizeof(value)); (void)ignored;
    }
  }
}
static xcb_atom_t atom_locked(XvContext *c, const char *name)
{
  xcb_intern_atom_reply_t *r = reply_locked(c,
      xcb_intern_atom(c->connection, FALSE, strlen(name), name).sequence);
  if (!r) return XCB_NONE;
  xcb_atom_t atom = r->atom; free(r); return atom;
}
static gboolean barrier_locked(XvContext *c)
{
  gpointer r = reply_locked(c, xcb_get_input_focus(c->connection).sequence);
  if (!r) return FALSE;
  free(r); return TRUE;
}
static gboolean open_locked(XvContext *c, const char *name, xcb_window_t window)
{
  int screen_number;
  c->connection = xcb_connect(name, &screen_number);
  if (!c->connection || xcb_connection_has_error(c->connection))
    return context_error(c, "A local X11 display is unavailable");
  xcb_screen_iterator_t screens = xcb_setup_roots_iterator(xcb_get_setup(c->connection));
  for (int n = 0; n < screen_number && screens.rem; n++) xcb_screen_next(&screens);
  if (!screens.rem) return context_error(c, "X11 screen is unavailable");
  xcb_screen_t *screen = screens.data;
  xcb_query_extension_reply_t *extension = reply_locked(c,
      xcb_query_extension(c->connection, 7, "MIT-SHM").sequence);
  if (!extension) return FALSE;
  gboolean available = extension->present;
  c->completion_type = extension->first_event + XCB_SHM_COMPLETION;
  free(extension);
  if (!available) return context_error(c, "MIT-SHM is unavailable; select the ordinary video output");
  extension = reply_locked(c, xcb_query_extension(c->connection, 6, "XVideo").sequence);
  if (!extension) return FALSE;
  available = extension->present; c->opcode = extension->major_opcode;
  free(extension);
  if (!available) return context_error(c, "XVideo is unavailable; select the ordinary video output");
  xcb_xv_query_adaptors_reply_t *adaptors = reply_locked(c,
      xcb_xv_query_adaptors(c->connection, screen->root).sequence);
  if (!adaptors) return FALSE;
  xcb_xv_adaptor_info_iterator_t a = xcb_xv_query_adaptors_info_iterator(adaptors);
  for (; a.rem && !c->port && !c->failed; xcb_xv_adaptor_info_next(&a)) {
    if (!(a.data->type & XCB_XV_TYPE_IMAGE_MASK)) continue;
    for (guint j = 0; j < a.data->num_ports && !c->port && !c->failed; j++) {
      xcb_xv_port_t candidate = a.data->base_id + j;
      xcb_xv_list_image_formats_reply_t *formats = reply_locked(c,
          xcb_xv_list_image_formats(c->connection, candidate).sequence);
      if (!formats) break;
      gboolean found = FALSE;
      xcb_xv_image_format_info_t *f = xcb_xv_list_image_formats_format(formats);
      for (int k = 0; k < xcb_xv_list_image_formats_format_length(formats); k++)
        if (f[k].id == XV_UYVY) found = TRUE;
      free(formats);
      if (!found) continue;
      xcb_xv_grab_port_reply_t *r = reply_locked(c,
          xcb_xv_grab_port(c->connection, candidate, XCB_CURRENT_TIME).sequence);
      if (!r) break;
      if (r->result == XCB_GRAB_STATUS_SUCCESS) c->port = candidate;
      free(r);
    }
  }
  free(adaptors);
  if (!c->port) return context_error(c, "No free UYVY XVideo port");
  c->colorspace_atom = XCB_NONE;
  xcb_xv_query_port_attributes_reply_t *attributes = reply_locked(c,
      xcb_xv_query_port_attributes(c->connection, c->port).sequence);
  if (!attributes) return FALSE;
  xcb_xv_attribute_info_iterator_t it = xcb_xv_query_port_attributes_attributes_iterator(attributes);
  for (; it.rem; xcb_xv_attribute_info_next(&it)) {
    const char *s = xcb_xv_attribute_info_name(it.data);
    guint length = xcb_xv_attribute_info_name_length(it.data);
    if ((it.data->flags & XCB_XV_ATTRIBUTE_FLAG_SETTABLE) &&
        (it.data->flags & XCB_XV_ATTRIBUTE_FLAG_GETTABLE) &&
        length >= 13 && (length == 13 || s[13] == '\0') &&
        it.data->min <= 0 && it.data->max >= 1) {
      if (!memcmp(s, "XV_COLORSPACE", 13)) c->colorspace_atom = atom_locked(c, "XV_COLORSPACE");
      if (!memcmp(s, "XV_ITURBT_709", 13)) c->colorspace_atom = atom_locked(c, "XV_ITURBT_709");
    }
  }
  free(attributes);
  if (c->colorspace_atom) {
    xcb_xv_get_port_attribute_reply_t *r = reply_locked(c,
        xcb_xv_get_port_attribute(c->connection, c->port, c->colorspace_atom).sequence);
    if (!r) return FALSE;
    c->original_colorspace = c->active_colorspace = r->value; free(r);
  }
  c->own_window = window == XCB_NONE;
  c->window = window ? window : xcb_generate_id(c->connection);
  c->width = MIN(screen->width_in_pixels, 960);
  c->height = MIN(screen->height_in_pixels, 540);
  if (c->own_window) {
    guint32 values[] = {screen->black_pixel, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c->connection, XCB_COPY_FROM_PARENT, c->window, screen->root,
        0, 0, c->width, c->height, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
        XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values);
    const char title[] = "CrystalHD video output";
    xcb_change_property(c->connection, XCB_PROP_MODE_REPLACE, c->window,
        XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, sizeof(title) - 1, title);
    c->delete_atom = atom_locked(c, "WM_DELETE_WINDOW");
    xcb_atom_t protocols = atom_locked(c, "WM_PROTOCOLS");
    xcb_change_property(c->connection, XCB_PROP_MODE_REPLACE, c->window,
        protocols, XCB_ATOM_ATOM, 32, 1, &c->delete_atom);
    xcb_map_window(c->connection, c->window);
  } else {
    guint32 mask = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    xcb_change_window_attributes(c->connection, c->window, XCB_CW_EVENT_MASK, &mask);
    xcb_get_geometry_reply_t *geometry = reply_locked(c,
        xcb_get_geometry(c->connection, c->window).sequence);
    if (!geometry) return FALSE;
    c->width = geometry->width; c->height = geometry->height; free(geometry);
  }
  c->gc = xcb_generate_id(c->connection);
  xcb_create_gc(c->connection, c->gc, c->window, 0, NULL);
  return barrier_locked(c);
}

static gboolean detached_memory(XvMemory *m)
{
  struct shmid_ds status;
  return shmctl(m->shmid, IPC_STAT, &status) == 0 && status.shm_nattch == 1;
}
static void unlink_memory_locked(XvContext *c, XvMemory *m)
{
  XvMemory **link = &c->images;
  while (*link && *link != m) link = &(*link)->next;
  if (!*link) { context_error(c, "Unknown shared image retirement"); return; }
  *link = m->next; c->image_count--; c->image_bytes -= m->size; c->freed++;
  g_cond_broadcast(&c->changed);
}
/* The registry no longer references m. Caller releases context refs unlocked. */
static void destroy_memory(XvMemory *m)
{
  shmdt(m->data); XvContext *c = m->context; g_free(m); context_unref(c);
}
static void dispose_memory_locked(XvContext *c, XvMemory *m)
{
  if (m->dispose) return;
  m->dispose = TRUE;
  if (c->connection && !c->failed) {
    xcb_shm_detach(c->connection, m->segment);
    if (xcb_flush(c->connection) <= 0) context_error(c, "Shared image detach flush failed");
  }
  wake_context(c);
}
static void memory_free(gpointer data)
{
  XvMemory *m = data; XvContext *c = m->context;
  g_mutex_lock(&c->lock);
  m->context_owned = TRUE;
  if (m->server_owned) context_error(c, "Shared image lost its Gst owner while submitted");
  dispose_memory_locked(c, m);
  gboolean ready = !m->server_owned && c->detached;
  if (ready) unlink_memory_locked(c, m);
  g_mutex_unlock(&c->lock);
  if (ready) destroy_memory(m);
}
static XvMemory *allocate_locked(XvContext *c, guint width, guint height)
{
  if (c->failed || c->closed || !c->connection) return NULL;
  xcb_xv_query_image_attributes_reply_t *r = reply_locked(c,
      xcb_xv_query_image_attributes(c->connection, c->port, XV_UYVY, width, height).sequence);
  if (!r) return NULL;
  gboolean valid = r->width == width && r->height == height && r->num_planes == 1 &&
      r->data_size && r->data_size <= XV_MAX_BYTES &&
      xcb_xv_query_image_attributes_pitches_length(r) == 1 &&
      xcb_xv_query_image_attributes_offsets_length(r) == 1;
  guint pitch = valid ? xcb_xv_query_image_attributes_pitches(r)[0] : 0;
  guint offset = valid ? xcb_xv_query_image_attributes_offsets(r)[0] : 0;
  valid = valid && pitch >= width * 2 && offset <= r->data_size &&
      (guint64)offset + (guint64)(height - 1) * pitch + width * 2 <= r->data_size;
  gint64 deadline = g_get_monotonic_time() + XV_TIMEOUT_US;
  while (valid && !c->failed && !g_atomic_int_get(&c->stopping) &&
      (c->image_count >= XV_MAX_IMAGES || r->data_size > XV_MAX_BYTES - c->image_bytes))
    if (!g_cond_wait_until(&c->changed, &c->lock, deadline)) break;
  if (!valid || c->failed || g_atomic_int_get(&c->stopping) ||
      c->image_count >= XV_MAX_IMAGES || r->data_size > XV_MAX_BYTES - c->image_bytes) {
    free(r); context_error(c, "Unsupported Xv layout or bounded image capacity exhausted"); return NULL;
  }
  XvMemory *m = g_new0(XvMemory, 1);
  m->shmid = shmget(IPC_PRIVATE, r->data_size, IPC_CREAT | 0600);
  if (m->shmid < 0) { free(r); g_free(m); context_error(c, "Cannot allocate Xv shared memory"); return NULL; }
  m->data = shmat(m->shmid, NULL, 0);
  if (m->data == (void *)-1) {
    shmctl(m->shmid, IPC_RMID, NULL); free(r); g_free(m);
    context_error(c, "Cannot attach Xv shared memory"); return NULL;
  }
  m->context = context_ref(c); m->width = width; m->height = height;
  m->pitch = pitch; m->offset = offset; m->size = r->data_size;
  m->context_owned = TRUE; m->segment = xcb_generate_id(c->connection);
  free(r);
  m->next = c->images; c->images = m; c->image_count++; c->image_bytes += m->size; c->allocated++;
  xcb_shm_attach(c->connection, m->segment, m->shmid, TRUE);
  if (shmctl(m->shmid, IPC_RMID, NULL) || !barrier_locked(c)) {
    context_error(c, "Cannot establish Xv shared memory attachment"); return NULL;
  }
  memset(m->data, 0, m->size);
  return m;
}
static gboolean matches_locked(XvContext *c, const XvFlight *f,
                                const xcb_shm_completion_event_t *event, guint sequence)
{
  return f->memory && f->memory->server_owned && !(event->response_type & 0x80) &&
      (event->response_type & 0x7f) == c->completion_type &&
      event->drawable == c->window && event->shmseg == f->memory->segment &&
      event->offset == 0 && event->major_event == c->opcode &&
      event->minor_event == XCB_XV_SHM_PUT_IMAGE && sequence == f->sequence &&
      f->generation == f->memory->generation;
}
static void retire_locked(XvContext *c, XvFlight *f, gboolean ack, GstBuffer **release, guint *n)
{
  f->memory->server_owned = FALSE;
  if (f->buffer) release[(*n)++] = f->buffer;
  memset(f, 0, sizeof(*f)); c->pending--;
  if (ack) c->acknowledged++; else c->terminal++;
  g_cond_broadcast(&c->changed);
}
static void receive_locked(XvContext *c, GstBuffer **release, guint *n)
{
  xcb_generic_event_t *raw;
  while (!c->failed && (raw = xcb_poll_for_event(c->connection))) {
    guint type = raw->response_type & 0x7f;
    if (!type) context_error(c, "XVideo protocol request failed");
    else if (type == c->completion_type) {
      XvFlight *owner = NULL;
      for (guint i = 0; i < XV_DEPTH; i++)
        if (matches_locked(c, &c->flights[i], (const void *)raw, raw->full_sequence)) {
          if (owner) { context_error(c, "Ambiguous shared image completion"); break; }
          owner = &c->flights[i];
        }
      if (!owner) { c->stale++; context_error(c, "Stale or mismatched shared image completion"); }
      else if (g_get_monotonic_time() - owner->submitted > XV_TIMEOUT_US)
        context_error(c, "Shared image completion exceeded its deadline");
      else if (!c->failed) retire_locked(c, owner, TRUE, release, n);
    } else if (type == XCB_CONFIGURE_NOTIFY) {
      const xcb_configure_notify_event_t *e = (const void *)raw;
      if (e->window == c->window) { c->width = e->width; c->height = e->height; }
    } else if (type == XCB_DESTROY_NOTIFY ||
        (type == XCB_CLIENT_MESSAGE && ((xcb_client_message_event_t *)raw)->data.data32[0] == c->delete_atom))
      context_error(c, "The video output window was closed");
    free(raw);
  }
  if (xcb_connection_has_error(c->connection)) context_error(c, "X11 display disconnected");
  for (guint i = 0; i < XV_DEPTH; i++)
    if (c->flights[i].memory &&
        g_get_monotonic_time() - c->flights[i].submitted > XV_TIMEOUT_US)
      context_error(c, "Missing shared image completion");
}
static gpointer event_reader(gpointer user)
{
  XvContext *c = user;
  gboolean posted = FALSE;
  gint64 disconnect_time = 0;
  for (;;) {
    GstBuffer *release[XV_DEPTH]; guint n = 0;
    XvMemory *garbage[XV_MAX_IMAGES]; guint ng = 0;
    gboolean done = FALSE, post = FALSE;
    g_mutex_lock(&c->lock);
    if (c->connection && !c->failed) receive_locked(c, release, &n);
    if (c->failed && !posted) { posted = TRUE; post = TRUE; }
    if (c->failed || g_atomic_int_get(&c->stopping)) {
      if (c->connection) {
        if (!c->failed && c->colorspace_atom && c->active_colorspace != c->original_colorspace) {
          xcb_xv_set_port_attribute(c->connection, c->port, c->colorspace_atom, c->original_colorspace);
          barrier_locked(c);
        }
        xcb_disconnect(c->connection); c->connection = NULL;
        disconnect_time = g_get_monotonic_time();
      }
      gboolean detached = TRUE;
      for (XvMemory *m = c->images; m; m = m->next)
        if (!detached_memory(m)) detached = FALSE;
      if (detached) {
        c->detached = TRUE; c->quarantined = FALSE;
        for (guint i = 0; i < XV_DEPTH; i++) {
          if (c->flights[i].memory) retire_locked(c, &c->flights[i], FALSE, release, &n);
          if (c->copies[i]) { c->copies[i]->dispose = TRUE; c->copies[i] = NULL; }
        }
        for (XvMemory *m = c->images; m; m = m->next)
          if (m->context_owned) m->dispose = TRUE;
        done = TRUE;
      }
      if (detached || g_get_monotonic_time() - disconnect_time >= XV_TIMEOUT_US) {
        c->closed = TRUE; c->quarantined = !detached;
        g_cond_broadcast(&c->changed);
      }
    }
    XvMemory *m = c->images;
    while (m) {
      XvMemory *next = m->next;
      if (m->context_owned && m->dispose && !m->server_owned &&
          (c->detached || detached_memory(m))) {
        unlink_memory_locked(c, m); garbage[ng++] = m;
      }
      m = next;
    }
    int fd = c->connection ? xcb_get_file_descriptor(c->connection) : -1;
    gint timeout = c->connection ? (c->pending ? 50 : -1) : 100;
    for (m = c->images; m; m = m->next)
      if (m->dispose) timeout = 10;
    const char *failure = c->failure;
    g_mutex_unlock(&c->lock);
    for (guint i = 0; i < n; i++) gst_buffer_unref(release[i]);
    for (guint i = 0; i < ng; i++) destroy_memory(garbage[i]);
    if (post && c->bus) {
      GError *error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_WRITE, failure);
      gst_bus_post(c->bus, gst_message_new_error(NULL, error, "CrystalHD Xv output; no successful EOS inferred"));
      g_error_free(error);
    }
    if (done) break;
    struct pollfd fds[2] = {{fd, POLLIN, 0}, {c->wake_fd, POLLIN, 0}};
    int result = poll(fds, 2, timeout);
    if (result < 0 && errno == EINTR) continue;
    if (fds[1].revents & POLLIN) {
      uint64_t value; ssize_t ignored = read(c->wake_fd, &value, sizeof(value)); (void)ignored;
    }
    if (result < 0 || (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
      g_mutex_lock(&c->lock); context_error(c, "X11 event transport failed"); g_mutex_unlock(&c->lock);
    }
  }
  context_unref(c);
  return NULL;
}
static XvContext *context_new(const char *name, xcb_window_t window, GstBus *bus, GError **error)
{
  const char *selected = name ? name : g_getenv("DISPLAY");
  if (!selected || !g_regex_match_simple("^(:|unix/:)[0-9]+(\\.[0-9]+)?$", selected, 0, 0)) {
    g_set_error_literal(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ_WRITE,
        "A local Unix X11 DISPLAY is required for shared-memory video output"); return NULL;
  }
  gint old;
  do {
    old = g_atomic_int_get(&live_contexts);
    if (old >= XV_MAX_CONTEXTS) {
      g_set_error_literal(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_BUSY,
          "Xv output context limit reached; a prior connection may still be quarantined"); return NULL;
    }
  } while (!g_atomic_int_compare_and_exchange(&live_contexts, old, old + 1));
  XvContext *c = g_new0(XvContext, 1);
  c->references = 1; c->wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  g_mutex_init(&c->lock); g_cond_init(&c->changed);
  c->bus = bus ? gst_object_ref(bus) : NULL;
  g_mutex_lock(&c->lock);
  gboolean opened = c->wake_fd >= 0 && open_locked(c, selected, window);
  g_mutex_unlock(&c->lock);
  if (!opened) {
    g_set_error_literal(error, GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ_WRITE,
        c->failure ? c->failure : "Cannot allocate the Xv event wake descriptor");
    if (c->connection) { xcb_disconnect(c->connection); c->connection = NULL; }
    context_unref(c); return NULL;
  }
  c->thread = g_thread_new("crystalhd-xv", event_reader, context_ref(c));
  return c;
}
static gboolean drain_locked(XvContext *c)
{
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US;
  while (c->pending && !c->failed && !g_atomic_int_get(&c->flushing) &&
      !g_atomic_int_get(&c->stopping)) {
    if (!g_cond_wait_until(&c->changed, &c->lock, end))
      return context_error(c, "Output drain exceeded its completion deadline");
  }
  return !c->failed && !g_atomic_int_get(&c->flushing) && !g_atomic_int_get(&c->stopping);
}
static gboolean colorspace_locked(XvContext *c, const GstVideoInfo *info)
{
  gint desired = info->colorimetry.matrix == GST_VIDEO_COLOR_MATRIX_BT709 ? 1 : 0;
  if (info->colorimetry.range == GST_VIDEO_COLOR_RANGE_0_255 ||
      (info->colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT709 &&
       info->colorimetry.matrix != GST_VIDEO_COLOR_MATRIX_BT601))
    return context_error(c, "Xv output requires limited-range BT.601 or BT.709 UYVY");
  if (!c->colorspace_atom && desired)
    return context_error(c, "This Xv adaptor has no supported BT.709 selection");
  if (c->colorspace_atom && c->active_colorspace != desired) {
    xcb_xv_set_port_attribute(c->connection, c->port, c->colorspace_atom, desired);
    if (!barrier_locked(c)) return FALSE;
    c->active_colorspace = desired;
  }
  return TRUE;
}
static GstFlowReturn submit_buffer(XvContext *c, const GstVideoInfo *info,
                                   GstBuffer *buffer, XvMemory *direct)
{
  GstVideoFrame frame;
  if (!gst_video_frame_map(&frame, info, buffer, GST_MAP_READ)) return GST_FLOW_ERROR;
  g_mutex_lock(&c->lock);
  gint64 end = g_get_monotonic_time() + XV_TIMEOUT_US;
  XvFlight *f = NULL; guint index = 0;
  while (!c->failed && !g_atomic_int_get(&c->flushing) && !g_atomic_int_get(&c->stopping)) {
    for (index = 0; index < XV_DEPTH; index++)
      if (!c->flights[index].memory) { f = &c->flights[index]; break; }
    if (f) break;
    if (!g_cond_wait_until(&c->changed, &c->lock, end)) {
      context_error(c, "No completed Xv ring slot within deadline"); break;
    }
  }
  GstFlowReturn result = GST_FLOW_ERROR;
  if (g_atomic_int_get(&c->flushing) || g_atomic_int_get(&c->stopping)) result = GST_FLOW_FLUSHING;
  else if (!c->failed && f && c->connection) {
    XvMemory *m = direct;
    if (!m) {
      m = c->copies[index];
      if (!m || m->width != (guint)info->width || m->height != (guint)info->height) {
        if (m) dispose_memory_locked(c, m);
        c->copies[index] = m = allocate_locked(c, info->width, info->height);
      }
    }
    gboolean valid = m && !m->server_owned && !m->dispose && m->context == c &&
        m->width == (guint)info->width && m->height == (guint)info->height;
    if (direct) valid = valid && GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) == m->data + m->offset &&
        GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) == (gint)m->pitch;
    if (valid) {
      if (!direct) {
        for (gint y = 0; y < info->height; y++)
          memcpy(m->data + m->offset + (gsize)y * m->pitch,
              (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) +
                  (gssize)y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), info->width * 2);
        c->copied_bytes += (guint64)info->width * info->height * 2;
      }
      guint ratio_n, ratio_d;
      valid = gst_video_calculate_display_ratio(&ratio_n, &ratio_d, info->width, info->height,
          info->par_n, info->par_d, 1, 1);
      if (valid) {
        guint dw = c->width, dh = c->height;
        if ((guint64)dw * ratio_d > (guint64)dh * ratio_n)
          dw = (guint64)dh * ratio_n / ratio_d;
        else dh = (guint64)dw * ratio_d / ratio_n;
        guint32 sequence = xcb_xv_shm_put_image(c->connection, c->port, c->window, c->gc,
            m->segment, XV_UYVY, 0, 0, 0, info->width, info->height,
            (c->width - dw) / 2, (c->height - dh) / 2, MAX(dw, 1), MAX(dh, 1),
            info->width, info->height, TRUE).sequence;
        m->generation = ++c->generation;
        *f = (XvFlight){.memory = m, .buffer = direct ? gst_buffer_ref(buffer) : NULL,
            .sequence = sequence, .generation = m->generation, .epoch = c->epoch,
            .pts = GST_BUFFER_PTS(buffer), .submitted = g_get_monotonic_time()};
        m->server_owned = TRUE; c->pending++; c->submitted++;
        if (direct) c->pool_submitted++;
        if (xcb_flush(c->connection) > 0) result = GST_FLOW_OK;
        else context_error(c, "Xv image submission flush failed");
        wake_context(c);
      }
    }
    if (!valid) context_error(c, "Invalid Xv image layout or ownership at submission");
  }
  g_mutex_unlock(&c->lock);
  gst_video_frame_unmap(&frame);
  return result;
}

/* Pool ownership is independent of X-server completion. Holding the actual
 * GstBuffer until ACK prevents the parent pool from recycling writable pixels;
 * an application's remaining references can delay recycling further. */
typedef struct { GstBufferPool parent; XvContext *context;
  GstVideoInfo info; guint size; } CrystalXvPool;
typedef struct { GstBufferPoolClass parent; } CrystalXvPoolClass;
G_DEFINE_TYPE(CrystalXvPool, crystal_xv_pool, GST_TYPE_BUFFER_POOL)
static GQuark memory_key(void) { return g_quark_from_static_string("crystalhd-xv-shm-memory"); }
static XvMemory *buffer_memory(GstBuffer *buffer)
{
  return gst_buffer_n_memory(buffer) == 1 ?
      gst_mini_object_get_qdata(GST_MINI_OBJECT(gst_buffer_peek_memory(buffer, 0)), memory_key()) : NULL;
}
static const gchar **pool_options(GstBufferPool *pool)
{
  (void)pool; static const gchar *options[] = {GST_BUFFER_POOL_OPTION_VIDEO_META, NULL}; return options;
}
static gboolean pool_config(GstBufferPool *base, GstStructure *config)
{
  CrystalXvPool *p = (CrystalXvPool *)base;
  GstCaps *caps; guint size, minimum, maximum; GstVideoInfo info;
  if (!gst_buffer_pool_config_get_params(config, &caps, &size, &minimum, &maximum) ||
      !caps || !gst_video_info_from_caps(&info, caps) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_UYVY ||
      info.width != p->info.width || info.height != p->info.height || size > p->size ||
      minimum > XV_DEPTH + 3 || !maximum || maximum > XV_DEPTH + 3 ||
      !gst_buffer_pool_config_has_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META)) return FALSE;
  return GST_BUFFER_POOL_CLASS(crystal_xv_pool_parent_class)->set_config(base, config);
}
static GstFlowReturn pool_allocate(GstBufferPool *base, GstBuffer **out, GstBufferPoolAcquireParams *params)
{
  (void)params; CrystalXvPool *p = (CrystalXvPool *)base; XvContext *c = p->context;
  g_mutex_lock(&c->lock);
  XvMemory *m = allocate_locked(c, p->info.width, p->info.height);
  if (m && m->size != p->size) { dispose_memory_locked(c, m); m = NULL; }
  if (m) m->context_owned = FALSE;
  g_mutex_unlock(&c->lock);
  if (!m) { *out = NULL; return GST_FLOW_ERROR; }
  GstMemory *memory = gst_memory_new_wrapped(0, m->data, m->size, 0, m->size, m, memory_free);
  gst_mini_object_set_qdata(GST_MINI_OBJECT(memory), memory_key(), m, NULL);
  GstBuffer *buffer = gst_buffer_new(); gst_buffer_append_memory(buffer, memory);
  gsize offsets[GST_VIDEO_MAX_PLANES] = {m->offset};
  gint strides[GST_VIDEO_MAX_PLANES] = {(gint)m->pitch};
  if (!gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_UYVY,
      m->width, m->height, 1, offsets, strides)) {
    gst_buffer_unref(buffer); *out = NULL; return GST_FLOW_ERROR;
  }
  *out = buffer; return GST_FLOW_OK;
}
static void pool_finalize(GObject *object)
{
  XvContext *c = ((CrystalXvPool *)object)->context;
  G_OBJECT_CLASS(crystal_xv_pool_parent_class)->finalize(object);
  if (c) context_unref(c);
}
static void crystal_xv_pool_class_init(CrystalXvPoolClass *klass)
{
  GstBufferPoolClass *base = GST_BUFFER_POOL_CLASS(klass);
  base->get_options = pool_options; base->set_config = pool_config; base->alloc_buffer = pool_allocate;
  G_OBJECT_CLASS(klass)->finalize = pool_finalize;
}
static void crystal_xv_pool_init(CrystalXvPool *pool) { (void)pool; }

typedef struct { GstVideoSink parent; GMutex lock; XvContext *context;
  GstVideoInfo info; gchar *display; guintptr window; GstStructure *last_stats; } GstCrystalHdXv;
typedef struct { GstVideoSinkClass parent; } GstCrystalHdXvClass;
static void overlay_init(GstVideoOverlayInterface *interface);
G_DEFINE_TYPE_WITH_CODE(GstCrystalHdXv, gst_crystalhd_xv, GST_TYPE_VIDEO_SINK,
    G_IMPLEMENT_INTERFACE(GST_TYPE_VIDEO_OVERLAY, overlay_init))
#define XV_SINK(o) ((GstCrystalHdXv *)(o))
enum { PROP_ZERO, PROP_DISPLAY, PROP_STATS };
static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK,
    GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw,format=UYVY,width=[2,4096],height=[1,2160],"
      "framerate=[0/1,2147483647/1],interlace-mode=progressive,colorimetry={bt601,bt709}"));
static XvContext *sink_context(GstCrystalHdXv *s)
{
  g_mutex_lock(&s->lock);
  XvContext *c = s->context ? context_ref(s->context) : NULL;
  g_mutex_unlock(&s->lock); return c;
}
static GstStructure *stats_locked(XvContext *c)
{
  return gst_structure_new("crystalhd-xv-statistics",
      "submitted", G_TYPE_UINT64, c->submitted, "acknowledged", G_TYPE_UINT64, c->acknowledged,
      "terminal-retired", G_TYPE_UINT64, c->terminal, "copied-bytes", G_TYPE_UINT64, c->copied_bytes,
      "pool-submitted", G_TYPE_UINT64, c->pool_submitted, "pending", G_TYPE_UINT, c->pending,
      "images", G_TYPE_UINT, c->image_count, "bytes", G_TYPE_UINT, c->image_bytes,
      "allocated", G_TYPE_UINT64, c->allocated, "freed", G_TYPE_UINT64, c->freed,
      "failed", G_TYPE_BOOLEAN, c->failed, "closed", G_TYPE_BOOLEAN, c->closed,
      "quarantined", G_TYPE_BOOLEAN, c->quarantined, NULL);
}
static gboolean sink_start(GstBaseSink *base)
{
  GstCrystalHdXv *s = XV_SINK(base);
  gst_video_overlay_prepare_window_handle(GST_VIDEO_OVERLAY(s));
  GstBus *bus = gst_element_get_bus(GST_ELEMENT(s));
  GError *error = NULL;
  XvContext *c = context_new(s->display, s->window, bus, &error);
  if (bus) gst_object_unref(bus);
  if (!c) {
    GST_ELEMENT_ERROR(s, RESOURCE, OPEN_READ_WRITE, ("%s", error->message), (NULL));
    g_error_free(error); return FALSE;
  }
  g_mutex_lock(&s->lock); s->context = c; g_mutex_unlock(&s->lock);
  return TRUE;
}
static gboolean sink_stop(GstBaseSink *base)
{
  GstCrystalHdXv *s = XV_SINK(base);
  g_mutex_lock(&s->lock); XvContext *c = s->context; s->context = NULL; g_mutex_unlock(&s->lock);
  if (!c) return TRUE;
  g_atomic_int_set(&c->stopping, TRUE); wake_context(c);
  g_mutex_lock(&c->lock);
  while (!c->closed) g_cond_wait(&c->changed, &c->lock);
  GstStructure *snapshot = stats_locked(c);
  g_mutex_unlock(&c->lock);
  /* The reader owns its own context reference; quarantine cannot retain the
   * element or abort the application. A later positive detach reaps mappings. */
  g_thread_unref(c->thread); c->thread = NULL;
  g_mutex_lock(&s->lock);
  if (s->last_stats) gst_structure_free(s->last_stats);
  s->last_stats = snapshot; g_mutex_unlock(&s->lock);
  context_unref(c); return TRUE;
}
static gboolean sink_unlock(GstBaseSink *base)
{
  XvContext *c = sink_context(XV_SINK(base));
  if (c) {
    g_atomic_int_set(&c->flushing, TRUE); wake_context(c);
    g_mutex_lock(&c->lock); g_cond_broadcast(&c->changed); g_mutex_unlock(&c->lock);
    context_unref(c);
  }
  return TRUE;
}
static gboolean sink_unlock_stop(GstBaseSink *base)
{
  XvContext *c = sink_context(XV_SINK(base));
  if (c) { g_atomic_int_set(&c->flushing, FALSE); wake_context(c); context_unref(c); }
  return TRUE;
}
static gboolean sink_set_caps(GstBaseSink *base, GstCaps *caps)
{
  GstCrystalHdXv *s = XV_SINK(base); GstVideoInfo info;
  if (!gst_video_info_from_caps(&info, caps) || GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_UYVY ||
      info.width <= 0 || info.width > 4096 || info.width % 2 || info.height <= 0 ||
      info.height > 2160 || (guint64)info.width * info.height > XV_MAX_PIXELS ||
      GST_VIDEO_INFO_IS_INTERLACED(&info)) return FALSE;
  XvContext *c = sink_context(s);
  if (!c) return FALSE;
  g_mutex_lock(&c->lock);
  gboolean ok = drain_locked(c) && colorspace_locked(c, &info);
  if (ok) { s->info = info; c->epoch++; }
  g_mutex_unlock(&c->lock); context_unref(c);
  return ok;
}
static GstFlowReturn sink_show_frame(GstVideoSink *video, GstBuffer *buffer)
{
  GstCrystalHdXv *s = XV_SINK(video); XvContext *c = sink_context(s);
  if (!c) return GST_FLOW_FLUSHING;
  XvMemory *m = buffer_memory(buffer);
  if (m && m->context != c) m = NULL;
  GstFlowReturn flow = submit_buffer(c, &s->info, buffer, m);
  context_unref(c); return flow;
}
static gboolean sink_propose_allocation(GstBaseSink *base, GstQuery *query)
{
  GstCaps *caps; gboolean need_pool; GstVideoInfo info;
  gst_query_parse_allocation(query, &caps, &need_pool);
  if (!caps || !gst_video_info_from_caps(&info, caps) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_UYVY) return FALSE;
  gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
  if (!need_pool) return TRUE;
  XvContext *c = sink_context(XV_SINK(base));
  if (!c) return FALSE;
  g_mutex_lock(&c->lock);
  xcb_xv_query_image_attributes_reply_t *r = c->connection && !c->failed ? reply_locked(c,
      xcb_xv_query_image_attributes(c->connection, c->port, XV_UYVY, info.width, info.height).sequence) : NULL;
  guint size = r ? r->data_size : 0;
  gboolean valid = r && r->width == info.width && r->height == info.height && r->num_planes == 1 &&
      size && size <= XV_MAX_BYTES / (XV_DEPTH + 3);
  free(r); g_mutex_unlock(&c->lock);
  if (!valid) { context_unref(c); return FALSE; }
  CrystalXvPool *pool = g_object_new(crystal_xv_pool_get_type(), NULL);
  pool->context = c; pool->info = info; pool->size = size;
  GstStructure *config = gst_buffer_pool_get_config(GST_BUFFER_POOL(pool));
  gst_buffer_pool_config_set_params(config, caps, size, XV_DEPTH + 3, XV_DEPTH + 3);
  gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
  gboolean ok = gst_buffer_pool_set_config(GST_BUFFER_POOL(pool), config);
  if (ok) gst_query_add_allocation_pool(query, GST_BUFFER_POOL(pool), size, XV_DEPTH + 3, XV_DEPTH + 3);
  gst_object_unref(pool); return ok;
}
static GstFlowReturn sink_wait_event(GstBaseSink *base, GstEvent *event)
{
  GstFlowReturn result = GST_BASE_SINK_CLASS(gst_crystalhd_xv_parent_class)->wait_event(base, event);
  if (result != GST_FLOW_OK || GST_EVENT_TYPE(event) != GST_EVENT_EOS) return result;
  XvContext *c = sink_context(XV_SINK(base));
  if (!c) return GST_FLOW_FLUSHING;
  g_mutex_lock(&c->lock); gboolean ok = drain_locked(c);
  result = ok ? GST_FLOW_OK : (c->failed ? GST_FLOW_ERROR : GST_FLOW_FLUSHING);
  g_mutex_unlock(&c->lock); context_unref(c); return result;
}
static gboolean sink_event(GstBaseSink *base, GstEvent *event)
{
  if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
    XvContext *c = sink_context(XV_SINK(base));
    if (c) {
      g_mutex_lock(&c->lock); gboolean ok = drain_locked(c);
      if (ok) c->epoch++;
      g_mutex_unlock(&c->lock); context_unref(c);
      if (!ok) { gst_event_unref(event); return FALSE; }
    }
  }
  return GST_BASE_SINK_CLASS(gst_crystalhd_xv_parent_class)->event(base, event);
}
static void sink_set_window(GstVideoOverlay *overlay, guintptr window)
{
  GstCrystalHdXv *s = XV_SINK(overlay);
  g_mutex_lock(&s->lock);
  if (!s->context && window <= G_MAXUINT32) s->window = window;
  else GST_WARNING_OBJECT(s, "Set the window handle before starting Xv output");
  g_mutex_unlock(&s->lock);
}
static void overlay_init(GstVideoOverlayInterface *interface) { interface->set_window_handle = sink_set_window; }
static void sink_set_property(GObject *object, guint id, const GValue *value, GParamSpec *spec)
{
  GstCrystalHdXv *s = XV_SINK(object);
  if (id == PROP_DISPLAY) {
    g_mutex_lock(&s->lock);
    if (!s->context) { g_free(s->display); s->display = g_value_dup_string(value); }
    else GST_WARNING_OBJECT(s, "Display changes take effect only while output is stopped");
    g_mutex_unlock(&s->lock);
  } else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void sink_get_property(GObject *object, guint id, GValue *value, GParamSpec *spec)
{
  GstCrystalHdXv *s = XV_SINK(object);
  if (id == PROP_DISPLAY) {
    g_mutex_lock(&s->lock); g_value_set_string(value, s->display); g_mutex_unlock(&s->lock);
  } else if (id == PROP_STATS) {
    XvContext *c = sink_context(s);
    if (c) {
      g_mutex_lock(&c->lock); g_value_take_boxed(value, stats_locked(c)); g_mutex_unlock(&c->lock);
      context_unref(c);
    } else {
      g_mutex_lock(&s->lock); g_value_set_boxed(value, s->last_stats); g_mutex_unlock(&s->lock);
    }
  } else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
static void sink_finalize(GObject *object)
{
  GstCrystalHdXv *s = XV_SINK(object);
  sink_stop(GST_BASE_SINK(s));
  g_free(s->display);
  if (s->last_stats) gst_structure_free(s->last_stats);
  g_mutex_clear(&s->lock);
  G_OBJECT_CLASS(gst_crystalhd_xv_parent_class)->finalize(object);
}
static void gst_crystalhd_xv_class_init(GstCrystalHdXvClass *klass)
{
  GObjectClass *object = G_OBJECT_CLASS(klass);
  GstElementClass *element = GST_ELEMENT_CLASS(klass);
  GstBaseSinkClass *base = GST_BASE_SINK_CLASS(klass);
  object->set_property = sink_set_property; object->get_property = sink_get_property;
  object->finalize = sink_finalize;
  g_object_class_install_property(object, PROP_DISPLAY, g_param_spec_string("display", "Display",
      "Local Unix X11 display; the DISPLAY environment is used when unset", NULL,
      G_PARAM_READWRITE | GST_PARAM_MUTABLE_READY | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(object, PROP_STATS, g_param_spec_boxed("xv-stats", "Xv statistics",
      "Submission/SHM ownership counters; not physical display statistics", GST_TYPE_STRUCTURE,
      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  gst_element_class_add_static_pad_template(element, &sink_template);
  gst_element_class_set_static_metadata(element, "CrystalHD completion-controlled Xv output",
      "Sink/Video", "Optional bounded Xv/SHM output with per-instance ownership", "CrystalHD contributors");
  base->start = sink_start; base->stop = sink_stop; base->set_caps = sink_set_caps;
  base->unlock = sink_unlock; base->unlock_stop = sink_unlock_stop;
  base->wait_event = sink_wait_event; base->event = sink_event;
  base->propose_allocation = sink_propose_allocation;
  GST_VIDEO_SINK_CLASS(klass)->show_frame = sink_show_frame;
}
static void gst_crystalhd_xv_init(GstCrystalHdXv *s)
{
  g_mutex_init(&s->lock); gst_video_info_init(&s->info);
  gst_base_sink_set_qos_enabled(GST_BASE_SINK(s), TRUE);
  /* Preroll still negotiates; display the first picture at its clock time. */
  g_object_set(s, "show-preroll-frame", FALSE, NULL);
}
static gboolean plugin_init(GstPlugin *plugin)
{
  return gst_element_register(plugin, "crystalhdxvsink", GST_RANK_NONE, gst_crystalhd_xv_get_type());
}
#ifndef PACKAGE
#define PACKAGE "crystalhd"
#endif
GST_PLUGIN_DEFINE(GST_VERSION_MAJOR, GST_VERSION_MINOR, crystalhdxv,
    "Optional completion-controlled CrystalHD Xv output", plugin_init, "1.3.0-dev.1",
    "MIT/X11", "CrystalHD", "https://github.com/ahnhy1324/crystalhd")
