# Historical Kodi investigation records — external patch withdrawn

**We do not modify third-party code.** See the repository's
[external code policy](../../../EXTERNAL-CODE-POLICY.md).

The Kodi publication-order patch, patch-applying test runner and modified-code
scaffolds have been removed. The former `make kodi-compat-check` target and
its source-copy mutation workflow are withdrawn. Do not recreate or apply them.

`publish-order-observation-20261029.json`,
`publish-order-playback-20261029.json` and
`publish-order-controls-20261029.json` remain historical records. Their
patched-candidate results are **WITHDRAWN / NOT STOCK ACCEPTANCE**. They do
not establish a CrystalHD repair, unmodified Kodi support or release quality.
The original observations/failures and component identities are retained;
retention does not authorize replaying the prohibited modified candidates.

The stock observation remains a **known issue** in its recorded Kodi VAAPI
publication/accounting scope. Receiver-priority diagnostic scheduling exposed
a last-picture natural-EOF drain timeout of approximately two seconds; the
normal-scheduling control reached EOF. The record does not establish a
general VA-API/CrystalHD-driver defect, other builds or ordinary-workload
frequency. See the [user-facing known issue](../../../USAGE.md#known-issues-and-deinterlacing-roadmap).

Read-only upstream source excerpts retain their original license/provenance.
Existing unmodified-client results elsewhere keep their own source-bound
scope. The following older narrative is historical and must be read with
this withdrawal notice.

The separate [FFmpeg loop-boundary record](ffmpeg-loop/README.md) reconciles
the retained 296/300 failure and executes that client's EOF/teardown functions.
It preserves the native failure and does not extend this Kodi test's scope.
