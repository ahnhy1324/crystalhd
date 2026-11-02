# Original-stock Bob: bounded OWN payload evidence

`stock-bob-payload-03` passed with unchanged original Kodi/FFmpeg images and the
private OWN VA diagnostic backend at source
`1a495f467a5cac076db060b0fa3e1f5b3ceb6d01`. The actual route was
`ff-h264-vaapi` plus `vaapi-bob` (original setting22). TFF and BFF each completed
one natural EOF:50 original bindings and100 unique successful VPP operations,
two operations per original. Application exit was0; boundary and kernel checks
were clean. Failed cases01/02 and their plans remain preserved.

Eight checkpoints cover both physical parities of four original pictures:

| Fixture | Original source ordinals | Successful checkpoint operations |
| --- | --- | --- |
| TFF |1,2 |1,2,5,6 |
| BFF |1,4 |101,102,103,104 |

Actual source/target canvases and requested origin rectangles were1280x736,
with1280-byte Y/UV pitches. Each packed checkpoint contains only the top active
1280x720 prefix. All11,059,200 target Y/U/V bytes matched the independently
selected rows of the captured OWN source. Equal requested Bob rectangles make
this prefix mapping exact independently for Y and UV. Coded bottom padding and
pitch tails are excluded. Every runtime checkpoint used a completed full-frame
source; compact first-field handling has CPU controls only in this run.

The observer copies inside existing CPU ownership windows, retains successful
current commits, and publishes frozen independent arrays from the existing
caller after its mutex is released. Successful selected decoder retirements
`(context2,generation1)` and `(context5,generation1)` join raw sequences1323 and2697.
The contiguous observed trace prefix contains2760 records. These are OWN storage
view/context receipts, with no Driver-destruction or GPU-release inference.

The ordinary VA artifact is unchanged:
`147c142e1bb6a527fcd5826651f8bfcee6e6d9667b01bf3fa9f548493e630c48`.
The separately loaded diagnostic VA artifact is
`563ae7fde928c6cd4132dad891b4e0bcead2d18b2d83b040d74ce83ae81ae76e`.
The normalized JSON receipt records inputs, checkpoints and rechecked artifact
hashes. Root separately replayed the committed CLI over the retained manifest,
trace and all blobs: PASS. The focused CPU219 and reader29 controls also pass.
The exact replay command is:

```sh
python3 -B /home/mache/.cache/crystalhd-va-vpp-payload-observer-20261031/tests/vaapi-vpp-payload-reader.py \
  --manifest /home/mache/.cache/crystalhd-all-work-20261031/stock-bob-payload-03/payload/vpp-payload-1214009-2.json \
  --trace /home/mache/.cache/crystalhd-all-work-20261031/stock-bob-payload-03/kodi.stdout
```

This proves the eight selected active OWN CPU payloads. It does not prove every
field's pixels, source/output PTS correspondence, chronological presentation,
GPU import/fence lifetime, displayed pixels, physical A/V synchronization or
performance. Only the current T8100/BCM70015 host was exercised.
