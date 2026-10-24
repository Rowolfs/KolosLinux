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
void initialize(Context& v){
    VkApplicationInfo app{}; app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName="Kolos AHB import test"; app.apiVersion=VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{}; ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo=&app;
        check(vkCreateInstance(&ici,nullptr,&v.instance),"create instance");
        uint32_t count=0;
        check(vkEnumeratePhysicalDevices(v.instance,&count,nullptr),"GPU count");
        need(count>0,"no GPU");
        std::vector<VkPhysicalDevice> gpus(count);
        check(vkEnumeratePhysicalDevices(v.instance,&count,gpus.data()),"enumerate GPUs");
        v.gpu=VK_NULL_HANDLE;
        auto& gpu=v.gpu;
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
        const char* diagnostic[]={"VK_KHR_external_memory_fd",
                                  "VK_EXT_external_memory_dma_buf",
                                  "VK_EXT_image_drm_format_modifier",
                                  "VK_EXT_queue_family_foreign",
                                  "VK_ANDROID_external_memory_android_hardware_buffer"};
        for(auto name:diagnostic) {
            bool found=false;
            for(uint32_t i=0;i<ec;i++) if(strcmp(name,exts[i].extensionName)==0) found=true;
            std::printf("Vulkan device extension %s: %s\n",name,found?"available":"absent");
        }
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
        v.family=UINT32_MAX;
        auto& family=v.family;
        for(uint32_t i=0;i<qc;i++) if(queues[i].queueCount && (queues[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) {family=i;break;}
        need(family!=UINT32_MAX,"no graphics queue");
        float priority=1;
        VkDeviceQueueCreateInfo qci{}; qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex=family; qci.queueCount=1; qci.pQueuePriorities=&priority;
        VkDeviceCreateInfo dci{}; dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount=1; dci.pQueueCreateInfos=&qci;
        dci.enabledExtensionCount=4; dci.ppEnabledExtensionNames=wanted;
        check(vkCreateDevice(gpu,&dci,nullptr,&v.device),"create device");
        vkGetDeviceQueue(v.device,family,0,&v.queue);


    VkCommandPoolCreateInfo pci{};pci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pci.queueFamilyIndex=v.family;
    check(vkCreateCommandPool(v.device,&pci,nullptr,&v.pool),"create pool");
    VkCommandBufferAllocateInfo ai{};ai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool=v.pool;ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ai.commandBufferCount=1;
    check(vkAllocateCommandBuffers(v.device,&ai,&v.cmd),"allocate commands");
    VkFenceCreateInfo fi{};fi.sType=VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    check(vkCreateFence(v.device,&fi,nullptr,&v.fence),"create fence");
}
}
