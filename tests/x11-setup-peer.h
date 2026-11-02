#ifndef CRYSTALHD_TEST_X11_SETUP_PEER_H
#define CRYSTALHD_TEST_X11_SETUP_PEER_H
#include <dirent.h>
/* SPDX-License-Identifier: MIT */
/* Owned local peers only. Their independent upper bound also releases an
 * unfixed client, so failure assertions happen after socket/thread cleanup. */
typedef struct {
  int listener;
  guint display;
  gboolean partial, validate, expired, accepted, valid, eof;
  guint method;
  pid_t caller_pid;
  unsigned char xdm_address[4];
} SetupPeer;
static const unsigned char setup_cookie[16] = {
  0x21,0x43,0x65,0x87,0xa9,0xcb,0xed,0x0f,0x31,0x53,0x75,0x97,0xb9,0xdb,0xfd,0x1f};

static guint setup_u16(const unsigned char *p, gboolean little)
{ return little ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1]; }
static gboolean setup_read(int fd, void *data, size_t size, gint64 end)
{
  unsigned char *p = (unsigned char *)data;
  while (size) {
    gint64 left = end - g_get_monotonic_time();
    if (left <= 0) return FALSE;
    struct pollfd event = {fd, POLLIN, 0};
    int ready = poll(&event, 1, (int)((left + 999) / 1000));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) return FALSE;
    ssize_t count = recv(fd, p, size, 0);
    if (count <= 0) return FALSE;
    p += count; size -= count;
  }
  return TRUE;
}
static gpointer setup_peer(gpointer user)
{
  SetupPeer *p = (SetupPeer *)user;
  gint64 end = g_get_monotonic_time() + 3500 * 1000;
  struct pollfd event = {p->listener, POLLIN, 0};
  if (poll(&event, 1, 3500) <= 0) return NULL;
  int fd = accept4(p->listener, NULL, NULL, SOCK_CLOEXEC);
  if (fd < 0) return NULL;
  p->accepted = TRUE;
  struct ucred credentials; socklen_t credential_size = sizeof(credentials);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &credential_size) == 0)
    p->caller_pid = credentials.pid;
  unsigned char header[12], body[256];
  gboolean little; guint name, data, padded_name, padded_data;
  if (!setup_read(fd, header, sizeof(header), end)) goto done;
  little = header[0] == 'l';
  if (!little && header[0] != 'B') goto done;
  name = setup_u16(header + 6, little); data = setup_u16(header + 8, little);
  padded_name = (name + 3) & ~3U; padded_data = (data + 3) & ~3U;
  if (padded_name + padded_data > sizeof(body) ||
      !setup_read(fd, body, padded_name + padded_data, end)) goto done;
  p->valid = TRUE;
  if (p->validate) {
    if (!p->method) p->valid = name == 0 && data == 0;
    else if (p->method == 1)
      p->valid = name == 18 && data == 16 && !memcmp(body, "MIT-MAGIC-COOKIE-1", 18) &&
          !memcmp(body + padded_name, setup_cookie, 16);
    else {
      unsigned char plain[24];
      p->valid = name == 19 && data == 24 && !memcmp(body, "XDM-AUTHORIZATION-1", 19);
      if (p->valid) {
        XdmcpUnwrap(body + padded_name, (unsigned char *)setup_cookie + 8, plain, 24);
        guint16 pid; memcpy(&pid, plain + 12, 2);
        guint32 stamp; memcpy(&stamp, plain + 14, 4);
        p->valid = !memcmp(plain, setup_cookie, 8) && ntohs(pid) == (guint16)getpid() &&
            llabs((long long)ntohl(stamp) - (long long)time(NULL)) <= 5;
        for (guint i = 18; i < 24; i++) if (plain[i]) p->valid = FALSE;
        memcpy(p->xdm_address, plain + 8, 4);
      }
    }
  }
  if (p->partial) {
    unsigned char reply[8] = {1,0,0,0,0,0,0,0};
    reply[little ? 2 : 3] = 11; reply[little ? 6 : 7] = 8;
    if (send(fd, reply, sizeof(reply), MSG_NOSIGNAL) != sizeof(reply)) goto done;
  }
  {
    unsigned char byte;
    while (g_get_monotonic_time() < end) {
      event.fd = fd; event.events = POLLIN; event.revents = 0;
      int ready = poll(&event, 1, 20);
      if (ready < 0 && errno == EINTR) continue;
      if (ready > 0 && recv(fd, &byte, 1, 0) == 0) { p->eof = TRUE; break; }
    }
    p->expired = !p->eof;
  }
done:
  close(fd); return NULL;
}
static void setup_listen(SetupPeer *p)
{
  p->listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  g_assert_cmpint(p->listener, >=, 0);
  for (guint attempt = 0; attempt < 100; attempt++) {
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address)); address.sun_family = AF_UNIX;
    p->display = 52000 + (getpid() % 10000) + attempt;
    int n = g_snprintf(address.sun_path + 1, sizeof(address.sun_path) - 1,
        "/tmp/.X11-unix/X%u", p->display);
    if (!bind(p->listener, (struct sockaddr *)&address,
        offsetof(struct sockaddr_un, sun_path) + 1 + n)) {
      g_assert_cmpint(listen(p->listener, 1), ==, 0); return;
    }
  }
  close(p->listener); g_error("No unused owned setup-test display");
}
static guint setup_fd_count(void)
{
  DIR *dir = opendir("/proc/self/fd"); g_assert_nonnull(dir);
  guint count = 0; struct dirent *entry;
  while ((entry = readdir(dir))) if (entry->d_name[0] != '.') count++;
  closedir(dir); return count;
}
#endif
