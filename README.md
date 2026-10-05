# KolosLinux

Linux-side Kolos prototype for ARM64 Debian and Qualcomm Turnip. Android owns
the AHardwareBuffer allocations and SurfaceControl presentation in a separate
repository. This repository contains no Android application code.

The starting implementation is the working `NativeTest/linux-utilites` AHB
diagnostic. Its Linux sources were supplied in this repository's initial working
directory. The first commit preserves that source before module extraction.

## Layout

- `include/kolos/`: Vulkan context, imported buffer ownership and renderer interfaces.
- `src/`: device initialization, AHB handle reception/dma-buf import, GPU clear.
- `tools/kolos_ahb_stream.cpp`: triple-buffer diagnostic and FIFO frame orchestration.
- `tools/kolos_ahb_import.cpp`: original standalone single-buffer diagnostic.
- `protocol/`: Android-compatible socket protocol and lifecycle specification.

## Build on ARM64 Debian

Install a C++17 compiler, CMake, Vulkan development headers/loader and the tested
Turnip ICD. For Debian build dependencies:

```sh
sudo apt install build-essential cmake libvulkan-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Select the tested ICD and start the Android application before running:

```sh
export VK_DRIVER_FILES=/absolute/path/to/tested/freedreno_icd.json
./build/kolos_ahb_stream /mnt/kolos-wayland/ahb-stream.sock 300
```

The socket must be accessible from Debian through the existing shared mount.
The default socket is `/mnt/kolos-wayland/ahb-stream.sock`; default frame count
is 300. The single-buffer tool uses `ahb-test.sock` and requires the separate
Android diagnostic endpoint, which the normal Android stream startup does not create.

## Preserved behavior and validation

Import is device-specific: 148 handle bytes, two FDs, 640x480 RGBA8,
linear DRM modifier 0, offset 0, row pitch 2560, allocation size 1228800.
The first FD is imported as dma-buf; the second is retained for cleanup.
Images have `TRANSFER_DST` usage only. This is an animated GPU clear, not a
graphics pipeline or generic AHB decoder.

The stream uses one command buffer and fence, waits for GPU completion before
PRESENT, waits for ACK, and targets 30 FPS. Pool selection, protocol and import
parameters retain the original behavior. Resource owners are non-copyable to
prevent accidental double destruction during future extraction.

No Linux/Vulkan build or runtime validation was performed on Windows. On Debian,
check that all three imports succeed, animated colors display, 300 frames finish
with PASS, and STOP receives DONE after the final Android release fence.

## Next: KolosBufferPool

This stage separates modules without introducing the pool API yet. The FIFO,
FREE validation and PRESENT/ACK handling remain visible in the diagnostic tool.
Move those into `KolosBufferPool` together with the imported slot array. The
intended acquire result is `{VkImage, slot}`; rendering must not select a slot.

Ownership/layout barriers currently remain in `draw(Context&, Imported&, frame)`
and depend on `Imported::initialized`. Before reducing this to `render(VkImage)`,
define how the pool records FOREIGN acquisition, initial UNDEFINED versus GENERAL
layout, and release back to FOREIGN. Preserve CPU GPU-fence waiting in the first
pool prototype. ACK does not release the presented slot; only Android FREE does.
See [protocol lifecycle](protocol/README.md).
