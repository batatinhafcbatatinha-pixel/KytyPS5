#include "graphics/host_gpu/renderer/cache/bufferCache.h"
#include "common/fpsProbe.h" // FPSPROBE

#include "common/alignment.h"
#include "common/assert.h"
#include "common/debugCounters.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/cache/textureCache.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "kernel/memory.h"

#include <algorithm>
#include <chrono> // FPSPROBE
#include <cstdio> // FPSPROBE
#include <cstdlib> // FPSPROBE
#include <map> // FPSPROBE
#include <string> // FPSPROBE
#include <unordered_map> // FPSPROBE
#include <cinttypes>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace Libs::Graphics {

namespace {
// FPSPROBE: temporary read-back statistics (env KYTY_PROFILE_GPU_WAITS).
bool ProbeEnabled() {
	static const bool enabled = std::getenv("KYTY_PROFILE_GPU_WAITS") != nullptr;
	return enabled;
}
std::unordered_map<uint64_t, uint64_t>& ProbeWriteTicks() {
	static std::unordered_map<uint64_t, uint64_t> ticks;
	return ticks;
}
uint64_t ProbeEager(uint64_t bytes, bool take_count = false, bool take_bytes = false) {
	static uint64_t count = 0, total = 0;
	if (take_count) { const auto value = count; count = 0; return value; }
	if (take_bytes) { const auto value = total; total = 0; return value; }
	count++;
	total += bytes;
	return 0;
}
void ProbeRecordWrite(uint64_t vaddr, uint64_t size, uint64_t tick) {
	static const bool age = std::getenv("KYTY_PROBE_AGE") != nullptr;
	if (!age || size == 0 || size > (uint64_t {64} << 20u)) return;
	for (uint64_t page = vaddr >> 12u; page <= (vaddr + size - 1) >> 12u; ++page) {
		ProbeWriteTicks()[page] = tick;
	}
}
void ProbeValue(bool same) {
	static uint64_t counts[2] {};
	static auto     last = std::chrono::steady_clock::now();
	counts[same ? 1 : 0]++;
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - last).count() < 5.0) return;
	std::printf("[walkval] gpu-thread waited words: same=%llu changed=%llu\n", (unsigned long long)counts[1], (unsigned long long)counts[0]);
	std::fflush(stdout);
	counts[0] = counts[1] = 0;
	last = now;
}
void ProbeSpeculation(bool hit) {
	static uint64_t hits = 0, misses = 0;
	static auto     last = std::chrono::steady_clock::now();
	(hit ? hits : misses)++;
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - last).count() < 5.0) return;
	std::printf("[speculate] walker loads=%llu mismatches=%llu\n", (unsigned long long)hits, (unsigned long long)misses);
	std::fflush(stdout);
	hits = misses = 0;
	last = now;
}
void ProbeRecordRead(uint64_t vaddr, uint64_t size, uint64_t current, bool dirty, bool gpu_thread,
                     std::source_location caller, std::chrono::steady_clock::time_point start) {
	if (!ProbeEnabled()) return;
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	uint64_t newest = 0;
	for (uint64_t page = vaddr >> 12u; page <= (vaddr + size - 1) >> 12u; ++page) {
		if (auto it = ProbeWriteTicks().find(page); it != ProbeWriteTicks().end()) {
			newest = std::max(newest, it->second);
		}
	}
	const uint64_t age    = newest == 0 ? 999 : current - newest;
	const int      bucket = age == 0 ? 0 : age == 1 ? 1 : age <= 4 ? 2 : age < 999 ? 3 : 4;
	struct Row {
		uint64_t count[5] {};
		double   ms = 0;
	};
	static std::map<std::string, Row> rows;
	static auto last = std::chrono::steady_clock::now();
	char key[512];
	std::snprintf(key, sizeof(key), "%s:%u %s %s", caller.file_name(), static_cast<unsigned>(caller.line()),
	              gpu_thread ? "gpu-thread" : "cpu-thread", dirty ? "dirty" : "clean");
	auto& row = rows[key];
	row.count[bucket]++;
	row.ms += ms;
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - last).count() < 5.0) return;
	for (const auto& [name, value]: rows) {
		std::printf("[readback] %s age(cur,1,2-4,5+,unk)=%llu,%llu,%llu,%llu,%llu ms=%.1f\n", name.c_str(),
		            (unsigned long long)value.count[0], (unsigned long long)value.count[1],
		            (unsigned long long)value.count[2], (unsigned long long)value.count[3],
		            (unsigned long long)value.count[4], value.ms);
	}
	std::fflush(stdout);
	rows.clear();
	last = now;
}

// PROBE (temporary): every read-back wait with its demanded bytes and the producer of that range
// (env KYTY_PROFILE_GPU_WAITS, aggregated every 10 s as [wait]).
struct ProducerWrite {
	uint64_t end      = 0;
	uint64_t producer = 0;
	uint64_t tick     = 0;
};
std::map<uint64_t, ProducerWrite>& ProbeProducers() {
	static std::map<uint64_t, ProducerWrite> writes;
	return writes;
}
void ProbeRecordProducer(uint64_t vaddr, uint64_t size, uint64_t tick) {
	if (!ProbeEnabled() || size == 0) return;
	auto& writes = ProbeProducers();
	// Newest write wins over the ranges it covers.
	auto it = writes.lower_bound(vaddr);
	if (it != writes.begin() && std::prev(it)->second.end > vaddr) --it;
	while (it != writes.end() && it->first < vaddr + size) {
		if (it->first >= vaddr && it->second.end <= vaddr + size) {
			it = writes.erase(it);
		} else {
			++it;
		}
	}
	writes[vaddr] = {vaddr + size, FpsProbe::g_producer, tick};
	if (writes.size() > 200000) writes.erase(writes.begin());
}
struct WaitProbe {
	uint64_t             vaddr = 0, size = 0, current = 0, downloaded = 0;
	bool                 gpu_thread = false, page_dirty = false, bytes_dirty = false, eager_bytes = false;
	double               eager_ms = 0, download_ms = 0;
	std::source_location caller;
};
void ProbeRecordWait(const WaitProbe& probe, CommandScheduler& scheduler) {
	if (!ProbeEnabled()) return;
	const double ms = probe.eager_ms + probe.download_ms;
	// The newest write overlapping the demanded bytes, else the nearest one before them (window).
	auto&         writes   = ProbeProducers();
	ProducerWrite producer {};
	bool          exact    = false;
	auto it = writes.upper_bound(probe.vaddr + probe.size - 1);
	if (it != writes.begin()) {
		--it;
		exact    = it->second.end > probe.vaddr;
		producer = it->second;
	}
	const char* kind = probe.bytes_dirty ? "bytes" : probe.page_dirty ? "page-only" : probe.eager_bytes ? "eager-bytes"
	                 : ms > 0.05 ? "window-only" : "none";
	const uint64_t age = producer.tick == 0 ? 999 : probe.current - producer.tick;
	struct Row {
		uint64_t count = 0, downloaded = 0, waits = 0;
		double   ms    = 0;
		std::map<uint64_t, uint64_t> ranges;
	};
	static std::map<std::string, Row> rows;
	static double total_ms[2] {};
	static auto   last = std::chrono::steady_clock::now();
	char key[512];
	std::snprintf(key, sizeof(key), "%s %s producer=%016llx%s age=%s caller=%s:%u",
	              probe.gpu_thread ? "gpu" : "cpu", kind, static_cast<unsigned long long>(producer.producer),
	              exact ? "" : "(near)", age == 0 ? "0" : age == 1 ? "1" : age <= 4 ? "2-4" : age < 999 ? "5+" : "unk",
	              probe.caller.file_name() + std::max<int>(0, static_cast<int>(std::strlen(probe.caller.file_name())) - 40),
	              static_cast<unsigned>(probe.caller.line()));
	auto& row = rows[key];
	row.count++;
	row.ms += ms;
	row.waits += ms > 0.05 ? 1 : 0;
	row.downloaded += probe.downloaded;
	row.ranges[probe.vaddr >> 12u]++;
	total_ms[probe.gpu_thread ? 1 : 0] += ms;
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - last).count() < 10.0) return;
	std::vector<std::pair<double, const std::string*>> order;
	for (const auto& [name, row_value]: rows) order.emplace_back(row_value.ms, &name);
	std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::printf("[wait] 10 s totals: gpu-thread=%.0f ms cpu-threads=%.0f ms\n", total_ms[1], total_ms[0]);
	for (size_t i = 0; i < std::min<size_t>(order.size(), 20); i++) {
		const auto& r = rows[*order[i].second];
		std::printf("[wait] ms=%.0f reads=%llu waited=%llu pages=%zu down_kib=%llu %s\n", r.ms,
		            static_cast<unsigned long long>(r.count), static_cast<unsigned long long>(r.waits),
		            r.ranges.size(), static_cast<unsigned long long>(r.downloaded >> 10u), order[i].second->c_str());
	}
	std::fflush(stdout);
	rows.clear();
	total_ms[0] = total_ms[1] = 0;
	last = now;
	(void)scheduler;
}
} // namespace

namespace {

constexpr uint64_t MiB           = 1024 * 1024;
constexpr uint64_t GdsBufferSize = 64 * 1024;
// GPU-modified ranges come as many small scattered islands, so a readback widens to a window.
constexpr uint64_t DownloadWindowSize = 512 * 1024;

std::pair<Buffer*, uint64_t> UploadMemory(GraphicContext& graphics, CommandScheduler& scheduler,
                                        StreamBuffer& staging, uint64_t size, uint64_t alignment,
                                        vk::BufferUsageFlags usage, const auto& write) {
	const auto [mapped, offset] = staging.Map(size, alignment);
	if (mapped != nullptr) {
		write(mapped, offset);
		staging.Commit();
		return {&staging, offset};
	}

	// Large transfers use the same one-time host allocation as ordinary buffer uploads.
	auto temporary = std::make_unique<Buffer>(graphics, scheduler, MemoryUsage::Upload, 0,
	                                         usage, size);
	write(temporary->Mapped().data(), 0);
	temporary->Flush(0, size);
	auto* buffer = temporary.get();
	scheduler.DeferOperation([owner = std::move(temporary)]() mutable { owner.reset(); });
	return {buffer, 0};
}

} // namespace

void BufferCache::WriteDataBuffer(Buffer& buffer, uint64_t address, const void* source,
                                  uint64_t size) {
	FpsProbe::CountEvent(FpsProbe::PageTableWrites); // PROBE
	auto* bytes = static_cast<const uint8_t*>(source);
	while (size != 0) {
		const auto chunk  = std::min(size, m_staging_buffer.Size());
		// Copy waits for ring reuse and flushes the fresh host data before submission.
		const auto offset = m_staging_buffer.Copy(bytes, chunk, 4);
		const auto destination_offset = buffer.Offset(address);
		EXIT_IF(destination_offset > buffer.Size() || chunk > buffer.Size() - destination_offset);
		m_scheduler.EndRendering();
		const auto command = m_scheduler.Current().Handle();
		vk::BufferMemoryBarrier2 before {};
		before.srcStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		before.srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		before.dstStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		before.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
		before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		before.buffer        = buffer.Handle();
		before.offset        = destination_offset;
		before.size          = chunk;
		vk::DependencyInfo dependency {};
		dependency.dependencyFlags          = vk::DependencyFlagBits::eByRegion;
		dependency.bufferMemoryBarrierCount = 1;
		dependency.pBufferMemoryBarriers    = &before;
		command.pipelineBarrier2(dependency);
		const vk::BufferCopy copy {offset, destination_offset, chunk};
		command.copyBuffer(m_staging_buffer.Handle(), buffer.Handle(), 1, &copy);
		auto after          = before;
		after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
		after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
		after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
		dependency.pBufferMemoryBarriers = &after;
		command.pipelineBarrier2(dependency);
		bytes += chunk;
		address += chunk;
		size -= chunk;
	}
}

void BufferCache::Register(BufferId id) {
	ChangeRegister<true>(id);
}

void BufferCache::Unregister(BufferId id) {
	ChangeRegister<false>(id);
}

template <bool insert>
void BufferCache::ChangeRegister(BufferId id) {
	auto& buffer = m_slot_buffers[id];
	PageTable::PageRange pages {};
	EXIT_IF(!(GuestRange {buffer.CpuAddress(), buffer.Size()}.Valid()) ||
	        !PageTable::TryGetPageRange(buffer.CpuAddress(), buffer.Size(), pages));
	for (size_t page = pages.first; page < pages.last_exclusive; ++page) {
		if constexpr (insert) {
			m_page_table[page] = id;
		} else {
			m_page_table[page] = {};
		}
	}
	const auto size_pages = pages.last_exclusive - pages.first;
	const auto table_offset = PageIndex(buffer.CpuAddress()) * sizeof(vk::DeviceAddress);
	if constexpr (insert) {
		m_buffers.emplace(buffer.CpuAddress(), id);
		m_total_used_memory += buffer.Size();
		buffer.lru_id = m_lru_cache.Insert(id, m_gc_tick);
		std::vector<vk::DeviceAddress> addresses;
		addresses.reserve(size_pages);
		for (uint64_t i = 0; i < size_pages; ++i) {
			addresses.push_back(buffer.BufferDeviceAddress() + (i << CACHING_PAGEBITS));
		}
		{
			std::lock_guard lock(m_bda_sync_mutex);
			m_dma_accessed.ForEachInRange(
			    buffer.CpuAddress(), buffer.Size(), [&](uint64_t begin, uint64_t end) {
				    for (auto address = begin; address < end; address += CACHING_PAGESIZE) {
					    addresses[buffer.Offset(address) >> CACHING_PAGEBITS] += BDA_ACCESSED;
				    }
			    });
		}
		if (HostViewEnabled()) {
			WriteBdaEntries(buffer.CpuAddress(), buffer.CpuAddress() + buffer.Size());
		} else {
			WriteDataBuffer(m_bda_pagetable_buffer, table_offset,
			                addresses.data(), addresses.size() * sizeof(vk::DeviceAddress));
		}
		QueueBdaSync(buffer.CpuAddress(), buffer.Size());
	} else {
		m_buffers.erase(m_buffers.find(buffer.CpuAddress()));
		m_total_used_memory -= buffer.Size();
		m_lru_cache.Free(buffer.lru_id);
		buffer.is_deleted = true;
		if (HostViewEnabled()) {
			// The page table no longer finds this buffer: its pages read the host view.
			WriteBdaEntries(buffer.CpuAddress(), buffer.CpuAddress() + buffer.Size());
		} else {
			m_bda_pagetable_buffer.Fill(table_offset,
			                            size_pages * sizeof(vk::DeviceAddress), 0);
		}
	}
}

void BufferCache::TouchBuffer(const Buffer& buffer) {
	m_lru_cache.Touch(buffer.lru_id, m_gc_tick);
}

void BufferCache::DeleteBuffer(BufferId id) {
	Unregister(id);
	if (m_scheduler.Active()) {
		m_scheduler.DeferOperation([this, id] { m_slot_buffers.erase(id); });
	} else {
		m_slot_buffers.erase(id);
	}
}

template <bool async>
bool BufferCache::DownloadBufferMemory(Buffer& buffer, uint64_t vaddr, uint64_t size) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size     = 0;
	const auto                  buffer_address = buffer.CpuAddress();
	m_memory_tracker.ForEachDownloadRange<false>(
	    vaddr, size, [&](uint64_t address, uint64_t bytes) noexcept {
		    m_memory_tracker.ValidateGpuDirtyPages(m_gpu_modified_ranges, address, bytes,
		                                           "buffer download");
		    m_gpu_modified_ranges.ForEachInRange(address, bytes, [&](uint64_t start, uint64_t end) {
			    copies.emplace_back(start - buffer_address, total_size, end - start);
			    // Start each packed range on its own 64-byte cache line.
			    total_size += Common::AlignUp(end - start, 64);
		    });
		    m_gpu_modified_ranges.Subtract(address, bytes);
		    ForgetColorMetadataChecks(address, bytes);
	    });
	if (copies.empty()) {
		return false;
	}
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferDownloadBytes, total_size);

	auto download = m_download_buffer.MapDownload(total_size, 64);
	for (auto& copy: copies) {
		copy.dstOffset += download.offset;
	}

	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	vk::BufferMemoryBarrier before {};
	before.srcAccessMask       = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite;
	before.dstAccessMask       = vk::AccessFlagBits::eTransferRead;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = buffer.Handle();
	before.offset              = 0;
	before.size                = buffer.Size();
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, {}, 0, nullptr, 1, &before, 0,
	                       nullptr);
	native.copyBuffer(buffer.Handle(), download.buffer->Handle(),
	                  static_cast<uint32_t>(copies.size()), copies.data());

	auto after          = before;
	after.srcAccessMask = vk::AccessFlagBits::eTransferWrite;
	after.dstAccessMask = vk::AccessFlagBits::eHostRead;
	after.buffer        = download.buffer->Handle();
	after.offset        = download.offset;
	after.size          = total_size;
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands |
	                           vk::PipelineStageFlagBits::eHost,
	                       {}, 0, nullptr, 1, &after, 0, nullptr);
	auto publish = [total_size, buffer_address, copies = std::move(copies),
	                download = std::move(download)] {
		download.buffer->Invalidate(download.offset, total_size);
		for (const auto& copy: copies) {
			Libs::LibKernel::Memory::WriteBacking(
			    buffer_address + copy.srcOffset,
			    download.mapped + (copy.dstOffset - download.offset), copy.size);
		}
	};
	if constexpr (async) {
		m_scheduler.DeferPriorityOperation(std::move(publish));
	} else {
		const auto tick = m_scheduler.CurrentTick();
		m_scheduler.Wait(tick);
		m_scheduler.WaitPriorityOperations(tick);
		publish();
	}
	return true;
}

BufferCache::BufferCache(GraphicContext& graphics, CommandScheduler& scheduler,
                         PageManager& page_manager, TextureCache& texture_cache)
    : m_graphics(graphics), m_scheduler(scheduler), m_fault_manager(graphics, scheduler, *this),
      m_gds_buffer(graphics, scheduler, MemoryUsage::Stream, 0, AllFlags, GdsBufferSize),
      m_bda_pagetable_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 0, AllFlags,
                             BDA_PAGETABLE_SIZE),
      m_memory_tracker(page_manager),
      m_staging_buffer(graphics, scheduler, MemoryUsage::Upload, 512 * MiB),
      m_stream_buffer(graphics, scheduler, MemoryUsage::Stream, 64 * MiB),
      m_download_buffer(graphics, scheduler, MemoryUsage::Download, 64 * MiB),
      m_device_buffer(graphics, scheduler, MemoryUsage::DeviceLocal, 128 * MiB),
      m_texture_cache(texture_cache) {
	std::memset(m_gds_buffer.Mapped().data(), 0, static_cast<size_t>(m_gds_buffer.Size()));
	m_gds_buffer.Flush(0, m_gds_buffer.Size());
	SetVulkanObjectNameF(m_graphics.device, m_bda_pagetable_buffer.Handle(),
	                     "BDA Page Table Buffer");
	m_scheduler.BeginCommand();
	for (auto* buffer: {&m_bda_pagetable_buffer, m_fault_manager.GetFaultBuffer()}) {
		buffer->Fill(0, buffer->Size(), 0);
	}
	const auto null_id =
	    m_slot_buffers.insert(m_graphics, m_scheduler, MemoryUsage::DeviceLocal, 0, AllFlags, 16);
	EXIT_IF(null_id != NULL_BUFFER_ID);
	SetVulkanObjectNameF(m_graphics.device, GetBuffer(null_id).Handle(), "Kyty.NullBuffer");
	InitHostView();
	if (!m_graphics.CanReportMemoryUsage()) {
		return;
	}
	constexpr int64_t GiB              = 1024ll * 1024 * 1024;
	constexpr int64_t target_threshold = 8 * GiB;
	const auto        budget =
	    static_cast<int64_t>(std::min<uint64_t>(m_graphics.GetTotalMemoryBudget(), INT64_MAX));
	const auto threshold = std::min(budget, target_threshold);
	const auto expected  = std::min(budget - 6 * threshold / 10, budget - GiB);
	const auto critical  = std::min(budget - 2 * threshold / 10, budget - GiB / 2);
	m_trigger_gc_memory  = static_cast<uint64_t>(std::max<int64_t>(expected, GiB));
	m_critical_gc_memory = static_cast<uint64_t>(std::max<int64_t>(critical, 2 * GiB));
}

BufferCache::~BufferCache() {
	DestroyHostView();
	if (!m_gpu_modified_ranges.Empty()) {
		EXIT("BufferCache: destroyed with pending GPU-modified ranges\n");
	}
	for (const auto& [vaddr, id]: m_buffers) {
		(void)vaddr;
		const auto& buffer = m_slot_buffers[id];
		if (m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size())) {
			EXIT("BufferCache: destroyed with GPU-modified buffer\n");
		}
	}
}

void BufferCache::InvalidateMemory(uint64_t vaddr, uint64_t size) {
	m_memory_tracker.InvalidateRegion(vaddr, size,
	                                  [this, vaddr, size] { ReadMemory(vaddr, size, true); });
	QueueBdaSync(vaddr, size, true);
}

void BufferCache::ReadMemory(uint64_t vaddr, uint64_t size, bool is_write,
                             std::source_location caller) {
	const bool from_gpu_thread = GuestGpu::IsGpuThread(); // FPSPROBE
	if (!GuestGpu::IsGpuThread() && CommandScheduler::InDeferredOperation()) {
		EXIT("unsupported buffer readback from an asynchronous GPU completion, "
		     "addr=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
		     vaddr, size);
	}
	m_scheduler.Context().GetGpu().SendCommandSync([this, vaddr, size, is_write, caller, from_gpu_thread] {
	FpsProbe::Scope fps_scope(FpsProbe::ReadBack); // FPSPROBE
		if (is_write && !IsRegionRegistered(vaddr, size)) {
			return;
		}
		// A walker fault on the GPU thread: compare the guest word before and after the wait.
		uint32_t   walker_before = 0;
		const auto walker_word   = Common::AlignDown(vaddr, 4);
		const bool walker_read   = GuestGpu::IsGpuThread() && size <= 4 &&
		                         (IsRegionGpuModified(vaddr, size) || HasEagerDownloads(vaddr, size)) &&
		                         LibKernel::Memory::TryReadBacking(walker_word, &walker_before, 4);
		// PROBE: classify what this read demands before anything changes state.
		WaitProbe probe;
		probe.vaddr       = vaddr;
		probe.size        = size;
		probe.gpu_thread  = from_gpu_thread;
		probe.caller      = caller;
		probe.page_dirty  = IsRegionGpuModified(vaddr, size);
		probe.bytes_dirty = m_gpu_modified_ranges.Intersects(vaddr, size);
		probe.eager_bytes = m_eager_pending.Intersects(vaddr, size);
		probe.current     = m_scheduler.CurrentTick();
		const auto probe_begin = std::chrono::steady_clock::now();
		{
			const auto window = Common::AlignDown(vaddr, DownloadWindowSize);
			CompleteEagerDownloads(window,
			                       std::max(window + DownloadWindowSize, vaddr + size) - window,
			                       false);
		}
		probe.eager_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - probe_begin).count();
		// Only small reads, such as faults and descriptor loads, predict later read-backs.
		if (size <= 16 * TRACKER_PAGE_SIZE && IsRegionGpuModified(vaddr, size)) {
			const auto page = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
			m_readback_pages.Add(page, Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE) - page);
		}
		auto& buffer = m_slot_buffers[FindBuffer(vaddr, size)];
		const auto probe_start = std::chrono::steady_clock::now(); // FPSPROBE
		const bool probe_dirty = IsRegionGpuModified(vaddr, size); // FPSPROBE
		const auto probe_tick  = m_scheduler.CurrentTick(); // FPSPROBE
		if (IsRegionGpuModified(vaddr, size)) {
			const auto start =
			    std::max(Common::AlignDown(vaddr, DownloadWindowSize), buffer.CpuAddress());
			const auto end = std::min(std::max(start + DownloadWindowSize, vaddr + size),
			                          buffer.CpuAddress() + buffer.Size());
			probe.downloaded = end - start;
			const auto download_begin = std::chrono::steady_clock::now();
			if (DownloadBufferMemory<false>(buffer, start, end - start)) {
				m_memory_tracker.UnmarkRegionAsGpuModified(start, end - start);
			}
			probe.download_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - download_begin).count();
		}
		ProbeRecordWait(probe, m_scheduler); // PROBE
		ProbeRecordRead(vaddr, size, probe_tick, probe_dirty, from_gpu_thread, caller, probe_start); // FPSPROBE
		if (walker_read) {
			uint32_t walker_after = walker_before;
			(void)LibKernel::Memory::TryReadBacking(walker_word, &walker_after, 4);
			ProbeValue(walker_after == walker_before); // FPSPROBE
			LearnWalkerWord(walker_word, walker_before, walker_after);
		}
		if (is_write) {
			m_memory_tracker.MarkRegionAsCpuModified(vaddr, size);
			QueueBdaSync(vaddr, size, true);
		}
	});
}

BufferId BufferCache::FindBuffer(uint64_t vaddr, uint64_t size) {
	if (vaddr == 0) {
		return NULL_BUFFER_ID;
	}
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid buffer discovery request\n");
	}
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			return *owner;
		}
	}
	return CreateBuffer(vaddr, size);
}

BufferCache::OverlapResult BufferCache::ResolveOverlaps(uint64_t vaddr, uint64_t size) {
	static constexpr int      StreamLeapThreshold = 16;
	static constexpr uint64_t StreamLeapSize      = CACHING_PAGESIZE * 128;

	auto       begin      = vaddr;
	auto       end        = vaddr + size;
	const auto find_first = [&](uint64_t address) {
		auto first = m_buffers.lower_bound(address);
		if (first != m_buffers.begin()) {
			const auto  previous = std::prev(first);
			const auto& buffer   = m_slot_buffers[previous->second];
			if (buffer.CpuAddress() + buffer.Size() > address) {
				first = previous;
			}
		}
		return first;
	};
	auto first           = find_first(begin);
	auto last            = first;
	int  stream_score    = 0;
	bool has_stream_leap = false;
	for (; last != m_buffers.end() && last->first < end; ++last) {
		const auto& buffer        = m_slot_buffers[last->second];
		const auto  buffer_begin  = buffer.CpuAddress();
		const auto  buffer_end    = buffer_begin + buffer.Size();
		const bool  expands_left  = buffer_begin < begin;
		const bool  expands_right = buffer_end > end;
		begin                     = std::min(begin, buffer_begin);
		end                       = std::max(end, buffer_end);
		if (!has_stream_leap && (stream_score += buffer.StreamScore()) > StreamLeapThreshold) {
			has_stream_leap = true;
			// Reserve space in the incoming stream's direction of growth.
			// The old buffer extending left of the request predicts growth to the right, and vice versa.
			if (expands_left) {
				end += std::min(StreamLeapSize, (vaddr < LOWER_ADDRESS_SIZE ? LOWER_ADDRESS_SIZE
				                                       : LibKernel::Memory::kExtendedMemoryBase +
				                                             LibKernel::Memory::kExtendedMemorySize) - end);
			}
			if (expands_right) {
				const auto minimum = vaddr < LOWER_ADDRESS_SIZE
				                         ? CACHING_PAGESIZE * 2
				                         : LibKernel::Memory::kExtendedMemoryBase;
				if (begin > minimum) {
					begin -= std::min(StreamLeapSize, begin - minimum);
				}
				first = find_first(begin);
				begin = std::min(begin, first->first);
			}
		}
	}
	return {first, last, begin, end, has_stream_leap};
}

void BufferCache::JoinOverlap(BufferId new_id, BufferId overlap_id, bool accumulate_stream_score) {
	auto& new_buffer = m_slot_buffers[new_id];
	auto& overlap    = m_slot_buffers[overlap_id];
	if (accumulate_stream_score) {
		new_buffer.IncreaseStreamScore(overlap.StreamScore() + 1);
	}
	new_buffer.CopyFrom(m_scheduler.Current(), overlap, 0,
	                    overlap.CpuAddress() - new_buffer.CpuAddress(), overlap.Size());
	DeleteBuffer(overlap_id);
}

BufferId BufferCache::CreateBuffer(uint64_t vaddr, uint64_t size) {
	const auto end = Common::AlignUp(vaddr + size, CACHING_PAGESIZE);
	vaddr = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	size               = end - vaddr;
	const auto overlap = ResolveOverlaps(vaddr, size);

	const auto id = m_slot_buffers.insert(
	    m_graphics, m_scheduler, MemoryUsage::DeviceLocal, overlap.begin,
	    AllFlags | vk::BufferUsageFlagBits::eShaderDeviceAddress, overlap.end - overlap.begin);
	const auto& buffer = m_slot_buffers[id];
	SetVulkanObjectNameF(m_graphics.device, buffer.Handle(),
	                     "Kyty.GameBuffer[guest=0x{:016x} size=0x{:x}]", overlap.begin,
	                     overlap.end - overlap.begin);
	for (auto it = overlap.first; it != overlap.last;) {
		const auto old_id = (it++)->second;
		JoinOverlap(id, old_id, !overlap.has_stream_leap);
	}
	Register(id);
	return id;
}

bool BufferCache::SynchronizeBuffer(Buffer& buffer, uint64_t vaddr, uint64_t size, bool is_written,
                                    bool is_texel_buffer) {
	std::vector<vk::BufferCopy> copies;
	uint64_t                    total_size = 0;
	m_memory_tracker.ForEachUploadRange(
	    vaddr, size, is_written,
	    [&](uint64_t address, uint64_t bytes) noexcept {
		    copies.emplace_back(total_size, buffer.Offset(address), bytes);
		    total_size += bytes;
	    },
	    [&]() noexcept {
		    const BufferUpload upload {&buffer, copies.size()};
		    UploadCopies({&upload, 1}, copies, total_size);
	    });
	if (is_texel_buffer && !is_written) {
		return SynchronizeBufferFromImage(buffer, vaddr, size);
	}
	return false;
}

void BufferCache::UploadCopies(std::span<const BufferUpload> uploads,
                               std::span<vk::BufferCopy> copies, uint64_t total_size) {
	if (m_host_view && !copies.empty()) {
		// Uploaded pages are current in their buffer: the page table sends BDA reads there again.
		std::lock_guard lock(m_bda_sync_mutex);
		size_t first = 0;
		for (const auto& upload: uploads) {
			for (size_t copy = first; copy < upload.end; ++copy) {
				const auto begin = upload.buffer->CpuAddress() + copies[copy].dstOffset;
				m_bda_entry_pending.Add(Common::AlignDown(begin, CACHING_PAGESIZE),
				                        Common::AlignUp(begin + copies[copy].size, CACHING_PAGESIZE) -
				                            Common::AlignDown(begin, CACHING_PAGESIZE));
			}
			first = upload.end;
		}
		m_bda_sync_pending.store(true, std::memory_order_release);
	}
	if (copies.empty()) {
		return;
	}
	FpsProbe::CountEvent(FpsProbe::Uploads); // PROBE
	Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferUploadBytes, total_size);

	const auto source = UploadMemory(
	    m_graphics, m_scheduler, m_staging_buffer, total_size, 4,
	    vk::BufferUsageFlagBits::eTransferSrc, [&](uint8_t* mapped, uint64_t base_offset) {
		    size_t first = 0;
		    for (const auto& upload: uploads) {
			    for (auto& copy: copies.subspan(first, upload.end - first)) {
				    const auto address = upload.buffer->CpuAddress() + copy.dstOffset;
				    if (!LibKernel::Memory::CopySparseMemory(address, mapped + copy.srcOffset,
					                                         copy.size)) {
					    EXIT("BufferCache: failed to read sparse guest buffer memory\n");
				    }
				    copy.srcOffset += base_offset;
			    }
			    first = upload.end;
		    }
	    });
	auto& command = m_scheduler.Current();
	command.EndRendering();
	const auto              native = command.Handle();
	const vk::MemoryBarrier before {
	    .srcAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite |
		                 vk::AccessFlagBits::eTransferRead | vk::AccessFlagBits::eTransferWrite,
	    .dstAccessMask = vk::AccessFlagBits::eTransferWrite};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
	                       vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
	                       1, &before, 0, nullptr, 0, nullptr);
	size_t first = 0;
	for (const auto& upload: uploads) {
		native.copyBuffer(source.first->Handle(), upload.buffer->Handle(),
		                  static_cast<uint32_t>(upload.end - first), copies.data() + first);
		first = upload.end;
	}
	const vk::MemoryBarrier after {.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
	                               .dstAccessMask = vk::AccessFlagBits::eMemoryRead |
	                                                vk::AccessFlagBits::eMemoryWrite};
	native.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
	                       vk::PipelineStageFlagBits::eAllCommands,
	                       vk::DependencyFlagBits::eByRegion, 1, &after, 0, nullptr, 0, nullptr);
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBuffer(uint64_t vaddr, uint64_t size,
                                                       bool is_written, bool is_texel_buffer,
                                                       BufferId id) {
	EXIT_IF(!(GuestRange {vaddr, size}.Valid()));

	if (!is_written && size <= CACHING_PAGESIZE &&
	    !m_memory_tracker.IsRegionGpuModified(vaddr, size) &&
	    m_memory_tracker.IsRegionCpuModified(vaddr, size)) {
		const auto alignment = std::max<uint64_t>(
		    m_graphics.physical_device_properties.limits.minUniformBufferOffsetAlignment, 1);
		auto [mapped, offset] = m_stream_buffer.Map(size, alignment, false);
		if (mapped != nullptr) {
			if (!LibKernel::Memory::CopySparseMemory(vaddr, mapped, size)) {
				EXIT("BufferCache: failed to read sparse guest buffer memory\n");
			}
			m_stream_buffer.Commit();
			return {&m_stream_buffer, offset};
		}
	}

	if (IsBufferInvalid(id) || !m_slot_buffers[id].IsInBounds(vaddr, size)) {
		id = FindBuffer(vaddr, size);
	}
	auto& buffer = m_slot_buffers[id];
	TouchBuffer(buffer);
	(void)SynchronizeBuffer(buffer, vaddr, size, is_written, is_texel_buffer);
	if (is_written && HostViewEnabled()) {
		// The written pages read the buffer from now on, so all of their bytes must be current.
		const auto begin = std::max(buffer.CpuAddress(), Common::AlignDown(vaddr, CACHING_PAGESIZE));
		const auto end   = std::min(buffer.CpuAddress() + buffer.Size(),
		                            Common::AlignUp(vaddr + size, CACHING_PAGESIZE));
		(void)SynchronizeBuffer(buffer, begin, end - begin, false, false);
		bool fresh = false;
		{
			std::lock_guard lock(m_bda_sync_mutex);
			fresh = !m_gpu_owned.Contains(begin, end - begin);
			m_gpu_owned.Add(begin, end - begin);
		}
		if (fresh) WriteBdaEntries(begin, end);
	}
	if (is_written) {
		ProbeRecordWrite(vaddr, size, m_scheduler.CurrentTick()); // FPSPROBE
		ProbeRecordProducer(vaddr, size, m_scheduler.CurrentTick()); // PROBE
		m_gpu_modified_ranges.Add(vaddr, size);
		ForgetColorMetadataChecks(vaddr, size);
		m_readback_pages.ForEachInRange(vaddr, size, [this](uint64_t begin, uint64_t end) {
			m_eager_writes.Add(begin, end - begin);
		});
	}
	return {&buffer, buffer.Offset(vaddr)};
}

std::pair<Buffer*, uint64_t> BufferCache::ObtainBufferForImage(uint64_t vaddr, uint64_t size) {
	const auto* owner = m_page_table.Find(vaddr >> PageTable::kPageBits);
	if (owner != nullptr && *owner) {
		auto& buffer = m_slot_buffers[*owner];
		if (buffer.IsInBounds(vaddr, size)) {
			TouchBuffer(buffer);
			(void)SynchronizeBuffer(buffer, vaddr, size, false, false);
			return {&buffer, buffer.Offset(vaddr)};
		}
	}
	if (IsRegionGpuModified(vaddr, size)) {
		return ObtainBuffer(vaddr, size, false, false);
	}

	return UploadMemory(m_graphics, m_scheduler, m_staging_buffer, size, 16,
	    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eStorageBuffer,
	    [&](uint8_t* mapped, uint64_t) {
		if (!Libs::LibKernel::Memory::TryReadSparseBacking(vaddr, mapped, size)) {
			EXIT("BufferCache: failed to read mapped guest image backing\n");
		}
	});
}

void BufferCache::FillBuffer(uint64_t vaddr, uint64_t size, uint32_t value, bool is_gds) {
	if ((vaddr & 3u) != 0 || size == 0 || (size & 3u) != 0 || size > UINT64_MAX - vaddr) {
		EXIT("BufferCache: fill range must be dword aligned\n");
	}
	if (is_gds) {
		if (vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - vaddr) {
			EXIT("BufferCache: GDS fill range is out of bounds\n");
		}
		m_gds_buffer.Fill(vaddr, size, value);
		return;
	}
	if (vaddr == 0) {
		EXIT("BufferCache: invalid fill memory address\n");
	}
	(void)m_texture_cache.ClearMeta(vaddr);
	if (!IsRegionGpuModified(vaddr, size)) {
		// Access the guest mapping so write faults invalidate cached buffers and images.
		auto* destination = reinterpret_cast<uint32_t*>(vaddr);
		std::fill(destination, destination + size / sizeof(uint32_t), value);
		return;
	}

	m_texture_cache.InvalidateMemoryFromGPU(vaddr, size);
	FpsProbe::g_producer = FpsProbe::ProducerFill; // PROBE
	auto [dst, dst_offset] = ObtainBuffer(vaddr, size, true, true);
	dst->Fill(dst_offset, size, value);
}

void BufferCache::CopyBuffer(uint64_t dst_vaddr, uint64_t src_vaddr, uint64_t size, bool dst_gds,
                             bool src_gds) {
	const bool dst_memory = !dst_gds;
	const bool src_memory = !src_gds;
	if ((dst_memory && dst_vaddr == 0) || (src_memory && src_vaddr == 0) || size == 0 ||
	    ((dst_gds || src_gds) && ((dst_vaddr | src_vaddr | size) & 3u) != 0) ||
	    size > UINT64_MAX - dst_vaddr || size > UINT64_MAX - src_vaddr || (dst_gds && src_gds) ||
	    (dst_gds && (dst_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - dst_vaddr)) ||
	    (src_gds && (src_vaddr > m_gds_buffer.Size() || size > m_gds_buffer.Size() - src_vaddr))) {
		EXIT("BufferCache: invalid copy range, src=0x%016" PRIx64 " dst=0x%016" PRIx64
		     " size=0x%016" PRIx64 " src_gds=%d dst_gds=%d\n",
		     src_vaddr, dst_vaddr, size, static_cast<int>(src_gds), static_cast<int>(dst_gds));
	}
	// A copy into whole levels of a GPU-written image lands on its GPU contents, not stale guest
	// bytes, while no buffer holds the range.
	if (src_memory && dst_memory && !IsRegionRegistered(dst_vaddr, size) &&
	    m_texture_cache.CopyToImage(dst_vaddr, src_vaddr, size)) {
		return;
	}
	if (src_memory && dst_memory && !IsRegionGpuModified(dst_vaddr, size) &&
	    !IsRegionGpuModified(src_vaddr, size) && !m_texture_cache.FindImageFromRange(src_vaddr, size)) {
		std::memcpy(reinterpret_cast<void*>(dst_vaddr), reinterpret_cast<const void*>(src_vaddr),
		            size);
		return;
	}

	auto& command = m_scheduler.Current();
	if (dst_memory) {
		m_texture_cache.InvalidateMemoryFromGPU(dst_vaddr, size);
	}
	const auto src_id      = src_memory ? FindBuffer(src_vaddr, size) : BufferId {};
	const auto dst_id      = dst_memory ? FindBuffer(dst_vaddr, size) : BufferId {};
	auto [src, src_offset] = src_memory ? ObtainBuffer(src_vaddr, size, false, true, src_id)
	                                    : std::pair {&m_gds_buffer, src_vaddr};
	FpsProbe::g_producer = FpsProbe::ProducerCopy; // PROBE
	auto [dst, dst_offset] = dst_memory ? ObtainBuffer(dst_vaddr, size, true, true, dst_id)
	                                    : std::pair {&m_gds_buffer, dst_vaddr};
	dst->CopyFrom(command, *src, src_offset, dst_offset, size);
}

bool BufferCache::IsRegionRegistered(uint64_t vaddr, uint64_t size) {
	if (!GuestRange {vaddr, size}.Valid()) {
		EXIT("BufferCache: invalid registered-region query\n");
	}
	// Cached buffers are ordered and non-overlapping. The last buffer beginning before the query
	// end is therefore the only possible intersection.
	const auto candidate = m_buffers.lower_bound(vaddr + size);
	if (candidate == m_buffers.begin()) {
		return false;
	}
	const auto& [address, id] = *std::prev(candidate);
	return address + m_slot_buffers[id].Size() > vaddr;
}

void BufferCache::MarkColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0) {
			m_checked_color_metadata[slot].Add(vaddr, size);
		}
	}
}

bool BufferCache::IsColorMetadataChecked(uint64_t vaddr, uint64_t size, uint32_t keys) const {
	for (uint32_t slot = 0; slot < m_checked_color_metadata.size(); slot++) {
		if ((keys & (1u << slot)) != 0 && !m_checked_color_metadata[slot].Contains(vaddr, size)) {
			return false;
		}
	}
	return true;
}

void BufferCache::ForgetColorMetadataChecks(uint64_t vaddr, uint64_t size) {
	for (auto& checked: m_checked_color_metadata) {
		checked.Subtract(vaddr, size);
	}
}

void BufferCache::AdoptGuestMemory(uint64_t vaddr, uint64_t size) {
	m_gpu_modified_ranges.Subtract(vaddr, size);
	ForgetColorMetadataChecks(vaddr, size);
	// Dirty state is tracked per page. A page shared with GPU-written bytes outside the range
	// keeps them.
	auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	if (begin < vaddr && m_gpu_modified_ranges.Intersects(begin, TRACKER_PAGE_SIZE)) {
		begin += TRACKER_PAGE_SIZE;
	}
	if (vaddr + size < end && end - begin >= TRACKER_PAGE_SIZE &&
	    m_gpu_modified_ranges.Intersects(end - TRACKER_PAGE_SIZE, TRACKER_PAGE_SIZE)) {
		end -= TRACKER_PAGE_SIZE;
	}
	if (begin >= end) {
		return;
	}
	m_memory_tracker.UnmarkRegionAsGpuModified(begin, end - begin);
	m_memory_tracker.MarkRegionAsCpuModified(begin, end - begin);
	QueueBdaSync(begin, end - begin, true);
}

bool BufferCache::IsRegionGpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionGpuModified(vaddr, size);
}

bool BufferCache::HasGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Intersects(vaddr, size) || HasEagerDownloads(vaddr, size);
}

bool BufferCache::HasEagerDownloads(uint64_t vaddr, uint64_t size) const {
	return m_eager_pending.Intersects(vaddr, size);
}

bool BufferCache::TrySpeculativeWalkerLoad(uint64_t vaddr, uint32_t& value) {
	static const bool enabled = std::getenv("KYTY_SRT_SPECULATE") != nullptr; // ABTEST
	if (!enabled) {
		return false;
	}
	if (!StableWord(vaddr) || !LibKernel::Memory::TryReadBacking(vaddr, &value, sizeof(value))) {
		return false;
	}
	m_speculations.insert_or_assign(vaddr, value);
	ProbeSpeculation(true); // FPSPROBE
	return true;
}

bool BufferCache::ReadTable(uint64_t vaddr, uint64_t size, void* data) {
	if (!IsRegionGpuModified(vaddr, size) && !HasEagerDownloads(vaddr, size)) {
		return LibKernel::Memory::TryReadBacking(vaddr, data, size);
	}
	static const bool speculate = std::getenv("KYTY_SRT_SPECULATE") != nullptr; // ABTEST
	const auto words_count = size / sizeof(uint32_t);
	bool       stable      = speculate && size % sizeof(uint32_t) == 0 && vaddr % 4 == 0;
	for (uint64_t i = 0; stable && i < words_count; i++) {
		stable = StableWord(vaddr + i * sizeof(uint32_t));
	}
	if (stable) {
		if (!LibKernel::Memory::TryReadBacking(vaddr, data, size)) {
			return false;
		}
		const auto* words = static_cast<const uint32_t*>(data);
		for (uint64_t i = 0; i < size / sizeof(uint32_t); i++) {
			m_speculations.insert_or_assign(vaddr + i * sizeof(uint32_t), words[i]);
		}
		ProbeSpeculation(true); // FPSPROBE
		return true;
	}
	std::vector<uint8_t> before(size);
	const bool           known = LibKernel::Memory::TryReadBacking(vaddr, before.data(), size);
	ReadMemory(vaddr, size);
	if (!LibKernel::Memory::TryReadBacking(vaddr, data, size)) {
		return false;
	}
	if (known && vaddr % 4 == 0) {
		const auto* after = static_cast<const uint8_t*>(data);
		for (uint64_t i = 0; i < words_count; i++) {
			LearnWord(vaddr + i * sizeof(uint32_t),
			          std::memcmp(before.data() + i * 4, after + i * 4, 4) == 0);
		}
	}
	return true;
}

void BufferCache::LearnWalkerWord(uint64_t vaddr, uint32_t before, uint32_t after) {
	LearnWord(vaddr, before == after);
}

bool BufferCache::StableWord(uint64_t address) const {
	constexpr uint16_t StableReads = 3;
	constexpr uint16_t MaxMisses   = 2;
	const auto         it          = m_word_stability.find(address);
	return it != m_word_stability.end() && it->second.same >= StableReads &&
	       it->second.misses < MaxMisses;
}

void BufferCache::LearnWord(uint64_t address, bool same) {
	auto& word = m_word_stability[address];
	word.same  = same ? static_cast<uint16_t>(std::min<uint32_t>(word.same + 1u, 0xffffu)) : 0;
}

void BufferCache::VerifySpeculations() {
	std::erase_if(m_speculations, [this](const auto& speculation) {
		const auto [address, value] = speculation;
		// Guest memory is current once no GPU write or download of the word is outstanding.
		if (IsRegionGpuModified(address, sizeof(uint32_t)) ||
		    HasEagerDownloads(address, sizeof(uint32_t))) {
			return false;
		}
		uint32_t current = value;
		(void)LibKernel::Memory::TryReadBacking(address, &current, sizeof(current));
		if (current != value) {
			// The word changed: relearn it, and stop speculating on it after a few misses.
			auto& word  = m_word_stability[address];
			word.same   = 0;
			word.misses = static_cast<uint16_t>(std::min<uint32_t>(word.misses + 1u, 0xffffu));
			ProbeSpeculation(false); // FPSPROBE
		}
		return true;
	});
}

void BufferCache::RecordEagerDownloads() {
	static const bool disabled = std::getenv("KYTY_NO_EAGER") != nullptr; // ABTEST
	if (disabled || m_recording_eager || !GuestGpu::IsGpuThread()) {
		return;
	}
	m_recording_eager = true;
	// Pages whose downloads have landed become readable without a fault.
	CompleteEagerDownloads(0, UINT64_MAX, true);
	VerifySpeculations();
	if (!m_eager_writes.Empty()) {
		const auto writes = std::exchange(m_eager_writes, {});
		writes.ForEach([&](uint64_t begin, uint64_t end) {
			for (auto address = begin; address < end;) {
				const auto* owner = m_page_table.Find(address >> PageTable::kPageBits);
				if (owner == nullptr || !*owner) {
					address = std::min(end, Common::AlignDown(address, CACHING_PAGESIZE) +
					                            CACHING_PAGESIZE);
					continue;
				}
				auto&      buffer = m_slot_buffers[*owner];
				const auto last   = std::min(end, buffer.CpuAddress() + buffer.Size());
				// A stream-buffer wrap can submit while recording: the copy's batch is current after.
				if (DownloadBufferMemory<true>(buffer, address, last - address)) {
					m_eager_downloads.push_back({address, last, m_scheduler.CurrentTick()});
					m_eager_pending.Add(address, last - address);
					ProbeEager(last - address); // FPSPROBE
				}
				address = last;
			}
		});
	}
	m_recording_eager = false;
	if (ProbeEnabled()) { // FPSPROBE
		static auto last = std::chrono::steady_clock::now();
		const auto  now  = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(now - last).count() >= 5.0) {
			uint64_t hot = 0;
			m_readback_pages.ForEach([&](uint64_t begin, uint64_t end) { hot += end - begin; });
			std::printf("[eager] pending=%zu hot_bytes=%llu downloads=%llu bytes=%llu\n",
			            m_eager_downloads.size(), static_cast<unsigned long long>(hot),
			            static_cast<unsigned long long>(ProbeEager(0, true)),
			            static_cast<unsigned long long>(ProbeEager(0, false, true)));
			std::fflush(stdout);
			last = now;
		}
	}
}

void BufferCache::CompleteEagerDownloads(uint64_t vaddr, uint64_t size, bool finished_only) {
	if (m_eager_downloads.empty()) {
		return;
	}
	const auto end = size > UINT64_MAX - vaddr ? UINT64_MAX : vaddr + size;
	if (!m_eager_pending.Intersects(vaddr, std::max<uint64_t>(end - vaddr, 1))) {
		return;
	}
	m_scheduler.GetMasterSemaphore().Refresh();
	RangeSet released;
	std::erase_if(m_eager_downloads, [&](const EagerDownload& download) {
		if (download.end <= vaddr || end <= download.begin ||
		    (finished_only && !m_scheduler.IsFree(download.tick))) {
			return false;
		}
		// The download was recorded before its batch was submitted, so no flush is needed.
		m_scheduler.Wait(download.tick);
		m_scheduler.WaitPriorityOperations(download.tick);
		const auto first = Common::AlignDown(download.begin, TRACKER_PAGE_SIZE);
		released.Add(first, Common::AlignUp(download.end, TRACKER_PAGE_SIZE) - first);
		return true;
	});
	if (released.Empty()) {
		return;
	}
	m_eager_pending = {};
	for (const auto& download: m_eager_downloads) {
		m_eager_pending.Add(download.begin, download.end - download.begin);
	}
	// A page keeps its protection while it holds GPU writes guest memory lacks.
	RangeSet keep;
	released.ForEach([&](uint64_t begin, uint64_t last) {
		m_gpu_modified_ranges.ForEachInRange(begin, last - begin, [&](uint64_t first, uint64_t stop) {
			const auto page = Common::AlignDown(first, TRACKER_PAGE_SIZE);
			keep.Add(page, Common::AlignUp(stop, TRACKER_PAGE_SIZE) - page);
		});
	});
	for (const auto& download: m_eager_downloads) {
		const auto first = Common::AlignDown(download.begin, TRACKER_PAGE_SIZE);
		keep.Add(first, Common::AlignUp(download.end, TRACKER_PAGE_SIZE) - first);
	}
	released.ForEach([&](uint64_t begin, uint64_t last) {
		RangeSet clean;
		clean.Add(begin, last - begin);
		keep.ForEachInRange(begin, last - begin, [&](uint64_t first, uint64_t stop) {
			clean.Subtract(first, stop - first);
		});
		clean.ForEach([&](uint64_t first, uint64_t stop) {
			m_memory_tracker.UnmarkRegionAsGpuModified(first, stop - first);
		});
	});
}

bool BufferCache::HasOnlyGpuDirtyBytes(uint64_t vaddr, uint64_t size) {
	return m_gpu_modified_ranges.Contains(vaddr, size);
}

bool BufferCache::IsRegionCpuModified(uint64_t vaddr, uint64_t size) {
	return m_memory_tracker.IsRegionCpuModified(vaddr, size);
}

void BufferCache::RunGarbageCollector() {
	const auto tick = m_gc_tick++;
	if (m_graphics.CanReportMemoryUsage()) {
		m_total_used_memory = m_graphics.GetDeviceMemoryUsage();
	}
	if (m_total_used_memory < m_trigger_gc_memory) {
		return;
	}

	const bool     aggressive = m_total_used_memory >= m_critical_gc_memory;
	const uint64_t age        = std::min<uint64_t>(aggressive ? 80 : 160, tick);
	const size_t   limit      = aggressive ? 64 : 32;

	std::vector<BufferId> dirty_buffers;
	size_t                retire_count = 0;
	m_lru_cache.ForEachItemBelow(tick - age, [&](BufferId id) {
		auto& buffer = m_slot_buffers[id];
		if (HasEagerDownloads(buffer.CpuAddress(), buffer.Size())) {
			return false;
		}
		m_memory_tracker.ValidateGpuDirtyOwnership(m_gpu_modified_ranges, buffer.CpuAddress(),
		                                           buffer.Size(), "garbage collection");
		const bool dirty = m_memory_tracker.IsRegionGpuModified(buffer.CpuAddress(), buffer.Size());
		if (dirty && !aggressive) {
			return false;
		}
		if (dirty) {
			EXIT_NOT_IMPLEMENTED(!DownloadBufferMemory<true>(buffer, buffer.CpuAddress(), buffer.Size()));
			dirty_buffers.push_back(id);
		} else {
			m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
			DeleteBuffer(id);
			Common::DebugCounters::Add(Common::DebugCounters::Counter::BufferEvictions);
		}
		return ++retire_count == limit;
	});
	if (dirty_buffers.empty()) {
		return;
	}

	// Publish all queued downloads before releasing their tracked pages and owners.
	const auto completion_tick = m_scheduler.CurrentTick();
	m_scheduler.Wait(completion_tick);
	m_scheduler.WaitPriorityOperations(completion_tick);
	for (const auto id: dirty_buffers) {
		auto& buffer = m_slot_buffers[id];
		m_memory_tracker.UnmarkRegionAsGpuModified(buffer.CpuAddress(), buffer.Size());
		m_memory_tracker.MarkRegionAsCpuModified(buffer.CpuAddress(), buffer.Size());
		Unregister(id);
		m_slot_buffers.erase(id);
	}
}

void BufferCache::ProcessFaultBuffer() {
	m_fault_manager.ProcessFaultBuffer();
}

void BufferCache::QueueBdaSync(uint64_t vaddr, uint64_t size, bool is_write) {
	const auto begin = Common::AlignDown(vaddr, TRACKER_PAGE_SIZE);
	const auto end   = Common::AlignUp(vaddr + size, TRACKER_PAGE_SIZE);
	std::lock_guard lock(m_bda_sync_mutex);
	if (m_host_view) {
		// Guest memory holds the newest bytes after a CPU write; a new mapping gets entries.
		const auto page_begin = Common::AlignDown(begin, CACHING_PAGESIZE);
		const auto page_end   = Common::AlignUp(end, CACHING_PAGESIZE);
		if (!is_write) {
			m_bda_entry_pending.Add(page_begin, page_end - page_begin);
		} else {
			// The cache-buffer copy stays mapped and takes the CPU bytes before the next DMA draw
			// (SynchronizeBda), so shaders keep reading VRAM; pages with GPU bytes stay owned.
			for (auto page = page_begin; page < page_end; page += CACHING_PAGESIZE) {
				if (m_gpu_owned.Contains(page, CACHING_PAGESIZE) &&
				    !(GuestRange {page, CACHING_PAGESIZE}.Valid() &&
				      m_memory_tracker.IsRegionGpuModified(page, CACHING_PAGESIZE))) {
					m_gpu_owned.Subtract(page, CACHING_PAGESIZE);
				}
				// TEST (temporary): KYTY_CPU_WRITE_HOST=1 sends CPU-written pages to guest memory.
				static const bool live = std::getenv("KYTY_CPU_WRITE_HOST") != nullptr;
				if (live && !m_gpu_owned.Contains(page, CACHING_PAGESIZE)) {
					m_bda_entry_pending.Add(page, CACHING_PAGESIZE);
				} else {
					m_bda_sync.Add(page, CACHING_PAGESIZE);
				}
			}
		}
		m_bda_sync_pending.store(true, std::memory_order_release);
	}
	if (!is_write) {
		m_bda_sync.Add(begin, end - begin);
		m_bda_sync_pending.store(true, std::memory_order_release);
		return;
	}
	m_dma_accessed.ForEachInRange(begin, end - begin, [this](uint64_t first, uint64_t last) {
		m_bda_sync.Add(first, last - first);
		m_bda_sync_pending.store(true, std::memory_order_release);
	});
}

void BufferCache::MarkDmaAccess(const RangeSet& pages) {
	if (HostViewEnabled()) {
		// Host-view entries carry the accessed tag, so a fault bit is a store or an unbacked page.
		RangeSet unbacked;
		pages.ForEach([&](uint64_t begin, uint64_t end) {
			for (auto address = begin; address < end; address += CACHING_PAGESIZE) {
				if (!GuestRange {address, CACHING_PAGESIZE}.Valid()) continue;
				bool owned = false;
				{
					std::lock_guard lock(m_bda_sync_mutex);
					owned = m_gpu_owned.Contains(address, CACHING_PAGESIZE);
				}
				// The store reached the GPU-owned buffer, or the page holds GPU bytes.
				if (owned || m_memory_tracker.IsRegionGpuModified(address, CACHING_PAGESIZE)) continue;
				if (HostAddress(address) == 0) {
					unbacked.Add(address, CACHING_PAGESIZE);
					continue;
				}
				const auto* page_owner = m_page_table.Find(address >> PageTable::kPageBits);
				const bool  device_entry = page_owner != nullptr && *page_owner &&
				                           !m_memory_tracker.IsRegionCpuModified(address, CACHING_PAGESIZE);
				if (device_entry) {
					// Only a store faults through a tagged cache-buffer entry: the buffer holds the
					// newest bytes now.
					auto& owner_buffer = m_slot_buffers[*page_owner];
					(void)SynchronizeBuffer(owner_buffer, address, CACHING_PAGESIZE, true, false);
					m_gpu_modified_ranges.Add(address, CACHING_PAGESIZE);
					std::lock_guard lock(m_bda_sync_mutex);
					m_gpu_owned.Add(address, CACHING_PAGESIZE);
					continue;
				}
				// A read or store through guest memory: (re)load the page into a cache buffer, whose
				// upload sends later reads there (UploadCopies).
				m_memory_tracker.MarkRegionAsCpuModified(address, CACHING_PAGESIZE);
				auto& buffer = m_slot_buffers[FindBuffer(address, CACHING_PAGESIZE)];
				(void)SynchronizeBuffer(buffer, address, CACHING_PAGESIZE, false, false);
			}
		});
		if (unbacked.Empty()) return;
		MarkDmaAccessUnbacked(unbacked);
		return;
	}
	MarkDmaAccessUnbacked(pages);
}

void BufferCache::MarkDmaAccessUnbacked(const RangeSet& pages) {
	{
		std::lock_guard lock(m_bda_sync_mutex);
		pages.ForEach([this](uint64_t begin, uint64_t end) {
			m_dma_accessed.Add(begin, end - begin);
			m_bda_sync.Add(begin, end - begin);
		});
		m_bda_sync_pending.store(true, std::memory_order_release);
	}
	RangeSet                       unbacked;
	std::vector<vk::DeviceAddress> entries;
	pages.ForEach([&](uint64_t begin, uint64_t end) {
		entries.clear();
		for (auto address = begin; address < end; address += CACHING_PAGESIZE) {
			const auto* owner = m_page_table.Find(address >> PageTable::kPageBits);
			if (owner == nullptr || !*owner) {
				LOGF("Accessed non-GPU cached memory at 0x%016" PRIx64 "\n", address);
				unbacked.Add(address, CACHING_PAGESIZE);
				entries.push_back(0);
				continue;
			}
			const auto& buffer = m_slot_buffers[*owner];
			entries.push_back(buffer.BufferDeviceAddress() + buffer.Offset(address) + BDA_ACCESSED);
		}
		if (!unbacked.Contains(begin, end - begin)) {
			WriteDataBuffer(m_bda_pagetable_buffer, PageIndex(begin) * sizeof(vk::DeviceAddress),
			                entries.data(), entries.size() * sizeof(vk::DeviceAddress));
		}
	});
	unbacked.ForEach(
	    [this](uint64_t start, uint64_t end) { (void)FindBuffer(start, end - start); });
}

void BufferCache::SynchronizeBda(const RangeSet& mapped) {
	if (!m_bda_sync_pending.exchange(false, std::memory_order_acq_rel)) {
		return;
	}
	const auto queued = [this] {
		std::lock_guard lock(m_bda_sync_mutex);
		return std::exchange(m_bda_sync, {});
	}();
	std::vector<vk::BufferCopy> copies;
	std::vector<BufferUpload>   uploads;
	uint64_t                    total_size = 0;
	queued.ForEach([&](uint64_t begin, uint64_t end) {
		mapped.ForEachInRange(begin, end - begin, [&](uint64_t start, uint64_t finish) {
			auto it = m_buffers.upper_bound(start);
			if (it != m_buffers.begin()) {
				--it;
			}
			for (; it != m_buffers.end() && it->first < finish; ++it) {
				auto&      buffer = m_slot_buffers[it->second];
				const auto first  = std::max(buffer.CpuAddress(), start);
				const auto last   = std::min(buffer.CpuAddress() + buffer.Size(), finish);
				if (first < last) {
					// Reads follow every range's protection; a write in between re-queues its page.
					m_memory_tracker.ForEachUploadRange(
					    first, last - first, false,
					    [&](uint64_t address, uint64_t bytes) noexcept {
						    if (uploads.empty() || uploads.back().buffer != &buffer) {
							    uploads.push_back({&buffer, 0});
						    }
						    copies.emplace_back(total_size, buffer.Offset(address), bytes);
						    uploads.back().end = copies.size();
						    total_size += bytes;
					    },
					    []() noexcept {});
				}
			}
		});
	});
	UploadCopies(uploads, copies, total_size);
	if (m_host_view) {
		// After the uploads, so pages they made current read their buffers at once.
		const auto entries = [this] {
			std::lock_guard lock(m_bda_sync_mutex);
			return std::exchange(m_bda_entry_pending, {});
		}();
		entries.ForEach([&](uint64_t begin, uint64_t end) {
			mapped.ForEachInRange(begin, end - begin, [&](uint64_t start, uint64_t finish) {
				WriteBdaEntries(Common::AlignDown(start, CACHING_PAGESIZE),
				                Common::AlignUp(finish, CACHING_PAGESIZE));
			});
		});
		FlushBdaEntries();
	}
}

void BufferCache::InitHostView() {
	if (!ShaderRecompiler::IR::GpuSrtEnabled()) {
		return;
	}
	if (!m_graphics.external_memory_host_enabled) {
		std::printf("[hostview] VK_EXT_external_memory_host is not available: GPU-SRT reads use cache buffers\n");
		std::fflush(stdout);
		return;
	}
	vk::PhysicalDeviceExternalMemoryHostPropertiesEXT host {};
	vk::PhysicalDeviceProperties2                     properties {};
	properties.pNext = &host;
	m_graphics.physical_device.getProperties2(&properties);
	m_graphics.host_import_alignment = host.minImportedHostPointerAlignment;
	const auto base = LibKernel::Memory::GuestBackingBase();
	const auto size = LibKernel::Memory::GuestBackingSize();
	if (base == 0 || size == 0 || host.minImportedHostPointerAlignment == 0 ||
	    ((uint64_t {1} << HostChunkBits) % host.minImportedHostPointerAlignment) != 0 ||
	    (base % host.minImportedHostPointerAlignment) != 0) {
		std::printf("[hostview] guest backing 0x%llx+0x%llx cannot be imported (alignment %llu)\n",
		            static_cast<unsigned long long>(base), static_cast<unsigned long long>(size),
		            static_cast<unsigned long long>(host.minImportedHostPointerAlignment));
		std::fflush(stdout);
		return;
	}
	m_host_view = true;
	ShaderRecompiler::IR::SetHostBdaWrites(true);
	std::printf("[hostview] enabled: guest backing 0x%llx+0x%llx, import alignment %llu, chunk %llu MiB\n",
	            static_cast<unsigned long long>(base), static_cast<unsigned long long>(size),
	            static_cast<unsigned long long>(host.minImportedHostPointerAlignment),
	            static_cast<unsigned long long>((uint64_t {1} << HostChunkBits) >> 20u));
	std::fflush(stdout);
}

void BufferCache::DestroyHostView() {
	for (auto& [index, chunk]: m_host_chunks) {
		(void)index;
		if (chunk.buffer) m_graphics.device.destroyBuffer(chunk.buffer, nullptr);
		if (chunk.memory) m_graphics.device.freeMemory(chunk.memory, nullptr);
	}
	m_host_chunks.clear();
}

vk::DeviceAddress BufferCache::HostAddress(uint64_t vaddr) {
	uint64_t offset = 0;
	if (!m_host_view || !LibKernel::Memory::GuestBackingOffset(vaddr, &offset)) {
		return 0;
	}
	const auto index = offset >> HostChunkBits;
	auto [found, inserted] = m_host_chunks.try_emplace(index);
	auto& chunk = found->second;
	if (inserted) {
		const auto backing = LibKernel::Memory::GuestBackingBase();
		const auto bytes   = std::min<uint64_t>(uint64_t {1} << HostChunkBits,
		                                        LibKernel::Memory::GuestBackingSize() - (index << HostChunkBits));
		auto* pointer = reinterpret_cast<void*>(backing + (index << HostChunkBits));
		const auto fail = [&](const char* step, vk::Result result) {
			std::printf("[hostview] import of backing chunk %llu failed at %s: %s; GPU-SRT reads of it use cache buffers\n",
			            static_cast<unsigned long long>(index), step, vk::to_string(result).c_str());
			std::fflush(stdout);
			if (chunk.buffer) m_graphics.device.destroyBuffer(chunk.buffer, nullptr);
			if (chunk.memory) m_graphics.device.freeMemory(chunk.memory, nullptr);
			chunk = {};
		};
		auto get_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
		    m_graphics.device.getProcAddr("vkGetMemoryHostPointerPropertiesEXT"));
		VkMemoryHostPointerPropertiesEXT pointer_properties {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
		auto result = get_properties == nullptr ? vk::Result::eErrorExtensionNotPresent
		    : static_cast<vk::Result>(get_properties(
		          m_graphics.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer,
		          &pointer_properties));
		if (result != vk::Result::eSuccess) {
			fail("vkGetMemoryHostPointerPropertiesEXT", result);
		} else {
			vk::ExternalMemoryBufferCreateInfo external {};
			external.handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
			vk::BufferCreateInfo info {};
			info.pNext = &external;
			info.size  = bytes;
			info.usage = vk::BufferUsageFlagBits::eStorageBuffer |
			             vk::BufferUsageFlagBits::eShaderDeviceAddress |
			             vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
			info.sharingMode = vk::SharingMode::eExclusive;
			result = m_graphics.device.createBuffer(&info, nullptr, &chunk.buffer);
			vk::MemoryRequirements requirements {};
			if (result == vk::Result::eSuccess) {
				m_graphics.device.getBufferMemoryRequirements(chunk.buffer, &requirements);
			}
			const auto types = requirements.memoryTypeBits & pointer_properties.memoryTypeBits;
			if (result != vk::Result::eSuccess) {
				fail("vkCreateBuffer", result);
			} else if (types == 0) {
				fail("memory type selection", vk::Result::eErrorFormatNotSupported);
			} else {
				vk::ImportMemoryHostPointerInfoEXT import {};
				import.handleType   = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT;
				import.pHostPointer = pointer;
				vk::MemoryAllocateFlagsInfo flags {};
				flags.pNext = &import;
				flags.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
				vk::MemoryAllocateInfo allocate {};
				allocate.pNext           = &flags;
				allocate.allocationSize  = bytes;
				allocate.memoryTypeIndex = static_cast<uint32_t>(std::countr_zero(types));
				result = m_graphics.device.allocateMemory(&allocate, nullptr, &chunk.memory);
				if (result != vk::Result::eSuccess) {
					fail("vkAllocateMemory", result);
				} else if (result = m_graphics.device.bindBufferMemory(chunk.buffer, chunk.memory, 0);
				           result != vk::Result::eSuccess) {
					fail("vkBindBufferMemory", result);
				} else {
					vk::BufferDeviceAddressInfo address {};
					address.buffer = chunk.buffer;
					chunk.address  = m_graphics.device.getBufferAddress(&address);
					static uint64_t imported = 0;
					imported += bytes;
					std::printf("[hostview] imported backing chunk %llu (%llu MiB in total)\n",
					            static_cast<unsigned long long>(index),
					            static_cast<unsigned long long>(imported >> 20u));
					std::fflush(stdout);
				}
			}
		}
	}
	return chunk.address == 0 ? 0 : chunk.address + (offset & ((uint64_t {1} << HostChunkBits) - 1u));
}

static uint64_t g_probe_device_entries = 0; // PROBE
static uint64_t g_probe_host_entries   = 0; // PROBE

void BufferCache::WriteBdaEntries(uint64_t begin, uint64_t end) {
	{ // PROBE: entries written per 5 s by target.
		static auto last = std::chrono::steady_clock::now();
		if (std::chrono::duration<double>(std::chrono::steady_clock::now() - last).count() >= 5.0) {
			std::printf("[hostview] entries written: device=%llu host=%llu\n",
			            static_cast<unsigned long long>(g_probe_device_entries),
			            static_cast<unsigned long long>(g_probe_host_entries));
			std::fflush(stdout);
			g_probe_device_entries = g_probe_host_entries = 0;
			last = std::chrono::steady_clock::now();
		}
	}
	begin = Common::AlignDown(begin, CACHING_PAGESIZE);
	end   = Common::AlignUp(end, CACHING_PAGESIZE);
	if (begin >= end) return;
	RangeSet owned;
	{
		std::lock_guard lock(m_bda_sync_mutex);
		m_gpu_owned.ForEachInRange(begin, end - begin, [&](uint64_t first, uint64_t last) {
			owned.Add(first, last - first);
		});
	}
	for (auto address = begin; address < end; address += CACHING_PAGESIZE) {
		const auto* owner  = m_page_table.Find(address >> PageTable::kPageBits);
		const bool  cached = owner != nullptr && *owner && !m_slot_buffers[*owner].is_deleted;
		vk::DeviceAddress entry = 0;
		// The cache buffer when its copy is current (GPU-owned, or no CPU write since its upload);
		// otherwise guest memory itself, untagged, so the first access asks for a buffer.
		if (cached && (owned.Contains(address, CACHING_PAGESIZE) ||
		               !m_memory_tracker.IsRegionCpuModified(address, CACHING_PAGESIZE))) {
			const auto& buffer = m_slot_buffers[*owner];
			entry = buffer.BufferDeviceAddress() + buffer.Offset(address) + BDA_ACCESSED;
			++g_probe_device_entries; // PROBE
		} else if (const auto host = HostAddress(address); host != 0) {
			// TEST (temporary): KYTY_HOST_TAG=1 tags guest-memory entries (no migration faults).
			static const bool tag = std::getenv("KYTY_HOST_TAG") != nullptr;
			entry = host + (tag ? BDA_ACCESSED : 0);
			++g_probe_host_entries; // PROBE
		} else if (cached) {
			// Private memory outside the backing keeps the fault-driven cache buffer path.
			const auto& buffer = m_slot_buffers[*owner];
			std::lock_guard lock(m_bda_sync_mutex);
			entry = buffer.BufferDeviceAddress() + buffer.Offset(address) +
			        (m_dma_accessed.Contains(address, CACHING_PAGESIZE) ? BDA_ACCESSED : 0);
		}
		const auto index = PageIndex(address);
		auto [shadow, inserted] = m_pt_shadow.try_emplace(index, 0);
		if (shadow->second == entry && !inserted) continue;
		if (inserted && entry == 0) continue; // The table starts zeroed.
		shadow->second = entry;
		m_pt_queue.emplace_back(index, entry);
	}
	if (!m_pt_queue.empty()) m_bda_sync_pending.store(true, std::memory_order_release);
}

void BufferCache::FlushBdaEntries() {
	if (m_pt_queue.empty()) return;
	// The last queued value of each page wins; contiguous pages share one copy region.
	std::stable_sort(m_pt_queue.begin(), m_pt_queue.end(),
	                 [](const auto& a, const auto& b) { return a.first < b.first; });
	std::vector<vk::DeviceAddress> data;
	std::vector<vk::BufferCopy>    regions;
	for (size_t i = 0; i < m_pt_queue.size(); ++i) {
		if (i + 1 < m_pt_queue.size() && m_pt_queue[i + 1].first == m_pt_queue[i].first) continue;
		const auto index = m_pt_queue[i].first;
		if (!regions.empty() &&
		    regions.back().dstOffset + regions.back().size == index * sizeof(vk::DeviceAddress)) {
			regions.back().size += sizeof(vk::DeviceAddress);
		} else {
			regions.push_back({data.size() * sizeof(vk::DeviceAddress), index * sizeof(vk::DeviceAddress),
			                   sizeof(vk::DeviceAddress)});
		}
		data.push_back(m_pt_queue[i].second);
	}
	m_pt_queue.clear();
	FpsProbe::CountEvent(FpsProbe::PageTableWrites); // PROBE
	const auto offset = m_staging_buffer.Copy(data.data(), data.size() * sizeof(vk::DeviceAddress), 8);
	for (auto& region: regions) region.srcOffset += offset;
	m_scheduler.EndRendering();
	const auto command = m_scheduler.Current().Handle();
	vk::BufferMemoryBarrier2 before {};
	before.srcStageMask        = vk::PipelineStageFlagBits2::eAllCommands;
	before.srcAccessMask       = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	before.dstStageMask        = vk::PipelineStageFlagBits2::eTransfer;
	before.dstAccessMask       = vk::AccessFlagBits2::eTransferWrite;
	before.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	before.buffer              = m_bda_pagetable_buffer.Handle();
	before.offset              = 0;
	before.size                = VK_WHOLE_SIZE;
	vk::DependencyInfo dependency {};
	dependency.bufferMemoryBarrierCount = 1;
	dependency.pBufferMemoryBarriers    = &before;
	command.pipelineBarrier2(dependency);
	command.copyBuffer(m_staging_buffer.Handle(), m_bda_pagetable_buffer.Handle(),
	                   static_cast<uint32_t>(regions.size()), regions.data());
	auto after          = before;
	after.srcStageMask  = vk::PipelineStageFlagBits2::eTransfer;
	after.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
	after.dstStageMask  = vk::PipelineStageFlagBits2::eAllCommands;
	after.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
	dependency.pBufferMemoryBarriers = &after;
	command.pipelineBarrier2(dependency);
}

void BufferCache::DescribePage(uint64_t vaddr) noexcept { // PROBE
	const auto page = Common::AlignDown(vaddr, CACHING_PAGESIZE);
	if (!GuestRange {page, CACHING_PAGESIZE}.Valid()) {
		std::printf("[page] 0x%llx invalid\n", static_cast<unsigned long long>(vaddr));
		return;
	}
	const auto* owner = m_page_table.Find(page >> PageTable::kPageBits);
	bool owned = false;
	{
		std::lock_guard lock(m_bda_sync_mutex);
		owned = m_gpu_owned.Contains(page, CACHING_PAGESIZE);
	}
	uint64_t offset = 0;
	const bool backed = LibKernel::Memory::GuestBackingOffset(page, &offset);
	std::printf("[page] 0x%llx cached=%d owned=%d gpu_mod=%d cpu_mod=%d gpu_ranges=%d backed=%d\n",
	            static_cast<unsigned long long>(vaddr), owner != nullptr && *owner ? 1 : 0, owned ? 1 : 0,
	            m_memory_tracker.IsRegionGpuModified(page, CACHING_PAGESIZE) ? 1 : 0,
	            m_memory_tracker.IsRegionCpuModified(page, CACHING_PAGESIZE) ? 1 : 0,
	            m_gpu_modified_ranges.Intersects(page, CACHING_PAGESIZE) ? 1 : 0, backed ? 1 : 0);
}

} // namespace Libs::Graphics
