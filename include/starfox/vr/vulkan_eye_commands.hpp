#pragma once
#include "starfox/vr/vulkan_eye_targets.hpp"
#include <functional>
namespace starfox::vr {
// One in-flight eye submission. Caller serializes access to the shared queue.
// Never release an XR image until poll() returns complete. Errors require
// device/session teardown, not reuse of an image with uncertain completion.
class VulkanEyeCommands {
public:
    enum class Completion {complete,pending,error};
    using Record=std::function<void(VkCommandBuffer,VkExtent2D)>;
    struct TimelineWait {VkSemaphore semaphore{};uint64_t value{};VkPipelineStageFlags stage{VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT};};
    VulkanEyeCommands()=default;
    ~VulkanEyeCommands();
    VulkanEyeCommands(const VulkanEyeCommands&)=delete;
    VulkanEyeCommands& operator=(const VulkanEyeCommands&)=delete;
    bool initialize(VkDevice,VkQueue,uint32_t queue_family,PFN_vkGetDeviceProcAddr);
    // Standalone producer work, without a render pass or XR image. Poll before
    // handing geometry to another API or reusing this command allocation.
    bool submit_work(VkExtent2D,const Record&,const TimelineWait* wait=nullptr);
    bool submit(const VulkanEyeTargets&,unsigned eye,unsigned image,
        const VkClearColorValue&,const Record& record={},const Record& before_render={},
        const Record& after_render={},const TimelineWait* wait=nullptr);
    // Optional wait requires a timeline-enabled device. Keep semaphore alive
    // until completion. after_render runs outside the pass for ownership release.
    // before_render runs after command begin, outside the render pass. It
    // owns compute-to-graphics barriers; both callbacks share one submission.
    // Optional bounded wait wakes on completion instead of a fixed host sleep.
    Completion poll(uint64_t timeout_ns=0);
    void close() noexcept;
    const std::string& status() const noexcept {return status_;}
private:
    VkDevice device_{};VkQueue queue_{};VkCommandPool pool_{};
    VkCommandBuffer command_{};VkFence fence_{};
    bool pending_{},failed_{};
    PFN_vkDestroyCommandPool destroy_pool_{};
    PFN_vkDestroyFence destroy_fence_{};
    PFN_vkQueueWaitIdle idle_{};
    PFN_vkResetCommandPool reset_pool_{};
    PFN_vkResetFences reset_fences_{};
    PFN_vkBeginCommandBuffer begin_{};
    PFN_vkEndCommandBuffer end_{};
    PFN_vkCmdBeginRenderPass begin_pass_{};
    PFN_vkCmdEndRenderPass end_pass_{};
    PFN_vkQueueSubmit submit_{};
    PFN_vkGetFenceStatus fence_status_{};
    PFN_vkWaitForFences wait_fences_{};
    std::string status_{"Eye commands not initialized"};
};
}
