#include <kolos/android_graphics_bridge.h>
#include <graphics_wire.h>
#include <kolos_stream_wire.h>

#include <cerrno>
#include <cstring>
#include <climits>
#include <poll.h>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace kolos {
namespace wire = graphics::wire;

GraphicsEvent::GraphicsEvent(GraphicsEvent&& other) noexcept
    : type(other.type), pool(other.pool), id(other.id), stride(other.stride),
      serial(other.serial), handle(std::move(other.handle)), fds(other.fds),
      message(std::move(other.message)) {
    other.fds = {{-1, -1}};
}
GraphicsEvent& GraphicsEvent::operator=(GraphicsEvent&& other) noexcept {
    if (this != &other) {
        for (int fd : fds) if (fd >= 0) close(fd);
        type = other.type; pool = other.pool; id = other.id;
        stride = other.stride; serial = other.serial;
        handle = std::move(other.handle); fds = other.fds;
        message = std::move(other.message);
        other.fds = {{-1, -1}};
    }
    return *this;
}
GraphicsEvent::~GraphicsEvent() {
    for (int fd : fds) if (fd >= 0) close(fd);
}

static void signal(int fd) {
    uint64_t one = 1;
    if (write(fd, &one, sizeof(one)) < 0 && errno != EAGAIN)
        throw std::runtime_error("graphics eventfd write failed");
}
static void drain(int fd) {
    uint64_t count;
    while (read(fd, &count, sizeof(count)) > 0) {}
}
static GraphicsIdentity identity(wire::Reader& r) {
    return {r.get64(), r.get64(), r.get32()};
}
static std::vector<uint8_t> encode(GraphicsIdentity id) {
    std::vector<uint8_t> bytes;
    wire::put64(bytes, id.session); wire::put64(bytes, id.generation);
    wire::put32(bytes, id.slot);
    return bytes;
}
static GraphicsPoolInfo poolInfo(const wire::Frame& frame) {
    wire::Reader r(frame.payload);
    GraphicsPoolInfo pool{r.get64(), r.get64(), r.get32(), r.get32(),
                          r.get32(), r.get32()};
    r.end();
    if (!pool.session || !pool.generation || !pool.width || !pool.height ||
        pool.width > INT32_MAX || pool.height > INT32_MAX ||
        pool.format != 1 || !pool.slots || pool.slots > 16)
        throw std::runtime_error("unsupported Android graphics pool");
    return pool;
}

// Reuse the smoke client's exact BUFFER_INFO + AHB receive boundary. The
// compositor retains opaque bytes and both FDs; it never imports into Vulkan.
static GraphicsEvent fetchBuffer(const std::string& path,
                                 GraphicsPoolInfo pool, uint32_t slot) {
    kw::Fd socket = kw::connectTo(path);
    GraphicsEvent event(GraphicsEvent::Type::Buffer);
    event.id = {pool.session, pool.generation, slot};
    wire::writeFrame(socket.value, wire::GET_BUFFER, encode(event.id));
    auto info = wire::readFrame(socket.value);
    if (info.opcode != wire::BUFFER_INFO)
        throw std::runtime_error("expected BUFFER_INFO");
    wire::Reader r(info.payload);
    auto id = identity(r);
    auto width = r.get32(), height = r.get32(), format = r.get32();
    auto flags = r.get32(); event.stride = r.get32(); r.end();
    if (id.session != pool.session || id.generation != pool.generation ||
        id.slot != slot || width != pool.width || height != pool.height ||
        format != pool.format || flags != 1 || event.stride < width)
        throw std::runtime_error("invalid BUFFER_INFO");
    for (;;) {
        kw::wait(socket.value, POLLIN);
        uint8_t bytes[4096];
        alignas(cmsghdr) uint8_t ancillary[CMSG_SPACE(64 * sizeof(int))]{};
        iovec iov{bytes, sizeof(bytes)};
        msghdr msg{};
        msg.msg_iov = &iov; msg.msg_iovlen = 1;
        msg.msg_control = ancillary; msg.msg_controllen = sizeof(ancillary);
        ssize_t n = recvmsg(socket.value, &msg, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n < 0) throw std::runtime_error("AHB handle receive failed");
        bool extraFd = false;
        for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
                continue;
            if (c->cmsg_len < CMSG_LEN(0))
                throw std::runtime_error("invalid AHB ancillary header");
            auto length = c->cmsg_len - CMSG_LEN(0);
            for (size_t offset = 0; offset + sizeof(int) <= length;
                 offset += sizeof(int)) {
                int fd;
                std::memcpy(&fd, CMSG_DATA(c) + offset, sizeof(fd));
                if (event.fds[0] < 0) event.fds[0] = fd;
                else if (event.fds[1] < 0) event.fds[1] = fd;
                else { close(fd); extraFd = true; }
            }
        }
        if (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
            throw std::runtime_error("truncated AHB handle transfer");
        if (extraFd) throw std::runtime_error("too many AHB FDs");
        if (n == 0) break;
        event.handle.insert(event.handle.end(), bytes, bytes + n);
        if (event.handle.size() > 4096)
            throw std::runtime_error("oversized AHB handle");
    }
    if (event.handle.size() != 148 || event.fds[0] < 0 || event.fds[1] < 0)
        throw std::runtime_error("unsupported AHB handle family");
    // Android waits for IMPORT_ACK before accepting the next GET_BUFFER.
    // Here it acknowledges safe FD custody, not a Vulkan import. Mesa reports
    // successful import of the complete pool later via POOL_READY / START.
    wire::writeFrame(socket.value, wire::IMPORT_ACK, encode(event.id));
    return event;
}

AndroidGraphicsBridge::AndroidGraphicsBridge(std::string path)
    : path_(std::move(path)) {
    eventFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    commandFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (eventFd_ < 0 || commandFd_ < 0) {
        if (eventFd_ >= 0) close(eventFd_);
        if (commandFd_ >= 0) close(commandFd_);
        throw std::runtime_error("graphics eventfd creation failed");
    }
}
AndroidGraphicsBridge::~AndroidGraphicsBridge() {
    if (started_) {
        stop();
        if (worker_.joinable()) worker_.join();
    }
    if (eventFd_ >= 0) close(eventFd_);
    if (commandFd_ >= 0) close(commandFd_);
}
void AndroidGraphicsBridge::start() {
    if (started_) throw std::runtime_error("graphics bridge already started");
    started_ = true;
    worker_ = std::thread(&AndroidGraphicsBridge::run, this);
}
void AndroidGraphicsBridge::command(Command value) {
    { std::lock_guard<std::mutex> lock(mutex_); commands_.push_back(std::move(value)); }
    signal(commandFd_);
}
void AndroidGraphicsBridge::poolReady(uint64_t session, uint64_t generation) {
    command({Command::Type::Ready, {session, generation, 0}, 0});
}
void AndroidGraphicsBridge::present(GraphicsIdentity id, uint64_t serial, int acquireFenceFd) {
    command({Command::Type::Present, id, serial, Command::Fence(acquireFenceFd)});
}
void AndroidGraphicsBridge::stop() { command({Command::Type::Stop, {}, 0}); }
bool AndroidGraphicsBridge::takeCommand(Command& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (commands_.empty()) return false;
    out = std::move(commands_.front()); commands_.pop_front();
    return true;
}
void AndroidGraphicsBridge::push(GraphicsEvent event) {
    { std::lock_guard<std::mutex> lock(mutex_); events_.push_back(std::move(event)); }
    signal(eventFd_);
}
std::vector<GraphicsEvent> AndroidGraphicsBridge::takeEvents() {
    drain(eventFd_);
    std::vector<GraphicsEvent> events;
    std::lock_guard<std::mutex> lock(mutex_);
    while (!events_.empty()) {
        events.push_back(std::move(events_.front())); events_.pop_front();
    }
    return events;
}

void AndroidGraphicsBridge::run() {
    try {
        kw::Fd control = kw::connectTo(path_);
        wire::writeFrame(control.value, wire::HELLO);
        auto hello = wire::readFrame(control.value);
        if (hello.opcode != wire::HELLO_ACK || !hello.payload.empty())
            throw std::runtime_error("expected graphics HELLO_ACK");
        auto first = wire::readFrame(control.value);
        if (first.opcode != wire::SESSION_INFO)
            throw std::runtime_error("expected graphics SESSION_INFO");
        GraphicsPoolInfo pool = poolInfo(first);
        auto exportPool = [&] {
            GraphicsEvent notice(GraphicsEvent::Type::Pool);
            notice.pool = pool; push(std::move(notice));
            for (uint32_t slot = 0; slot < pool.slots; ++slot) {
                auto buffer = fetchBuffer(path_, pool, slot);
                push(std::move(buffer));
            }
        };
        exportPool();
        bool running = false;
        bool awaitingReady = true;
        for (;;) {
            pollfd fds[2]{{control.value, POLLIN, 0}, {commandFd_, POLLIN, 0}};
            int count;
            do { count = poll(fds, 2, -1); } while (count < 0 && errno == EINTR);
            if (count < 0) throw std::runtime_error("graphics bridge poll failed");
            if (fds[1].revents & POLLIN) {
                drain(commandFd_);
                Command cmd{};
                while (takeCommand(cmd)) {
                    if (cmd.type == Command::Type::Stop) {
                        // Android accepts STOP during replacement only after
                        // POOL_READY. Closing this connection instead lets its
                        // existing disconnect cleanup release the session.
                        if (!running || awaitingReady) return;
                        std::vector<uint8_t> bytes;
                        wire::put64(bytes, pool.session);
                        wire::put64(bytes, pool.generation);
                        wire::writeFrame(control.value, wire::STOP, bytes);
                        for (;;) {
                            auto reply = wire::readFrame(control.value);
                            if (reply.opcode == wire::DONE) {
                                wire::Reader r(reply.payload);
                                auto session = r.get64(), generation = r.get64();
                                (void)r.get64(); r.end();
                                if (session != pool.session || generation > pool.generation)
                                    throw std::runtime_error("invalid graphics DONE");
                                return;
                            }
                            if (reply.opcode == wire::FREE || reply.opcode == wire::ACK) {
                                wire::Reader r(reply.payload);
                                GraphicsEvent event(reply.opcode == wire::FREE ?
                                    GraphicsEvent::Type::Free : GraphicsEvent::Type::Ack);
                                event.id = identity(r);
                                if (reply.opcode == wire::ACK) event.serial = r.get64();
                                r.end(); push(std::move(event));
                            } else throw std::runtime_error("unexpected reply after STOP");
                        }
                    }
                    if (cmd.type == Command::Type::Ready) {
                        if (cmd.id.session != pool.session ||
                            cmd.id.generation != pool.generation)
                            throw std::runtime_error("stale graphics POOL_READY");
                        std::vector<uint8_t> bytes;
                        wire::put64(bytes, pool.session);
                        wire::put64(bytes, pool.generation);
                        wire::writeFrame(control.value,
                                         running ? wire::POOL_READY : wire::START,
                                         bytes);
                        running = true;
                        awaitingReady = false;
                    } else if (cmd.type == Command::Type::Present) {
                        if (cmd.id.session == pool.session &&
                            cmd.id.generation < pool.generation) {
                            // The compositor may have queued this before the
                            // Android replacement reached the bridge worker.
                            continue;
                        }
                        if (!running || cmd.id.session != pool.session ||
                            cmd.id.generation != pool.generation ||
                            cmd.id.slot >= pool.slots)
                            throw std::runtime_error("stale graphics PRESENT");
                        auto bytes = encode(cmd.id);
                        wire::put64(bytes, cmd.serial);
                        if (cmd.fence.fd >= 0)
                            wire::writeFrameWithFd(control.value, wire::PRESENT_FENCED,
                                                   bytes, cmd.fence.fd);
                        else
                            wire::writeFrame(control.value, wire::PRESENT, bytes);
                    }
                }
            }
            if (fds[0].revents & POLLIN) {
                auto frame = wire::readFrame(control.value);
                if (frame.opcode == wire::POOL_REPLACED) {
                    auto next = poolInfo(frame);
                    if (next.session != pool.session ||
                        next.generation <= pool.generation)
                        throw std::runtime_error("invalid replacement pool");
                    pool = next;
                    awaitingReady = true;
                    exportPool();
                } else if (frame.opcode == wire::FREE || frame.opcode == wire::ACK) {
                    wire::Reader r(frame.payload);
                    GraphicsEvent event(frame.opcode == wire::FREE ?
                        GraphicsEvent::Type::Free : GraphicsEvent::Type::Ack);
                    event.id = identity(r);
                    if (frame.opcode == wire::ACK) event.serial = r.get64();
                    r.end(); push(std::move(event));
                } else if (frame.opcode == wire::ERROR) {
                    throw std::runtime_error("Android graphics ERROR");
                } else throw std::runtime_error("unexpected Android graphics frame");
            }
            if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
                throw std::runtime_error("Android graphics disconnected");
        }
    } catch (const std::exception& error) {
        GraphicsEvent event(GraphicsEvent::Type::Error);
        event.message = error.what(); push(std::move(event));
    }
    push(GraphicsEvent(GraphicsEvent::Type::Closed));
}

} // namespace kolos
