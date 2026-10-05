#pragma once
#include <kolos/ahb_import.h>
#include <kolos_stream_wire.h>
#include <array>
#include <deque>
#include <memory>

namespace kolos {
class KolosBufferPool;

// Opaque acquisition token. It does not own or automatically release the image.
// The pool and Vulkan context must outlive its use.
class AcquiredBuffer {
public:
    AcquiredBuffer() = default;
    AcquiredBuffer(const AcquiredBuffer&) = delete;
    AcquiredBuffer& operator=(const AcquiredBuffer&) = delete;
    AcquiredBuffer(AcquiredBuffer&& other) noexcept;
    AcquiredBuffer& operator=(AcquiredBuffer&& other) noexcept;
    VkImage image() const;
private:
    friend class KolosBufferPool;
    AcquiredBuffer(std::shared_ptr<const int> owner, uint32_t slot,
                   uint64_t generation, VkImage image);
    std::shared_ptr<const int> owner_;
    uint32_t slot_ = UINT32_MAX;
    uint64_t generation_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
};

class KolosBufferPool {
public:
    enum class SlotState { FREE, ACQUIRED, PRESENTED };
    // Imports three buffers and completes START / initial FREE handshake.
    KolosBufferPool(Context& context, const std::string& path);
    KolosBufferPool(const KolosBufferPool&) = delete;
    KolosBufferPool& operator=(const KolosBufferPool&) = delete;
    // Blocks on control IPC when the FIFO is empty; propagates timeout/shutdown errors.
    AcquiredBuffer acquire();
    // Begin the one frame command buffer and record FOREIGN -> graphics barrier.
    VkCommandBuffer prepareForRender(const AcquiredBuffer& buffer);
    // Record graphics -> FOREIGN barrier, submit once, and wait for GPU fence.
    void prepareForPresent(const AcquiredBuffer& buffer);
    // Requires successful prepareForPresent. ACK does not release a slot.
    void present(const AcquiredBuffer& buffer, uint32_t frame);
    // Explicit STOP / DONE handshake; destructor only destroys local resources.
    void stop();
private:
    static constexpr uint32_t slotCount = 3;
    // RENDER_READY describes recorded commands until their synchronous submission.
    enum class ImageState { FOREIGN_UNDEFINED, FOREIGN_GENERAL, RENDER_READY, PRESENT_READY };
    struct Slot {
        Imported buffer;
        SlotState state = SlotState::FREE;
        ImageState imageState = ImageState::FOREIGN_UNDEFINED;
        uint64_t generation = 0;
        bool initialFreeReceived = false;
    };
    enum class IncomingPhase { INITIAL_FREE, ACQUIRE, PRESENT_ACK, STOP_DONE };
    // Read and validate one message. All control reads go through this dispatcher.
    uint32_t processIncomingMessageBlocking(IncomingPhase phase,
                                            uint32_t presentedSlot = UINT32_MAX);
    void processFree(const kw::Message& message, bool initial);
    Slot& validateAcquired(const AcquiredBuffer& buffer);
    void requireActive() const;
    Context& context_;
    std::array<Slot, slotCount> slots_;
    std::deque<uint32_t> free_;
    kw::Fd control_;
    std::shared_ptr<const int> identity_ = std::make_shared<const int>(0);
    uint32_t nextFrame_ = 0;
    uint32_t recordingSlot_ = UINT32_MAX;
    bool stopped_ = false;
    bool failed_ = false;
};
}
