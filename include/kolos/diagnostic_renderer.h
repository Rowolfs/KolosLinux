#pragma once
#include <vulkan/vulkan.h>
namespace kolos {
// Records only clear commands; caller supplies a recording command buffer and
// an image prepared in TRANSFER_DST_OPTIMAL for graphics queue ownership.
class DiagnosticRenderer {
public:
    void record(VkCommandBuffer commandBuffer, VkImage image, uint32_t frame);
};
}
