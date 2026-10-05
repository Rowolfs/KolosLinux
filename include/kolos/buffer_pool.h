#pragma once
#include <kolos/ahb_import.h>
#include <kolos_stream_wire.h>
#include <array>
#include <deque>
#include <memory>

namespace kolos {
class KolosBufferPool;
class DiagnosticRenderer;

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
    AcquiredBuffer acquire();
    // Caller must complete GPU work before present. ACK does not release a slot.
    void present(const AcquiredBuffer& buffer, uint32_t frame);
    // Explicit STOP / DONE handshake; destructor only destroys local resources.
    void stop();
private:
    friend class DiagnosticRenderer;
    static constexpr uint32_t slotCount = 3;
    struct Slot {
        Imported buffer;
        SlotState state = SlotState::FREE;
        uint64_t generation = 0;
        bool initialFreeReceived = false;
    };
    void processFree(const kw::Message& message, bool initial);
    Imported& importedForRender(VkImage image);
    void requireActive() const;
    std::array<Slot, slotCount> slots_;
    std::deque<uint32_t> free_;
    kw::Fd control_;
    std::shared_ptr<const int> identity_ = std::make_shared<const int>(0);
    uint32_t nextFrame_ = 0;
    bool stopped_ = false;
    bool failed_ = false;
};
}
