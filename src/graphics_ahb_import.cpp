#include <kolos/ahb_import.h>
#include <kolos_stream_wire.h>
#include <cstdio>
#include <sys/stat.h>
#include <fcntl.h>
#include <cmath>
#include <cerrno>
#include <cstring>
namespace kolos {
static void need(bool ok,const char* text){kw::require(ok,text);}
static void check(VkResult r,const char* text){
    if(r!=VK_SUCCESS)throw std::runtime_error(std::string(text)+": VkResult="+std::to_string(r));
}
static std::vector<unsigned char> receive(Imported& s,int socketFd){
    s.sock=socketFd;
    std::vector<unsigned char> bytes;
    for(;;){
        kw::wait(s.sock,POLLIN);
        unsigned char data[4096];alignas(cmsghdr) unsigned char control[CMSG_SPACE(64*sizeof(int))]{};
        iovec iov{data,sizeof(data)};msghdr m{};m.msg_iov=&iov;m.msg_iovlen=1;
        m.msg_control=control;m.msg_controllen=sizeof(control);
        ssize_t n=recvmsg(s.sock,&m,MSG_CMSG_CLOEXEC|MSG_DONTWAIT);
        if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK))continue;
        need(n>=0,"AHB receive failed");
        for(auto* c=CMSG_FIRSTHDR(&m);c;c=CMSG_NXTHDR(&m,c)){
            if(c->cmsg_level==SOL_SOCKET&&c->cmsg_type==SCM_RIGHTS){
                need(c->cmsg_len>=CMSG_LEN(0),"bad ancillary header");size_t length=c->cmsg_len-CMSG_LEN(0);
                for(size_t off=0;off+sizeof(int)<=length;off+=sizeof(int)){
                    int fd;memcpy(&fd,CMSG_DATA(c)+off,sizeof(fd));s.fds.push_back(fd);
                }
            }
        }
        need(!(m.msg_flags&(MSG_CTRUNC|MSG_TRUNC)),"truncated AHB transfer");
        if(n==0)break;bytes.insert(bytes.end(),data,data+n);need(bytes.size()<=4096,"oversized handle");
    }return bytes;
}
void importGraphicsBuffer(Context& v,Imported& s,int socketFd,uint32_t width,
                          uint32_t height,uint32_t stride,uint32_t ahbFormat,uint32_t index){
    s.device=v.device;auto gpu=v.gpu;
    std::printf("AHB slot=%u metadata width=%u height=%u stride=%u format=%u usage=unavailable-in-graphics-v1\n",
                index,width,height,stride,ahbFormat);
    need(width>0 && height>0 && stride>=width && ahbFormat==1,"unsupported AHB metadata");
    auto bytes=receive(s,socketFd);
    std::printf("Received %zu bytes, %zu FDs\n",bytes.size(),s.fds.size());
        need(bytes.size()==148 && s.fds.size()==2,"unexpected AHB wire layout; stop");
        uint32_t w[37]; memcpy(w,bytes.data(),sizeof(w));
        for(uint32_t i=0;i<37;i++)
            std::printf("AHB handle w[%02u]=0x%08x (%u)\n",i,w[i],w[i]);
        for(size_t i=0;i<s.fds.size();i++) {
            struct stat fdStat{};
            if(fstat(s.fds[i],&fdStat)==0)
                std::printf("AHB fd[%zu] size=%lld mode=0%o\n",i,
                            static_cast<long long>(fdStat.st_size),unsigned(fdStat.st_mode));
            else
                std::printf("AHB fd[%zu] fstat failed: errno=%d (%s)\n",i,errno,strerror(errno));
        }
        // This is still a device-specific layout hypothesis. The observed
        // allocation has stride-sized rows and may have more rows than the
        // visible image. Require the handle, AHB metadata and pixel FD size
        // to agree before attempting the known linear Vulkan import path.
        need(stride<=UINT32_MAX/4,"unsupported AHB extent");
        const uint64_t minimumSize=uint64_t(stride)*4*height;
        need(minimumSize<=UINT32_MAX,"unsupported AHB extent");
        bool candidate=true;
        auto equal=[&](uint32_t index,uint32_t expected) {
            const bool match=w[index]==expected;
            std::printf("AHB candidate w[%02u]: %s actual=0x%08x expected=0x%08x\n",
                        index,match?"PASS":"FAIL",w[index],expected);
            candidate &= match;
        };
        equal(0,0x47423031);equal(1,width);equal(2,height);equal(3,stride);
        equal(4,1);equal(5,1);equal(6,0xb00);equal(10,2);equal(11,24);
        equal(13,0x676d736d);equal(14,0x00100288);
        equal(15,stride);
        const bool allocationRowsMatch=w[16]>=height;
        std::printf("AHB candidate w[16] allocation rows: %s actual=%u visibleHeight=%u\n",
                    allocationRowsMatch?"PASS":"FAIL",w[16],height);
        candidate &= allocationRowsMatch;
        equal(17,width);equal(18,height);
        equal(19,1);equal(20,0);equal(21,1);equal(24,0xb00);equal(25,0);
        const uint64_t allocationBytes=uint64_t(stride)*w[16]*4;
        const bool sizeMatch=allocationBytes<=UINT32_MAX && w[26]==allocationBytes;
        std::printf("AHB candidate w[26]: %s actual=%u stride*allocationRows*4=%llu minimum=%llu\n",
                    sizeMatch?"PASS":"FAIL",w[26],
                    static_cast<unsigned long long>(allocationBytes),
                    static_cast<unsigned long long>(minimumSize));
        candidate &= sizeMatch;
        equal(27,0);equal(28,0);

        constexpr VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
        constexpr auto handle=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkDrmFormatModifierPropertiesListEXT list{};
        list.sType=VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
        VkFormatProperties2 fp{}; fp.sType=VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2; fp.pNext=&list;
        vkGetPhysicalDeviceFormatProperties2(gpu,format,&fp);
        std::vector<VkDrmFormatModifierPropertiesEXT> modifiers(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties=modifiers.data();
        vkGetPhysicalDeviceFormatProperties2(gpu,format,&fp);
        std::printf("Vulkan RGBA8 DRM modifiers: %u\n",list.drmFormatModifierCount);
        for(const auto& m:modifiers)
            std::printf("  modifier=0x%016llx planes=%u tilingFeatures=0x%llx\n",
                        static_cast<unsigned long long>(m.drmFormatModifier),
                        m.drmFormatModifierPlaneCount,
                        static_cast<unsigned long long>(m.drmFormatModifierTilingFeatures));
        need(candidate,"AHB differs from tested linear-layout candidate; stop");
        struct stat st{};
        need(fstat(s.fds[0],&st)==0 && st.st_size>=static_cast<off_t>(minimumSize) &&
             uint64_t(st.st_size)==w[26],"unexpected pixel allocation size");
        std::printf("Validated observed Qualcomm handle family; hypothesis RGBA8 linear modifier=0 offset=0 pitch=%u. Untested sizes/layouts fail closed.\n",stride*4);

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
        need(props.imageFormatProperties.maxExtent.width>=width && props.imageFormatProperties.maxExtent.height>=height,
             "image extent unsupported");

        VkSubresourceLayout layout{}; layout.offset=0; layout.rowPitch=VkDeviceSize(stride)*4;
        VkImageDrmFormatModifierExplicitCreateInfoEXT explicitMod{};
        explicitMod.sType=VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT;
        explicitMod.drmFormatModifier=0; explicitMod.drmFormatModifierPlaneCount=1; explicitMod.pPlaneLayouts=&layout;
        VkExternalMemoryImageCreateInfo externalImage{};
        externalImage.sType=VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImage.handleTypes=handle; externalImage.pNext=&explicitMod;
        VkImageCreateInfo imageInfo{}; imageInfo.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext=&externalImage; imageInfo.imageType=VK_IMAGE_TYPE_2D; imageInfo.format=format;
        imageInfo.extent={width,height,1}; imageInfo.mipLevels=1; imageInfo.arrayLayers=1;
        imageInfo.samples=VK_SAMPLE_COUNT_1_BIT; imageInfo.tiling=VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        imageInfo.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT; imageInfo.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(v.device,&imageInfo,nullptr,&s.image),"create image with explicit pitch");
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(v.device,s.image,&req);
        std::printf("Memory requirements: size=%llu alignment=%llu bits=0x%x\n",
            (unsigned long long)req.size,(unsigned long long)req.alignment,req.memoryTypeBits);
        need(req.size<=static_cast<VkDeviceSize>(st.st_size),"allocation too small for image");
        auto getFdProps=reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(v.device,"vkGetMemoryFdPropertiesKHR"));
        need(getFdProps!=nullptr,"missing vkGetMemoryFdPropertiesKHR");
        VkMemoryFdPropertiesKHR fdProps{}; fdProps.sType=VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
        check(getFdProps(v.device,handle,s.fds[0],&fdProps),"query DMA-BUF memory types");
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
        check(vkAllocateMemory(v.device,&alloc,nullptr,&s.memory),"import DMA-BUF memory");
        s.importFd=-1; // Vulkan now owns the duplicated descriptor.
        check(vkBindImageMemory(v.device,s.image,s.memory,0),"bind image memory");


    std::printf("Slot %u: imported and bound\n",index);
}
}
