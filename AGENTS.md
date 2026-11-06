# Mandatory project instructions

Before working on this repository, read [EXTERNAL-CODE-POLICY.md](EXTERNAL-CODE-POLICY.md).

**Do not implement or ship CrystalHD fixes by patching external applications
or dependencies.** Fix maintained CrystalHD components and project-owned
integration code; qualify stock support with unmodified external runtimes.
Record confirmed external limitations as scoped known issues.

**Debugging and test probes are allowed.** Isolated instrumentation,
source-unit scaffolds, temporary excerpt variants, semantic mutation controls
and firmware/interpreter models are diagnostic tools. Do not remove existing
coverage by misapplying the external product-code rule. Keep original inputs,
provenance and test scope explicit; diagnostic variants are not product fixes
or stock compatibility/release acceptance. Follow the full policy.

## Current source and installed baseline

At the start of new work, check the current owning issues, TODO, maintained
`origin/master` and latest published stable release. Base new fixes on the
latest maintained source. An older source is an explicitly named comparison
input; audit its difference from current code before reusing a proposed fix.

Keep the persistent host baseline matched to the current official stable
release: selected and loaded kernel module, library, frontends and tools must
belong to that exact released source. Published `1.3.0` is frozen source
`24a5182b988b` / module-DKMS `3.10.20`. A module version alone, or temporarily
loading that module over older userspace, does not establish a matched install.
If the host is still older or mixed, record migration as pending and establish
the matched stable baseline before further experiments.

After publication, older module13/1.2 or development-runtime states are
historical comparison inputs, never an implicit default restoration target.
Experiments must return to the established current stable baseline. A rollback
to an older release needs an explicit, recorded purpose; do not silently keep
using a pre-release host snapshot as the normal baseline.

Preserve frozen releases, registered sources, old guards, captured baselines
and every original failure. Create a new source/version-qualified guard and
baseline receipt after the real migration; do not edit an old receipt or loosen
an old guard to accept the new state. Record actual source/runtime provenance,
selected/loaded module, ownership, options and restoration with the existing
checks. Keep verification proportional; this adds no per-action hash inventory
or new generic test/CI gate.

## Integration into master

Use a task branch and a PR for every change to master, including documentation.
Run affected checks and independent review, then require the existing `CI gate`
to pass against the current base before merging. Refresh the integration if
master advances. Do not push directly to master, bypass checks or disable its
protection to finish a task. GitHub protection also applies to administrators
and disallows force pushes and deletion. Keep automatic branch deletion off.

## Upstream PR branches

Preserve `upstream/01-linux-6.1`, the submission branch for
[yeradis/crystalhd#9](https://github.com/yeradis/crystalhd/pull/9), at its original
PR head. Do not delete, rewrite or reuse upstream PR branches without an explicit
maintainer instruction for that branch. A local merge or closed fork PR does
not establish that an upstream submission is disposable. Check cross-repository
PR relationships before any branch cleanup.

## Planning and verification

Before choosing development work, read the current owning issue and TODO.
Distinguish implemented/merged work, actual remaining work, scoped known issues
and unavailable-target dispositions. A successful rerun does not establish
that an earlier crash was fixed. Nonblocking research continues.

Use checks that can distinguish the changed behavior. Do not calculate hashes
for every action or rerun whole suites merely to produce more verification.
This does not authorize removing existing diagnostic probes, meaningful
pixel/integrity tests or frozen release requirements.

Use current equipment only. Source-qualified no-drift logs satisfy A/V for the
exact route; accepted renderer/QoS losses do not create a universal zero-loss
gate. Keep source-picture/ownership/ABI requirements. The original #538 heap
subtask stays excluded; #538 remains open for its other work.

Before choosing an experimental playback gate, read the accepted-loss reference
and counter boundaries in [RELEASING.md](RELEASING.md#experimental-playback-acceptance).
Consult the original owning decisions before claiming a prior criterion is
absent or asking the maintainer to repeat it.
