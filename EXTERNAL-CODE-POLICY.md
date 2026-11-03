# External code policy

**We do not modify third-party code.** This is a maintainer requirement.

It applies to development, debugging, experiments, tests, private builds,
temporary source copies, CI, packaging and releases. There is no private-build,
research-only or test-only exception.

- Do not create, apply, maintain, build, execute or distribute project patches
  to Kodi, FFmpeg, GStreamer, Mesa, GTK, the Linux kernel core, external
  libraries, external binaries or firmware.
- Do not rewrite copied third-party source excerpts or create modified
  third-party candidates in temporary test trees. Location inside this
  repository does not make third-party code ours.
- Inspect, build and run external components as supplied by their upstream
  project or official distribution, using their supported build options.
  Do not add source edits, replacement translation units or binary-code patches.
  Modified external code must not be run through an emulator or interpreter.
- Implement integration fixes in the maintained CrystalHD driver, library,
  VA-API/GStreamer components, Player and project-owned tools. These maintained
  project components remain editable; third-party snapshots do not.
- If an unmodified external component exposes a limitation, retain the failing
  result and investigate a valid fix in our own code. Record an unsupported or
  unresolved boundary when appropriate. Do not patch the external component,
  fabricate success or weaken the acceptance criteria.

Malformed-input tests of our own parsers and integrity guards are test data,
not external repair candidates. Such data must be rejected before external
code is executed. This does not authorize building or running modified
third-party code, even in a temporary test tree.

Historical results obtained with modified external code are historical
observations only. They cannot qualify stock/unmodified compatibility, count
as a CrystalHD repair or justify a release claim. Keep their provenance and
failure records visible; do not silently relabel them as stock results.

The former Kodi publication-order patch and its patch-applying/mutating
test harness have been withdrawn under this policy. Their historical receipts
are retained with an explicit withdrawn status; the patch itself is removed.
