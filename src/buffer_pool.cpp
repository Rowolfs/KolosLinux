#include <kolos/buffer_pool.h>
#include <graphics_wire.h>
#include <utility>
#include <cstdio>
#include <poll.h>

namespace kolos {
namespace gw = graphics::wire;
static void putIdentity(std::vector<uint8_t>& p,uint64_t session,uint64_t generation,uint32_t slot) {
    gw::put64(p,session);gw::put64(p,generation);gw::put32(p,slot);
}
static void checkIdentity(gw::Reader& r,uint64_t session,uint64_t generation,uint32_t& slot) {
    auto s=r.get64(),g=r.get64();slot=r.get32();
    kw::require(s==session && g==generation,"graphics identity mismatch");
}
static void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + ": VkResult=" + std::to_string(result));
}
AcquiredBuffer::AcquiredBuffer(std::shared_ptr<const int> owner, uint32_t slot,
                               uint64_t generation, VkImage image, std::shared_ptr<void> lease)
    : owner_(std::move(owner)), lease_(std::move(lease)), slot_(slot),
      generation_(generation), image_(image) {}

AcquiredBuffer::AcquiredBuffer(AcquiredBuffer&& other) noexcept {
    *this = std::move(other);
}
AcquiredBuffer& AcquiredBuffer::operator=(AcquiredBuffer&& other) noexcept {
    if (this != &other) {
        owner_ = std::move(other.owner_);
        lease_ = std::move(other.lease_);
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
        importBuffer(context, current_->slots[i].buffer, path, i);
    control_ = kw::connectTo(path);
    kw::send(control_.value, kw::START);
    for (uint32_t i = 0; i < slotCount; ++i)
        processIncomingMessageBlocking(IncomingPhase::INITIAL_FREE);
}

KolosBufferPool::KolosBufferPool(Context& context,const std::string& path,GraphicsV2)
    :context_(context),graphicsWire_(true),path_(path) {
    control_=kw::connectTo(path);
    gw::writeFrame(control_.value,gw::HELLO);
    auto hello=gw::readFrame(control_.value);
    kw::require(hello.opcode==gw::HELLO_ACK && hello.payload.empty(),"expected HELLO_ACK");
    auto info=gw::readFrame(control_.value);
    kw::require(info.opcode==gw::SESSION_INFO,"expected SESSION_INFO");
    gw::Reader ir(info.payload);
    sessionId_=ir.get64();poolGeneration_=ir.get64();
    uint32_t width=ir.get32(),height=ir.get32(),format=ir.get32(),count=ir.get32();ir.end();
    current_=importGraphicsGeneration(poolGeneration_,width,height,format,count);
    std::vector<uint8_t> start;gw::put64(start,sessionId_);gw::put64(start,poolGeneration_);
    gw::writeFrame(control_.value,gw::START,start);
    for(uint32_t i=0;i<slotCount;i++) processIncomingMessageBlocking(IncomingPhase::INITIAL_FREE);
}

KolosBufferPool::~KolosBufferPool() {
    if(submissionPending_ && context_.fence)
        (void)vkWaitForFences(context_.device,1,&context_.fence,VK_TRUE,UINT64_MAX);
}

std::shared_ptr<KolosBufferPool::Generation> KolosBufferPool::importGraphicsGeneration(
    uint64_t generation,uint32_t width,uint32_t height,uint32_t format,uint32_t count) {
    kw::require(sessionId_ && generation && width && height && format==1 && count==slotCount,
                "unsupported graphics pool configuration");
    auto imported=std::make_shared<Generation>();
    imported->number=generation;
    std::printf("Graphics session=%llu generation=%llu size=%ux%u slots=%u\n",
        (unsigned long long)sessionId_,(unsigned long long)generation,width,height,count);
    for(uint32_t i=0;i<slotCount;i++) {
        auto connection=kw::connectTo(path_);
        std::vector<uint8_t> p;putIdentity(p,sessionId_,generation,i);
        gw::writeFrame(connection.value,gw::GET_BUFFER,p);
        auto reply=gw::readFrame(connection.value);
        kw::require(reply.opcode==gw::BUFFER_INFO,"expected BUFFER_INFO");
        gw::Reader br(reply.payload);uint32_t index=UINT32_MAX;
        auto sid=br.get64(),gen=br.get64();index=br.get32();
        kw::require(sid==sessionId_ && gen==generation,"graphics identity mismatch");
        uint32_t w=br.get32(),h=br.get32(),f=br.get32(),flags=br.get32(),stride=br.get32();br.end();
        kw::require(index==i && w==width && h==height && f==format && flags==1 && stride>=w,
                    "unsupported BUFFER_INFO");
        int transferFd=connection.value;connection.value=-1;
        imported->slots[i].buffer.waitForDeviceIdleOnDestroy=false;
        importGraphicsBuffer(context_,imported->slots[i].buffer,transferFd,w,h,stride,f,i);
        // This client waits its per-submit fence before the generation can retire.
        gw::writeFrame(imported->slots[i].buffer.sock,gw::IMPORT_ACK,p);
        close(imported->slots[i].buffer.sock);imported->slots[i].buffer.sock=-1;
        std::printf("Imported slot %u\n",i);
    }
    return imported;
}

void KolosBufferPool::requireActive() const {
    kw::require(!stopped_ && !failed_, "buffer pool stopped or failed");
}

std::shared_ptr<KolosBufferPool::Generation> KolosBufferPool::findGeneration(uint64_t number) const {
    if(current_->number==number) return current_;
    for(const auto& pool:retired_) if(pool->number==number) return pool;
    throw std::runtime_error("FREE for unknown graphics generation");
}

void KolosBufferPool::collectRetired() {
    auto it=retired_.begin();
    while(it!=retired_.end()) {
        bool outstanding=false;
        for(const auto& slot:(*it)->slots)
            outstanding |= slot.state!=SlotState::FREE;
        if(outstanding || it->use_count()!=1) {++it;continue;}
        std::printf("Graphics generation=%llu DESTROYED\n",
                    (unsigned long long)(*it)->number);
        it=retired_.erase(it);
    }
}

void KolosBufferPool::processFree(const kw::Message& message, bool initial,
                                  uint64_t generation) {
    kw::require(message.op == kw::FREE, "expected FREE");
    kw::require(message.index < slotCount, "invalid FREE slot");
    auto pool=findGeneration(generation);
    auto& slot = pool->slots[message.index];
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
    if(pool==current_) pool->free.push_back(message.index);
}

uint32_t KolosBufferPool::processIncomingMessageBlocking(IncomingPhase phase,
                                                        uint32_t presentedSlot) {
    try {
        kw::Message message;
        uint64_t releaseGeneration=current_->number;
        if(graphicsWire_) {
            auto frame=gw::readFrame(control_.value);
            if(frame.opcode==gw::ERROR) throw std::runtime_error("Android graphics ERROR");
            gw::Reader r(frame.payload);uint32_t index=0;
            switch(frame.opcode) {
            case gw::POOL_REPLACED: {
                auto sid=r.get64(),generation=r.get64();
                uint32_t width=r.get32(),height=r.get32(),format=r.get32(),count=r.get32();r.end();
                kw::require(sid==sessionId_ && generation>poolGeneration_,
                            "invalid replacement generation");
                auto replacement=importGraphicsGeneration(generation,width,height,format,count);
                std::vector<uint8_t> ready;gw::put64(ready,sessionId_);gw::put64(ready,generation);
                gw::writeFrame(control_.value,gw::POOL_READY,ready);
                retired_.push_back(current_);
                std::printf("Graphics generation=%llu RETIRED\n",
                            (unsigned long long)poolGeneration_);
                current_=std::move(replacement);
                poolGeneration_=generation;
                std::printf("Graphics generation=%llu CURRENT\n",
                            (unsigned long long)poolGeneration_);
                collectRetired();
                return gw::POOL_REPLACED;
            }
            case gw::FREE: {
                auto sid=r.get64();releaseGeneration=r.get64();index=r.get32();r.end();
                kw::require(sid==sessionId_,"FREE for foreign graphics session");
                message={kw::MAGIC,kw::FREE,index,0};break;
            }
            case gw::ACK: {
                checkIdentity(r,sessionId_,poolGeneration_,index);
                uint64_t serial=r.get64();r.end();
                kw::require(serial<=UINT32_MAX,"ACK serial overflow");
                message={kw::MAGIC,kw::ACK,index,uint32_t(serial)};break;
            }
            case gw::DONE: {
                auto s=r.get64(),g=r.get64(),frames=r.get64();r.end();
                kw::require(s==sessionId_ && g<=poolGeneration_ && frames<=UINT32_MAX,
                            "DONE identity/count mismatch");
                message={kw::MAGIC,kw::DONE,0,uint32_t(frames)};break;
            }
            default: throw std::runtime_error("unexpected graphics control opcode");
            }
        } else message=kw::read(control_.value);
        switch (message.op) {
        case kw::FREE:
            kw::require(graphicsWire_ || phase != IncomingPhase::STOP_DONE,
                        "expected final DONE, got FREE");
            kw::require(message.index < slotCount,"invalid FREE slot");
            if (phase == IncomingPhase::PRESENT_ACK && releaseGeneration==current_->number)
                kw::require(message.index != presentedSlot, "FREE for current presentation");
            processFree(message, phase == IncomingPhase::INITIAL_FREE ||
                        !findGeneration(releaseGeneration)->slots[message.index].initialFreeReceived,
                        releaseGeneration);
            collectRetired();
            break;
        case kw::ACK:
            // present() consumes its ACK synchronously. An ACK during acquire
            // therefore has no pending presentation and is a protocol error.
            kw::require(phase == IncomingPhase::PRESENT_ACK, "unexpected presentation ACK");
            kw::require(message.index < slotCount, "invalid ACK slot");
            kw::require(message.index == presentedSlot && message.serial == nextFrame_,
                        "invalid presentation ACK");
            kw::require(current_->slots[message.index].state == SlotState::PRESENTED,
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
    for(;;) {
        pollfd pending{control_.value,POLLIN,0};
        int ready=graphicsWire_ ? poll(&pending,1,0) : 0;
        if(ready<0) throw std::runtime_error("graphics control poll failed");
        if(ready>0 && pending.revents) {
            processIncomingMessageBlocking(IncomingPhase::ACQUIRE);
            continue;
        }
        if(!current_->free.empty()) break;
        processIncomingMessageBlocking(IncomingPhase::ACQUIRE);
    }
    const uint32_t index = current_->free.front();
    kw::require(index < slotCount, "invalid acquire slot");
    auto& slot = current_->slots[index];
    kw::require(slot.state == SlotState::FREE && slot.initialFreeReceived,
                "acquire slot not FREE");
    kw::require(slot.imageState == ImageState::FOREIGN_UNDEFINED ||
                slot.imageState == ImageState::FOREIGN_GENERAL, "acquire image not FOREIGN");
    current_->free.pop_front();
    slot.state = SlotState::ACQUIRED;
    ++slot.generation;
    return AcquiredBuffer(current_->identity, index, slot.generation,
                          slot.buffer.image, current_);
}

KolosBufferPool::Slot& KolosBufferPool::validateAcquired(const AcquiredBuffer& buffer) {
    requireActive();
    kw::require(buffer.owner_ == current_->identity, "buffer was not acquired from this generation");
    kw::require(buffer.slot_ < slotCount, "invalid acquisition slot");
    auto& slot = current_->slots[buffer.slot_];
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
        submissionPending_ = true;
        check(vkWaitForFences(v.device,1,&v.fence,VK_TRUE,10000000000ULL),"GPU completion");
        submissionPending_ = false;
        slot.imageState = ImageState::PRESENT_READY;
        recordingSlot_ = UINT32_MAX;
    } catch (...) {
        failed_ = true; // Unknown GPU completion/ownership: never send PRESENT.
        throw;
    }
}

bool KolosBufferPool::present(const AcquiredBuffer& buffer, uint32_t frame) {
    auto& slot = validateAcquired(buffer);
    auto presenting=current_;
    kw::require(recordingSlot_ == UINT32_MAX, "present during command recording");
    kw::require(slot.imageState == ImageState::PRESENT_READY, "present before GPU completion / FOREIGN release");
    kw::require(frame == nextFrame_, "invalid presentation frame sequence");
    slot.state = SlotState::PRESENTED;
    try {
        if(graphicsWire_) {
            std::vector<uint8_t> p;putIdentity(p,sessionId_,poolGeneration_,buffer.slot_);
            gw::put64(p,frame);gw::writeFrame(control_.value,gw::PRESENT,p);
        } else kw::send(control_.value, kw::PRESENT, buffer.slot_, frame);
        for(;;) {
            auto op=processIncomingMessageBlocking(IncomingPhase::PRESENT_ACK,buffer.slot_);
            if(op==kw::ACK) break;
            if(op==gw::POOL_REPLACED) {
                // Android discards this stale in-flight PRESENT. The completed
                // GPU work is never displayed and needs no Android FREE.
                presenting->slots[buffer.slot_].state=SlotState::FREE;
                collectRetired();
                return false;
            }
        }
        ++nextFrame_;
        return true;
    } catch (...) {
        failed_ = true; // Never retry after an ambiguous IPC send/receive failure.
        throw;
    }
}

void KolosBufferPool::stop() {
    requireActive();
    for (const auto& slot : current_->slots)
        kw::require(slot.state != SlotState::ACQUIRED, "stop with acquired buffer");
    try {
        if(graphicsWire_) {
            std::vector<uint8_t> p;gw::put64(p,sessionId_);gw::put64(p,poolGeneration_);
            gw::writeFrame(control_.value,gw::STOP,p);
            while(processIncomingMessageBlocking(IncomingPhase::STOP_DONE)!=kw::DONE) {}
        } else {
            kw::send(control_.value, kw::STOP, 0, nextFrame_);
            processIncomingMessageBlocking(IncomingPhase::STOP_DONE);
        }
    } catch (...) {
        failed_ = true;
        throw;
    }
}
}
