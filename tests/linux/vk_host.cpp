#include "linux/vk_host.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstring>

using namespace brocompositor;

namespace bctest {

namespace {

bool has_extensions(const VkHostFns& f, VkPhysicalDevice pd, const std::vector<const char*>& want) {
    uint32_t n = 0;
    f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
    for (const char* w : want) {
        bool found = false;
        for (auto& e : exts)
            if (std::strcmp(e.extensionName, w) == 0) found = true;
        if (!found) return false;
    }
    return true;
}

}  // namespace

VkHost::~VkHost() {
    close_device();
    if (instance_) f_.vkDestroyInstance(instance_, nullptr);
    if (loader_) dlclose(loader_);
}

void VkHost::close_device() {
    if (!device_) return;
    f_.vkDeviceWaitIdle(device_);
    if (fence_) f_.vkDestroyFence(device_, fence_, nullptr);
    if (pool_) f_.vkDestroyCommandPool(device_, pool_, nullptr);
    f_.vkDestroyDevice(device_, nullptr);
    device_ = VK_NULL_HANDLE;
    fence_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
}

bool VkHost::load(std::string* why) {
    loader_ = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader_) {
        *why = "libvulkan.so.1 not found";
        return false;
    }
    gipa_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader_, "vkGetInstanceProcAddr"));
    auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa_(nullptr, "vkCreateInstance"));
    if (!create) {
        *why = "no vkCreateInstance";
        return false;
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "brocompositor-tests";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (create(&ici, nullptr, &instance_) != VK_SUCCESS) {
        *why = "vkCreateInstance failed";
        return false;
    }
#define BC_LOAD(name) f_.name = reinterpret_cast<PFN_##name>(gipa_(instance_, #name));
    BC_VKH_FNS(BC_LOAD)
#undef BC_LOAD
    uint32_t n = 0;
    f_.vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    f_.vkEnumeratePhysicalDevices(instance_, &n, pds.data());
    auto want = vk::required_device_extensions();
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p{};
        f_.vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_2 || !has_extensions(f_, pds[i], want)) continue;
        VkHostDevice d;
        d.index = i;
        d.name = p.deviceName;
        d.cpu = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        d.adapter = vk::adapter_of(pds[i], gipa_, instance_);
        devices_.push_back(d);
        physical_.push_back(pds[i]);
    }
    if (devices_.empty()) {
        *why = "no Vulkan 1.2 device with the dmabuf / sync_fd extensions";
        return false;
    }
    return true;
}

bool VkHost::open(size_t i, std::string* why) {
    close_device();
    open_ = i;
    VkPhysicalDevice pd = physical_[i];
    uint32_t qn = 0;
    f_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    f_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf.data());
    family_ = UINT32_MAX;
    for (uint32_t q = 0; q < qn && family_ == UINT32_MAX; ++q)
        if (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) family_ = q;
    if (family_ == UINT32_MAX) {
        *why = "no graphics queue";
        return false;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    auto exts = vk::required_device_extensions();
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = uint32_t(exts.size());
    dci.ppEnabledExtensionNames = exts.data();
    if (f_.vkCreateDevice(pd, &dci, nullptr, &device_) != VK_SUCCESS) {
        *why = "vkCreateDevice failed";
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

vk::DeviceContext VkHost::device_context() const {
    return vk::DeviceContext{instance_, physical_[open_], device_, gipa_};
}

bool VkHost::submit(VkSemaphore wait, VkSemaphore signal, std::string* why) {
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = wait ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = signal ? 1 : 0;
    si.pSignalSemaphores = &signal;
    f_.vkResetFences(device_, 1, &fence_);
    if (f_.vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
        *why = "vkQueueSubmit failed";
        return false;
    }
    if (f_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5000000000ull) != VK_SUCCESS) {
        *why = "fence wait timed out";
        return false;
    }
    return true;
}

std::vector<uint8_t> VkHost::read_back(vk::Importer& importer, const vk::ImportedImage& image, NativeHandle sync_fd,
                                       std::string* why) {
    const VkDeviceSize size = VkDeviceSize(image.extent.width) * image.extent.height * 4;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    f_.vkCreateBuffer(device_, &bci, nullptr, &buf);
    VkMemoryRequirements req{};
    f_.vkGetBufferMemoryRequirements(device_, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    f_.vkGetPhysicalDeviceMemoryProperties(physical_[open_], &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && type == UINT32_MAX; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            type = i;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    std::vector<uint8_t> out;
    VkSemaphore wait = VK_NULL_HANDLE;
    if (type == UINT32_MAX || f_.vkAllocateMemory(device_, &mai, nullptr, &mem) != VK_SUCCESS) {
        *why = "no host-visible memory";
    } else {
        f_.vkBindBufferMemory(device_, buf, mem, 0);
        wait = importer.import_sync_file(sync_fd, why);
        if (wait) {
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            f_.vkResetCommandBuffer(cmd_, 0);
            f_.vkBeginCommandBuffer(cmd_, &begin);
            importer.cmd_acquire(cmd_, image, family_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {image.extent.width, image.extent.height, 1};
            f_.vkCmdCopyImageToBuffer(cmd_, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
            importer.cmd_release(cmd_, image, family_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
            f_.vkEndCommandBuffer(cmd_);
            if (submit(wait, VK_NULL_HANDLE, why)) {
                void* p = nullptr;
                f_.vkMapMemory(device_, mem, 0, size, 0, &p);
                out.assign(static_cast<uint8_t*>(p), static_cast<uint8_t*>(p) + size);
                f_.vkUnmapMemory(device_, mem);
            }
        }
    }
    importer.destroy(wait);
    f_.vkDestroyBuffer(device_, buf, nullptr);
    if (mem) f_.vkFreeMemory(device_, mem, nullptr);
    return out;
}

std::optional<NativeHandle> VkHost::clear(vk::Importer& importer, const vk::ImportedImage& image, const float rgba[4],
                                          std::string* why) {
    VkSemaphore done = importer.create_exportable_semaphore(why);
    if (!done) return std::nullopt;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    f_.vkResetCommandBuffer(cmd_, 0);
    f_.vkBeginCommandBuffer(cmd_, &begin);
    importer.cmd_acquire(cmd_, image, family_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
    VkClearColorValue color{};
    std::memcpy(color.float32, rgba, sizeof color.float32);
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f_.vkCmdClearColorImage(cmd_, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
    importer.cmd_release(cmd_, image, family_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
    f_.vkEndCommandBuffer(cmd_);
    // Export before waiting the fence: SYNC_FD export needs a pending signal.
    VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd_;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &done;
    f_.vkResetFences(device_, 1, &fence_);
    if (f_.vkQueueSubmit(queue_, 1, &si, fence_) != VK_SUCCESS) {
        importer.destroy(done);
        *why = "vkQueueSubmit failed";
        return std::nullopt;
    }
    std::string err;
    NativeHandle fd = importer.export_sync_file(done, &err);
    f_.vkWaitForFences(device_, 1, &fence_, VK_TRUE, 5000000000ull);
    importer.destroy(done);
    if (!err.empty()) {
        *why = err;
        return std::nullopt;
    }
    return fd;
}

}  // namespace bctest
