# Experimental native tiled outputs

`AQ_DRM_TILED_OUTPUTS=1` combines two horizontal DRM tiles into one output.
It is disabled by default. This path is intended for a single physical
monitor whose driver exposes a separate connector and CRTC for each half.
Window tiling in the compositor then uses the full logical output normally.
A driver that already advertises a full-panel mode continues to use that
mode instead.

## Supported configuration

The prototype requires an atomic DRM backend without a multi-GPU blit path,
a complete single-monitor 2×1 TILE group, distinct available CRTCs, matching
native timings, and a shared XRGB8888 or ARGB8888 format/modifier pair.
The tested panel is an iMac 5K whose two 2560×2880 links form a 5120×2880
output. Connector names and hardware IDs are not hard-coded.

The compositor sees the origin connector's output name and aggregate mode
size. Each CRTC receives its own native modeline and scans its rectangle of
a shared full-size framebuffer. The aggregate size is never submitted as a
physical modeline. Buffer pins are held per head, and a frame is presented
only after every member CRTC reports completion. A two-second timeout
attempts a blocking group modeset if a notification is lost.

Some panels expose the second link only after the origin uses native timing.
At startup, an incomplete group with no aggregate mode can activate that
origin using a black buffer, then probe up to 20 times at 100 ms intervals.
The buffer remains alive until replacement. Failure leaves ordinary outputs
available; the backend does not invent a missing tile or promote an output
that has already been published.

## Limits

The group uses SDR and software cursors. VRR, HDR/wide color, gamma/degamma
ramps, nonidentity CTMs, tearing, queued commits and DRM leases containing
the tiled output are rejected or not advertised. Explicit synchronization
is not advertised; a supplied input fence is waited before submission.
DPMS preserves both heads' routing, modes and framebuffer pins when an
ACTIVE-only transition is sufficient. VT restore uses a complete group
modeset when that state cannot be trusted.

Arbitrary topology, multi-GPU operation, rotation, general hotplug and
suspend/resume are outside the tested scope. Teardown while the seat is
inactive or a group-disable ioctl fails retains resources; final backend
destruction in that state still needs a resource-lifetime design. In
particular, the output/connector/group ownership cycle must not be mistaken
for a completed teardown. This is a review limitation, not a validated path.

## Validation

`drmTile` tests bounded TILE parsing, topology/timing validation, aggregate
mode detection and format/modifier intersection. `drmTileFrame` tests
out-of-order, duplicate and stale events, cancellation, and rollback of a
rejected replacement frame. Build and run with CTest; `simpleWindow` needs
a running Wayland compositor.

Hardware validation must also check both native CRTC modes, matching shared
framebuffer IDs and source rectangles, lifecycle transitions, and physical
continuity across the seam. A 5120×2880 value in the compositor's monitor
listing alone does not prove scanout on both links.
