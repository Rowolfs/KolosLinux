#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "kolos-graphics-v1-client-protocol.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <cstdint>
#include <poll.h>
#include <set>
#include <string>
#include <unistd.h>

struct Probe {
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm = nullptr;
    kolos_graphics_v1* manager = nullptr;
    xdg_surface* xdg = nullptr;
    kolos_surface_v1* graphics = nullptr;
    bool configured = false;
    bool poolReceived = false;
    bool readySent = false;
    bool failed = false;
    uint64_t session = 0, generation = 0;
    uint32_t width = 0, height = 0, format = 0, slots = 0;
    std::set<uint32_t> imported, freeSlots;
    std::string failure;
};
static uint64_t join(uint32_t high, uint32_t low) {
    return (uint64_t(high) << 32) | low;
}
static void global(void* data, wl_registry* registry, uint32_t name,
                   const char* interface, uint32_t version) {
    auto& probe = *static_cast<Probe*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0)
        probe.compositor = static_cast<wl_compositor*>(wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 1 ? version : 1));
    else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
        probe.wm = static_cast<xdg_wm_base*>(wl_registry_bind(
            registry, name, &xdg_wm_base_interface, version < 1 ? version : 1));
    else if (std::strcmp(interface, kolos_graphics_v1_interface.name) == 0)
        probe.manager = static_cast<kolos_graphics_v1*>(wl_registry_bind(
            registry, name, &kolos_graphics_v1_interface, version < 1 ? version : 1));
}
static void globalRemove(void*, wl_registry*, uint32_t) {}
static const wl_registry_listener registryListener{global, globalRemove};
static void ping(void*, xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}
static const xdg_wm_base_listener wmListener{ping};
static void configure(void* data, xdg_surface* surface, uint32_t serial) {
    xdg_surface_ack_configure(surface, serial);
    static_cast<Probe*>(data)->configured = true;
    std::printf("PASS: xdg_toplevel configured, serial=%u\n", serial);
}
static const xdg_surface_listener surfaceListener{configure};
static void topConfigure(void*, xdg_toplevel*, int32_t width, int32_t height,
                         wl_array*) {
    std::printf("xdg_toplevel.configure size=%dx%d\n", width, height);
}
static void topClose(void*, xdg_toplevel*) {}
static const xdg_toplevel_listener topListener{topConfigure, topClose};

static void newPool(void* data, kolos_surface_v1* surface,
                    uint32_t sessionHi, uint32_t sessionLo,
                    uint32_t generationHi, uint32_t generationLo,
                    uint32_t width, uint32_t height, uint32_t format,
                    uint32_t slots) {
    auto& probe = *static_cast<Probe*>(data);
    probe.session = join(sessionHi, sessionLo);
    probe.generation = join(generationHi, generationLo);
    probe.width = width; probe.height = height;
    probe.format = format; probe.slots = slots;
    probe.poolReceived = true; probe.readySent = false;
    probe.imported.clear(); probe.freeSlots.clear();
    std::printf("private NEW_POOL session=%llu generation=%llu size=%ux%u slots=%u\n",
        (unsigned long long)probe.session, (unsigned long long)probe.generation,
        width, height, slots);
    if (!probe.session || !probe.generation || !width || !height ||
        format != 1 || !slots || slots > 16) {
        probe.failed = true; probe.failure = "invalid private pool metadata";
        return;
    }
    for (uint32_t slot = 0; slot < slots; ++slot)
        kolos_surface_v1_request_buffer(surface, generationHi, generationLo, slot);
}
static void poolReplaced(void* data, kolos_surface_v1* surface,
                         uint32_t generationHi, uint32_t generationLo,
                         uint32_t width, uint32_t height, uint32_t format,
                         uint32_t slots) {
    auto& probe = *static_cast<Probe*>(data);
    uint64_t next = join(generationHi, generationLo);
    if (!probe.poolReceived || next <= probe.generation) {
        probe.failed = true; probe.failure = "invalid private pool replacement";
        return;
    }
    newPool(data, surface, uint32_t(probe.session >> 32), uint32_t(probe.session),
            generationHi, generationLo, width, height, format, slots);
}
static void buffer(void* data, kolos_surface_v1* surface,
                   uint32_t generationHi, uint32_t generationLo, uint32_t slot,
                   uint32_t width, uint32_t height, uint32_t format,
                   uint32_t stride, wl_array* handle, int32_t pixelFd,
                   int32_t metadataFd) {
    auto& probe = *static_cast<Probe*>(data);
    bool valid = probe.poolReceived &&
        join(generationHi, generationLo) == probe.generation &&
        slot < probe.slots && width == probe.width && height == probe.height &&
        format == probe.format && stride >= width &&
        handle && handle->size == 148 && pixelFd >= 0 && metadataFd >= 0 &&
        probe.imported.insert(slot).second;
    // Wayland transferred ownership of both received FDs to this client.
    if (pixelFd >= 0) close(pixelFd);
    if (metadataFd >= 0) close(metadataFd);
    if (!valid) {
        probe.failed = true; probe.failure = "invalid private BUFFER/FD";
        return;
    }
    std::printf("private BUFFER generation=%llu slot=%u bytes=%zu FDs=2\n",
        (unsigned long long)probe.generation, slot, handle->size);
    if (probe.imported.size() == probe.slots && !probe.readySent) {
        probe.readySent = true;
        kolos_surface_v1_pool_ready(surface, generationHi, generationLo);
        std::printf("private POOL_READY generation=%llu\n",
                    (unsigned long long)probe.generation);
    }
}
static void freeSlot(void* data, kolos_surface_v1*,
                     uint32_t generationHi, uint32_t generationLo,
                     uint32_t slot) {
    auto& probe = *static_cast<Probe*>(data);
    uint64_t generation = join(generationHi, generationLo);
    if (generation < probe.generation) return; // Retired pool release.
    if (!probe.readySent || generation != probe.generation ||
        slot >= probe.slots || !probe.freeSlots.insert(slot).second) {
        probe.failed = true; probe.failure = "invalid private FREE";
        return;
    }
    std::printf("private FREE generation=%llu slot=%u\n",
                (unsigned long long)probe.generation, slot);
}
static void ack(void*, kolos_surface_v1*, uint32_t, uint32_t, uint32_t,
                uint32_t, uint32_t) {}
static void graphicsError(void* data, kolos_surface_v1*, const char* message) {
    auto& probe = *static_cast<Probe*>(data);
    probe.failed = true;
    probe.failure = message ? message : "private graphics error";
}
static const kolos_surface_v1_listener graphicsListener{
    newPool, poolReplaced, buffer, freeSlot, ack, graphicsError
};

int main(int argc, char** argv) {
    bool graphicsMode = argc == 2 && std::strcmp(argv[1], "--graphics") == 0;
    if (argc > 2 || (argc == 2 && !graphicsMode)) {
        std::fprintf(stderr, "usage: kolos-wayland-probe [--graphics]\n");
        return 1;
    }
    wl_display* display = wl_display_connect(nullptr);
    if (!display) { std::fprintf(stderr, "Wayland connect failed\n"); return 1; }
    Probe probe;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registryListener, &probe);
    if (wl_display_roundtrip(display) < 0 || !probe.compositor || !probe.wm) {
        std::fprintf(stderr, "missing wl_compositor or xdg_wm_base\n");
        return 1;
    }
    xdg_wm_base_add_listener(probe.wm, &wmListener, &probe);
    wl_surface* surface = wl_compositor_create_surface(probe.compositor);
    probe.xdg = xdg_wm_base_get_xdg_surface(probe.wm, surface);
    xdg_surface_add_listener(probe.xdg, &surfaceListener, &probe);
    xdg_toplevel* top = xdg_surface_get_toplevel(probe.xdg);
    xdg_toplevel_add_listener(top, &topListener, &probe);
    wl_surface_commit(surface);
    wl_display_flush(display);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!probe.configured && std::chrono::steady_clock::now() < deadline) {
        pollfd wait{wl_display_get_fd(display), POLLIN, 0};
        int result = poll(&wait, 1, 100);
        if (result > 0 && (wait.revents & POLLIN) &&
            wl_display_dispatch(display) < 0) break;
        if (result < 0) break;
    }
    bool passed = probe.configured;
    if (passed && graphicsMode) {
        if (!probe.manager) {
            passed = false;
            probe.failure = "missing kolos_graphics_v1 global";
        } else {
            probe.graphics = kolos_graphics_v1_get_surface(probe.manager, surface);
            kolos_surface_v1_add_listener(probe.graphics, &graphicsListener, &probe);
            wl_display_flush(display);
            auto graphicsDeadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(15);
            while (!probe.failed &&
                   !(probe.readySent && probe.freeSlots.size() == probe.slots) &&
                   std::chrono::steady_clock::now() < graphicsDeadline) {
                // Requests queued by NEW_POOL/BUFFER listeners must reach the
                // compositor before waiting for its next event.
                if (wl_display_flush(display) < 0) {
                    probe.failed = true; probe.failure = "Wayland flush failed";
                    break;
                }
                pollfd wait{wl_display_get_fd(display), POLLIN, 0};
                int result = poll(&wait, 1, 100);
                if (result > 0 && (wait.revents & POLLIN) &&
                    wl_display_dispatch(display) < 0) {
                    probe.failed = true; probe.failure = "Wayland dispatch failed";
                } else if (result > 0 && (wait.revents & (POLLERR | POLLHUP))) {
                    probe.failed = true; probe.failure = "Wayland compositor disconnected";
                } else if (result < 0) {
                    probe.failed = true; probe.failure = "Wayland poll failed";
                }
            }
            passed = !probe.failed && probe.readySent &&
                     probe.freeSlots.size() == probe.slots;
            if (passed)
                std::printf("PASS: private graphics pool %zu/%u handles, %zu/%u FREE\n",
                    probe.imported.size(), probe.slots,
                    probe.freeSlots.size(), probe.slots);
            else if (probe.failure.empty())
                probe.failure = "timed out waiting for graphics pool/handles/FREE";
        }
    }
    if (probe.graphics) kolos_surface_v1_destroy(probe.graphics);
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(probe.xdg);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(probe.wm);
    if (probe.manager) kolos_graphics_v1_destroy(probe.manager);
    wl_compositor_destroy(probe.compositor);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    if (!passed) std::fprintf(stderr, "%s\n",
        probe.failure.empty() ? "timed out waiting for xdg configure" :
                                probe.failure.c_str());
    return passed ? 0 : 1;
}
