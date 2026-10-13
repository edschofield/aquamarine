# Experimental native tiled outputs

`AQ_DRM_TILED_OUTPUTS=1` lets a compositor use one physical monitor whose
left and right halves have separate DRM connectors. For the tested iMac,
two 2560×2880 links become one 5120×2880 output. Hyprland can then tile
windows across the whole screen. The tested cleanup requires a matching
consumer rebuild and the [Hyprland shutdown patch](hyprland-drm-shutdown.patch).

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

Final shutdown now stops submissions, drains commit work and detaches every
head before emitting destruction or buffer-release events. A successful
blocking disable permits buffer release. If the seat is inactive or disabling
scanout fails, an independent device context retains the buffers and their
pins while the output, connectors and group detach. Recovery is bounded;
unresolved resources enter process-lifetime quarantine without release
notifications. This deliberately retained set is not leak-free cleanup.
See [DRM shutdown](drm-shutdown.md) for consumer requirements and failure policy.

## Validation

`drmTile` tests bounded TILE parsing, topology/timing validation, aggregate
mode detection and format/modifier intersection. `drmTileFrame` tests
out-of-order, duplicate and stale events, cancellation, and rollback of a
rejected replacement frame. `drmShutdown` adds 19 cases using production
lifetime objects and injected DRM operations. All six noninteractive suites
passed normally and with ASan/UBSan and leak detection on 13 October 2026;
[reproduction commands](drm-shutdown.md#tests) select those suites explicitly.
`simpleWindow` needs a running Wayland compositor.

The iMac hardware and matching Hyprland integration passed native 5K and
ordinary 4K, active shutdown, inactive-session quarantine and deferred
retirement after seat activation. Compositor checks covered DPMS, VT return,
scales 1/1.25/2, windows across the seam and fullscreen. Physical observation
confirmed the center seam and movement onto the external Cinema Display.
The AMDGPU `enabling link 1 failed: 15` warning recurred during a sanitizer
trial; its cause remains unresolved. Runtime physical unplug/reconnect,
suspend/resume and the broader multi-device/lease matrix remain unproven on
hardware.

Hardware validation must also check both native CRTC modes, matching shared
framebuffer IDs and source rectangles, lifecycle transitions, and physical
continuity across the seam. A 5120×2880 value in the compositor's monitor
listing alone does not prove scanout on both links.
