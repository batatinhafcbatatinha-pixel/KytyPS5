#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RECORDER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RECORDER_H_

#include "graphics/host_gpu/vulkanCommon.h"

// Vulkan commands of the GPU thread recorded on their own thread (KYTY_RECORDER=1 turns it on).
// Ported from AnyPS5 (glue/Recorder.cpp). See recorder.cpp.
namespace Libs::Graphics::Recorder {

// After VULKAN_HPP_DEFAULT_DISPATCHER.init(device), before anything else fetches device functions
// through the dispatcher (the memory allocator gets the waiting destroy and free functions).
void Install();

[[nodiscard]] bool Enabled() noexcept;

// Before code that records into `buffer` with its own Vulkan function pointers (upscaler SDKs,
// ImGui): every command queued so far has reached the real command buffer when this returns.
void Sync(VkCommandBuffer buffer);
inline void Sync(vk::CommandBuffer buffer) {
	Sync(static_cast<VkCommandBuffer>(buffer));
}

// Before code that destroys Vulkan objects with its own function pointers: waits until every
// queued command ran.
void SyncAll();

} // namespace Libs::Graphics::Recorder

#endif /* EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_RECORDER_H_ */
