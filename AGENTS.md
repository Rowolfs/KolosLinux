# Repository Guidelines

## Project Structure & Module Organization

KolosLinux contains the Linux side of the ARM64 Debian/Qualcomm Turnip prototype.
Keep Android application code in its separate repository.

- `include/kolos/`: public C++ interfaces and resource owners.
- `src/`: Vulkan context, device-specific AHB/dma-buf import, buffer pool, and diagnostic renderer.
- `tools/`: triple-buffer stream and standalone single-buffer diagnostics.
- `protocol/`: Android-compatible wire header and lifecycle documentation.
- `CMakeLists.txt`: library and executable targets. No test or asset directories currently exist.

## Build, Test, and Development Commands

Build and run on ARM64 Debian; do not launch Linux/Vulkan builds on Windows.

```sh
sudo apt install build-essential cmake libvulkan-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
export VK_DRIVER_FILES=/absolute/path/to/tested/freedreno_icd.json
./build/kolos_ahb_stream /mnt/kolos-wayland/ahb-stream.sock 300
```

These commands install dependencies, configure and build the diagnostics, select
the tested Turnip ICD, and run 300 frames. Start Android first and expose its
socket through the existing shared mount. Use `git diff --check` for whitespace validation.

## Coding Style & Naming Conventions

Use C++17, four-space indentation for new code, and LF line endings. Preserve
legacy formatting when moving working prototype code. Use `namespace kolos`,
PascalCase classes, camelCase functions, snake_case filenames, and trailing
underscores for private members. Keep owning Vulkan/FD types non-copyable.
No formatter or linter is configured.

## Testing Guidelines

No automated test framework or coverage threshold is configured. On Debian,
verify three successful imports, animated colors, final PASS, and STOP/DONE
completion. Exercise invalid lifecycle cases when changing pool validation.
Report checks performed and hardware/ICD configuration; explicitly identify
unrun build or runtime checks.

## Architecture Constraints

The pool owns slots and IPC; renderer receives a `VkImage`, never a slot number.
Preserve `FREE -> ACQUIRED -> PRESENTED -> FREE`: ACK does not release a buffer.
Keep GPU fence waiting before PRESENT and Android release-fence gating before
reuse. Import assumes the documented Qualcomm linear layout; changes to pitch,
modifiers, barriers, or protocol require explicit scope and hardware validation.

## Commit & Pull Request Guidelines

Check `git status` before edits. Make small logical commits with imperative
messages, following history such as `Encapsulate imported buffer lifecycle`.
Push completed changes to `origin/main` as requested for this project. Never
commit binaries, build directories, shell dotfiles, or temporary files.
PR descriptions should explain behavior changes, lifecycle implications, and
validation; link relevant issues and include visual evidence when display behavior changes.
