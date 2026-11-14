// FPSPROBE: temporary CPU-time zones for the fps investigation (env KYTY_PERF_LOG).
#ifndef KYTY_COMMON_FPSPROBE_H_
#define KYTY_COMMON_FPSPROBE_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace FpsProbe {

enum Zone {
	Process,
	Draw,
	DrawPrepare,
	Dispatch,
	Bindings,
	Commit,
	Programs,
	Pipeline,
	TexRefresh,
	ReadBack,
	Submit,
	FaultGpu,
	FaultOther,
	WaitGpu,
	WaitOther,
	FindBuf,
	Bda,
	RebindImg,
	RebindBuf,
	Count
};

inline const char* const kNames[Count] = {"process", "draw",    "drawprep", "dispatch",
                                          "bind",    "commit",  "programs", "pipeline",
                                          "texref",  "readback", "submit",  "faultgpu",
                                          "faultoth", "waitgpu", "waitoth", "findbuf", "bda", "rebimg", "rebbuf"};

inline std::atomic<uint64_t> g_ns[Count] {};

// Event counts per report: buffer uploads, BDA page-table writes, heap transitions, bucket uploads.
enum Event { Uploads, PageTableWrites, HeapTransits, BucketUploads, EventCount };
inline const char* const kEventNames[EventCount] = {"uploads", "ptwrites", "transits", "buckets"};
inline std::atomic<uint64_t> g_events[EventCount] {};
inline void CountEvent(Event event) { g_events[event].fetch_add(1, std::memory_order_relaxed); }

// PROBE: program (shader hash) or pseudo producer (copy/fill) now writing buffers (GPU thread).
inline constexpr uint64_t ProducerCopy = 0xC0C0C0C0C0C0C0C0ull;
inline constexpr uint64_t ProducerFill = 0xF1F1F1F1F1F1F1F1ull;
inline thread_local uint64_t g_producer = 0;

inline bool Enabled() {
	static const bool enabled = std::getenv("KYTY_PERF_LOG") != nullptr;
	return enabled;
}

inline uint64_t NowNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

class Scope {
public:
	explicit Scope(Zone zone): m_zone(zone), m_start(Enabled() ? NowNs() : 0) {}
	~Scope() {
		if (m_start != 0) {
			g_ns[m_zone].fetch_add(NowNs() - m_start, std::memory_order_relaxed);
		}
	}
	Scope(const Scope&)            = delete;
	Scope& operator=(const Scope&) = delete;

private:
	Zone     m_zone;
	uint64_t m_start;
};

// Milliseconds per second of each zone since the last report.
inline std::string Report(double seconds) {
	std::string text;
	char        item[64];
	for (int zone = 0; zone < Count; zone++) {
		const auto ns = g_ns[zone].exchange(0, std::memory_order_relaxed);
		std::snprintf(item, sizeof(item), " %s=%.0f", kNames[zone], ns / 1e6 / seconds);
		text += item;
	}
	for (int event = 0; event < EventCount; event++) {
		const auto count = g_events[event].exchange(0, std::memory_order_relaxed);
		std::snprintf(item, sizeof(item), " %s=%.0f", kEventNames[event], count / seconds);
		text += item;
	}
	return text;
}

} // namespace FpsProbe

#endif /* KYTY_COMMON_FPSPROBE_H_ */
