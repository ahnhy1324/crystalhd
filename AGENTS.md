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
