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
