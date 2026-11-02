/* SPDX-License-Identifier: MIT */
/* Owned local peers only. Their independent upper bound also releases an
 * unfixed client, so failure assertions happen after socket/thread cleanup. */
typedef struct {
  int listener;
  guint display;
  gboolean partial, validate, expired, accepted, valid, eof;
  guint method;
  unsigned char xdm_address[4];
} SetupPeer;
static const unsigned char setup_cookie[16] = {
  0x21,0x43,0x65,0x87,0xa9,0xcb,0xed,0x0f,0x31,0x53,0x75,0x97,0xb9,0xdb,0xfd,0x1f};

static guint setup_u16(const unsigned char *p, gboolean little)
{ return little ? p[0] | (p[1] << 8) : (p[0] << 8) | p[1]; }
static gboolean setup_read(int fd, void *data, size_t size, gint64 end)
{
  unsigned char *p = data;
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
  SetupPeer *p = user;
  gint64 end = g_get_monotonic_time() + 3500 * 1000;
  struct pollfd event = {p->listener, POLLIN, 0};
  if (poll(&event, 1, 3500) <= 0) return NULL;
  int fd = accept4(p->listener, NULL, NULL, SOCK_CLOEXEC);
  if (fd < 0) return NULL;
  p->accepted = TRUE;
  unsigned char header[12], body[256];
  if (!setup_read(fd, header, sizeof(header), end)) goto done;
  gboolean little = header[0] == 'l';
  if (!little && header[0] != 'B') goto done;
  guint name = setup_u16(header + 6, little), data = setup_u16(header + 8, little);
  guint padded_name = (name + 3) & ~3U, padded_data = (data + 3) & ~3U;
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
      event = (struct pollfd){fd, POLLIN, 0};
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
    struct sockaddr_un address = {.sun_family = AF_UNIX};
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
static void setup_exchange(SetupPeer *peer, gboolean unix_name, gboolean direct)
{
  guint before = setup_fd_count();
  GThread *server = g_thread_new("owned-setup-peer", setup_peer, peer);
  gchar *name = g_strdup_printf("%s:%u.2", unix_name ? "unix/" : "", peer->display);
  GError *error = NULL; const char *failure = NULL;
  xcb_connection_t *connection = NULL; XvContext *context = NULL; int screen = -1;
  gint64 started = g_get_monotonic_time();
  if (direct) {
    connection = connect_local_display(name, &screen, &failure);
  } else {
    context = context_new(name, 0, NULL, 0, &error);
    failure = error ? error->message : NULL;
  }
  gint64 elapsed = g_get_monotonic_time() - started;
  gboolean unexpected = connection || context;
  if (connection) xcb_disconnect(connection);
  if (context) {
    g_atomic_int_set(&context->stopping, TRUE); wake_context(context); context_unref(context);
  }
  g_thread_join(server); g_free(name);
  clean_contexts();
  g_assert_false(unexpected); if (direct) g_assert_cmpint(screen, ==, 2);
  g_assert_true(peer->accepted); g_assert_true(peer->valid);
  g_assert_true(peer->eof); g_assert_false(peer->expired);
  g_assert_cmpstr(failure, ==, "X11 connection setup timed out");
  g_assert_cmpint(elapsed, <, 2 * G_USEC_PER_SEC);
  g_assert_cmpint(g_atomic_int_get(&live_contexts), ==, 0);
  g_assert_cmpuint(setup_fd_count(), ==, before);
  g_clear_error(&error);
  g_print("setup timeout method=%u partial=%u unix=%u elapsed_us=%" G_GINT64_FORMAT " fd_delta=0 contexts=0\n",
      peer->method, peer->partial, unix_name, elapsed);
}

static int setup_deadline_child(void)
{
  SetupPeer peer = {0}; setup_listen(&peer);
  GError *error = NULL; gchar *directory = g_dir_make_tmp("crystalhd-xv-auth-XXXXXX", &error);
  g_assert_no_error(error);
  gchar *path = g_build_filename(directory, "authority", NULL);
  g_assert_true(g_setenv("XAUTHORITY", path, TRUE)); /* Before creating threads. */
  unsigned char prior_xdm[4] = {0};
  for (guint trial = 0; trial < 6; trial++) {
    guint method = trial / 2;
    FILE *file = fdopen(open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600), "wb");
    g_assert_nonnull(file);
    if (method) {
      char host[256], number[32]; g_assert_cmpint(gethostname(host, sizeof(host) - 1), ==, 0);
      host[sizeof(host) - 1] = 0; g_snprintf(number, sizeof(number), "%u", peer.display);
      char *type = method == 1 ? "MIT-MAGIC-COOKIE-1" : "XDM-AUTHORIZATION-1";
      Xauth a = {.family=FamilyLocal,.address_length=strlen(host),.address=host,
          .number_length=strlen(number),.number=number,.name_length=strlen(type),.name=type,
          .data_length=sizeof(setup_cookie),.data=(char *)setup_cookie};
      g_assert_cmpint(XauWriteAuth(file, &a), ==, 1);
    }
    g_assert_cmpint(fclose(file), ==, 0);
    peer.partial = trial & 1; peer.validate = TRUE; peer.method = method;
    peer.expired = peer.accepted = peer.valid = peer.eof = FALSE;
    setup_exchange(&peer, trial & 1, trial & 1);
    if (trial == 4) memcpy(prior_xdm, peer.xdm_address, 4);
    if (trial == 5) g_assert_cmpint(memcmp(prior_xdm, peer.xdm_address, 4), !=, 0);
  }
  close(peer.listener); unlink(path); rmdir(directory); g_free(path); g_free(directory);
  /* Force the setup-write EPIPE race on our socketpair. No process-global
   * handler changes; retain both the caller mask and a preexisting SIGPIPE. */
  for (guint prior = 0; prior < 2; prior++) {
    int sockets[2]; g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), ==, 0);
    close(sockets[1]);
    sigset_t original, pipe, before, after;
    sigemptyset(&pipe); sigaddset(&pipe, SIGPIPE);
    g_assert_cmpint(pthread_sigmask(SIG_BLOCK, NULL, &original), ==, 0);
    if (prior) {
      g_assert_cmpint(pthread_sigmask(SIG_BLOCK, &pipe, NULL), ==, 0);
      g_assert_cmpint(raise(SIGPIPE), ==, 0);
    }
    pthread_sigmask(SIG_BLOCK, NULL, &before);
    force_setup_pipe = TRUE; observed_setup_pipe = FALSE;
    xcb_connection_t *connection = setup_local_socket(sockets[0], NULL);
    force_setup_pipe = FALSE;
    g_assert_nonnull(connection); g_assert_cmpint(xcb_connection_has_error(connection), !=, 0);
    xcb_disconnect(connection); g_assert_true(observed_setup_pipe);
    pthread_sigmask(SIG_BLOCK, NULL, &after);
    for (int signal = 1; signal < NSIG; signal++)
      g_assert_cmpint(sigismember(&before, signal), ==, sigismember(&after, signal));
    sigpending(&after); g_assert_cmpint(sigismember(&after, SIGPIPE), ==, prior);
    if (prior) { struct timespec zero = {0}; g_assert_cmpint(sigtimedwait(&pipe, NULL, &zero), ==, SIGPIPE); }
    g_assert_cmpint(pthread_sigmask(SIG_SETMASK, &original, NULL), ==, 0);
  }
  g_print("setup EPIPE: caller signal mask and prior pending SIGPIPE preserved\n");
  return 0;
}
