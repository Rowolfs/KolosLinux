// Kolos diagnostic: Android AHardwareBuffer -> Debian Turnip -> GPU clear.
// Build: g++ -std=c++17 -O2 -Wall -Wextra kolos_ahb_import.cpp -lvulkan -o kolos_ahb_import
// Run with VK_DRIVER_FILES pointing to the tested freedreno ICD.
// This is a device-specific layout hypothesis, NOT a generic AHB decoder.
#include <vulkan/vulkan.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>

static void need(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void check(VkResult r, const char* what) {
    std::printf("%s: %d\n", what, int(r));
    if (r != VK_SUCCESS) throw std::runtime_error(what);
}
struct State {
    int sock=-1, importFd=-1;
    std::vector<int> fds;
    VkInstance instance=VK_NULL_HANDLE;
    VkDevice device=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory memory=VK_NULL_HANDLE;
    VkCommandPool pool=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    ~State() {
        if (device) {
            // This diagnostic waits before freeing resources even on an error.
            vkDeviceWaitIdle(device);
            if (fence) vkDestroyFence(device, fence, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (image) vkDestroyImage(device, image, nullptr);
            if (memory) vkFreeMemory(device, memory, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance) vkDestroyInstance(instance, nullptr);
        if (importFd>=0) close(importFd);
        for (int fd:fds) close(fd);
        if (sock>=0) close(sock);
    }
};

static std::vector<unsigned char> receive(State& s, const char* path) {
    s.sock=socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
    need(s.sock>=0, "socket failed");
    timeval timeout{10,0};
    need(setsockopt(s.sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout))==0,"timeout failed");
    sockaddr_un addr{}; addr.sun_family=AF_UNIX;
    need(std::strlen(path)<sizeof(addr.sun_path),"socket path too long");
    std::strcpy(addr.sun_path,path);
    if (connect(s.sock,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))!=0)
        throw std::runtime_error(std::string("connect: ")+strerror(errno));
    std::vector<unsigned char> bytes;
    for (;;) {
        unsigned char data[4096];
        alignas(cmsghdr) unsigned char control[CMSG_SPACE(64*sizeof(int))]{};
        iovec iov{data,sizeof(data)};
        msghdr msg{}; msg.msg_iov=&iov; msg.msg_iovlen=1;
        msg.msg_control=control; msg.msg_controllen=sizeof(control);
        ssize_t n;
        do { n=recvmsg(s.sock,&msg,MSG_CMSG_CLOEXEC); } while(n<0 && errno==EINTR);
        need(n>=0,"recvmsg failed");
        for(cmsghdr* c=CMSG_FIRSTHDR(&msg); c; c=CMSG_NXTHDR(&msg,c)) {
            if(c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS) {
                need(c->cmsg_len>=CMSG_LEN(0),"bad ancillary message");
                size_t size=c->cmsg_len-CMSG_LEN(0);
                for(size_t i=0;i+sizeof(int)<=size;i+=sizeof(int)) {
                    int fd; memcpy(&fd,CMSG_DATA(c)+i,sizeof(fd)); s.fds.push_back(fd);
                }
            }
        }
        need(!(msg.msg_flags & (MSG_CTRUNC|MSG_TRUNC)),"truncated message");
        if(n==0) break;
        bytes.insert(bytes.end(),data,data+n);
        need(bytes.size()<=4096,"unexpected handle size");
    }
    return bytes;
}

int main(int argc,char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    State s;
    try {
        // Establish Vulkan before connecting, so Android's ACK timeout is not
        // spent loading the driver.
        VkApplicationInfo app{}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName="Kolos AHB import test"; app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo=&app;
        check(vkCreateInstance(&ici,nullptr,&s.instance),"create instance");
        uint32_t count=0;
        check(vkEnumeratePhysicalDevices(s.instance,&count,nullptr),"GPU count");
        need(count>0,"no GPU");
        std::vector<VkPhysicalDevice> gpus(count);
        check(vkEnumeratePhysicalDevices(s.instance,&count,gpus.data()),"enumerate GPUs");
        VkPhysicalDevice gpu=VK_NULL_HANDLE;
        for(uint32_t i=0;i<count;i++) {
            VkPhysicalDeviceProperties p{}; vkGetPhysicalDeviceProperties(gpus[i],&p);
            std::printf("GPU: %s\n",p.deviceName);
            if(p.vendorID==0x5143 && strstr(p.deviceName,"Turnip")) gpu=gpus[i];
        }
        need(gpu!=VK_NULL_HANDLE,"expected Qualcomm Turnip; select freedreno ICD");
        uint32_t ec=0;
        check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&ec,nullptr),"extension count");
        std::vector<VkExtensionProperties> exts(ec);
        check(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&ec,exts.data()),"extensions");
        const char* wanted[]={"VK_KHR_external_memory_fd","VK_EXT_external_memory_dma_buf",
                              "VK_EXT_image_drm_format_modifier","VK_EXT_queue_family_foreign"};
        for(auto name:wanted) {
            bool found=false;
            for(uint32_t i=0;i<ec;i++) if(strcmp(name,exts[i].extensionName)==0) found=true;
            need(found,name);
        }
        uint32_t qc=0; vkGetPhysicalDeviceQueueFamilyProperties(gpu,&qc,nullptr);
        std::vector<VkQueueFamilyProperties> queues(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu,&qc,queues.data());
        uint32_t family=UINT32_MAX;
        for(uint32_t i=0;i<qc;i++) if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {family=i;break;}
        need(family!=UINT32_MAX,"no graphics queue");
        float priority=1;
        VkDeviceQueueCreateInfo qci{}; qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
        VkDeviceCreateInfo dci{}; dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        dci.enabledExtensionCount=4; dci.ppEnabledExtensionNames=wanted;
        check(vkCreateDevice(gpu,&dci,nullptr,&s.device),"create device");
        VkQueue queue; vkGetDeviceQueue(s.device,family,0,&queue);

        auto bytes=receive(s,argc>1?argv[1]:"/mnt/kolos-wayland/ahb-test.sock");
        std::printf("Received %zu bytes, %zu FDs\n",bytes.size(),s.fds.size());
        need(bytes.size()==148 && s.fds.size()==2,"unexpected AHB wire layout; stop");
        uint32_t w[37]; memcpy(w,bytes.data(),sizeof(w));
        // Match the exact handle family observed on the user's device. Padding,
        // process IDs, allocation IDs and old process addresses are not used.
        need(w[0]==0x47423031 && w[1]==640 && w[2]==480 && w[3]==640 &&
             w[4]==1 && w[5]==1 && w[6]==0xb00 && w[10]==2 && w[11]==24 &&
             w[13]==0x676d736d && w[14]==0x00100288 &&
             w[15]==640 && w[16]==480 && w[17]==640 && w[18]==480 &&
             w[19]==1 && w[20]==0 && w[21]==1 && w[24]==0xb00 &&
             w[25]==0 && w[26]==1228800 && w[27]==0 && w[28]==0,
             "AHB differs from tested linear-layout candidate; stop");
        struct stat st{};
        need(fstat(s.fds[0],&st)==0 && st.st_size==1228800,"unexpected pixel allocation size");
        std::puts("Layout hypothesis: RGBA8, linear modifier=0, offset=0, rowPitch=2560");

        constexpr VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
        constexpr auto handle=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkDrmFormatModifierPropertiesListEXT list{};
        list.sType=VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 fp{}; fp.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2; fp.pNext=&list;
        vkGetPhysicalDeviceFormatProperties2(gpu,format,&fp);
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties=modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(gpu,format,&fp);
        bool linear=false;
        for(auto& m:modifiers) if(m.drmFormatModifier==0 && m.drmFormatModifierPlaneCount==1 &&
            (m.drmFormatModifierTilingFeatures&VK_FORMAT_FEATURE_TRANSFER_DST_BIT)) linear=true;
        need(linear,"linear RGBA8 transfer destination not supported");
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modQuery{};
        modQuery.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT;
        modQuery.drmFormatModifier=0; modQuery.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo externalQuery{};
        externalQuery.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
        externalQuery.handleType=handle; externalQuery.pNext=&modQuery;
        VkPhysicalDeviceImageFormatInfo2 query{};
        query.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
        query.pNext=&externalQuery; query.format=format; query.type=VK_IMAGE_TYPE_2D;
        query.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT; query.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties externalProps{};
        externalProps.sType=VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
        VkImageFormatProperties2 props{}; props.sType=VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        props.pNext=&externalProps;
        check(vkGetPhysicalDeviceImageFormatProperties2(gpu,&query,&props),"query linear DMA-BUF import");
        need(externalProps.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT,
             "image not importable");
        need(props.imageFormatProperties.maxExtent.width>=640 && props.imageFormatProperties.maxExtent.height>=480,
             "image extent unsupported");

        VkSubresourceLayout layout{}; layout.offset=0; layout.rowPitch=2560;
        VkImageDrmFormatModifierExplicitCreateInfoEXT explicitMod{};
        explicitMod.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        explicitMod.drmFormatModifier=0; explicitMod.drmFormatModifierPlaneCount=1; explicitMod.pPlaneLayouts=&layout;
        VkExternalMemoryImageCreateInfo externalImage{};
        externalImage.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImage.handleTypes=handle; externalImage.pNext=&explicitMod;
        VkImageCreateInfo imageInfo{}; imageInfo.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext=&externalImage; imageInfo.imageType=VK_IMAGE_TYPE_2D; imageInfo.format=format;
        imageInfo.extent={640,480,1}; imageInfo.mipLevels=1; imageInfo.arrayLayers=1;
        imageInfo.samples=VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imageInfo.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT; imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(s.device,&imageInfo,nullptr,&s.image),"create image with explicit pitch");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(s.device,s.image,&req);
        std::printf("Memory requirements: size=%llu alignment=%llu bits=0x%x\n",
            (unsigned long long)req.size,(unsigned long long)req.alignment,req.memoryTypeBits);
        need(req.size<=static_cast<VkDeviceSize>(st.st_size),"allocation too small for image");
        auto getFdProps=reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(s.device,"vkGetMemoryFdPropertiesKHR"));
        need(getFdProps!=nullptr,"missing vkGetMemoryFdPropertiesKHR");
        VkMemoryFdPropertiesKHR fdProps{}; fdProps.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        check(getFdProps(s.device,handle,s.fds[0],&fdProps),"query DMA-BUF memory types");
        uint32_t bits=req.memoryTypeBits&fdProps.memoryTypeBits;
        need(bits!=0,"no compatible memory type");
        uint32_t memoryType=0; while(!(bits&(1u<<memoryType))) ++memoryType;
        s.importFd=fcntl(s.fds[0],F_DUPFD_CLOEXEC,0); need(s.importFd>=0,"dup failed");
        VkMemoryDedicatedAllocateInfo dedicated{}; dedicated.sType=VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image=s.image;
        VkImportMemoryFdInfoKHR import{}; import.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        import.pNext=&dedicated; import.handleType=handle; import.fd=s.importFd;
        VkMemoryAllocateInfo alloc{}; alloc.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.pNext=&import; alloc.allocationSize=req.size; alloc.memoryTypeIndex=memoryType;
        check(vkAllocateMemory(s.device,&alloc,nullptr,&s.memory),"import DMA-BUF memory");
        s.importFd=-1; // Vulkan now owns the duplicated descriptor.
        check(vkBindImageMemory(s.device,s.image,s.memory,0),"bind image memory");

        VkCommandPoolCreateInfo pci{}; pci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pci.queueFamilyIndex=family;
        check(vkCreateCommandPool(s.device,&pci,nullptr,&s.pool),"create command pool");
        VkCommandBufferAllocateInfo cai{}; cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool=s.pool; cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount=1;
        VkCommandBuffer cmd;
        check(vkAllocateCommandBuffers(s.device,&cai,&cmd),"allocate command buffer");
        VkCommandBufferBeginInfo begin{}; begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd,&begin),"begin commands");
        VkImageMemoryBarrier barrier{}; barrier.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT; barrier.dstQueueFamilyIndex=family;
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.image=s.image;
        barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        // Newly allocated, never-used AHB: no outstanding producer work.
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,
                             0,nullptr,0,nullptr,1,&barrier);
        VkClearColorValue color{}; color.float32[0]=1; color.float32[1]=0; color.float32[2]=1; color.float32[3]=1;
        vkCmdClearColorImage(cmd,s.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&color,1,&barrier.subresourceRange);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex=family; barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,
                             0,nullptr,0,nullptr,1,&barrier);
        check(vkEndCommandBuffer(cmd),"end commands");
        VkFenceCreateInfo fci{}; fci.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(s.device,&fci,nullptr,&s.fence),"create fence");
        VkSubmitInfo submit{}; submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
        check(vkQueueSubmit(queue,1,&submit,s.fence),"submit GPU clear");
        check(vkWaitForFences(s.device,1,&s.fence,VK_TRUE,10000000000ULL),"wait GPU completion");
        need(send(s.sock,"K",1,MSG_NOSIGNAL)==1,"Android ACK failed (increase its receive timeout)");
        std::puts("PASS: DMA-BUF imported, image bound, GPU clear completed. Pixels not yet visually verified.");
        return 0;
    } catch(const std::exception& e) {
        std::fprintf(stderr,"STOP: %s\n",e.what());
        if(s.sock>=0) send(s.sock,"E",1,MSG_NOSIGNAL);
        return 1;
    }
}
