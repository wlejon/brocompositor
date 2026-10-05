#include "vk_mac.h"

#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <cstring>

using namespace brocompositor;

namespace bctest {

VkMac::~VkMac() {
    if (device_) {
        dev<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device_);
        if (pool_) dev<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(device_, pool_, nullptr);
        dev<PFN_vkDestroyDevice>("vkDestroyDevice")(device_, nullptr);
    }
    if (instance_) inst<PFN_vkDestroyInstance>("vkDestroyInstance")(instance_, nullptr);
    if (loader_) dlclose(loader_);
}

bool VkMac::init(const AdapterId* want, std::string* why) {
    // Homebrew's loader finds MoltenVK through this ICD file; point the
    // (process-local) environment at it when nothing else is configured.
    const char* icd = "/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json";
    struct stat st{};
    if (!std::getenv("VK_ICD_FILENAMES") && !std::getenv("VK_DRIVER_FILES") && stat(icd, &st) == 0)
        setenv("VK_ICD_FILENAMES", icd, 0);
    for (const char* name : {"libvulkan.1.dylib", "/opt/homebrew/lib/libvulkan.1.dylib",
                             "/usr/local/lib/libvulkan.1.dylib", "libMoltenVK.dylib",
                             "/opt/homebrew/lib/libMoltenVK.dylib"}) {
        loader_ = dlopen(name, RTLD_NOW | RTLD_LOCAL);
        if (loader_) break;
    }
    if (!loader_) {
        *why = "no Vulkan loader or MoltenVK (brew install vulkan-loader molten-vk)";
        return false;
    }
    gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader_, "vkGetInstanceProcAddr"));
    auto create = gipa_ ? reinterpret_cast<PFN_vkCreateInstance>(gipa_(nullptr, "vkCreateInstance")) : nullptr;
    if (!create) {
        *why = "no vkCreateInstance";
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "brocompositor-tests";
    app.apiVersion = VK_API_VERSION_1_2;
    const char* exts[] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME,
                          VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = exts;
    if (create(&ici, nullptr, &instance_) != VK_SUCCESS) {
        *why = "vkCreateInstance failed (no MoltenVK ICD?)";
        return false;
    }
    uint32_t n = 0;
    auto enumerate = inst<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    enumerate(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    enumerate(instance_, &n, pds.data());
    auto ext_props = inst<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    std::vector<const char*> need = vk::required_device_extensions();
    need.push_back("VK_KHR_portability_subset");
    for (VkPhysicalDevice pd : pds) {
        auto id = vk::adapter_of(pd, gipa_, instance_);
        if (want && (!id || id->metal_registry_id != want->metal_registry_id)) continue;
        uint32_t en = 0;
        ext_props(pd, nullptr, &en, nullptr);
        std::vector<VkExtensionProperties> have(en);
        ext_props(pd, nullptr, &en, have.data());
        bool all = true;
        for (const char* e : need) {
            bool found = false;
            for (auto& h : have) found = found || std::strcmp(h.extensionName, e) == 0;
            all = all && found;
        }
        if (!all) continue;
        physical_ = pd;
        if (id) adapter_ = *id;
        break;
    }
    if (!physical_) {
        *why = "no Vulkan device with VK_EXT_metal_objects on the requested adapter";
        return false;
    }
    inst<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties")(physical_, &memory_);
    uint32_t qn = 0;
    auto qprops = inst<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    qprops(physical_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    qprops(physical_, &qn, qf.data());
    for (uint32_t i = 0; i < qn; ++i)
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) {
            family_ = i;
            break;
        }
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(need.size());
    dci.ppEnabledExtensionNames = need.data();
    if (inst<PFN_vkCreateDevice>("vkCreateDevice")(physical_, &dci, nullptr, &device_) != VK_SUCCESS) {
        *why = "vkCreateDevice failed";
        return false;
    }
    gdpa_ = inst<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    dev<PFN_vkGetDeviceQueue>("vkGetDeviceQueue")(device_, family_, 0, &queue_);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    if (dev<PFN_vkCreateCommandPool>("vkCreateCommandPool")(device_, &pci, nullptr, &pool_) != VK_SUCCESS) {
        *why = "vkCreateCommandPool failed";
        return false;
    }
    return true;
}

vk::DeviceContext VkMac::context() const { return vk::DeviceContext{instance_, physical_, device_, gipa_}; }

std::optional<std::vector<uint8_t>> VkMac::read(const vk::Importer& importer, const vk::ImportedImage& image,
                                                VkSemaphore timeline, uint64_t value, std::string* why) {
    VkDeviceSize size = VkDeviceSize(image.extent.width) * image.extent.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (dev<PFN_vkCreateBuffer>("vkCreateBuffer")(device_, &bci, nullptr, &buffer) != VK_SUCCESS) {
        *why = "vkCreateBuffer";
        return std::nullopt;
    }
    VkMemoryRequirements req{};
    dev<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(device_, buffer, &req);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i) {
        auto f = memory_.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            type = i;
            break;
        }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    auto destroy_buffer = [&] {
        dev<PFN_vkDestroyBuffer>("vkDestroyBuffer")(device_, buffer, nullptr);
        if (memory) dev<PFN_vkFreeMemory>("vkFreeMemory")(device_, memory, nullptr);
    };
    if (type == UINT32_MAX || dev<PFN_vkAllocateMemory>("vkAllocateMemory")(device_, &mai, nullptr, &memory) != VK_SUCCESS ||
        dev<PFN_vkBindBufferMemory>("vkBindBufferMemory")(device_, buffer, memory, 0) != VK_SUCCESS) {
        destroy_buffer();
        *why = "host-visible buffer";
        return std::nullopt;
    }

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    dev<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(device_, &cai, &cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dev<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(cmd, &begin);
    importer.cmd_acquire(cmd, image, family_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.extent.width, image.extent.height, 1};
    dev<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                                             buffer, 1, &region);
    importer.cmd_release(cmd, image, family_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
    dev<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(cmd);

    VkTimelineSemaphoreSubmitInfo tsi{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    tsi.waitSemaphoreValueCount = 1;
    tsi.pWaitSemaphoreValues = &value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &timeline;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    VkResult r = dev<PFN_vkQueueSubmit>("vkQueueSubmit")(queue_, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) r = dev<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(queue_);
    dev<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers")(device_, pool_, 1, &cmd);
    if (r != VK_SUCCESS) {
        destroy_buffer();
        *why = "submit/wait failed: " + std::to_string(int(r));
        return std::nullopt;
    }
    void* p = nullptr;
    dev<PFN_vkMapMemory>("vkMapMemory")(device_, memory, 0, VK_WHOLE_SIZE, 0, &p);
    std::vector<uint8_t> out(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
    dev<PFN_vkUnmapMemory>("vkUnmapMemory")(device_, memory);
    destroy_buffer();
    return out;
}

}  // namespace bctest
