# libcrystalhd

`libcrystalhd` is the legacy userspace API used to control the CrystalHD
kernel driver and firmware. This fork preserves its public headers and ABI
while updating the build and installation layout for current Linux systems.

The library builds in CI and is exercised indirectly by the GStreamer and
VA-API hardware paths on BCM70015. There is no comprehensive unit, ABI, or
error-path test suite, and BCM70012 has not been tested recently. New clients
should treat the API as a compatibility layer rather than a complete modern
media framework.

Build and stage the library with:

```sh
make
make DESTDIR=/tmp/crystalhd-library install
```

The install target provides the shared library, public headers, and
`libcrystalhd.pc` for `pkg-config`.
