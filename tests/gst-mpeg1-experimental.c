/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Optional source/caps contracts only: no state transition or decoder open. */
#include <gst/gst.h>

extern gboolean gst_crystalhd_mpeg1_experimental_self_test(void);

int
main(int argc, char **argv)
{
  gst_init(&argc, &argv);
  if (!gst_crystalhd_mpeg1_experimental_self_test()) {
    g_printerr("Experimental MPEG-1 source/caps self-test: FAIL\n");
    return 1;
  }
  g_print("Experimental MPEG-1 source/caps self-test: PASS\n");
  return 0;
}
