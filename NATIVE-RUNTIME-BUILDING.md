# Rebuild the unchanged native FFmpeg runtime

Verify the outer SHA256SUMS first. Extract the source and its separately
supplied independent Git metadata into new writable work directories:

```sh
mkdir rebuild
tar -xf materials/source-materials.tar -C rebuild
tar -xzf materials/ffmpeg-offline-git-metadata.tar.gz -C rebuild/source
git -C rebuild/source rev-parse HEAD
git -C rebuild/source rev-parse HEAD^{tree}
git -C rebuild/source diff --quiet HEAD --
git -C rebuild/source fsck --full --no-reflogs
```

Expected revision is `12cc1f42f06ff0e85633c2602c96786fbd280b70`, tree
`10bdcda36b5c1b96cce93bfe5571c97e2b2a7cef`. The original complete source
archive is unchanged. Its added Git companion contains no remote, alternate,
promisor, hooks or host-checkout dependency. Offline reconstruction was checked
with network disabled and the original checkout absent.

Install ordinary build tools separately: GNU make4.4.1, GCC15.2.0
Ubuntu15.2.0-16ubuntu1, binutils2.46, NASM3.01, pkg-config and headers/libraries
libdrm2.4.131, libva pkg-config1.23.0 (libva2.23.0), zlib1.3.1 for the recorded
reproduction environment. These are compile-time Ubuntu inputs; they differ
from the target Platform providers documented in SUPPORT. A different
toolchain may produce different ELF bytes and needs its own qualification.
No compiler binaries are included in the runtime or required at activation.

With NASM3.01 on PATH, use new absolute, disjoint build/prefix paths:

```sh
export LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1790018630 FFMPEG_CPU=core2
sh tools/build-native-ffmpeg.sh "$PWD/rebuild/source" \
  "$PWD/rebuild/build-a" "$PWD/rebuild/prefix-a"
sh tools/build-native-ffmpeg.sh "$PWD/rebuild/source" \
  "$PWD/rebuild/build-b" "$PWD/rebuild/prefix-b"
```

The helper does not edit external source. Its SHA-256 is
`32db3e781ee4911a5b87c04af3c46d90d3e6d1ac62b31bed6d62fd7c73aa18df`.
It configures `/usr` as metadata prefix, stages privately through DESTDIR and
uses `make -j1`. Full original config/log materials are in the source TAR.

`evidence/build-proof.json` records two actual independent, device-free,
network-free builds with original source and host `/usr` read-only. Every one
of nine regular ELF payload files matches between A/B and the retained runtime
byte-for-byte. This establishes source/binary correspondence by reproduction;
it does not reconstruct the historical command log of the older build.
Compare file sizes and hashes to that receipt before assembling another runtime.

Unpack `materials/dependency-materials.tar` separately to inspect the full
source and SDK recipe/patch context for all four declared provider components.
Their exact revisions, URLs, checksums and notices are retained. Provider
binaries were not rebuilt in this campaign and are acquired from the pinned
Platform. This recipe does not claim to reproduce the entire SDK/Platform.
