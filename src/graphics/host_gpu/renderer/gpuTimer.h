// PROBE (temporary): GPU time per program from timestamp pairs (env KYTY_PROFILE_GPU_TIME).
#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_

#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/commandScheduler.h"
#include "graphics/host_gpu/renderer/render.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <utility>
#include <vector>

namespace Libs::Graphics::GpuTimer {

inline bool Enabled() {
	static const bool enabled = std::getenv("KYTY_PROFILE_GPU_TIME") != nullptr;
	return enabled;
}

struct State {
	static constexpr uint32_t Queries = 32768;
	struct Entry {
		uint64_t key0 = 0;
		uint64_t key1 = 0;
		uint64_t tick = 0;
		uint32_t query = 0;
	};
	vk::Device         device = nullptr;
	vk::QueryPool      pool   = nullptr;
	double             period = 1.0;
	uint32_t           next   = 0;
	std::deque<Entry>  pending;
	std::map<std::pair<uint64_t, uint64_t>, std::pair<double, uint32_t>> totals;
	double                                   all = 0;
	std::chrono::steady_clock::time_point    last = std::chrono::steady_clock::now();
};

inline State& Get() {
	static State state;
	return state;
}

inline void Collect(CommandScheduler& scheduler) {
	auto& s = Get();
	while (!s.pending.empty() && scheduler.IsFree(s.pending.front().tick)) {
		const auto entry = s.pending.front();
		s.pending.pop_front();
		uint64_t values[2] {};
		if (s.device.getQueryPoolResults(s.pool, entry.query, 2, sizeof(values), values, sizeof(uint64_t),
		                                 vk::QueryResultFlagBits::e64) == vk::Result::eSuccess) {
			const auto ms = static_cast<double>(values[1] - values[0]) * s.period / 1e6;
			auto& total = s.totals[{entry.key0, entry.key1}];
			total.first += ms;
			total.second++;
			s.all += ms;
		}
		s.device.resetQueryPool(s.pool, entry.query, 2);
	}
	const auto now = std::chrono::steady_clock::now();
	if (std::chrono::duration<double>(now - s.last).count() < 5.0) return;
	std::vector<std::pair<double, std::pair<std::pair<uint64_t, uint64_t>, uint32_t>>> top;
	for (const auto& [key, total]: s.totals) top.push_back({total.first, {key, total.second}});
	std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	std::printf("[gputime] all=%.0f ms over 5 s\n", s.all);
	for (size_t i = 0; i < std::min<size_t>(top.size(), 12); i++) {
		std::printf("[gputime] ms=%.0f n=%u a=%016llx b=%016llx\n", top[i].first, top[i].second.second,
		            static_cast<unsigned long long>(top[i].second.first.first),
		            static_cast<unsigned long long>(top[i].second.first.second));
	}
	std::fflush(stdout);
	s.totals.clear();
	s.all  = 0;
	s.last = now;
}

class Scope {
public:
	Scope(CommandBuffer& buffer, CommandScheduler& scheduler, uint64_t key0, uint64_t key1)
	    : m_buffer(buffer), m_scheduler(scheduler), m_key0(key0), m_key1(key1) {
		if (!Enabled()) return;
		auto& s = Get();
		if (s.pool == nullptr) {
			auto& graphics = buffer.GetGraphics();
			s.device       = graphics.device;
			s.period       = graphics.physical_device_properties.limits.timestampPeriod;
			vk::QueryPoolCreateInfo info {};
			info.queryType  = vk::QueryType::eTimestamp;
			info.queryCount = State::Queries;
			if (s.device.createQueryPool(&info, nullptr, &s.pool) != vk::Result::eSuccess) return;
			s.device.resetQueryPool(s.pool, 0, State::Queries);
		}
		Collect(scheduler);
		if (s.pending.size() * 2 + 2 >= State::Queries) return;
		m_query  = s.next;
		s.next   = (s.next + 2) % State::Queries;
		m_handle = buffer.Handle();
		m_handle.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, s.pool, m_query);
		m_active = true;
	}
	~Scope() {
		if (!m_active || m_buffer.Handle() != m_handle) return;
		auto& s = Get();
		m_handle.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, s.pool, m_query + 1);
		s.pending.push_back({m_key0, m_key1, m_scheduler.CurrentTick(), m_query});
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	CommandBuffer&    m_buffer;
	CommandScheduler& m_scheduler;
	uint64_t          m_key0;
	uint64_t          m_key1;
	vk::CommandBuffer m_handle = nullptr;
	uint32_t          m_query  = 0;
	bool              m_active = false;
};

} // namespace Libs::Graphics::GpuTimer

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_GPUTIMER_H_
