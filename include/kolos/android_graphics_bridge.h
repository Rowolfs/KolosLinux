#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace kolos {

struct GraphicsIdentity {
    uint64_t session = 0;
    uint64_t generation = 0;
    uint32_t slot = 0;
};

struct GraphicsPoolInfo {
    uint64_t session = 0;
    uint64_t generation = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t format = 0;
    uint32_t slots = 0;
};

// FDs received from Android are owned here until moved into the compositor's
// pool cache. Wayland duplicates them when posting fd arguments.
struct GraphicsEvent {
    enum class Type { Pool, Buffer, Free, Ack, Error, Closed } type;
    GraphicsPoolInfo pool;
    GraphicsIdentity id;
    uint32_t stride = 0;
    uint64_t serial = 0;
    std::vector<uint8_t> handle;
    std::array<int, 2> fds{{-1, -1}};
    std::string message;
    GraphicsEvent(Type kind) : type(kind) {}
    GraphicsEvent(const GraphicsEvent&) = delete;
    GraphicsEvent& operator=(const GraphicsEvent&) = delete;
    GraphicsEvent(GraphicsEvent&& other) noexcept;
    GraphicsEvent& operator=(GraphicsEvent&& other) noexcept;
    ~GraphicsEvent();
};

// Sole owner of the Android Graphics Protocol v2 connection. One instance is
// bound to one KolosWindow. Protocol v2 currently exposes one pre-existing
// Android session; it has no CREATE_SESSION or window-id routing operation.
class AndroidGraphicsBridge {
public:
    explicit AndroidGraphicsBridge(std::string socketPath);
    ~AndroidGraphicsBridge();
    AndroidGraphicsBridge(const AndroidGraphicsBridge&) = delete;
    AndroidGraphicsBridge& operator=(const AndroidGraphicsBridge&) = delete;
    void start();
    void poolReady(uint64_t session, uint64_t generation);
    // Takes ownership of acquireFenceFd; -1 selects the legacy CPU-wait path.
    void present(GraphicsIdentity id, uint64_t serial, int acquireFenceFd = -1);
    void stop();
    int eventFd() const { return eventFd_; }
    std::vector<GraphicsEvent> takeEvents();
private:
    struct Command {
        enum class Type { Ready, Present, Stop } type;
        GraphicsIdentity id;
        uint64_t serial = 0;
        struct Fence {
            int fd = -1;
            Fence() = default;
            explicit Fence(int value) : fd(value) {}
            ~Fence() { if (fd >= 0) close(fd); }
            Fence(const Fence&) = delete;
            Fence& operator=(const Fence&) = delete;
            Fence(Fence&& other) noexcept : fd(other.fd) { other.fd = -1; }
            Fence& operator=(Fence&& other) noexcept {
                if (this != &other) { if (fd >= 0) close(fd); fd = other.fd; other.fd = -1; }
                return *this;
            }
        } fence;
    };
    void run();
    void push(GraphicsEvent event);
    void command(Command command);
    bool takeCommand(Command& out);
    std::string path_;
    int eventFd_ = -1;
    int commandFd_ = -1;
    std::thread worker_;
    std::mutex mutex_;
    std::deque<GraphicsEvent> events_;
    std::deque<Command> commands_;
    bool started_ = false;
};

} // namespace kolos
