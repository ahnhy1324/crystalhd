# Contributing

Bug reports, test results, documentation fixes, and patches are welcome. A CrystalHD card is not required to contribute.

## Reporting bugs

Check existing issues first. Include what you can:

- Release tag/commit and the **installed** kernel module, library, and frontend versions.
- Card model (BCM70012/BCM70015), CPU architecture, distribution/kernel, application, and enabled options.
- Reproduction steps, expected and actual behavior, media codec/profile/resolution, and relevant logs.
- For playback failures, frame/timestamp counts, error messages, and close/reopen behavior where available.

Mark results **PASS**, **FAIL**, or **NOT RUN**, and distinguish real hardware from containers or emulation. Redact private paths, credentials, sensitive environment values, and media bytes before posting logs. See [BRINGUP.md](BRINGUP.md) for diagnostics.

## Issue triage and known-issue policy

Record impact and scheduling separately in the existing owning issue. Labels
are optional; this policy does not require mass relabeling or duplicate issues.

| Severity: impact | Definition |
| --- | --- |
| S0 critical | Confirmed unsafe DMA/buffer ownership, corruption, security impact, kernel crash or unrecoverable device/resource failure. Stop affected release activity and investigate. |
| S1 high | Supported valid streams lose decoded picture identity/PTS or fail; serious reproducible regressions in ordinary seek, EOS, Stop, reopen or recovery. |
| S2 moderate | Bounded, recoverable playback/output performance or quality degradation. Record measured scope and available alternatives. |
| S3 low | Small user-visible impact, minor diagnostics/ergonomics, or isolated experimental quality deviation without a supported-path correctness claim. |

**Unassessed is not S3.** Missing evidence does not establish low impact.

| Priority: scheduling | Action |
| --- | --- |
| P0 immediate | Block affected work/release and investigate urgently. |
| P1 high | Fix before the next affected release, or explicitly exclude the defective feature from its claims. |
| P2 normal | Schedule behind safety, correctness and reliability work. |
| P3 deferred | Known limitation, nonessential tuning or bounded research awaiting a stated trigger. |

Choose priority using supported scope, repeatability, regression status,
reachability, workaround, release claims, cost/risk and competing work. A
workaround can change scheduling without removing the underlying impact.
Only the maintainer/release tracker can designate an issue nonblocking for a
specific release scope. [Release gates](RELEASING.md) and frozen oracles take
precedence: deferral never turns **FAIL, HOLD, REVIEW, NOT RUN or UNKNOWN**
into **PASS**, or supplies missing qualification.

Keep video stages explicit: compressed AU admission; decoded picture/field
completeness, identity and PTS; frontend output/QoS; sink render calls or
X-server acknowledgments; physical scanout and A/V observation. A render drop
is not a hardware decoder drop, and submission/completion is not physical
presentation. Preserve exact source/component/host identities with results.
Separate static firmware findings, synthetic/emulated tests, native hardware
measurements and proven supported behavior.

Known issues cannot waive unsafe DMA, memory corruption, kernel/lifecycle
deadlock, silent unsupported fallback, unexplained valid decoded-picture loss,
or supported count/pixel/PTS, EOS/drain, lifetime/resource and ABI/ioctl gates.
Quantified deadline/QoS losses may be nonblocking for a specifically bounded
or excluded display-performance claim; record frequency and burstiness. There
is no universal acceptable drop percentage or resulting FPS/smoothness claim.
Experimental pixel differences need separate investigation of chroma siting,
filtering, rounding and scaling. They never silently change a frozen bit-exact
oracle; production promotion requires conformance/quality evidence.

Use this compact decision record in the owning issue; link existing evidence
rather than copying long logs. A deferred limitation needs a reason and a
revisit trigger, while a fixed result needs evidence of corrected behavior.

```text
Type: correctness / reliability / security / compatibility / quality / performance / research
Severity: unassessed or S0–S3; Priority: P0–P3
Disposition: active / known limitation (deferred) / not supported / needs evidence / fixed
Affected: device, host, frontend/feature mode, code/package/version, fixture
Observed / expected:
Evidence: exact source, pipeline stage, static/synthetic/emulated/native, PASS/FAIL/REVIEW/NOT RUN
Supported scope / next-release GO/HOLD impact / workaround:
Decision: deferral reason, risks, owner or tracking issue, revisit triggers
```

Revisit on regression, broader support claims, loss of the workaround, QoS
bursts, worse physical A/V or new evidence. Illustrative dispositions, not
assignments to entire umbrella issues:

- [#91](https://github.com/ahnhy1324/crystalhd/issues/91): the measured CPU-FHD/Xv
  QoS row on the X61 can be S2/P2; retain its strict full-census FAIL. Card
  `--scale-width 1280` with sink fitting is a measured alternative on that host,
  not a general guarantee or a classification of all #91 work.
- [#513](https://github.com/ahnhy1324/crystalhd/issues/513): async Xv/SHM is
  experimental research/P3 pending prerequisites, not a supported backend or
  proof of scanout/physical A/V.
- [#92](https://github.com/ahnhy1324/crystalhd/issues/92): MPEG-1 via algorithm 1
  is non-production research. Investigate chroma/reference differences before
  making a production pixel-tolerance claim.
- Confirmed valid picture loss, unrecoverable decoder ownership or a
  DMA-lifetime fault on a supported path cannot use a renderer-QoS allowance.

Keep user-facing limitations and workarounds in the existing
[usage guide](USAGE.md#known-issues-and-deinterlacing-roadmap); reproduction,
evidence and scheduling remain in each problem's issue.

## Pull requests

- Keep changes focused and link related issues. Discuss broad ABI, firmware, DMA, or device-lifetime changes before implementation.
- Preserve the documented ioctl and `libcrystalhd.so.3` contracts, default behavior, and single-session device ownership. Experimental features must be explicit opt-ins.
- Add regression tests when practical, report what was actually run, and update affected documentation.
- Keep existing copyright and SPDX notices; see [LICENSES.md](LICENSES.md).

## Validation

Run relevant [developer checks](README.md#developer-checks). Hardware evidence should identify the installed components, device, test input, frame/PTS/pixel results, errors, and clean teardown. If hardware is unavailable, say **NOT RUN**; CI/build success does not prove physical playback.

Maintainer release gates and support claims are tracked in [RELEASING.md](RELEASING.md).

PR CI examines the complete change from its merge base. Modifications of the
explicit prose files in [the CI selector](tests/ci-plan.py) use lightweight
checks; configuration documentation also runs its inventory/schema validators.

The first component lane covers ordinary modifications to five explicit packed
scaler CPU inputs: the plugin, engine and CPU-test C files, `check.sh`, and
`tests/packed-sanitizer-loader.test.py`. A PR confined to those inputs, with
optional reviewed prose/configuration documentation, runs the ordinary packed
CPU check, the isolated ASan/UBSan/leak check and the five loader contracts on
both Ubuntu releases. It also runs the configuration inventory/schema validators,
which check the plugin's read-only properties. GStreamer 1.28.2 executes the
36 stock/candidate frame comparisons; other versions check factory loading and
explicit version rejection only. This lane adds no X11 or hardware qualification.

Other modifications of the selector's explicit userspace source/test list run
both complete userspace builds while skipping the ten upstream kernel-API jobs.
Mixed packed/other-component changes also keep both complete builds. These
builds still compile/test the driver against each runner's packaged headers.
They run the packed normal, sanitizer and loader checks inline; the standalone
packed lane is then unselected, so each Ubuntu job runs these checks once.
The userspace list covers selected GStreamer/VA/DIL implementation files,
Player and associated tests, not every userspace path. Separate GStreamer
decoder, VA, DIL, Player and Xv component lanes and broader sanitizer lanes
remain unimplemented pending dependency and check-coverage review.

Both full userspace jobs also run `make library-check-sanitized`: fresh isolated
ASan/UBSan/leak builds of the actual DIL TX ring, concurrent status/FIFO, raw
YUY2 copy, planar copy and format-conversion regressions. The concurrent case
runs ordinary, status-observer and serial controls. Device opens and unexpected
ioctls abort; normal binaries and CPU-mode stamps are not rebuilt. This selected
CPU coverage does not qualify the complete DIL, hardware/DMA lifecycle, native
i386 sanitizers or the excluded shutdown-heap follow-up. Raw-frame serialization
already has separate sanitizer coverage in `raw-frame-check`.

All headers, including packed-private headers, build inputs, fixtures, generated
files, workflow/selector changes, new or unknown paths, renames, deletions and
file-mode changes keep the full userspace and upstream kernel checks. There
are no directory or extension exemptions. An empty diff also takes the full
route; an incomplete diff or failed classifier fails CI. The final **CI gate**
requires every selected job to succeed and every explicitly unselected lane to
be skipped. Missing, invalid or inconsistent selections and failed, cancelled,
timed-out or unexpectedly skipped jobs cannot pass the gate.

The userspace build runs on explicitly selected `ubuntu-24.04` and
`ubuntu-26.04` runners, preserving the older supported API check alongside
the newer stack. Both results are required by the final gate. These labels
select Ubuntu releases; hosted images and installed packages receive updates,
so each job records its actual OS, image and toolchain/library versions in the
step summary. This is compatibility coverage, not an immutable build environment.

Feature branches run CI through their PR, without a duplicate push build.
Master/tag pushes always run the full suite. To request full CI for another
branch before opening a PR, select that branch under **Actions → CrystalHD CI →
Run workflow**. CI does not replace the hardware release gates above.
