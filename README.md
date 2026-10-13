## Aquamarine

Aquamarine is a very light linux rendering backend library. It provides basic abstractions
for an application to render on a Wayland session (in a window) or a native DRM session.

It is agnostic of the rendering API (Vulkan/OpenGL) and designed to be lightweight, performant, and
minimal.

Aquamarine provides no bindings for other languages. It is C++-only.

## iMac 5K development branch

This branch adds opt-in native tiled outputs: two 2560×2880 DRM links can
drive one 5120×2880 logical output with `AQ_DRM_TILED_OUTPUTS=1`. The feature
remains experimental and disabled by default. See [tiled outputs](docs/tiled-outputs.md)
for the supported configuration and validation results.

Final DRM shutdown now retires scanout before releasing buffers and detaches
outputs even when retirement fails. Unresolved buffers stay pinned until
process exit. The [shutdown guide](docs/drm-shutdown.md) explains that policy,
the tests, and the required consumer changes. Rebuild consumers with matching
headers; this branch changes public C++ layouts while retaining SONAME 14.

## Stability

Aquamarine depends on the ABI stability of the stdlib implementation of your compiler. Sover bumps will be done only for aquamarine ABI breaks, not stdlib.

## Building

```sh
cmake --no-warn-unused-cli -DCMAKE_BUILD_TYPE:STRING=Release -DCMAKE_INSTALL_PREFIX:PATH=/usr -S . -B ./build
cmake --build ./build --config Release --target all -j`nproc 2>/dev/null || getconf _NPROCESSORS_CONF`
```


