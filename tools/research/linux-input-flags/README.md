# Clear input flag observation for #690

This isolated diagnostic reuses the current `tests/library-drain.cpp` input,
decode, count/PTS/EOS, STOP/CLOSE and ownership implementation. It changes only
the public `DtsProcInput` argument from 0 to the selected 0 or 1. It does not
modify the library, driver, firmware, external clients or installed runtime.

The only input admitted is the retained project synthetic
`high-annexb.h264`: 787,883 bytes, SHA-256
`767d0fc307052fe995ca0921aaf01b48b4e6477698188e1e7a4e42da690f304e`,
640×360, 180 H.264 packets/pictures. The digest is checked over the actual
demuxed packets which the existing drain will submit. The old fixture's
identity does not establish that today's generation script reproduces it.

Build without opening hardware:

```sh
python3 -B tools/research/linux-input-flags/build.py /absolute/new/build-results
/absolute/new/build-results/probe --preflight /path/to/high-annexb.h264
```

The build compiles the current unmodified DIL into this worktree, then the
probe and focused observer controls. It never loads or installs a module,
opens a device, or executes a hardware mode. Each result directory is new;
compiler or control failures are retained with the actual command and exit.

Only the hardware coordinator may run the separately reviewed native step:

```text
probe --hardware INPUT 0|1 ABSOLUTE_NEW_OBSERVATION_DIRECTORY
```

The probe exclusively creates the observation directory and its files. There
is one 180-picture run, with the existing 60-second decode deadline; an
external timeout is also required because userspace cannot interrupt a stuck
kernel operation safely. Baseline checks, exclusive device ownership, failure
handoff and official-1.3 recovery are the coordinator's responsibility. Reuse
ordinary 180-picture controls before and after; no runtime-marker or custom
register/memory observation is required. `--preflight` opens no device and
creates no output files.

| File | Actual evidence |
| --- | --- |
| `public-input.bin` | Exact ordered AU bytes given to the public API. `INPUT_BEGIN/END` records token, selected argument and real return status. |
| `tx-input.bin` | Exact ordered buffers immediately before the actual `BCM_IOC_PROC_INPUT` syscall. `TX_BEGIN/END` records offset, length, actual flag, syscall/errno, driver return status and returned DRAM offset. Chunk boundaries may differ between runs. |
| `output.yuy2` | Validated ordinary packed output, copied during its lease and written only after successful actual release. Each frame is 460,800 bytes; a complete 180-frame run is 82,944,000 bytes. |
| `events.tsv` | The input/TX/output/release rows above; existing firmware command/sequence and response header/status; final counts and process FD/thread accounting. |

The executable exports an observer for the DIL's existing `ioctl` call. It
forwards the unchanged envelope to `SYS_ioctl`. The observed boundary is
**userspace → kernel**, not a physical DMA transfer or card decryption. Stage A
predicts TX flag 0 for both public arguments; any nonzero or mapped TX request
is refused before the syscall. Full TX bytes are bounded to 32 MiB. The
existing driver forwarding/preparation audit remains source evidence.

For outputs, the actual `Pout` format, width/height, completed DWORD counts,
source token, picture/session/flags, owned copy size and release status are
recorded. The existing `CopyCapturedPixels` rejects encrypted, interlaced,
short or mismatched output before pixel access. It selects the returned DIL
buffer's tight YUY2 layout, 1,280 bytes per row; this is not an independently
measured physical RX pitch or register-derived packing claim. No MFD/SCL
register reads are added. A successful fetch remains owned by the original
drain's lease even if observation fails; a failed release is never retried.

`END result=PASS` means only the finite transport/count/EOS/cleanup and
observation checks passed. Visual agreement with the authored scene/reference
is a separate result. `decryption_evidence=NOT_TESTED` is unconditional: the
same clear bytes and correct output cannot establish hardware decryption.
The probe contains no encrypted fixture, key/session operations or positive
protected-session experiment.

Preparation on 2026-11-26: current DIL/probe `-Werror` builds, 20 focused
observer checks in normal and ASan/UBSan/leak builds, and the exact fixture
preflight pass in `crystalhd-linux-input-flags-build04-20261126`. No native
result follows. The first two compiler failures remain in build01/build02:
missing `include/link`, then the UAPI's `7411d.h` prerequisite and explicit
arguments to the reused `Load`. Build03 passed before the final receipt-error
handling refinement; build04 is the executable handoff and contains source
snapshots, commands, raw logs and exits. No original failure was relabelled.
