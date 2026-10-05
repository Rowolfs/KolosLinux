#include <kolos/diagnostic_renderer.h>
#include <cmath>

namespace kolos {
void DiagnosticRenderer::record(VkCommandBuffer commandBuffer, VkImage image, uint32_t frame) {
    float phase=float(frame)*0.025f;
    VkClearColorValue color{};
    for(int i=0;i<3;i++)color.float32[i]=0.5f+0.5f*std::sin(phase+float(i)*2.0943951f);
    color.float32[3]=1;
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdClearColorImage(commandBuffer,image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&range);
}
}
