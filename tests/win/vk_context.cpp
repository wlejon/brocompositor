#include <windows.h>

#include "win/vk_context.h"

#include <cstring>

namespace bctest {

namespace {
std::string vkerr(const char* what, VkResult r) { return std::string(what) + ": VkResult " + std::to_string(int(r)); }
}  // namespace

VkContext::~VkContext() {
    if (device_) {
        if (fence_) f_.vkDestroyFence(device_, fence_, nullptr);
        if (pool_) f_.vkDestroyCommandPool(device_, pool_, nullptr);
        f_.vkDestroyDevice(device_, nullptr);
    }
    if (instance_) f_.vkDestroyInstance(instance_, nullptr);
    if (loader_) FreeLibrary(static_cast<HMODULE>(loader_));
}

bool VkContext::init(std::string* why) {
    HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
    if (!lib) {
        *why = "vulkan-1.dll not found";
        return false;
    }
    loader_ = lib;
    gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
    auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(gipa_(nullptr, "vkCreateInstance"));
    if (!gipa_ || !create_instance) {
        *why = "no vkGetInstanceProcAddr";
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "brocompositor-test";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkResult r = create_instance(&ici, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        *why = vkerr("vkCreateInstance", r);
        return false;
    }
#define BC_LOAD(name) f_.name = reinterpret_cast<PFN_##name>(gipa_(instance_, #name));
    BC_VKT_FNS(BC_LOAD)
#undef BC_LOAD

    uint32_t n = 0;
    f_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    f_.vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    auto needed = brocompositor::vk::required_device_extensions();
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties props{};
        f_.vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_2) continue;
        auto luid = brocompositor::vk::adapter_of(pd, gipa_, instance_);
        if (!luid) continue;
        uint32_t ne = 0;
        f_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        f_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
        bool all = true;
        for (const char* e : needed) {
            bool found = false;
            for (auto& x : exts) found |= std::strcmp(x.extensionName, e) == 0;
            all &= found;
        }
        if (!all) continue;
        uint32_t nq = 0;
        f_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        f_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qs.data());
        for (uint32_t i = 0; i < nq; ++i) {
            if (qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                physical_ = pd;
                family_ = i;
                adapter_ = *luid;
                device_name_ = props.deviceName;
                break;
            }
        }
        if (physical_) break;
    }
    if (!physical_) {
        *why = "no Vulkan 1.2 device with a LUID and the Win32 external memory/semaphore extensions";
        return false;
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(needed.size());
    dci.ppEnabledExtensionNames = needed.data();
    r = f_.vkCreateDevice(physical_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) {
        *why = vkerr("vkCreateDevice", r);
        return false;
    }
    f_.vkGetDeviceQueue(device_, family_, 0, &queue_);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = family_;
    f_.vkCreateCommandPool(device_, &pci, nullptr, &pool_);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    f_.vkAllocateCommandBuffers(device_, &cai, &cmd_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    f_.vkCreateFence(device_, &fci, nullptr, &fence_);
    return true;
}

brocompositor::vk::DeviceContext VkContext::device_context() const {
    return {instance_, physical_, device_, gipa_};
}

std::vector<uint8_t> VkContext::read_back(brocompositor::vk::Importer& importer,
                                          const brocompositor::vk::ImportedImage& image,
                                          const brocompositor::vk::ImportedTimeline& timeline, uint64_t wait_value,
                                          std::string* why) {
    VkDeviceSize size = VkDeviceSize(image.extent.width) * image.extent.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (f_.vkCreateBuffer(device_, &bci, nullptr, &buf) != VK_SUCCESS) return {};
    VkMemoryRequirements req{};
    f_.vkGetBufferMemoryRequirements(device_, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    f_.vkGetPhysicalDeviceMemoryProperties(physical_, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    f_.vkAllocateMemory(device_, &mai, nullptr, &mem);
    f_.vkBindBufferMemory(device_, buf, mem, 0);

    f_.vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f_.vkBeginCommandBuffer(cmd_, &begin);
    importer.cmd_acquire(cmd_, image, family_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {image.extent.width, image.extent.height, 1};
    f_.vkCmdCopyImageToBuffer(cmd_, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    importer.cmd_release(cmd_, image, family_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_READ_BIT);
    f_.vkEndCommandBuffer(cmd_);

    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = 1;
    ts.pWaitSemaphoreValues = &wait_value;
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &timeline.semaphore;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    std::vector<uint8_t> out;
    VkResult r = f_.vkQueueSubmit(queue_, 1, &si, fence_);
    if (r == VK_SUCCESS) r = f_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5'000'000'000ull);
    if (r == VK_SUCCESS) {
        void* p = nullptr;
        f_.vkMapMemory(device_, mem, 0, size, 0, &p);
        out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
        f_.vkUnmapMemory(device_, mem);
    } else if (why) {
        *why = vkerr("submit/wait", r);
    }
    f_.vkResetFences(device_, 1, &fence_);
    f_.vkDestroyBuffer(device_, buf, nullptr);
    f_.vkFreeMemory(device_, mem, nullptr);
    return out;
}

}  // namespace bctest
