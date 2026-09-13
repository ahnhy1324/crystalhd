# Userspace DMA ownership

The legacy ioctl ABI remains unchanged. This driver pins ordinary userspace
memory; it does not expose a dma-buf import/export API. Linux 6.1 remains the
minimum kernel version.

## Mapping policy

| Buffer | Pin flags | DMA direction | Ownership ends |
| --- | --- | --- | --- |
| Compressed input (`PROC_INPUT`) | `FOLL_LONGTERM` | `DMA_TO_DEVICE` | Synchronous input ioctl completion, including cancellation/error cleanup |
| Registered capture (`ADD_RXBUFFS`) | `FOLL_LONGTERM | FOLL_WRITE` | `DMA_BIDIRECTIONAL` | Frame fetch, full capture flush, playback release/close, or PCI removal |

Both paths call `pin_user_pages_fast()`, which supplies `FOLL_PIN` internally.
Capture registrations can remain queued indefinitely, including across discard
flush and suspend. Input normally completes quickly, but the buffer is mapped
before an unbounded, interruptible firmware-FIFO wait; the three-second timeout
starts only after DMA submission. Consequently both paths request long-term
pins. There are no MMU notifiers that could revoke a mapping early. The kernel
decides which memory mappings can support these pins; unsupported memory,
including DAX, is rejected without changing the ABI. Read-only input memory does
not require `FOLL_WRITE`. This follows the kernel's
[pinning API guidance](https://docs.kernel.org/core-api/pin_user_pages.html).

Capture uses bidirectional DMA mapping because the CPU metadata parser both
reads the completed Y plane and updates its metadata words. The driver transfers
ownership to the CPU before parsing, and back to the device before reposting.
Repeated metadata inspection tracks CPU ownership to avoid overwriting CPU
changes with an old bounce-buffer copy. Final cleanup also preserves those edits.

`page_cnt` counts acquired pins, `sg_nents` counts the original entries passed to
`dma_map_sg()`, and `sg_cnt` counts the returned DMA segments. Segment merging
never changes the unmap count. The UV plane is located in the mapped segments,
including exact segment boundaries. TX partial words use a coherent fill buffer;
their bytes are excluded before mapping, so a merged segment is never discarded
afterwards. Descriptor builders reject a ring that lacks room for a plane split
or fill descriptor. These rules follow the kernel's
[DMA mapping guide](https://docs.kernel.org/core-api/dma-api-howto.html).

## Release and failure audit

| Path | Cleanup or retained ownership |
| --- | --- |
| Invalid address, alignment, size, or page count | Reject before pinning |
| Negative/short pin result | Record only positive acquired pins and unpin that prefix |
| Fill-byte copy or SG mapping failure | Unpin acquired pages; do not unmap a mapping that never succeeded |
| Normal input completion | Wait for the IRQ callback to finish, unmap DMA, then unpin |
| Input signal/timeout cancellation | Drain the IRQ handler, stop TX without holding a spinlock, complete the request, then unmap/unpin |
| Capture descriptor/queue failure | Return packet ownership to the caller; process context releases pins |
| Capture repost error in IRQ context | Retain the packet in the free queue for process-context cleanup |
| Frame fetch | Synchronize metadata access, then unmap and dirty-unpin the capture pages |
| Discard flush | Stop capture; retain registered buffers for reuse |
| Full flush | Serialize capture mutations/metadata peeks, exclude IRQ completion, stop capture, drain active/ready/free queues, then release pins |
| Playback release/close | Exclude ioctls and IRQ access while draining capture and freeing hardware context/pools |
| Suspend | Stop RX/TX; capture registrations remain pinned for later cleanup |
| PCI removal | Cancel all FIFO waiters, exclude ioctls, clear bus mastering, drain/free IRQ, then destroy queues and pools |
| DIO pool allocation failure | Free the partially allocated coherent buffers and DIO objects |

Every successful streaming mapping is unmapped while its pages remain pinned.
Only then does `unpin_user_pages_dirty_lock()` release them. Capture mappings are
conservatively dirtied after a successful map, including aborted transfers; input
and partial-pin failures are not dirtied. No dirty-unpin runs in a hard IRQ.

TX input ioctls are serialized because they share engine cancellation state.
Capture mutation, metadata inspection, and process-context wake/repost paths
share `fetch_sem`; queue waits and firmware command waits do not hold it.
Successful stop resets RX list bookkeeping, and discard flush notifies capture
start before reposting. A failed stop retains registrations for teardown.
Removal uses a persistent `present == 0` condition, so cancelling one FIFO waiter
cannot leave another waiting indefinitely. Existing file descriptors are bound
to a device generation and cannot operate on a later PCI reprobe.

An engine stop timeout disables PCI bus mastering, drains pending transactions,
and marks the hardware context faulted. New TX/RX work is rejected until a fresh
open resets the device. A pending-transaction drain timeout is logged; physically
stuck PCI transactions remain an unvalidated hardware failure condition.

## Verification

Run `sh tests/dma-descriptors.sh` without hardware. It compiles the production
descriptor functions and structures against a small DMA-address shim and checks
merged/unmerged plane splits, exact plane boundaries, descriptor-capacity
rejection, and one-to-three-byte input tails under AddressSanitizer and UBSan.

For hardware validation, compare `nr_foll_pin_acquired` and
`nr_foll_pin_released` in `/proc/vmstat` before and after complete playback and
close. Capture registrations legitimately keep those counters unequal while
open. The counters are global, so unrelated pinning activity must be accounted
for. Existing GStreamer and VA-API hardware tests exercise normal registration,
fetch, reuse and close.

DMA-map fault injection, stop-timeout injection, DAX and
other file-backed memory variants, concurrent power-management callbacks, and
physical removal during DMA need dedicated kernel/hardware validation. The
descriptor harness does not emulate these paths. Suspend/resume should not be
described as validated solely because ordinary playback succeeds.

`sh tests/ioctl-smoke.sh` runs against the loaded driver by default: its
playback probe registers a buffer ending in a protected
page and an entirely inaccessible buffer. Both must fail during pinning,
before DMA posting, and release any partial pin. Run this only on an idle
device; `make check` merely compiles this probe using `--build-only`.
