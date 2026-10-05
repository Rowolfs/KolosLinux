#pragma once
#include <kolos/buffer_pool.h>
namespace kolos {
// Synchronous clear including ownership/layout transitions; no slot selection.
class DiagnosticRenderer {
public:
    DiagnosticRenderer(Context& context, KolosBufferPool& pool)
        : context_(context), pool_(pool) {}
    void draw(VkImage image, uint32_t frame);
private:
    Context& context_;
    KolosBufferPool& pool_;
};
}
