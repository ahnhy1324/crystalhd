# External code and diagnostic probes

**We do not implement or ship CrystalHD fixes by modifying external applications
or dependencies.** This is a maintainer requirement. Debugging and test probes
are distinct from changes to another project's product code.

## Product and compatibility boundary

- Do not maintain, apply or distribute functional patches to external Kodi,
  FFmpeg, GStreamer, Mesa, GTK, kernel core, libraries, binaries or firmware
  as a CrystalHD product fix or required replacement dependency.
- Implement product fixes in the maintained CrystalHD driver, library,
  VA-API/GStreamer components, Player and project-owned integration tools.
- Qualify stock compatibility and releases with external components as supplied
  by upstream or their official distribution. A private functional fix does
  not qualify the unmodified application.
- Record confirmed external limitations as scoped known issues. Preserve actual
  failures, original component identities and unresolved boundaries. Do not
  fabricate success, change our outputs incorrectly to mask a client defect,
  or weaken acceptance criteria.

## Debugging and test probes

- Isolated debugging instrumentation, observation hooks, source-unit scaffolds,
  semantic mutation controls, negative inputs, emulation and interpreter models
  are allowed. Temporary modified excerpts or diagnostic copies may be used
  to test a hypothesis or validate an oracle.
- Preserve the original inputs, license notices, provenance and integrity
  checks. Keep diagnostic variants isolated from installed components,
  distribution artifacts and normal product execution.
- Label original-runtime observations, instrumented runs, counterfactual test
  variants and actual product fixes separately. State what each test executes
  and its limits. A scaffold or modified-client result cannot establish stock
  application support or release acceptance.
- Do not delete debugging or regression coverage merely because it observes
  external code or constructs a test-only variant. The product-code boundary
  is not a blanket ban on debugging and tests.

The former distributable Kodi publication-order patch remains removed. Its
observed stock EOF issue is documented as a known issue. Publication/lifecycle
source-unit probes and their mutation controls remain diagnostic coverage.
Historical private modified-Kodi playback records retain their provenance and
cannot qualify stock compatibility or a CrystalHD release.
