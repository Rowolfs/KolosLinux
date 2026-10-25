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
- `src/buffer_pool.cpp`: imported slots, FIFO availability, acquisition tokens and IPC lifecycle.
- `tools/kolos_ahb_stream.cpp`: acquire/render/present diagnostic and frame pacing.
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

## KolosBufferPool

```cpp
Context context;
initialize(context);
KolosBufferPool pool(context, socketPath);
DiagnosticRenderer renderer;
for (uint32_t frame = 0; frame < frames; ++frame) {
    auto buffer = pool.acquire();
    auto commands = pool.prepareForRender(buffer);
    renderer.record(commands, buffer.image(), frame);
    pool.prepareForPresent(buffer);
    pool.present(buffer, frame);
}
pool.stop();
```

The pool imports and owns all three buffers, opens the control connection and
processes initial FREE grants. `acquire()` returns a move-only `AcquiredBuffer`
with `image()`, not a public slot number. Its private pool identity, slot and
acquisition generation are checked by `present()`.

Slots follow `FREE -> ACQUIRED -> PRESENTED -> FREE`. The last transition only
occurs on Android FREE, processed by the shared incoming-message dispatcher.
Initial FREE grants populate the queue separately. ACK leaves the current slot
PRESENTED. `stop()` retains STOP/DONE and rejects outstanding acquisitions; DONE
confirms final release and closes the session logically, without adding a FREE.

The pool rejects invalid indices, duplicate FREE, FREE for an ACQUIRED slot,
foreign/default/moved-from/stale tokens, repeated present, and invalid frame or ACK.
When the FIFO is empty, `acquire()` blocks on the existing socket read until FREE
arrives. `present()` uses the same dispatcher while waiting for its ACK, and
`stop()` uses it for DONE. An unsolicited ACK is rejected: synchronous present
has already consumed every expected ACK before returning.
It is synchronous and single-threaded. An IPC
failure makes the pool unusable, since remote ownership is then uncertain.
EOF, peer STOP, unexpected DONE, and the existing wire I/O timeout throw rather
than wait indefinitely. The wire read retains its existing 30-second inactivity
timeout; no acquire-specific sleep loop or wire change is introduced.
Tokens do not automatically return slots when destroyed. The pool must outlive
renderer and token use; the Vulkan context must outlive the pool.

Renderer records only the animated clear into the supplied command buffer, using
an image in TRANSFER_DST_OPTIMAL. It has no pool, slot, ownership or first-use state.

`prepareForRender(buffer)` validates the token and ACQUIRED state, resets/begins
the existing command buffer, and records the FOREIGN-to-graphics barrier.
`prepareForPresent(buffer)` records the graphics-to-FOREIGN barrier, ends recording,
submits once and waits on the existing GPU fence. Only successful fence completion
sets PRESENT_READY; `present()` rejects any image without that state.

Each slot has a separate image lifecycle:

```text
FOREIGN_UNDEFINED (new import) or FOREIGN_GENERAL (after Android FREE)
  -> prepareForRender: RENDER_READY (recorded graphics / TRANSFER_DST_OPTIMAL)
  -> prepareForPresent + submit + fence: PRESENT_READY (GENERAL / FOREIGN)
  -> present: stays PRESENT_READY
  -> Android FREE: FOREIGN_GENERAL
```

The slot lifecycle remains FREE -> ACQUIRED -> PRESENTED -> FREE. Image first-use
is represented by FOREIGN_UNDEFINED; `Imported::initialized` is removed. Recording
state describes the commands being constructed, not completed GPU execution. Only
one slot may record into the shared command buffer at a time. Token identity and
generation checks apply to both preparation methods and present. Repeated begin,
end before begin, and present before GPU completion are rejected. Any Vulkan error
invalidates the pool, preventing presentation with uncertain GPU ownership.

For Debian validation, retain the 300-frame visual/STOP-DONE checks and exercise
present before preparation, repeated preparation, wrong/stale tokens and end before
begin. No build or binaries are run during Windows-side refactoring.

The standalone pool described above does not exchange explicit fence FDs. See
[protocol lifecycle](protocol/README.md).

# Kolos Wayland compositor foundation

`kolos-compositor` is the owner of the Android Graphics Protocol v2 connection.
It provides `wl_compositor` v4, `wl_output` v2, `wp_viewporter` v1,
`xdg_wm_base`, and the private `kolos_graphics_v1` global. A private
`kolos_surface_v1` is bound to a same-client `wl_surface` that already has an
`xdg_toplevel` role. The compositor
selects the Android session; the Wayland client cannot choose a session ID.

Build on ARM64 Debian with `libwayland-dev`, `wayland-protocols`,
`libvulkan-dev`, CMake, and a C++17 compiler:

```sh
apt install build-essential cmake pkg-config libvulkan-dev libwayland-dev wayland-protocols
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Launch after the Android graphics activity has created its socket:

```sh
./build/kolos-compositor /run/kolos/graphics.sock /run/kolos/wayland
```

In another shell in the same Debian environment:

```sh
export XDG_RUNTIME_DIR=/run/kolos/wayland
export WAYLAND_DISPLAY=wayland-0
./build/kolos-wayland-probe
```

The probe creates one `wl_surface` and `xdg_toplevel` and exits after its
initial configure. It does not use Vulkan or the private graphics protocol.
With the Android graphics activity running, the same tool can exercise the
private transport without Mesa or Vulkan:

```sh
./build/kolos-wayland-probe --graphics
```

It requests every slot, validates the received handle size and two FDs,
sends `POOL_READY`, and waits for one initial `FREE` per slot. It closes the
received FDs without importing them; the graphics object is then destroyed,
causing the bridge to clean up its Android connection.
The initial configure is 0x0 (client chooses a size); after the Android pool is
bound, `xdg_toplevel.configure` carries the AHB generation's width and height.
Requested Wayland size, Android surface size, and AHB pool size remain separate
values. Only Android `POOL_REPLACED` advances the AHB generation.

Graphics Protocol v2 currently exposes one pre-created Android session through
`graphics.sock`, with no CREATE_SESSION operation. The compositor creates a
separate `KolosWindow` for each toplevel, but only one window may bind the
Android bridge at a time. A second private graphics binding receives an error
event. Multiwindow display needs an Android session-create/routing extension.
After an abnormal Android disconnect, the private client can destroy its
`kolos_surface_v1` and bind a new one to the same toplevel; this creates a fresh
Graphics Protocol v2 connection and reexports the current AHB generation.
MesaKolos imports the AHB images for its private Wayland WSI path. The Wayland
probe does not import them. The compositor does not import AHB into Vulkan or
compose ordinary `wl_buffer` contents. Its virtual `wl_output` mode follows the
Android pool size; refresh is unspecified. Viewport requests are accepted for
the private AHB presentation path, without compositing attached `wl_buffer`s.

For MPV 0.40 with MesaKolos installed, keep the Android graphics activity and
the compositor running, then set the MesaKolos ICD and run:

```sh
export XDG_RUNTIME_DIR=/run/kolos/wayland
export WAYLAND_DISPLAY=wayland-0
export VK_DRIVER_FILES=/opt/mesa-kolos/share/vulkan/icd.d/freedreno_icd.aarch64.json
export LD_LIBRARY_PATH=/opt/mesa-kolos/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
mpv --no-config --vo=gpu-next --gpu-api=vulkan --gpu-context=waylandvk \
    --vulkan-swap-mode=fifo --hwdec=no --audio=no \
    "/home/kolos/Videos/Test Jellyfin 4K AVC 40M.mp4"
```

The private MesaKolos WSI currently offers FIFO presentation; MPV's `auto`
mode can select MAILBOX on Wayland, so request FIFO explicitly.

The bridge repeats the proven HELLO/SESSION_INFO, per-slot GET_BUFFER and
IMPORT_ACK, START, POOL_REPLACED/POOL_READY, PRESENT/ACK/FREE and STOP/DONE
protocol. It forwards opaque 148-byte native-handle data and two FDs through
Wayland fd arguments. `IMPORT_ACK` means that the compositor safely holds the
handle; `POOL_READY` is sent to Android only after Mesa later confirms import
of every slot. The private protocol supplies raw handle data and AHB stride so
Mesa's observed Qualcomm layout validator can derive its explicit Vulkan
modifier/offset/pitch/allocation descriptor. No new layout is assumed here.

On a replacement, the compositor delivers `pool_replaced` and new buffers,
updates the Wayland configure size, and forwards `pool_ready` only when the
private client sends it. Old-generation FREE events retain their generation.
The compositor drops its old handle cache after replacement; Mesa's imported
image and FD ownership remain independent of this cache.
