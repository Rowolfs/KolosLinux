#include <kolos/buffer_pool.h>
#include <utility>

namespace kolos {
static void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}
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

KolosBufferPool::KolosBufferPool(Context& context, const std::string& path) : context_(context) {
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
        kw::require(slot.imageState == ImageState::PRESENT_READY, "FREE for image not presentation ready");
        slot.state = SlotState::FREE;
        slot.imageState = ImageState::FOREIGN_GENERAL;
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
    kw::require(recordingSlot_ == UINT32_MAX, "acquire during command recording");
    while (free_.empty())
        processIncomingMessageBlocking(IncomingPhase::ACQUIRE);
    const uint32_t index = free_.front();
    kw::require(index < slotCount, "invalid acquire slot");
    auto& slot = slots_[index];
    kw::require(slot.state == SlotState::FREE && slot.initialFreeReceived,
                "acquire slot not FREE");
    kw::require(slot.imageState == ImageState::FOREIGN_UNDEFINED ||
                slot.imageState == ImageState::FOREIGN_GENERAL, "acquire image not FOREIGN");
    free_.pop_front();
    slot.state = SlotState::ACQUIRED;
    ++slot.generation;
    return AcquiredBuffer(identity_, index, slot.generation, slot.buffer.image);
}

KolosBufferPool::Slot& KolosBufferPool::validateAcquired(const AcquiredBuffer& buffer) {
    requireActive();
    kw::require(buffer.owner_ == identity_, "buffer was not acquired from this pool");
    kw::require(buffer.slot_ < slotCount, "invalid acquisition slot");
    auto& slot = slots_[buffer.slot_];
    kw::require(slot.generation == buffer.generation_ && slot.buffer.image == buffer.image_,
                "stale acquisition token");
    kw::require(slot.state == SlotState::ACQUIRED, "operation requires ACQUIRED buffer (or repeated present)");
    return slot;
}

VkCommandBuffer KolosBufferPool::prepareForRender(const AcquiredBuffer& buffer) {
    auto& slot = validateAcquired(buffer);
    kw::require(recordingSlot_ == UINT32_MAX, "another buffer is already recording");
    kw::require(slot.imageState == ImageState::FOREIGN_UNDEFINED ||
                slot.imageState == ImageState::FOREIGN_GENERAL, "render already begun for acquisition");
    auto& v = context_;
    try {
        check(vkResetCommandPool(v.device,v.pool,0),"reset commands");
        check(vkResetFences(v.device,1,&v.fence),"reset fence");
        VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(v.cmd,&begin),"begin commands");
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=slot.imageState == ImageState::FOREIGN_UNDEFINED
            ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;barrier.dstQueueFamilyIndex=v.family;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.image=slot.buffer.image;
        barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        // Reuse is allowed only after Android's release fence signalled and FREE arrived.
        vkCmdPipelineBarrier(v.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
                             0,nullptr,0,nullptr,1,&barrier);
        slot.imageState = ImageState::RENDER_READY;
        recordingSlot_ = buffer.slot_;
        return v.cmd;
    } catch (...) {
        failed_ = true;
        throw;
    }
}

void KolosBufferPool::prepareForPresent(const AcquiredBuffer& buffer) {
    auto& slot = validateAcquired(buffer);
    kw::require(recordingSlot_ == buffer.slot_ && slot.imageState == ImageState::RENDER_READY,
                "prepareForPresent requires active render preparation");
    auto& v = context_;
    try {
        VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=v.family;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
        barrier.image=slot.buffer.image;
        barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdPipelineBarrier(v.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,
                             0,nullptr,0,nullptr,1,&barrier);
        check(vkEndCommandBuffer(v.cmd),"end commands");
        VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&v.cmd;
        check(vkQueueSubmit(v.queue,1,&submit,v.fence),"submit GPU clear");
        check(vkWaitForFences(v.device,1,&v.fence,VK_TRUE,10000000000ULL),"GPU completion");
        slot.imageState = ImageState::PRESENT_READY;
        recordingSlot_ = UINT32_MAX;
    } catch (...) {
        failed_ = true; // Unknown GPU completion/ownership: never send PRESENT.
        throw;
    }
}

void KolosBufferPool::present(const AcquiredBuffer& buffer, uint32_t frame) {
    auto& slot = validateAcquired(buffer);
    kw::require(recordingSlot_ == UINT32_MAX, "present during command recording");
    kw::require(slot.imageState == ImageState::PRESENT_READY, "present before GPU completion / FOREIGN release");
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
