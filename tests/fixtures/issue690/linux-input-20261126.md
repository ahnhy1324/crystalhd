# Linux encrypted-input boundary: device-free Stage A

**2026-11-26, PASS for the checks below; no hardware decryption claim.**
Owner: [#690](https://github.com/ahnhy1324/crystalhd/issues/690).
Source: maintained master `1427d2de0825` plus this test-only change.
The completed [#469 audit](https://github.com/ahnhy1324/crystalhd/issues/469#issuecomment-6259948458)
already identified these boundaries. This work adds executable regression
coverage; it does not add protected playback or reopen that audit.

## What actually reaches each boundary

| Boundary | Current behavior and evidence |
| --- | --- |
| Public `DtsProcInput` → `DtsSendData` | The ordinary TX ring carries bytes, not the caller's encryption argument. Paired calls with identical clear H.264 input produce identical packets and ring bytes. Fragment-consistency bookkeeping is not a TX flag channel. See [actual input tests](../../library-input.cpp). |
| Byte ring → `txThreadProc` | The worker creates its own flag: zero for H.264, `0x02` for VC1MP. Paired caller values do not change it. Both successful and failed DMA retire the ring owner; failure retains the existing TX fault. See [worker tests](../../library-tx-flush.cpp). |
| Internal `DtsTxDmaText` → `DtsDrvCmd` → `BCM_IOC_PROC_INPUT` | The supplied byte is preserved in `BC_PROC_INPUT.Encrypted`, with the existing VC1MP `OR 0x02`. [New direct tests](../../library-encrypted-input.cpp) execute both production functions and replace only the syscall boundary; they check bytes, native IOCTL layout, status/offset and command-pool return. |
| Kernel command → TX admission | `bc_cproc_proc_input` passes the IOCTL byte through the existing bounded transfer path. [TX admission tests](../../tx-admission.c) already cover exact generic flag forwarding; new cases exercise actual FLEA FIFO/preparation bodies with input bit0 clear/set. |
| BCM70015/FLEA, wake already complete | Input bit0 does not select an encryption operation. Each existing FIFO mode (`0`, `0x04`, `0x08`, `0x0c`) and its bit0 pair retain the same destination and reservation policy. Descriptor construction has no encrypted-flag parameter; the actual preparation writes exactly three DWORDs `(0, 0, byte_count)`. Success publishes/completes once; failed preparation retains ownership and blocks a second post. Descriptor/device operations remain fixture boundaries, not physical DMA. |
| BCM70015/FLEA, wake incomplete | Nonzero flags can take a different wake/query path. The already-awake result above does **not** qualify that path for a native bit0 experiment. Existing wake-failure controls remain in the TX suite. |
| BCM70012/LINK | Its legacy encrypted 188-byte packet branch is separate. Source presence is not a BCM70015 capability or a native LINK result. |
| Output completion → library → VA | `COMP_FLAG_DATA_ENC` maps to `BC_POUT_FLAGS_ENCRYPTED` in the library. Existing packed-output and VA handling reject encrypted output before CPU materialization and release only acquired output. VA rejects nonzero encryption configuration and protected buffer types. See [VA replay](../../vaapi-replay.cpp), [VPP](../../vaapi-vpp.cpp) and [library output implementation](../../../linux_lib/libcrystalhd/libcrystalhd_priv.cpp). These existing controls and #469 results are reused, not newly qualified hardware behavior. |

Input flags and output completion flags are different domains. In particular,
output `COMP_FLAG_DATA_ENC=0x08` must not be used as an input encryption bit:
input `0x08` participates in FLEA statistics/wrap handling. Direct unit values
`0x80`/`0xff` are ABI transport controls, not native experiment parameters.

## Executed checks

| Check | Actual result |
| --- | --- |
| Direct internal input | 324 checks, zero failures on native and actual i386 builds; six flags × two codec modes × six driver statuses, plus syscall failure, invalid arguments and empty pool. |
| Existing public input testcase, with new paired controls | PASS; eight new calls cover caller flag 0/1 with Annex B/length-prefixed input and supplied/missing headers. |
| Existing TX/flush testcase, with new paired worker controls | PASS; eight new cases cover caller flag 0/1, H.264/VC1MP and DMA success/error. |
| ASan/UBSan and leak checks | All three affected library testcases PASS. |
| `make tx-admission-check` | 11,252 checks, zero failures in each normal and ASan/UBSan run, including eight FIFO flag values × success/failure. |

The three library cases used the corresponding `make library-check` compile
recipes; the direct case also ran with `-m32`. The new test is registered in
both `library-check` and `userspace32-check`. No entire library/VA suite rerun
or native playback result is implied by these local results. Standard CI
continues to select the required broader checks.

Integration with #689's cleanup guards also passes the combined TX admission
suite: **11,548 checks, zero failures** in each normal and ASan/UBSan run.
This is the affected overlap check; the individual results above remain scoped
to the Stage A change.

## Native follow-up and remaining scope

Stage A's mandatory transport/output/negative-test work is complete. The
later [B0/B1 clear-input observation](native-clear-flags-20261126.json) on
maintained source `5e9dd72` confirms public arguments 0 and 1 both reach the
actual IOCTL as flag 0. Each returns 180 pictures with firmware EOS and clean
leases; complete native outputs match, all software-reference luma planes
match, and saved pictures 0/89/179 pass visual comparison. Full official 1.3
restoration and ordinary 180-picture/EOS recovery pass. Its diagnostic
initialization failure is preserved separately. These native results do not
change the original device-free Stage A scope or establish physical DMA
observation, card decryption, physical display or A/V behavior.

Optional offline OEM interface research has not run in either change. B2
and Stage C remain `NOT_RUN / PROTOCOL_UNKNOWN` until a supported BCM70015
session/packet contract is identified. A direct internal flag1 native
diagnostic was not run; it would require separately established active state
and completed wake. No guessed protocol or security configuration was used.
