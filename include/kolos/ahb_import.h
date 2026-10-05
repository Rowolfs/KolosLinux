#pragma once
#include <kolos/vulkan_context.h>
#include <unistd.h>
#include <vector>
#include <string>
namespace kolos {
struct Imported {
    VkDevice device=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    int sock=-1,importFd=-1;
    std::vector<int> fds;
    Imported() = default;
    Imported(const Imported&) = delete;
    Imported& operator=(const Imported&) = delete;
    ~Imported(){
        if(device){vkDeviceWaitIdle(device);if(image)vkDestroyImage(device,image,nullptr);
            if(memory)vkFreeMemory(device,memory,nullptr);}
        if(sock>=0)close(sock);if(importFd>=0)close(importFd);for(int fd:fds)close(fd);
    }
};
void importBuffer(Context& v, Imported& s, const std::string& path, uint32_t index);
}
