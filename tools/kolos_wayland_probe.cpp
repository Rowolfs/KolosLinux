#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

#include <cstdio>
#include <cstring>
#include <chrono>
#include <poll.h>

struct Probe {
    wl_compositor* compositor = nullptr;
    xdg_wm_base* wm = nullptr;
    xdg_surface* xdg = nullptr;
    bool configured = false;
};
static void global(void* data, wl_registry* registry, uint32_t name,
                   const char* interface, uint32_t version) {
    auto& probe = *static_cast<Probe*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0)
        probe.compositor = static_cast<wl_compositor*>(wl_registry_bind(
            registry, name, &wl_compositor_interface, version < 1 ? version : 1));
    else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
        probe.wm = static_cast<xdg_wm_base*>(wl_registry_bind(
            registry, name, &xdg_wm_base_interface, version < 1 ? version : 1));
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

int main() {
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
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(probe.xdg);
    wl_surface_destroy(surface);
    xdg_wm_base_destroy(probe.wm);
    wl_compositor_destroy(probe.compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
    if (!passed) std::fprintf(stderr, "timed out waiting for xdg configure\n");
    return passed ? 0 : 1;
}
