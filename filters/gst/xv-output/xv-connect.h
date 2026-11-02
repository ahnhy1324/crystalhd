/* SPDX-License-Identifier: MIT */
/* Local X11 establishment with a deadline. XCB still owns protocol setup;
 * the joined watchdog can only shut down a duplicate of our own socket. */
#ifndef CRYSTALHD_XV_CONNECT_H
#define CRYSTALHD_XV_CONNECT_H
#include <X11/Xauth.h>
#ifndef HASXDMAUTH
#define HASXDMAUTH 1
#endif
#include <X11/Xdmcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stddef.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

/* Separate consumers reserve disjoint high-half XDM client-ID ranges.
 * Header-local counters are not shared across plugins in one process. */
#ifndef CRYSTALHD_X11_NONCE_FIRST
#define CRYSTALHD_X11_NONCE_FIRST UINT32_C(0x80000000)
#endif
#ifndef CRYSTALHD_X11_NONCE_LAST
#define CRYSTALHD_X11_NONCE_LAST UINT32_C(0xbfffffff)
#endif

typedef struct {
  GMutex lock;
  GCond changed;
  gint64 deadline;
  int fd;
  gboolean finished, expired;
} XvSetupWatch;

static gpointer setup_watch(gpointer user)
{
  XvSetupWatch *w = (XvSetupWatch *)user;
  g_mutex_lock(&w->lock);
  while (!w->finished && g_cond_wait_until(&w->changed, &w->lock, w->deadline)) {}
  if (!w->finished) {
    w->expired = TRUE;
    shutdown(w->fd, SHUT_RDWR);
  }
  g_mutex_unlock(&w->lock);
  return NULL;
}

static int connect_local_socket(guint display, gboolean abstract, gint64 deadline)
{
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
  char path[sizeof(address.sun_path) - 1];
  int length = g_snprintf(path, sizeof(path), "/tmp/.X11-unix/X%u", display);
  if (length < 0 || (gsize)length >= sizeof(path)) return -1;
  memcpy(address.sun_path + (abstract ? 1 : 0), path, length + 1);
  socklen_t bytes = offsetof(struct sockaddr_un, sun_path) + length + 1;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) return -1;
  if (!connect(fd, (struct sockaddr *)&address, bytes)) return fd;
  if (errno == EINPROGRESS) {
    for (;;) {
      gint64 left = deadline - g_get_monotonic_time();
      if (left <= 0) break;
      struct pollfd p = {fd, POLLOUT, 0};
      int result = poll(&p, 1, (int)((left + 999) / 1000));
      if (result < 0 && errno == EINTR) continue;
      int error = 0; socklen_t size = sizeof(error);
      if (result > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error)
        return fd;
      break;
    }
  }
  close(fd);
  return -1;
}

/* Match libxcb's local Xauthority method preference, including XDM's
 * per-connection nonce/PID/time wire value. Never log authority data. */
static gboolean local_authority(guint display, Xauth **owned, xcb_auth_info_t *info,
                                unsigned char xdm[24])
{
  static char xdm_type[] = "XDM-AUTHORIZATION-1", mit_type[] = "MIT-MAGIC-COOKIE-1";
  static char *types[] = {xdm_type, mit_type};
  static const int lengths[] = {19, 18};
  char hostname[256], number[32];
  if (gethostname(hostname, sizeof(hostname) - 1)) return FALSE;
  hostname[sizeof(hostname) - 1] = 0;
  int n = g_snprintf(number, sizeof(number), "%u", display);
  *owned = XauGetBestAuthByAddr(FamilyLocal, strlen(hostname), hostname,
      n, number, G_N_ELEMENTS(types), types, lengths);
  if (!*owned) return TRUE; /* Ordinary local ACL/no-authority fallback. */
  Xauth *a = *owned;
  info->name = a->name; info->namelen = a->name_length;
  if (a->name_length == 18 && !memcmp(a->name, types[1], 18)) {
    info->data = a->data; info->datalen = a->data_length;
    return a->data_length != 0;
  }
  if (a->name_length != 19 || memcmp(a->name, types[0], 19) || a->data_length != 16)
    return FALSE;
  static GMutex nonce_lock;
  /* Keep the immediate wire values distinct from libxcb's counter starting0.
   * Refuse exhaustion rather than wrapping into that ordinary counter range. */
  static guint32 nonce = CRYSTALHD_X11_NONCE_FIRST;
  g_mutex_lock(&nonce_lock);
  guint32 serial = nonce;
  if (nonce) nonce = nonce == CRYSTALHD_X11_NONCE_LAST ? 0 : nonce + 1;
  g_mutex_unlock(&nonce_lock);
  if (!serial) return FALSE;
  guint32 address = htonl(UINT32_MAX - serial), stamp = htonl((guint32)time(NULL));
  guint16 port = htons((guint16)getpid());
  memset(xdm, 0, 24); memcpy(xdm, a->data, 8);
  memcpy(xdm + 8, &address, 4); memcpy(xdm + 12, &port, 2); memcpy(xdm + 14, &stamp, 4);
  XdmcpWrap(xdm, (unsigned char *)a->data + 8, xdm, 24);
  info->data = (char *)xdm; info->datalen = 24;
  return TRUE;
}

static xcb_connection_t *setup_local_socket(int fd, xcb_auth_info_t *auth)
{
  /* A deadline shutdown can race XCB's initial writev. Suppress only a new
   * SIGPIPE in this caller, retaining its mask and any already-pending one. */
  sigset_t pipe, original, pending;
  sigemptyset(&pipe); sigaddset(&pipe, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &pipe, &original)) { close(fd); return NULL; }
  sigpending(&pending);
  gboolean had_pipe = sigismember(&pending, SIGPIPE) == 1;
  xcb_connection_t *connection = xcb_connect_to_fd(fd, auth);
  if (!had_pipe) {
    struct timespec zero = {0, 0};
    while (sigtimedwait(&pipe, NULL, &zero) < 0 && errno == EINTR) {}
  }
  pthread_sigmask(SIG_SETMASK, &original, NULL);
  return connection;
}

static xcb_connection_t *connect_local_display(const char *name, int *screen,
                                             const char **failure)
{
  gint64 deadline = g_get_monotonic_time() + XV_TIMEOUT_US;
  char *host = NULL; int display;
  *failure = "A local X11 display is unavailable";
  if (!xcb_parse_display(name, &host, &display, screen)) return NULL;
  gboolean local = host && !*host && display >= 0;
  free(host);
  if (!local) return NULL;
  Xauth *authority = NULL;
  xcb_auth_info_t auth; unsigned char xdm[24];
  memset(&auth, 0, sizeof(auth));
  if (!local_authority(display, &authority, &auth, xdm)) {
    if (authority) XauDisposeAuth(authority);
    *failure = "Cannot prepare local X11 authority"; return NULL;
  }
  int fd = -1;
  if (g_get_monotonic_time() < deadline) fd = connect_local_socket(display, TRUE, deadline);
  if (fd < 0 && g_get_monotonic_time() < deadline)
    fd = connect_local_socket(display, FALSE, deadline);
  if (fd < 0) {
    if (authority) XauDisposeAuth(authority);
    return NULL;
  }
  XvSetupWatch watch;
  memset(&watch, 0, sizeof(watch));
  watch.deadline = deadline; watch.fd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
  g_mutex_init(&watch.lock); g_cond_init(&watch.changed);
  GThread *thread = watch.fd >= 0 ? g_thread_try_new("xv-setup-deadline", setup_watch, &watch, NULL) : NULL;
  xcb_connection_t *connection = NULL;
  if (thread) {
    connection = setup_local_socket(fd, authority ? &auth : NULL);
    gint64 finished_at = g_get_monotonic_time();
    g_mutex_lock(&watch.lock); watch.finished = TRUE;
    g_cond_signal(&watch.changed); g_mutex_unlock(&watch.lock);
    g_thread_join(thread);
    if (watch.expired || finished_at >= deadline) {
      xcb_disconnect(connection); connection = NULL;
      *failure = "X11 connection setup timed out";
    }
  } else {
    close(fd); *failure = "Cannot allocate the X11 setup deadline guard";
  }
  if (watch.fd >= 0) close(watch.fd);
  g_cond_clear(&watch.changed); g_mutex_clear(&watch.lock);
  if (authority) XauDisposeAuth(authority);
  return connection;
}
#endif
