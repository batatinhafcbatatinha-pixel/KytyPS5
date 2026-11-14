#include "graphics/host_gpu/renderer/masterSemaphore.h"
#include "common/fpsProbe.h" // FPSPROBE
#include "graphics/guest_gpu/graphicsRun.h" // FPSPROBE

#include "common/assert.h"
#include "common/debugCounters.h"
#include "graphics/host_gpu/graphicContext.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#if defined(_WIN32)
#include <windows.h>
#endif

namespace Libs::Graphics {

// PROBE (temporary): GPU-thread waits by caller stack, weighted by wait time (KYTY_PROFILE_GPU_WAITS).
static void ProbeGpuWait(double ms) {
#if defined(_WIN32)
	static const bool enabled = std::getenv("KYTY_PROFILE_GPU_WAITS") != nullptr;
	if (!enabled || !GuestGpu::IsGpuThread()) return;
	static std::map<std::vector<uint64_t>, double> stacks;
	static auto                                    last = std::chrono::steady_clock::now();
	static const auto base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
	void*             frames[14] {};
	const auto        count = RtlCaptureStackBackTrace(1, 14, frames, nullptr);
	std::vector<uint64_t> key;
	for (USHORT i = 0; i < count; i++) key.push_back(reinterpret_cast<uint64_t>(frames[i]) - base);
	stacks[key] += ms;
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - last).count() < 5.0) return;
	std::vector<std::pair<double, const std::vector<uint64_t>*>> top;
	for (const auto& [stack, total]: stacks) top.emplace_back(total, &stack);
	std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	for (size_t i = 0; i < std::min<size_t>(top.size(), 5); i++) {
		std::printf("[gpuwait] ms=%.0f rva", top[i].first);
		for (const auto rva: *top[i].second) std::printf(" 0x%llx", static_cast<unsigned long long>(rva));
		std::printf("\n");
	}
	std::fflush(stdout);
	stacks.clear();
	last = now;
#else
	(void)ms;
#endif
}

MasterSemaphore::MasterSemaphore(GraphicContext& graphics): m_graphics(graphics) {
	vk::SemaphoreTypeCreateInfo type_info {};
	type_info.semaphoreType = vk::SemaphoreType::eTimeline;
	type_info.initialValue  = 0;

	vk::SemaphoreCreateInfo create_info {};
	create_info.pNext = &type_info;

	const auto result = m_graphics.device.createSemaphore(&create_info, nullptr, &m_semaphore);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess || m_semaphore == nullptr);
}

MasterSemaphore::~MasterSemaphore() {
	m_graphics.device.destroySemaphore(m_semaphore, nullptr);
}

void MasterSemaphore::Refresh() {
	uint64_t   counter = 0;
	const auto result  = m_graphics.device.getSemaphoreCounterValue(m_semaphore, &counter);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);

	auto known = m_gpu_tick.load(std::memory_order_acquire);
	while (known < counter &&
	       !m_gpu_tick.compare_exchange_weak(known, counter, std::memory_order_release,
	                                         std::memory_order_relaxed)) {
	}
}

void MasterSemaphore::Wait(uint64_t tick) {
	FpsProbe::Scope fps_scope(GuestGpu::IsGpuThread() ? FpsProbe::WaitGpu : FpsProbe::WaitOther); // FPSPROBE
	if (IsFree(tick)) {
		return;
	}
	Refresh();
	if (IsFree(tick)) {
		return;
	}

	vk::SemaphoreWaitInfo wait_info {};
	wait_info.semaphoreCount = 1;
	wait_info.pSemaphores    = &m_semaphore;
	wait_info.pValues        = &tick;

	Common::DebugCounters::ScopedTimer timer(Common::DebugCounters::Counter::GpuWaits,
	                                         Common::DebugCounters::Counter::GpuWaitNs);
	const auto probe_start = std::chrono::steady_clock::now(); // PROBE
	const auto result = m_graphics.device.waitSemaphores(&wait_info, UINT64_MAX);
	EXIT_NOT_IMPLEMENTED(result != vk::Result::eSuccess);
	ProbeGpuWait(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probe_start).count()); // PROBE
	Refresh();
}

} // namespace Libs::Graphics
