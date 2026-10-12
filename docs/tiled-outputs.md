# Experimental native tiled outputs

`AQ_DRM_TILED_OUTPUTS=1` lets a compositor use one physical monitor whose
left and right halves have separate DRM connectors. For the tested iMac,
two 2560×2880 links become one 5120×2880 output. Hyprland can then tile
windows across the whole screen without a Hyprland source change.

The feature is experimental and disabled by default. If the driver already
offers a full-panel mode on one connector, Aquamarine keeps using that mode.

## Supported configuration

The prototype requires atomic DRM on one GPU, a complete single-monitor
2×1 TILE group, and a separate available display controller (CRTC) for each
half. Both halves must have matching native timings and share an XRGB8888
or ARGB8888 format/modifier pair. Connector names and hardware IDs are not
hard-coded.

The compositor sees the left connector's name and the full panel size.
Each CRTC runs at its native timing and reads its half of one shared
framebuffer. The full-panel size is never sent to either link as a physical
modeline. Aquamarine keeps buffers alive for each half and reports a frame
as presented only after both CRTCs finish. If a completion notification is
lost, a two-second timeout attempts to restore both heads with a blocking
modeset.

Some panels expose the second link only after the left link uses native
timing. At startup, an incomplete group with no full-panel mode can activate
the left link with a black buffer, then probe up to 20 times at 100 ms
intervals. That buffer stays alive until replacement. If discovery fails,
ordinary outputs remain available. An output already published to the
compositor cannot later become a tiled group.

## Limits

The group uses SDR and software cursors. VRR, HDR/wide color, gamma/degamma
ramps, nonidentity CTMs, tearing, queued commits and DRM leases containing
the tiled output are rejected or not advertised. Explicit synchronization
is not advertised; a supplied input fence is waited before submission.
DPMS preserves both heads' routing, modes and framebuffer pins when an
ACTIVE-only transition is sufficient. VT restore uses a complete group
modeset when that state cannot be trusted.

Arbitrary topology, multi-GPU operation, rotation, general hotplug and
suspend/resume are outside the tested scope.

Shutdown cleanup still needs work before upstream submission. If the seat
is inactive or disabling the group fails, Aquamarine retains resources so
it cannot free buffers that the display may still be reading. The output,
connectors and group can then keep each other alive even when the backend
is destroyed. Final cleanup needs to release these references safely after
the DRM device stops using them. This path has not been validated.

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
