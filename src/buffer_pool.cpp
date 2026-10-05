#include <kolos/buffer_pool.h>
#include <utility>

namespace kolos {
AcquiredBuffer::AcquiredBuffer(std::shared_ptr<const int> owner, uint32_t slot,
                               uint64_t generation, VkImage image)
    : owner_(std::move(owner)), slot_(slot), generation_(generation), image_(image) {}

AcquiredBuffer::AcquiredBuffer(AcquiredBuffer&& other) noexcept {
    *this = std::move(other);
}
AcquiredBuffer& AcquiredBuffer::operator=(AcquiredBuffer&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        slot_ = std::exchange(other.slot_, UINT32_MAX);
        generation_ = std::exchange(other.generation_, 0);
        image_ = std::exchange(other.image_, VK_NULL_HANDLE);
    }
    return *this;
}
VkImage AcquiredBuffer::image() const {
    kw::require(owner_ && image_ != VK_NULL_HANDLE, "invalid acquisition token");
    return image_;
}

KolosBufferPool::KolosBufferPool(Context& context, const std::string& path) {
    for (uint32_t i = 0; i < slotCount; ++i)
        importBuffer(context, slots_[i].buffer, path, i);
    control_ = kw::connectTo(path);
    kw::send(control_.value, kw::START);
    for (uint32_t i = 0; i < slotCount; ++i)
        processIncomingMessageBlocking(IncomingPhase::INITIAL_FREE);
}

void KolosBufferPool::requireActive() const {
    kw::require(!stopped_ && !failed_, "buffer pool stopped or failed");
}

void KolosBufferPool::processFree(const kw::Message& message, bool initial) {
    kw::require(message.op == kw::FREE, "expected FREE");
    kw::require(message.index < slotCount, "invalid FREE slot");
    auto& slot = slots_[message.index];
    if (initial) {
        // Initial grants establish availability; they are not presentation releases.
        kw::require(!slot.initialFreeReceived, "duplicate initial FREE");
        kw::require(slot.state == SlotState::FREE, "invalid initial slot state");
        slot.initialFreeReceived = true;
    } else {
        kw::require(slot.state != SlotState::FREE, "duplicate FREE");
        kw::require(slot.state == SlotState::PRESENTED, "FREE for slot not PRESENTED");
        slot.state = SlotState::FREE;
    }
    free_.push_back(message.index);
}

uint32_t KolosBufferPool::processIncomingMessageBlocking(IncomingPhase phase,
                                                        uint32_t presentedSlot) {
    try {
        const auto message = kw::read(control_.value);
        switch (message.op) {
        case kw::FREE:
            kw::require(phase != IncomingPhase::STOP_DONE, "expected final DONE, got FREE");
            if (phase == IncomingPhase::PRESENT_ACK)
                kw::require(message.index != presentedSlot, "FREE for current presentation");
            processFree(message, phase == IncomingPhase::INITIAL_FREE);
            break;
        case kw::ACK:
            // present() consumes its ACK synchronously. An ACK during acquire
            // therefore has no pending presentation and is a protocol error.
            kw::require(phase == IncomingPhase::PRESENT_ACK, "unexpected presentation ACK");
            kw::require(message.index < slotCount, "invalid ACK slot");
            kw::require(message.index == presentedSlot && message.serial == nextFrame_,
                        "invalid presentation ACK");
            kw::require(slots_[message.index].state == SlotState::PRESENTED,
                        "ACK for slot not PRESENTED");
            break; // ACK never changes slot availability.
        case kw::DONE:
            if (phase != IncomingPhase::STOP_DONE) {
                stopped_ = true;
                throw std::runtime_error("Android ended stream with unexpected DONE");
            }
            kw::require(message.serial == nextFrame_, "missing final release confirmation");
            stopped_ = true; // Final displayed buffer released; no new acquisition.
            break;
        case kw::STOP:
            // STOP is normally outbound only. Treat a peer STOP as termination,
            // without inventing a reverse handshake or issuing another acquire.
            stopped_ = true;
            throw std::runtime_error("Android stopped stream");
        default:
            throw std::runtime_error("unexpected control message");
        }
        return message.op;
    } catch (...) {
        failed_ = true; // Includes EOF, socket timeout and protocol violations.
        throw;
    }
}

AcquiredBuffer KolosBufferPool::acquire() {
    requireActive();
    while (free_.empty())
        processIncomingMessageBlocking(IncomingPhase::ACQUIRE);
    const uint32_t index = free_.front();
    kw::require(index < slotCount, "invalid acquire slot");
    auto& slot = slots_[index];
    kw::require(slot.state == SlotState::FREE && slot.initialFreeReceived,
                "acquire slot not FREE");
    free_.pop_front();
    slot.state = SlotState::ACQUIRED;
    ++slot.generation;
    return AcquiredBuffer(identity_, index, slot.generation, slot.buffer.image);
}

void KolosBufferPool::present(const AcquiredBuffer& buffer, uint32_t frame) {
    requireActive();
    kw::require(buffer.owner_ == identity_, "buffer was not acquired from this pool");
    kw::require(buffer.slot_ < slotCount, "invalid present slot");
    auto& slot = slots_[buffer.slot_];
    kw::require(slot.generation == buffer.generation_ && slot.buffer.image == buffer.image_,
                "stale acquisition token");
    kw::require(slot.state == SlotState::ACQUIRED, "present requires ACQUIRED buffer (or repeated present)");
    kw::require(frame == nextFrame_, "invalid presentation frame sequence");
    slot.state = SlotState::PRESENTED;
    try {
        kw::send(control_.value, kw::PRESENT, buffer.slot_, frame);
        while (processIncomingMessageBlocking(IncomingPhase::PRESENT_ACK,
                                               buffer.slot_) != kw::ACK) {}
        ++nextFrame_;
    } catch (...) {
        failed_ = true; // Never retry after an ambiguous IPC send/receive failure.
        throw;
    }
}

Imported& KolosBufferPool::importedForRender(VkImage image) {
    requireActive();
    for (auto& slot : slots_) {
        if (slot.buffer.image == image) {
            kw::require(slot.state == SlotState::ACQUIRED, "render requires ACQUIRED image");
            return slot.buffer;
        }
    }
    throw std::runtime_error("image does not belong to buffer pool");
}

void KolosBufferPool::stop() {
    requireActive();
    for (const auto& slot : slots_)
        kw::require(slot.state != SlotState::ACQUIRED, "stop with acquired buffer");
    try {
        kw::send(control_.value, kw::STOP, 0, nextFrame_);
        processIncomingMessageBlocking(IncomingPhase::STOP_DONE);
        // DONE confirms release of the last displayed buffer, without another FREE.
    } catch (...) {
        failed_ = true;
        throw;
    }
}
}
