/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef LIBCRYSTALHD_RAW_FRAME_H
#define LIBCRYSTALHD_RAW_FRAME_H

#include <stdint.h>
#include "libcrystalhd_if.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BC_RAW_FRAME_WIDTH 256u
#define BC_RAW_FRAME_HEIGHT 96u
#define BC_RAW_FRAME_FPS 30u
#define BC_RAW_FRAME_MASK_BYTES 96u
#define BC_RAW_FRAME_AU_CAPACITY 65536u

typedef struct BC_RAW_FRAME_BUILDER BC_RAW_FRAME_BUILDER;

/* Fixed 8-bit YUV420P: Y 256x96, Cb/Cr 128x48, in that order.
 * Each readable plane must cover (rows-1)*stride+width bytes. Padding is
 * neither encoded nor inspected. Plane backing remains caller-owned and
 * must not overlap the builder's borrowed AU. Samples 0..255 are encoded
 * verbatim; this is not an assertion about native output packing.
 */
typedef struct BC_RAW_FRAME_PLANES {
    const uint8_t *planes[3];
    uint32_t strides[3];
    uint32_t plane_bytes[3];
} BC_RAW_FRAME_PLANES;

/* Experimental standard-H264 producer, not a raw MFD/scaler API or on-card
 * surface lease. Use one serialized builder for one fresh, noninterleaved
 * progressive H264/256x96/30fps stream. The caller configures/owns the normal
 * device, output buffers, timestamps, EOS, drain, stop and close operations.
 * No function here opens, resets, flushes or closes a device.
 * The stream is fixed Baseline Level 3.0; callers must keep upload cadence
 * and aggregate coded rate within that level. No bitrate control, pacing
 * or rate-control implementation is provided. Hardware qualification covers
 * sparse uploads plus composition, not sustained 30fps full-plane uploads.
 *
 * Create requires out != NULL and *out == NULL; calloc failure returns
 * INSUFF_RES. Non-NULL builder arguments must be live, exclusively owned
 * handles returned by Create; forged/dangling handles are caller errors.
 * Destroy consumes a valid handle and sets it to NULL, including pending/
 * aborted builders, but cannot retire device or output ownership.
 * Destroy(NULL) is INV_ARG; Destroy(&null_handle) is idempotent SUCCESS.
 * Do not destroy concurrently with Submit or any use of the borrowed AU.
 */
DRVIFLIB_API BC_STATUS DtsRawFrameCreate(BC_RAW_FRAME_BUILDER **out);
DRVIFLIB_API BC_STATUS DtsRawFrameDestroy(BC_RAW_FRAME_BUILDER **builder);

/* Prepare copies caller pixels/mask into an owned Annex-B access unit.
 * Upload slot0 first (IDR/AUD/SPS/PPS), then slot1; thereafter either slot
 * can be replaced without IDR. Compose requires both committed slots and
 * 96 row-major 16x16 macroblock choices, each exactly 0 or 1. Motion is zero.
 * All units disable deblocking and carry the fixed public Baseline profile.
 *
 * Prepare NEVER advances committed frame_num/reference state. One pending
 * AU is allowed: another Prepare returns BUSY without modifying its pointer
 * or bytes. On every failure *au and *bytes remain unchanged. On success the
 * borrowed pointer is valid until Finish, Discard, Abort or Destroy; never
 * modify/free it. The maximum frame index is 0x3fffffff (signed32 POC2 bound).
 */
DRVIFLIB_API BC_STATUS DtsRawFramePrepareUpload(BC_RAW_FRAME_BUILDER *builder,
                                 uint32_t slot,
                                 const BC_RAW_FRAME_PLANES *frame,
                                 const uint8_t **au, uint32_t *bytes);
DRVIFLIB_API BC_STATUS DtsRawFramePrepareCompose(BC_RAW_FRAME_BUILDER *builder,
                                  const uint8_t *mask, uint32_t mask_bytes,
                                  const uint8_t **au, uint32_t *bytes);

/* Translate requires both committed slots and selects one long-term slot
 * (0 or 1) for the whole picture. Motion components are H264 quarter-luma-
 * sample units, each in [-3,3]; positive components select larger reference
 * coordinates. Standard H264 interpolation and edge extension apply.
 * This is a coded P picture, with the same pending/commit/poison contract
 * as the other Prepare operations, not a scaler or raw-surface operation.
 */
DRVIFLIB_API BC_STATUS DtsRawFramePrepareTranslate(BC_RAW_FRAME_BUILDER *builder,
                                    uint32_t slot,
                                    int32_t mv_x_qpel, int32_t mv_y_qpel,
                                    const uint8_t **au, uint32_t *bytes);

/* Finish requires a pending AU and the ACTUAL complete DtsProcInput result.
 * Only SUCCESS commits frame/reference state. Every other result poisons
 * the builder and is returned unchanged, even if some bytes were queued.
 * Submit performs that DtsProcInput(..., encrypted=FALSE) call and Finish.
 * Its device must already use BC_MSUBTYPE_H264 (not AVC1 conversion) and
 * this Annex-B stream. Submit passes the owned AU to the legacy mutable
 * DtsProcInput ABI; the borrowed AU's lifetime ends when Finish returns.
 * Input SUCCESS is not decoded-output, physical-acceptance or lease proof.
 */
DRVIFLIB_API BC_STATUS DtsRawFrameFinish(BC_RAW_FRAME_BUILDER *builder,
                          BC_STATUS input_status);
DRVIFLIB_API BC_STATUS DtsRawFrameSubmit(BC_RAW_FRAME_BUILDER *builder, HANDLE device,
                          uint64_t timestamp);

/* Discard is valid ONLY when none of the pending AU was transmitted; it
 * preserves committed state. Abort is the conservative response to any
 * uncertain/partial submission or later decoder failure, and is idempotent.
 * An aborted builder permits only Abort/Destroy; other operations return
 * IO_USER_ABORT. A new builder does not recover that same live decoder:
 * first use ordinary owned stop/close and a fresh stream. State misuse
 * returns ERR_USAGE; NULL/invalid arguments return INV_ARG.
 */
DRVIFLIB_API BC_STATUS DtsRawFrameDiscard(BC_RAW_FRAME_BUILDER *builder);
DRVIFLIB_API BC_STATUS DtsRawFrameAbort(BC_RAW_FRAME_BUILDER *builder);

#ifdef __cplusplus
}
#endif
#endif
