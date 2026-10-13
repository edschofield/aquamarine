# DRM shutdown and consumer integration

Final shutdown now separates output destruction from scanout retirement.
It stops new submissions, drains async commit work, cancels scheduled work
and detaches every head before notifying consumers. Retained outputs and
framebuffer attachments become inert. Callbacks collected before shutdown
check their owner's lifetime before accessing it or a descriptor.

Ordinary runtime disconnect remains retryable. Final shutdown is idempotent
and runs before a backend is removed, including startup failure, GPU removal
and top-level backend destruction.

## Retirement and release

A complete blocking disable on the original device context permits release
of that backend's scanout pins. All relevant primary/cursor planes, CRTCs
and connector routings are covered after submissions have drained. Removing
a framebuffer, closing an FD or querying software plane state does not prove
that scanout has stopped.

If retirement is unresolved, the independent device context retains strong
buffer and allocator references as well as the exact pin counts. It gets a
bounded recovery opportunity. Session termination or a failed retry moves
those resources into terminal quarantine for the remainder of the process.
Outputs and tiled groups still detach. Buffer-release notifications wait.

Quarantine deliberately retains resources until process exit; it is not
leak-free cleanup. It also rejects buffer reuse through Aquamarine and blocks
new DRM backend admission for the rest of the process. A timeout cannot
authorize reuse of storage the display may still be reading. Successful
retirement relies on the driver's blocking-commit contract; userspace cannot
guarantee cleanup after an arbitrary device failure.

`CDRMBackend::shutdownStatus()` reports the disposition separately from kernel
resource cleanup:

| State | Meaning |
| --- | --- |
| `RUNNING` | Final shutdown has not begun. |
| `DEFERRED` | Display objects are detached; unresolved buffers and pins await recovery. |
| `RETIRED` | Scanout retirement is proven; this backend's pins can release. |
| `QUARANTINED` | Unresolved resources remain retained until process exit. |

The status also reports outstanding buffers/pins, the disable error and the
historical count of cleanup errors. Proven retirement can release pins even
when kernel-resource disposal fails. Successful framebuffer removal alone
cannot authorize release. Pins held by another backend still delay a buffer's
final release event.

## Matching consumers

Public C++ layouts changed while the library still uses SONAME 14. Rebuild
downstream consumers with these headers and select the matching library at
runtime. An older compositor binary must keep using its older library.

The tested consumer is Hyprland v0.56.2 at
`efb50993780079460b0cbed1363e2166a2de1d9f`, with the supplied
[consumer patch](hyprland-drm-shutdown.patch). It guards cursor updates when
an output's backend has detached and calls `CBackend::shutdown()` before
Hyprland terminates the shared EGL display. Integration exposed a null-backend
cursor crash and an EGL teardown error; these changes addressed both.

Check and apply the patch in the matching Hyprland source checkout, using
the absolute path to this repo's `docs/hyprland-drm-shutdown.patch`:

```sh
git apply --check /path/to/aquamarine/docs/hyprland-drm-shutdown.patch
git apply /path/to/aquamarine/docs/hyprland-drm-shutdown.patch
```

Then rebuild Hyprland against the new Aquamarine headers and library. Other
consumers need the same lifetime rules: tolerate detached outputs during
destruction notifications, honour buffer pins and shut Aquamarine down while
their shared rendering services remain usable.

## Tests

Run the noninteractive suites on Linux:

```sh
cmake -S . -B build/cleanup -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/cleanup --target drmShutdown attachments output commitThread drmTile drmTileFrame -j3
ctest --test-dir build/cleanup --output-on-failure -R '^(drmShutdown|attachments|output|commitThread|drmTile|drmTileFrame)$'

cmake -S . -B build/cleanup-sanitize -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'
cmake --build build/cleanup-sanitize --target drmShutdown attachments output commitThread drmTile drmTileFrame -j3
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/cleanup-sanitize --output-on-failure -R '^(drmShutdown|attachments|output|commitThread|drmTile|drmTileFrame)$'
```

`drmShutdown` exercises production backends, connectors, outputs, imports and
async requests with injected DRM operations, a pipe descriptor and a timerfd.
It opens no GPU or libseat session. Its 19 cases cover reentrancy, stale
callbacks, descriptor reuse, partial imports, shared buffers, worker drain,
startup failure, deferred recovery and terminal retention. Quarantine cases
run in child processes and assert the retained set without forced unpinning.

`drmShutdownHardware` is a separate, iMac-specific manual probe. It is excluded
from CTest and the default build because it changes the physical display and
requires an otherwise unused local seat. `simpleWindow` requires a Wayland
compositor; its unfinished event loop does not provide interactive rendering
coverage.

Validation on 13 October 2026 used kernel `7.2.9-imac-dc5k2`, GCC 16.2.1,
Hyprutils 0.14.2, libdrm 2.4.134, Mesa GBM 26.2.4-arch1.1, libseat 0.9.3 and
libinput 1.32.0. All six selected suites passed normally and with ASan/UBSan
and leak detection, including a fresh rerun when preparing this branch.
The matching patched Hyprland passed all 270 unit tests in both builds.
Hardware and compositor trials covered active retirement, inactive quarantine
and recovery after seat activation with real dumb and GBM buffers.

Those results do not cover the complete multi-device/lease matrix, hardware
suspend/resume or runtime physical unplug/reconnect. Injected libseat-close
and retirement-record provisioning failures remain coverage gaps. The AMDGPU
link warning described in [tiled outputs](tiled-outputs.md) remains unresolved.
