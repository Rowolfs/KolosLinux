#include <kolos/diagnostic_renderer.h>
#include <kolos/ahb_import.h>
#include <kolos_stream_wire.h>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <cmath>
namespace kolos {
static void need(bool ok,const char* text){kw::require(ok,text);}
static void check(VkResult r,const char* text){
    if(r!=VK_SUCCESS)throw std::runtime_error(std::string(text)+": VkResult="+std::to_string(r));
}
void DiagnosticRenderer::draw(VkImage image,uint32_t frame){
    auto& v = context_;
    auto& b = pool_.importedForRender(image);
    check(vkResetCommandPool(v.device,v.pool,0),"reset commands");
    check(vkResetFences(v.device,1,&v.fence),"reset fence");
    VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(v.cmd,&begin),"begin commands");
    VkImageMemoryBarrier barrier{};barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout=b.initialized?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;barrier.dstQueueFamilyIndex=v.family;
    barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.image=b.image;
    barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    // Reuse is allowed only after Android's release fence has signalled and FREE arrived.
    vkCmdPipelineBarrier(v.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
                         0,nullptr,0,nullptr,1,&barrier);
    float phase=float(frame)*0.025f;
    VkClearColorValue color{};
    for(int i=0;i<3;i++)color.float32[i]=0.5f+0.5f*std::sin(phase+float(i)*2.0943951f);
    color.float32[3]=1;
    vkCmdClearColorImage(v.cmd,b.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
    barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex=v.family;barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;
    barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;barrier.dstAccessMask=0;
    vkCmdPipelineBarrier(v.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,
                         0,nullptr,0,nullptr,1,&barrier);
    check(vkEndCommandBuffer(v.cmd),"end commands");
    VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&v.cmd;
    check(vkQueueSubmit(v.queue,1,&submit,v.fence),"submit GPU clear");
    check(vkWaitForFences(v.device,1,&v.fence,VK_TRUE,10000000000ULL),"GPU completion");
    b.initialized=true;
}
}
