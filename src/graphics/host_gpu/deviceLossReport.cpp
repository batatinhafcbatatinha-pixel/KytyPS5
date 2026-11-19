#include "graphics/host_gpu/deviceLossReport.h"

#include "graphics/host_gpu/graphicContext.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace Libs::Graphics {

void ReportDeviceLoss(const GraphicContext& graphics, const char* where, int result) {
	static std::atomic<bool> reported {false};
	if (reported.exchange(true)) return;
	constexpr double MiB = 1024.0 * 1024.0;
	double ram_free = -1, ram_total = -1, commit = -1, commit_limit = -1;
#if defined(_WIN32)
	MEMORYSTATUSEX status {};
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status) != 0) {
		ram_free     = static_cast<double>(status.ullAvailPhys) / MiB;
		ram_total    = static_cast<double>(status.ullTotalPhys) / MiB;
		commit_limit = static_cast<double>(status.ullTotalPageFile) / MiB;
		commit       = static_cast<double>(status.ullTotalPageFile - status.ullAvailPageFile) / MiB;
	}
#endif
	double vram_usage = -1, vram_budget = -1, host_usage = -1, host_budget = -1;
	std::array<GraphicContext::HeapUsage, VK_MAX_MEMORY_HEAPS> heaps {};
	const auto count = graphics.GetHeapUsage(heaps);
	for (uint32_t i = 0; i < count; i++) {
		auto& usage  = heaps[i].device_local ? vram_usage : host_usage;
		auto& budget = heaps[i].device_local ? vram_budget : host_budget;
		usage        = (usage < 0 ? 0 : usage) + static_cast<double>(heaps[i].usage) / MiB;
		budget       = (budget < 0 ? 0 : budget) + static_cast<double>(heaps[i].budget) / MiB;
	}
	std::printf("[device-loss] %s result=%d | RAM free %.0f / %.0f MiB | commit %.0f / %.0f MiB | "
	            "VRAM %.0f / budget %.0f MiB | host heaps %.0f / budget %.0f MiB%s\n",
	            where, result, ram_free, ram_total, commit, commit_limit, vram_usage, vram_budget,
	            host_usage, host_budget, graphics.CanReportMemoryUsage() ? "" : " (no memory budget)");
	if (graphics.device_fault_enabled) {
		vk::DeviceFaultCountsEXT counts {};
		const auto first = graphics.device.getFaultInfoEXT(&counts, nullptr);
		std::printf("[device-loss] fault query: %s\n", vk::to_string(first).c_str());
		{
			std::vector<vk::DeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
			std::vector<vk::DeviceFaultVendorInfoEXT>  vendors(counts.vendorInfoCount);
			counts.vendorBinarySize = 0;
			vk::DeviceFaultInfoEXT info {};
			info.pAddressInfos = addresses.data();
			info.pVendorInfos  = vendors.data();
			(void)graphics.device.getFaultInfoEXT(&counts, &info);
			std::printf("[device-loss] fault: %s\n", info.description.data());
			for (const auto& address: addresses) {
				std::printf("[device-loss] fault address %s 0x%016llx (precision 0x%llx)\n",
				            vk::to_string(address.addressType).c_str(),
				            static_cast<unsigned long long>(address.reportedAddress),
				            static_cast<unsigned long long>(address.addressPrecision));
			}
			for (const auto& vendor: vendors) {
				std::printf("[device-loss] fault vendor: %s (code 0x%llx, data 0x%llx)\n",
				            vendor.description.data(),
				            static_cast<unsigned long long>(vendor.vendorFaultCode),
				            static_cast<unsigned long long>(vendor.vendorFaultData));
			}
		}
	}
	if (graphics.gpu_checkpoints_enabled) {
		// The last markers each queue passed: program hashes of the draws/dispatches.
		for (const auto [queue, name]: {std::pair {graphics.queue, "main"},
		                                std::pair {graphics.side_queue, "side"}}) {
			if (!queue) continue;
			uint32_t count = 0;
			queue.getCheckpointDataNV(&count, nullptr);
			std::vector<vk::CheckpointDataNV> checkpoints(count);
			queue.getCheckpointDataNV(&count, checkpoints.data());
			for (const auto& checkpoint: checkpoints) {
				std::printf("[device-loss] %s queue checkpoint %s: program 0x%016llx\n", name,
				            vk::to_string(checkpoint.stage).c_str(),
				            static_cast<unsigned long long>(
				                reinterpret_cast<uintptr_t>(checkpoint.pCheckpointMarker)));
			}
		}
	}
	std::fflush(stdout);
}

} // namespace Libs::Graphics
