# Documentation

## Start here

For ordinary local video playback, start with the recommended stable release
and `crystalhd-play` in the [README quick start](README.md#quick-start).
Then use [everyday usage](USAGE.md) for playback, audio, subtitles and controls.

| What do you want to do? | Read first |
| --- | --- |
| Install and play your first file | [Quick start](README.md#quick-start), then [play a local file](USAGE.md#play-a-local-file) |
| Change playback options, audio or subtitles | [Everyday usage](USAGE.md) |
| Change shared defaults or use a settings UI | [Settings](README.md#settings); managed defaults, GTK and the Kodi add-on need the matching development/RC2 tools |
| Fix missing hardware, wrong components or playback failures | [Troubleshooting](USAGE.md#troubleshooting-and-proof-of-hardware-use), then [bring-up diagnostics](BRINGUP.md) |
| Use Kodi, VLC or a custom pipeline | [Application paths](USAGE.md#va-api-ffmpeg-kodi-and-vlc), [GStreamer](filters/gst/gst-plugin-1.0/README.md) |
| Stream from Sunshine | [Moonlight setup](MOONLIGHT.md) |
| Evaluate the optional native V4L2 Kodi route | [Native runtime delivery](NATIVE-RUNTIME-DELIVERY.md), with its exact prerelease prerequisites |

Current guides below explain how to use a feature. Detailed test histories and
research are listed afterward so they do not interrupt first-time setup.

## Current guides and contracts

| Task | Authoritative document |
| --- | --- |
| Install a released driver or use DKMS | [README](README.md), [DKMS guide](README.dkms) |
| Run playback, controls and supported opt-ins | [Usage](USAGE.md), [Moonlight](MOONLIGHT.md) |
| Diagnose the current host and decoder | [Bring-up](BRINGUP.md) |
| Select and persist settings | [Configuration](CONFIGURATION.md), [schema and inventory](CONFIGURATION-SCHEMA.md) |
| Check frontend support and acceptance limits | [Frontend acceptance](FRONTEND-ACCEPTANCE.md) |
| Acquire, build and activate the native companion | [Delivery](NATIVE-RUNTIME-DELIVERY.md), [building](NATIVE-RUNTIME-BUILDING.md), [support](NATIVE-RUNTIME-SUPPORT.md) |
| Review native qualification and its boundaries | [Native acceptance](NATIVE-RUNTIME-ACCEPTANCE.md) |

Released packages are identified by their tags and qualification receipts.
The published complete 1.3 RC2 and its assets are frozen; newer master work does
not replace that package or its registered module source. Stable promotion is
a separate maintainer decision. Use the release entry points above rather than
commands copied from an old experiment.

## Component guides

| Component | Guide |
| --- | --- |
| Library and examples | [libcrystalhd](linux_lib/libcrystalhd/README.md), [examples](examples/README.md) |
| GStreamer decoder and Player | [GStreamer 1.0](filters/gst/gst-plugin-1.0/README.md), [experimental MPEG-1](filters/gst/gst-plugin-1.0/MPEG1-EXPERIMENTAL.md) |
| Optional packed scaler and Xv output | [Packed scaler](filters/gst/packed-upscale/README.md), [Xv output](filters/gst/xv-output/README.md) |
| VA-API | [VA-API guide](filters/vaapi/README.md) |
| Kodi settings and route diagnostics | [Settings add-on](addons/script.crystalhd.settings/README.md), [route probe](tools/kodi-route-probe/README.md) |

The retained [GStreamer 0.10 README](filters/gst/gst-plugin/README) and
[generated INSTALL text](filters/gst/gst-plugin/INSTALL) are legacy material,
not the maintained build route. Its [COPYING](filters/gst/gst-plugin/COPYING)
remains the authoritative license text.

## Development and maintenance

| Task | Guide |
| --- | --- |
| Find remaining work and completed subsets | [Current work](TODO.md#current-priorities) |
| Contribute or update documentation | [Contributing](CONTRIBUTING.md), [maintainer rules](AGENTS.md), [external-code policy](EXTERNAL-CODE-POLICY.md) |
| Review the driver memory contract | [DMA notes](DMA.md) |
| Prepare a new release | [Releasing](RELEASING.md) |
| Check licensing and translation maintenance | [Licensing](LICENSES.md), [translation guide](po/README) |

## Historical evidence and diagnostic protocols

A dated record describes its named source, runtime and equipment. It is not a
live support matrix or the recommended setup procedure.

| Record | How to read it |
| --- | --- |
| [Hardware ledger](HARDWARE-2026-09-13.md) | Dated runs, failures and source-specific results; use its navigation and keep each run's scope. |
| [Release history](HISTORY.md) | Release chronology, not the current work queue. |
| [October 31 status](STATUS-2026-10-31.md) | Historical status and later annotations, with links to the current queue and archived text. |
| [Archived TODO](docs/history/TODO-2026-11-03.md) | Preserved completed milestones and earlier work descriptions; superseded live status stays historical. |
| [Archived status text](docs/history/STATUS-2026-10-31-through-2026-11-03.md) | Preserved dated claims and evidence, including their original limits. |
| [Centered payload protocol](tests/vaapi-centered-payload-protocol.md), [VPP payload protocol](tests/vaapi-vpp-payload-protocol.md) | Explicit OWN diagnostic observations; their results do not qualify the normal backend or a release. |

Fixture notes preserve the original inputs, failures and interpretation:
[VA/Kodi](tests/fixtures/issue445/README.md),
[FFmpeg loop](tests/fixtures/issue445/ffmpeg-loop/README.md),
[stock Bob payload](tests/fixtures/issue445/stock-bob-payload-20261031.md),
[admission](tests/fixtures/issue529/README.md),
[field output](tests/fixtures/issue530/README.md),
[V4L2 contracts](tests/fixtures/issue533/README.md),
[native playback](tests/fixtures/issue538/README.md), and
[runtime playback](tests/fixtures/issue606/README.md).

Current completion, remaining work and known limitations belong in the live
queue and owning issues. Update those summaries in place; add a dated evidence
record when a new observation needs preservation. A later passing run never
changes an earlier FAIL into PASS.
