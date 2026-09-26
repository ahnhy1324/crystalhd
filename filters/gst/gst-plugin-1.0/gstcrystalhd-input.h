/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef GST_CRYSTALHD_INPUT_H
#define GST_CRYSTALHD_INPUT_H

#include <glib.h>
#include <bc_dts_defs.h>

/* libcrystalhd_priv.h: CIRC_TX_BUF_SIZE. DtsSendData waits for space rather
 * than returning BUSY, so a sole producer must admit the entire framed call
 * before entering the library. The normal TX worker only frees ring space.
 * This does not claim concurrent producers or suspend recovery are supported.
 */
#define GST_CRYSTALHD_INPUT_CAPACITY ((gsize)1024 * 1024)
#define GST_CRYSTALHD_INPUT_TIMEOUT_US ((gint64)10 * G_USEC_PER_SEC)
#define GST_CRYSTALHD_EOS_RESERVATION ((gsize)1024)

static gboolean
gst_crystalhd_input_reservation(BC_MEDIA_SUBTYPE subtype, gsize payload,
                                gsize metadata, gsize *reservation)
{
  gsize extra = 0;
  gsize sequence = 0;
  gsize total;

  /* Check before arithmetic, including caller-supplied metadata. */
  if (payload == 0 || payload > GST_CRYSTALHD_INPUT_CAPACITY ||
      metadata > GST_CRYSTALHD_INPUT_CAPACITY)
    return FALSE;
  switch (subtype) {
    case BC_MSUBTYPE_H264:
    case BC_MSUBTYPE_MPEG2VIDEO:
    case BC_MSUBTYPE_VC1:
      break;
    case BC_MSUBTYPE_WVC1:
      extra = 4;
      sequence = metadata;
      break;
    case BC_MSUBTYPE_WMV3:
      extra = 48; /* Link: round_up(payload + 17,32); Flea: payload +4. */
      sequence = 32; /* Link sequence header; Flea uses only12 bytes. */
      break;
    default:
      return FALSE;
  }

  /* DtsAddVC1SCode/DtsSetVC1SH (libcrystalhd_parser.cpp) add the bytes
   * above. DtsProcInput may inject that sequence on each keyframe, then split
   * an ES picture around its startcode: at most three DtsAlignSendData calls.
   * H264 here is Annex-B, never the expanding AVC1 length-prefix converter.
   * Ordinary PES packets add at most14 header bytes and carry >=65512 bytes
   * when full (MAX_RE_PES_BOUND=0xfff0). 32 bytes per60000 bytes plus one
   * packet per call is conservative; <=41-byte Link SPES per call fits128.
   * The maximum intermediate value is <3MiB, safe even on32-bit size_t.
   */
  total = payload + extra + sequence;
  total += 32 * (total / 60000 + 3) + 128;
  if (total > GST_CRYSTALHD_INPUT_CAPACITY)
    return FALSE;
  *reservation = total;
  return TRUE;
}

/* DtsSendEOS in libcrystalhd_if.cpp writes at most three 8-byte codec EOS
 * packets and one <256-byte timing marker with private/extension/stuffing
 * headers on Flea, or a single <=32-byte packet on Link. 1024 covers all
 * four complete writes, not merely the first packet of the operation.
 */

#endif
