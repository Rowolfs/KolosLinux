#pragma once
#include <kolos/ahb_import.h>
#include <kolos_stream_wire.h>
#include <array>
#include <deque>
#include <memory>
#include <vector>

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
                   uint64_t generation, VkImage image, std::shared_ptr<void> lease);
    std::shared_ptr<const int> owner_;
    std::shared_ptr<void> lease_;
    uint32_t slot_ = UINT32_MAX;
    uint64_t generation_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
};

class KolosBufferPool {
public:
    enum class SlotState { FREE, ACQUIRED, PRESENTED };
    struct GraphicsV2 {};
    // Imports three buffers and completes START / initial FREE handshake.
    KolosBufferPool(Context& context, const std::string& path);
    KolosBufferPool(Context& context, const std::string& path, GraphicsV2);
    ~KolosBufferPool();
    uint64_t sessionId() const { return sessionId_; }
    uint64_t poolGeneration() const { return poolGeneration_; }
    KolosBufferPool(const KolosBufferPool&) = delete;
    KolosBufferPool& operator=(const KolosBufferPool&) = delete;
    // Blocks on control IPC when the FIFO is empty; propagates timeout/shutdown errors.
    AcquiredBuffer acquire();
    // Begin the one frame command buffer and record FOREIGN -> graphics barrier.
    VkCommandBuffer prepareForRender(const AcquiredBuffer& buffer);
    // Record graphics -> FOREIGN barrier, submit once, and wait for GPU fence.
    void prepareForPresent(const AcquiredBuffer& buffer);
    // Requires successful prepareForPresent. ACK does not release a slot.
    // False means resize discarded this old-generation frame; render it again.
    bool present(const AcquiredBuffer& buffer, uint32_t frame);
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
    struct Generation {
        uint64_t number = 0;
        std::array<Slot, slotCount> slots;
        std::deque<uint32_t> free;
        std::shared_ptr<const int> identity = std::make_shared<const int>(0);
    };
    enum class IncomingPhase { INITIAL_FREE, ACQUIRE, PRESENT_ACK, STOP_DONE };
    // Read and validate one message. All control reads go through this dispatcher.
    uint32_t processIncomingMessageBlocking(IncomingPhase phase,
                                            uint32_t presentedSlot = UINT32_MAX);
    void processFree(const kw::Message& message, bool initial, uint64_t generation);
    std::shared_ptr<Generation> findGeneration(uint64_t generation) const;
    std::shared_ptr<Generation> importGraphicsGeneration(uint64_t generation,
        uint32_t width, uint32_t height, uint32_t format, uint32_t count);
    void collectRetired();
    Slot& validateAcquired(const AcquiredBuffer& buffer);
    void requireActive() const;
    Context& context_;
    std::shared_ptr<Generation> current_ = std::make_shared<Generation>();
    std::vector<std::shared_ptr<Generation>> retired_;
    kw::Fd control_;
    uint32_t nextFrame_ = 0;
    uint32_t recordingSlot_ = UINT32_MAX;
    bool stopped_ = false;
    bool failed_ = false;
    bool graphicsWire_ = false;
    bool submissionPending_ = false;
    std::string path_;
    uint64_t sessionId_ = 0;
    uint64_t poolGeneration_ = 0;
};
}
