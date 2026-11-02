/* SPDX-License-Identifier: MIT */
#include "x11-setup-peer.h"
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
  g_assert_cmpint(peer->caller_pid, ==, getpid());
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
    if (method == 2) {
      guint32 address; memcpy(&address, peer.xdm_address, 4); address = ntohl(address);
      g_assert_cmphex(address, >=, UINT32_C(0x40000000));
      g_assert_cmphex(address, <=, UINT32_C(0x7fffffff));
    }
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
