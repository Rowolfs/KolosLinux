#include <kolos/android_graphics_bridge.h>
#include <wayland-server.h>
#include "xdg-shell-server-protocol.h"
#include "viewporter-server-protocol.h"
#include "kolos-graphics-v1-server-protocol.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <sys/stat.h>

namespace {
using kolos::AndroidGraphicsBridge;
using kolos::GraphicsEvent;
using kolos::GraphicsIdentity;
using kolos::GraphicsPoolInfo;

uint32_t hi(uint64_t n) { return uint32_t(n >> 32); }
uint32_t lo(uint64_t n) { return uint32_t(n); }
uint64_t join(uint32_t high, uint32_t low) {
    return (uint64_t(high) << 32) | low;
}

class KolosCompositor;
struct KolosWindow;
struct Surface {
    KolosCompositor* compositor;
    wl_resource* resource = nullptr;
    wl_resource* xdgSurface = nullptr;
    wl_resource* viewport = nullptr;
    wl_resource* pendingBuffer = nullptr;
    std::vector<wl_resource*> frameCallbacks;
    std::unique_ptr<KolosWindow> window;
};
struct KolosWindow {
    Surface* surface;
    wl_resource* toplevel = nullptr;
    wl_resource* graphics = nullptr;
    std::unique_ptr<AndroidGraphicsBridge> bridge;
    wl_event_source* bridgeSource = nullptr;
    GraphicsPoolInfo pool;
    bool hasPool = false;
    uint64_t readyGeneration = 0;
    uint32_t configuredSerial = 0;
    uint32_t ackedSerial = 0;
    uint32_t requestedWidth = 0, requestedHeight = 0;
    std::string title;
    std::map<std::pair<uint64_t, uint32_t>, GraphicsEvent> buffers;
    std::set<std::pair<uint64_t, uint32_t>> requests;
    explicit KolosWindow(Surface* owner) : surface(owner) {}
};

class KolosCompositor {
public:
    KolosCompositor(const std::string& androidPath, const std::string& runtime)
        : androidPath_(androidPath) {
        std::filesystem::create_directories(runtime);
        if (chmod(runtime.c_str(), 0700) != 0)
            throw std::runtime_error("cannot set Wayland runtime directory permissions");
        if (setenv("XDG_RUNTIME_DIR", runtime.c_str(), 1) != 0)
            throw std::runtime_error("cannot set XDG_RUNTIME_DIR");
        display_ = wl_display_create();
        if (!display_) throw std::runtime_error("wl_display_create failed");
        if (wl_display_add_socket(display_, "wayland-0") != 0)
            throw std::runtime_error("cannot create wayland-0 socket");
        wl_global_create(display_, &wl_compositor_interface, 4, this, bindCompositor);
        wl_global_create(display_, &xdg_wm_base_interface, 1, this, bindXdg);
        wl_global_create(display_, &wl_output_interface, 2, this, bindOutput);
        wl_global_create(display_, &wp_viewporter_interface, 1, this, bindViewporter);
        wl_global_create(display_, &kolos_graphics_v1_interface, 1, this, bindGraphics);
        clientCreated_.notify = clientCreated;
        wl_display_add_client_created_listener(display_, &clientCreated_);
        auto* loop = wl_display_get_event_loop(display_);
        interruptSource_ = wl_event_loop_add_signal(loop, SIGINT, terminate, this);
        terminateSource_ = wl_event_loop_add_signal(loop, SIGTERM, terminate, this);
        std::printf("kolos-compositor: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=wayland-0 Android=%s\n",
                    runtime.c_str(), androidPath.c_str());
    }
    ~KolosCompositor() {
        if (display_) {
            wl_display_destroy_clients(display_);
            surfaces_.clear();
            wl_display_destroy(display_);
        }
    }
    void run() { wl_display_run(display_); }

private:
    static int terminate(int, void* data) {
        wl_display_terminate(static_cast<KolosCompositor*>(data)->display_);
        return 0;
    }
    static void clientCreated(wl_listener*, void*) {
        std::puts("Wayland client connected");
    }
    static wl_resource* resource(wl_client* client, const wl_interface* type,
                                 uint32_t id, int version = 1) {
        wl_resource* value = wl_resource_create(client, type, version, id);
        if (!value) wl_client_post_no_memory(client);
        return value;
    }
    static void bindCompositor(wl_client* client, void* data, uint32_t version, uint32_t id) {
        auto* value = resource(client, &wl_compositor_interface, id, version);
        if (value) wl_resource_set_dispatcher(value, dispatchCompositor, nullptr, data, nullptr);
    }
    static void bindOutput(wl_client* client, void* data, uint32_t version, uint32_t id) {
        auto* self = static_cast<KolosCompositor*>(data);
        auto* value = resource(client, &wl_output_interface, id, version);
        if (!value) return;
        wl_resource_set_dispatcher(value, dispatchOutput, nullptr, self, destroyOutput);
        self->outputs_.emplace(value, client);
        wl_output_send_geometry(value, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                                "Kolos", "Android", WL_OUTPUT_TRANSFORM_NORMAL);
        wl_output_send_mode(value, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                            self->outputWidth_, self->outputHeight_, 0);
        wl_output_send_scale(value, 1);
        wl_output_send_done(value);
        for (const auto& entry : self->surfaces_) {
            auto* surfaceResource = entry.first;
            if (wl_resource_get_client(surfaceResource) == client &&
                wl_resource_get_version(surfaceResource) >= 2)
                wl_surface_send_enter(surfaceResource, value);
        }
    }
    static int dispatchOutput(const void*, void*, uint32_t,
                              const wl_message*, wl_argument*) { return 0; }
    static void destroyOutput(wl_resource* value) {
        auto* self = static_cast<KolosCompositor*>(wl_resource_get_user_data(value));
        self->outputs_.erase(value);
    }
    static void bindViewporter(wl_client* client, void* data, uint32_t, uint32_t id) {
        auto* value = resource(client, &wp_viewporter_interface, id);
        if (value) wl_resource_set_dispatcher(value, dispatchViewporter, nullptr, data, nullptr);
    }
    static void bindXdg(wl_client* client, void* data, uint32_t, uint32_t id) {
        auto* value = resource(client, &xdg_wm_base_interface, id);
        if (value) wl_resource_set_dispatcher(value, dispatchXdg, nullptr, data, nullptr);
    }
    static void bindGraphics(wl_client* client, void* data, uint32_t, uint32_t id) {
        auto* value = resource(client, &kolos_graphics_v1_interface, id);
        if (value) wl_resource_set_dispatcher(value, dispatchGraphics, nullptr, data, nullptr);
    }
    static int dispatchCompositor(const void*, void* target, uint32_t opcode,
                                  const wl_message*, wl_argument* args) {
        auto* manager = static_cast<wl_resource*>(target);
        auto* self = static_cast<KolosCompositor*>(wl_resource_get_user_data(manager));
        auto* client = wl_resource_get_client(manager);
        if (opcode == 0) {
            auto* value = resource(client, &wl_surface_interface, args[0].n,
                                   wl_resource_get_version(manager));
            if (!value) return 0;
            auto surface = std::make_unique<Surface>();
            surface->compositor = self; surface->resource = value;
            wl_resource_set_dispatcher(value, dispatchSurface, nullptr,
                                       surface.get(), destroySurface);
            self->surfaces_.emplace(value, std::move(surface));
            for (const auto& [output, owner] : self->outputs_)
                if (owner == client && wl_resource_get_version(value) >= 2)
                    wl_surface_send_enter(value, output);
            std::puts("wl_surface created");
        } else if (opcode == 1) {
            auto* value = resource(client, &wl_region_interface, args[0].n);
            if (value) wl_resource_set_dispatcher(value, dispatchRegion, nullptr,
                                                  nullptr, nullptr);
        }
        return 0;
    }
    static void destroySurface(wl_resource* resource) {
        auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        if (!surface) return;
        if (surface->window) {
            if (surface->window->toplevel)
                wl_resource_destroy(surface->window->toplevel);
            else if (surface->window->graphics)
                wl_resource_destroy(surface->window->graphics);
            surface->window.reset();
        }
        if (surface->xdgSurface) wl_resource_destroy(surface->xdgSurface);
        if (surface->viewport) wl_resource_set_user_data(surface->viewport, nullptr);
        for (auto* callback : surface->frameCallbacks)
            wl_resource_destroy(callback);
        std::puts("wl_surface destroyed");
        surface->compositor->surfaces_.erase(resource);
    }
    static int dispatchSurface(const void*, void* target, uint32_t opcode,
                               const wl_message*, wl_argument* args) {
        auto* value = static_cast<wl_resource*>(target);
        auto* surface = static_cast<Surface*>(wl_resource_get_user_data(value));
        switch (opcode) {
        case 0: wl_resource_destroy(value); break;
        case 1: surface->pendingBuffer = reinterpret_cast<wl_resource*>(args[0].o); break;
        case 3: {
            auto* callback = resource(wl_resource_get_client(value),
                                      &wl_callback_interface, args[0].n);
            if (callback) surface->frameCallbacks.push_back(callback);
            break;
        }
        case 6: {
            if (surface->window && !surface->window->configuredSerial)
                surface->compositor->configure(*surface->window, 0, 0);
            if (surface->window && surface->pendingBuffer &&
                !surface->window->ackedSerial) {
                wl_resource_post_error(surface->xdgSurface, 3,
                    "buffer committed before xdg_surface configure ack");
                break;
            }
            if (surface->pendingBuffer) {
                wl_buffer_send_release(surface->pendingBuffer);
                surface->pendingBuffer = nullptr;
            }
            auto milliseconds = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            for (auto* callback : surface->frameCallbacks) {
                wl_callback_send_done(callback, milliseconds);
                wl_resource_destroy(callback);
            }
            surface->frameCallbacks.clear();
            break;
        }
        default: break; // damage/regions are not composited at this milestone.
        }
        return 0;
    }
    static int dispatchRegion(const void*, void* target, uint32_t opcode,
                              const wl_message*, wl_argument*) {
        if (opcode == 0) wl_resource_destroy(static_cast<wl_resource*>(target));
        return 0;
    }
    static int dispatchViewporter(const void*, void* target, uint32_t opcode,
                                  const wl_message*, wl_argument* args) {
        auto* manager = static_cast<wl_resource*>(target);
        if (opcode == 0) {
            wl_resource_destroy(manager);
        } else if (opcode == 1) {
            auto* self = static_cast<KolosCompositor*>(wl_resource_get_user_data(manager));
            auto* wlSurface = reinterpret_cast<wl_resource*>(args[1].o);
            auto it = self->surfaces_.find(wlSurface);
            if (it == self->surfaces_.end() ||
                wl_resource_get_client(wlSurface) != wl_resource_get_client(manager) ||
                it->second->viewport) {
                wl_resource_post_error(manager, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                                       "invalid or already viewported wl_surface");
                return 0;
            }
            auto* viewport = resource(wl_resource_get_client(manager),
                                      &wp_viewport_interface, args[0].n);
            if (!viewport) return 0;
            it->second->viewport = viewport;
            wl_resource_set_dispatcher(viewport, dispatchViewport, nullptr,
                                       it->second.get(), destroyViewport);
        }
        return 0;
    }
    static int dispatchViewport(const void*, void* target, uint32_t opcode,
                                const wl_message*, wl_argument* args) {
        auto* viewport = static_cast<wl_resource*>(target);
        if (opcode == 0) {
            wl_resource_destroy(viewport);
            return 0;
        }
        if (!wl_resource_get_user_data(viewport)) {
            wl_resource_post_error(viewport, WP_VIEWPORT_ERROR_NO_SURFACE,
                                   "viewport surface was destroyed");
            return 0;
        }
        // AHB frames use private PRESENT rather than wl_buffer.attach. Keep
        // viewport request validation even though no wl_buffer is composited.
        if (opcode == 1) {
            const bool unset = args[0].f == -256 && args[1].f == -256 &&
                               args[2].f == -256 && args[3].f == -256;
            if (!unset && (args[0].f < 0 || args[1].f < 0 ||
                           args[2].f <= 0 || args[3].f <= 0))
                wl_resource_post_error(viewport, WP_VIEWPORT_ERROR_BAD_VALUE,
                                       "invalid viewport source");
        } else if (opcode == 2 &&
                   !((args[0].i == -1 && args[1].i == -1) ||
                     (args[0].i > 0 && args[1].i > 0))) {
            wl_resource_post_error(viewport, WP_VIEWPORT_ERROR_BAD_VALUE,
                                   "invalid viewport destination");
        }
        return 0;
    }
    static void destroyViewport(wl_resource* viewport) {
        auto* surface = static_cast<Surface*>(wl_resource_get_user_data(viewport));
        if (surface && surface->viewport == viewport) surface->viewport = nullptr;
    }
    static int dispatchXdg(const void*, void* target, uint32_t opcode,
                           const wl_message*, wl_argument* args) {
        auto* manager = static_cast<wl_resource*>(target);
        auto* self = static_cast<KolosCompositor*>(wl_resource_get_user_data(manager));
        auto* client = wl_resource_get_client(manager);
        switch (opcode) {
        case 0: wl_resource_destroy(manager); break;
        case 1: {
            auto* value = resource(client, &xdg_positioner_interface, args[0].n);
            if (value) wl_resource_set_dispatcher(value, dispatchPositioner,
                                                  nullptr, nullptr, nullptr);
            break;
        }
        case 2: {
            auto* wlSurface = reinterpret_cast<wl_resource*>(args[1].o);
            auto it = self->surfaces_.find(wlSurface);
            if (it == self->surfaces_.end() || wl_resource_get_client(wlSurface) != client ||
                it->second->xdgSurface) {
                wl_resource_post_error(manager, 0, "invalid or already roled wl_surface");
                break;
            }
            auto* value = resource(client, &xdg_surface_interface, args[0].n);
            if (!value) break;
            it->second->xdgSurface = value;
            wl_resource_set_dispatcher(value, dispatchXdgSurface, nullptr,
                                       it->second.get(), destroyXdgSurface);
            break;
        }
        case 3: break; // pong
        }
        return 0;
    }
    static int dispatchPositioner(const void*, void* target, uint32_t opcode,
                                  const wl_message*, wl_argument*) {
        if (opcode == 0) wl_resource_destroy(static_cast<wl_resource*>(target));
        return 0;
    }
    static void destroyXdgSurface(wl_resource* resource) {
        auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        if (surface) surface->xdgSurface = nullptr;
    }
    static int dispatchXdgSurface(const void*, void* target, uint32_t opcode,
                                  const wl_message*, wl_argument* args) {
        auto* resource = static_cast<wl_resource*>(target);
        auto* surface = static_cast<Surface*>(wl_resource_get_user_data(resource));
        switch (opcode) {
        case 0:
            if (surface->window && surface->window->toplevel) {
                wl_resource_post_error(resource, 6, "destroy xdg_toplevel first");
                break;
            }
            wl_resource_destroy(resource);
            break;
        case 1: {
            if (surface->window) {
                wl_resource_post_error(resource, 2, "xdg role already assigned");
                break;
            }
            auto* top = KolosCompositor::resource(wl_resource_get_client(resource),
                                                  &xdg_toplevel_interface, args[0].n);
            if (!top) break;
            surface->window = std::make_unique<KolosWindow>(surface);
            surface->window->toplevel = top;
            wl_resource_set_dispatcher(top, dispatchToplevel, nullptr,
                                       surface->window.get(), destroyToplevel);
            std::puts("xdg_toplevel created; KolosWindow created");
            break;
        }
        case 2:
            wl_resource_post_error(resource, 0, "xdg_popup is not supported yet");
            break;
        case 3: break; // client window geometry; distinct from Android size.
        case 4:
            if (surface->window) surface->window->ackedSerial = args[0].u;
            break;
        }
        return 0;
    }
    static void destroyToplevel(wl_resource* resource) {
        auto* window = static_cast<KolosWindow*>(wl_resource_get_user_data(resource));
        if (!window) return;
        window->toplevel = nullptr;
        if (window->graphics) wl_resource_destroy(window->graphics);
        if (window->bridgeSource) {
            wl_event_source_remove(window->bridgeSource);
            window->bridgeSource = nullptr;
        }
        if (window->bridge) window->bridge.reset();
        auto* self = window->surface->compositor;
        if (self->activeWindow_ == window) self->activeWindow_ = nullptr;
        std::puts("KolosWindow destroyed");
        window->surface->window.reset();
    }
    static int dispatchToplevel(const void*, void* target, uint32_t opcode,
                                const wl_message*, wl_argument* args) {
        auto* resource = static_cast<wl_resource*>(target);
        auto* window = static_cast<KolosWindow*>(wl_resource_get_user_data(resource));
        if (opcode == 0) wl_resource_destroy(resource);
        else if (opcode == 2 && args[0].s) window->title = args[0].s;
        return 0;
    }
    static int dispatchGraphics(const void*, void* target, uint32_t opcode,
                                const wl_message*, wl_argument* args) {
        auto* manager = static_cast<wl_resource*>(target);
        auto* self = static_cast<KolosCompositor*>(wl_resource_get_user_data(manager));
        if (opcode == 0) { wl_resource_destroy(manager); return 0; }
        if (opcode != 1) return 0;
        auto* wlSurface = reinterpret_cast<wl_resource*>(args[1].o);
        auto it = self->surfaces_.find(wlSurface);
        if (it == self->surfaces_.end() ||
            wl_resource_get_client(wlSurface) != wl_resource_get_client(manager) ||
            !it->second->window || it->second->window->graphics) {
            wl_resource_post_error(manager, 0, "graphics requires own xdg_toplevel");
            return 0;
        }
        auto* resource = KolosCompositor::resource(wl_resource_get_client(manager),
                                                   &kolos_surface_v1_interface, args[0].n);
        if (!resource) return 0;
        auto* window = it->second->window.get();
        window->graphics = resource;
        wl_resource_set_dispatcher(resource, dispatchKolosSurface, nullptr,
                                   window, destroyKolosSurface);
        if (self->activeWindow_ && self->activeWindow_ != window) {
            kolos_surface_v1_send_error(resource,
                "Graphics Protocol v2 exposes one Android session; another toplevel owns it");
            return 0;
        }
        self->activeWindow_ = window;
        window->bridge = std::make_unique<AndroidGraphicsBridge>(self->androidPath_);
        window->bridgeSource = wl_event_loop_add_fd(
            wl_display_get_event_loop(self->display_), window->bridge->eventFd(),
            WL_EVENT_READABLE, bridgeEvents, window);
        if (!window->bridgeSource) {
            kolos_surface_v1_send_error(resource, "cannot watch Android graphics bridge");
            window->bridge.reset();
            self->activeWindow_ = nullptr;
            return 0;
        }
        window->bridge->start();
        std::puts("Mesa kolos_surface bound; Android graphics bridge started");
        return 0;
    }
    static void destroyKolosSurface(wl_resource* resource) {
        auto* window = static_cast<KolosWindow*>(wl_resource_get_user_data(resource));
        if (!window) return;
        window->graphics = nullptr;
        if (window->bridgeSource) {
            wl_event_source_remove(window->bridgeSource);
            window->bridgeSource = nullptr;
        }
        window->bridge.reset();
        auto* self = window->surface->compositor;
        if (self->activeWindow_ == window) self->activeWindow_ = nullptr;
        window->buffers.clear(); window->requests.clear();
        window->pool = {};
        window->hasPool = false;
        window->readyGeneration = 0;
    }
    static int dispatchKolosSurface(const void*, void* target, uint32_t opcode,
                                    const wl_message*, wl_argument* args) {
        auto* resource = static_cast<wl_resource*>(target);
        auto* window = static_cast<KolosWindow*>(wl_resource_get_user_data(resource));
        if (opcode == 0) { wl_resource_destroy(resource); return 0; }
        if (!window->bridge || !window->hasPool) {
            kolos_surface_v1_send_error(resource, "Android graphics session unavailable");
            return 0;
        }
        uint64_t generation = join(args[0].u, args[1].u);
        if (opcode == 1) {
            uint32_t slot = args[2].u;
            if (generation != window->pool.generation || slot >= window->pool.slots) {
                kolos_surface_v1_send_error(resource, "stale REQUEST_BUFFER");
                return 0;
            }
            auto key = std::make_pair(generation, slot);
            window->requests.insert(key);
            auto found = window->buffers.find(key);
            if (found != window->buffers.end()) sendBuffer(*window, found->second);
        } else if (opcode == 2) {
            if (generation != window->pool.generation ||
                window->readyGeneration == generation) {
                kolos_surface_v1_send_error(resource, "stale POOL_READY");
                return 0;
            }
            window->readyGeneration = generation;
            window->bridge->poolReady(window->pool.session, generation);
        } else if (opcode == 3) {
            uint32_t slot = args[2].u;
            uint64_t serial = join(args[3].u, args[4].u);
            if (generation < window->pool.generation) {
                // An old swapchain may finish a queued present after resize.
                // Android has retired that pool, so this frame cannot display.
                std::printf("PRESENT dropped retired generation=%llu slot=%u serial=%llu\n",
                    (unsigned long long)generation, slot,
                    (unsigned long long)serial);
                return 0;
            }
            if (generation != window->pool.generation || slot >= window->pool.slots) {
                kolos_surface_v1_send_error(resource, "stale PRESENT");
                return 0;
            }
            window->bridge->present({window->pool.session, generation, slot}, serial);
            std::printf("PRESENT forwarded session=%llu generation=%llu slot=%u serial=%llu\n",
                (unsigned long long)window->pool.session,
                (unsigned long long)generation, slot, (unsigned long long)serial);
        }
        return 0;
    }
    static void sendBuffer(KolosWindow& window, const GraphicsEvent& event) {
        if (!window.graphics) return;
        wl_array bytes;
        wl_array_init(&bytes);
        void* data = wl_array_add(&bytes, event.handle.size());
        if (!data) {
            wl_array_release(&bytes);
            wl_resource_post_no_memory(window.graphics);
            return;
        }
        std::memcpy(data, event.handle.data(), event.handle.size());
        kolos_surface_v1_send_buffer(window.graphics,
            hi(event.id.generation), lo(event.id.generation), event.id.slot,
            window.pool.width, window.pool.height, window.pool.format,
            event.stride, &bytes, event.fds[0], event.fds[1]);
        wl_array_release(&bytes);
        wl_display_flush_clients(window.surface->compositor->display_);
        std::printf("buffer slot=%u generation=%llu forwarded with 2 FDs\n",
                    event.id.slot, (unsigned long long)event.id.generation);
    }
    static int bridgeEvents(int, uint32_t, void* data) {
        auto* window = static_cast<KolosWindow*>(data);
        auto* self = window->surface->compositor;
        auto events = window->bridge->takeEvents();
        for (auto& event : events) {
            switch (event.type) {
            case GraphicsEvent::Type::Pool: {
                bool replacement = window->hasPool;
                if (replacement && (event.pool.session != window->pool.session ||
                    event.pool.generation <= window->pool.generation)) {
                    kolos_surface_v1_send_error(window->graphics, "invalid Android pool replacement");
                    break;
                }
                window->pool = event.pool; window->hasPool = true;
                window->readyGeneration = 0;
                window->buffers.clear(); window->requests.clear();
                self->setOutputSize(event.pool.width, event.pool.height);
                if (replacement) {
                    kolos_surface_v1_send_pool_replaced(window->graphics,
                        hi(event.pool.generation), lo(event.pool.generation),
                        event.pool.width, event.pool.height, event.pool.format,
                        event.pool.slots);
                    std::printf("POOL_REPLACED session=%llu generation=%llu size=%ux%u\n",
                        (unsigned long long)event.pool.session,
                        (unsigned long long)event.pool.generation,
                        event.pool.width, event.pool.height);
                } else {
                    kolos_surface_v1_send_new_pool(window->graphics,
                        hi(event.pool.session), lo(event.pool.session),
                        hi(event.pool.generation), lo(event.pool.generation),
                        event.pool.width, event.pool.height, event.pool.format,
                        event.pool.slots);
                    std::printf("session=%llu generation=%llu size=%ux%u slots=%u\n",
                        (unsigned long long)event.pool.session,
                        (unsigned long long)event.pool.generation,
                        event.pool.width, event.pool.height, event.pool.slots);
                }
                self->configure(*window, event.pool.width, event.pool.height);
                break;
            }
            case GraphicsEvent::Type::Buffer: {
                if (!window->hasPool || event.id.session != window->pool.session ||
                    event.id.generation != window->pool.generation ||
                    event.id.slot >= window->pool.slots) break;
                auto key = std::make_pair(event.id.generation, event.id.slot);
                window->buffers.emplace(key, std::move(event));
                if (window->requests.count(key))
                    sendBuffer(*window, window->buffers.find(key)->second);
                break;
            }
            case GraphicsEvent::Type::Free:
                if (window->hasPool && event.id.session == window->pool.session &&
                    window->graphics) {
                    kolos_surface_v1_send_free(window->graphics,
                        hi(event.id.generation), lo(event.id.generation), event.id.slot);
                    std::printf("FREE forwarded session=%llu generation=%llu slot=%u\n",
                        (unsigned long long)event.id.session,
                        (unsigned long long)event.id.generation, event.id.slot);
                }
                break;
            case GraphicsEvent::Type::Ack:
                if (window->hasPool && event.id.session == window->pool.session &&
                    window->graphics)
                    kolos_surface_v1_send_ack(window->graphics,
                        hi(event.id.generation), lo(event.id.generation), event.id.slot,
                        hi(event.serial), lo(event.serial));
                break;
            case GraphicsEvent::Type::Error:
                std::fprintf(stderr, "Android graphics bridge: %s\n", event.message.c_str());
                if (window->graphics)
                    kolos_surface_v1_send_error(window->graphics, event.message.c_str());
                break;
            case GraphicsEvent::Type::Closed:
                std::puts("Android graphics connection closed");
                break;
            }
        }
        wl_display_flush_clients(self->display_);
        return 0;
    }
    void configure(KolosWindow& window, uint32_t width, uint32_t height) {
        if (!window.toplevel || !window.surface->xdgSurface) return;
        window.requestedWidth = width; window.requestedHeight = height;
        wl_array states;
        wl_array_init(&states);
        xdg_toplevel_send_configure(window.toplevel, int32_t(width), int32_t(height),
                                    &states);
        wl_array_release(&states);
        window.configuredSerial = wl_display_next_serial(display_);
        xdg_surface_send_configure(window.surface->xdgSurface,
                                   window.configuredSerial);
        std::printf("xdg_toplevel.configure requested=%ux%u AHB-generation=%llu\n",
                    width, height, (unsigned long long)window.pool.generation);
    }
    void setOutputSize(uint32_t width, uint32_t height) {
        if (outputWidth_ == width && outputHeight_ == height) return;
        outputWidth_ = width;
        outputHeight_ = height;
        for (const auto& entry : outputs_) {
            auto* output = entry.first;
            wl_output_send_mode(output, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                                width, height, 0);
            wl_output_send_done(output);
        }
    }

    std::string androidPath_;
    wl_display* display_ = nullptr;
    wl_event_source* interruptSource_ = nullptr;
    wl_event_source* terminateSource_ = nullptr;
    wl_listener clientCreated_{};
    std::unordered_map<wl_resource*, std::unique_ptr<Surface>> surfaces_;
    std::unordered_map<wl_resource*, wl_client*> outputs_;
    uint32_t outputWidth_ = 1920, outputHeight_ = 1080;
    KolosWindow* activeWindow_ = nullptr;
};
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 3) throw std::runtime_error(
            "usage: kolos-compositor [graphics.sock] [wayland-runtime-dir]");
        std::string graphics = argc >= 2 ? argv[1] : "/run/kolos/graphics.sock";
        std::string runtime = argc >= 3 ? argv[2] : "/run/kolos/wayland";
        KolosCompositor compositor(graphics, runtime);
        compositor.run();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "kolos-compositor: %s\n", error.what());
        return 1;
    }
}
