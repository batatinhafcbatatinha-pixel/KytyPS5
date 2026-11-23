#include "common/hostException.h"
#include "common/virtualMemory.h"
#include "graphics/host_gpu/memoryTracker.h"
#include "graphics/host_gpu/rangeSet.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#include <csignal>
#include <limits.h>
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <csignal>
#include <map>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using Libs::Graphics::GuestRange;
using Libs::Graphics::MemoryTracker;
using Libs::Graphics::PageManager;
using Libs::Graphics::RangeSet;
using Libs::Graphics::TRACKER_ADDRESS_SIZE;

void Check(bool value, const char *text) {
  if (!value) {
    std::fprintf(stderr, "MemoryTrackerTests: failed: %s\n", text);
    std::abort();
  }
}

#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
using DWORD = uint32_t;
constexpr uint32_t PAGE_NOACCESS = 1;
constexpr uint32_t PAGE_READONLY = 2;
constexpr uint32_t PAGE_READWRITE = 3;
constexpr uint32_t MEM_RESERVE = 0;
constexpr uint32_t MEM_COMMIT = 0;
constexpr uint32_t MEM_RELEASE = 0;

int ToHostProt(uint32_t protection) {
  switch (protection) {
  case PAGE_NOACCESS:
    return PROT_NONE;
  case PAGE_READONLY:
    return PROT_READ;
  default:
    return PROT_READ | PROT_WRITE;
  }
}

uint32_t Protection(const void *address) {
#if defined(__APPLE__)
  mach_vm_address_t region_address =
      reinterpret_cast<mach_vm_address_t>(address);
  mach_vm_size_t region_size = 0;
  vm_region_basic_info_data_64_t info{};
  mach_msg_type_number_t info_count = VM_REGION_BASIC_INFO_COUNT_64;
  mach_port_t object_name = MACH_PORT_NULL;
  Check(mach_vm_region(mach_task_self(), &region_address, &region_size,
                       VM_REGION_BASIC_INFO_64,
                       reinterpret_cast<vm_region_info_t>(&info), &info_count,
                       &object_name) == KERN_SUCCESS,
        "mach_vm_region failed");
  if (object_name != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), object_name);
  }
  return (info.protection & VM_PROT_WRITE) != 0
             ? PAGE_READWRITE
             : (info.protection & VM_PROT_READ) != 0 ? PAGE_READONLY
                                                     : PAGE_NOACCESS;
#else
  const auto addr = reinterpret_cast<uintptr_t>(address);
  std::FILE *maps = std::fopen("/proc/self/maps", "r");
  Check(maps != nullptr, "open /proc/self/maps failed");
  char line[512];
  uint32_t result = 0;
  while (std::fgets(line, sizeof(line), maps) != nullptr) {
    unsigned long start = 0;
    unsigned long end = 0;
    char perms[8]{};
    if (std::sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) {
      continue;
    }
    if (addr >= start && addr < end) {
      result = perms[1] == 'w'   ? PAGE_READWRITE
               : perms[0] == 'r' ? PAGE_READONLY
                                 : PAGE_NOACCESS;
      break;
    }
  }
  std::fclose(maps);
  return result;
#endif
}

std::map<void *, size_t> &AllocationSizes() {
  static std::map<void *, size_t> sizes;
  return sizes;
}

void *VirtualAlloc(void *address, size_t size, DWORD, uint32_t protection) {
#if defined(__APPLE__)
  mach_vm_address_t raw_address = reinterpret_cast<mach_vm_address_t>(address);
  const auto flags = address != nullptr ? VM_FLAGS_FIXED : VM_FLAGS_ANYWHERE;
  if (mach_vm_allocate(mach_task_self(), &raw_address, size, flags) !=
      KERN_SUCCESS) {
    return nullptr;
  }
  if (mach_vm_protect(mach_task_self(), raw_address, size, false,
                      static_cast<vm_prot_t>(ToHostProt(protection))) !=
      KERN_SUCCESS) {
    mach_vm_deallocate(mach_task_self(), raw_address, size);
    return nullptr;
  }
  void *raw = reinterpret_cast<void *>(raw_address);
  AllocationSizes()[raw] = size;
  return raw;
#else
  const int extra = address != nullptr ? MAP_FIXED_NOREPLACE : 0;
  void *raw = ::mmap(address, size, ToHostProt(protection),
                     MAP_PRIVATE | MAP_ANONYMOUS | extra, -1, 0);
  if (raw == MAP_FAILED) {
    return nullptr;
  }
  AllocationSizes()[raw] = size;
  return raw;
#endif
}

int VirtualFree(void *address, size_t, DWORD) {
  auto &sizes = AllocationSizes();
  auto it = sizes.find(address);
  if (it == sizes.end()) {
    return 0;
  }
#if defined(__APPLE__)
  const int ok = mach_vm_deallocate(mach_task_self(),
                                    reinterpret_cast<mach_vm_address_t>(address),
                                    it->second) == KERN_SUCCESS
                     ? 1
                     : 0;
#else
  const int ok = ::munmap(address, it->second) == 0 ? 1 : 0;
#endif
  sizes.erase(it);
  return ok;
}

int VirtualProtect(void *address, size_t size, uint32_t protection,
                   DWORD *old_protection) {
  if (old_protection != nullptr) {
    *old_protection = Protection(address);
  }
#if defined(__APPLE__)
  return mach_vm_protect(mach_task_self(),
                         reinterpret_cast<mach_vm_address_t>(address), size,
                         false, static_cast<vm_prot_t>(ToHostProt(protection))) ==
                 KERN_SUCCESS
             ? 1
             : 0;
#else
  return ::mprotect(address, size, ToHostProt(protection)) == 0 ? 1 : 0;
#endif
}
#else
uint32_t Protection(const void *address) {
  MEMORY_BASIC_INFORMATION info{};
  Check(VirtualQuery(address, &info, sizeof(info)) != 0, "VirtualQuery failed");
  return info.Protect;
}
#endif

bool IsWritable(const void *address) {
  return Protection(address) == PAGE_READWRITE;
}

uint64_t g_protection_calls = 0;

struct ProtectionCall {
  uint64_t address;
  uint64_t size;
  Common::VirtualMemory::Mode mode;
};

std::vector<ProtectionCall> g_protection_log;
std::mutex g_protection_log_mutex;

void ResetProtectionLog() {
  g_protection_calls = 0;
  g_protection_log.clear();
}

bool ProtectAddressSpace(uint64_t vaddr, uint64_t size,
                         Common::VirtualMemory::Mode mode) {
  uint32_t protection = PAGE_NOACCESS;
  if (mode == Common::VirtualMemory::Mode::Read) {
    protection = PAGE_READONLY;
  } else if (mode == Common::VirtualMemory::Mode::ReadWrite) {
    protection = PAGE_READWRITE;
  }
  DWORD old_protection = 0;
  {
    std::lock_guard lock(g_protection_log_mutex);
    g_protection_calls++;
    g_protection_log.push_back({vaddr, size, mode});
  }
  return VirtualProtect(reinterpret_cast<void *>(vaddr), size, protection,
                        &old_protection) != 0;
}

struct TrackerHarness {
  TrackerHarness() : tracker(page_manager) {}

  PageManager page_manager;
  MemoryTracker tracker;
};

uint8_t *AllocateFixedGuestRange(uint64_t size, uintptr_t offset) {
  // Keep the mapping inside the tracker's guest address space and preserve the
  // requested region alignment. A fixed address can be occupied by the host
  // process (notably by the macOS runner's ASLR layout).
  constexpr uintptr_t first_base = 0x0000000200000000ull;
  constexpr uintptr_t stride = 0x0000000100000000ull;
  for (uintptr_t attempt = 0; attempt < 256; attempt++) {
    auto *wanted = reinterpret_cast<void *>(first_base + offset + attempt * stride);
    auto *memory = static_cast<uint8_t *>(
        VirtualAlloc(wanted, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (memory == wanted) {
      return memory;
    }
    if (memory != nullptr) {
      Check(VirtualFree(memory, 0, MEM_RELEASE) != 0,
            "release unexpected fixed allocation failed");
    }
  }
  Check(false, "no free fixed guest address found");
  return nullptr;
}

uint8_t *Allocate(uint64_t pages) {
  const auto size = Libs::Graphics::TRACKER_PAGE_SIZE * pages;
  return AllocateFixedGuestRange(size, 0x10000);
}

void Release(uint8_t *memory) {
  Check(VirtualFree(memory, 0, MEM_RELEASE) != 0, "VirtualFree failed");
}

void MarkGpuModified(MemoryTracker &tracker, uint64_t address, uint64_t size) {
  tracker.ForEachUploadRange(
      address, size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
}

void TestRangeSet() {
  RangeSet ranges;
  ranges.Add(0x1000, 0x80);
  ranges.Add(0x1080, 0x80);
  ranges.Add(0x1200, 0x40);
  Check(ranges.Contains(0x1010, 0xe0) && !ranges.Contains(0x1010, 0x200),
        "range set containment did not require full coverage");
  Check(ranges.Intersects(0x0fff, 2) && ranges.Intersects(0x11ff, 2) &&
            !ranges.Intersects(0x1100, 0x100) &&
            !ranges.Intersects(0x1240, 1),
        "range set intersection did not preserve half-open boundaries");
  std::vector<std::pair<uint64_t, uint64_t>> intersections;
  ranges.ForEachInRange(0x1070, 0x1b0, [&](uint64_t start, uint64_t end) {
    intersections.emplace_back(start, end);
  });
  Check(intersections.size() == 2 && intersections[0].first == 0x1070 &&
            intersections[0].second == 0x1100 &&
            intersections[1].first == 0x1200 && intersections[1].second == 0x1220,
        "range set did not merge and intersect exact byte ranges");
  ranges.Subtract(0x1040, 0x1e0);
  intersections.clear();
  ranges.ForEachInRange(0x1000, 0x300, [&](uint64_t start, uint64_t end) {
    intersections.emplace_back(start, end);
  });
  Check(intersections.size() == 2 && intersections[0].first == 0x1000 &&
            intersections[0].second == 0x1040 &&
            intersections[1].first == 0x1220 && intersections[1].second == 0x1240,
        "range set subtraction did not preserve both exact tails");
}

// RangeSet::Extract reports what ForEachInRange does and removes it like Subtract; a range
// added again after a removal is not taken for present.
void TestRangeSetExtractAndReAdd() {
  RangeSet ranges;
  ranges.Add(0x1000, 0x100);
  ranges.Add(0x1000, 0x100);
  ranges.Add(0x1300, 0x100);
  std::vector<std::pair<uint64_t, uint64_t>> pieces;
  ranges.Extract(0x1080, 0x300, [&](uint64_t start, uint64_t end) {
    pieces.emplace_back(start, end);
  });
  Check(pieces.size() == 2 && pieces[0].first == 0x1080 &&
            pieces[0].second == 0x1100 && pieces[1].first == 0x1300 &&
            pieces[1].second == 0x1380,
        "range set extract did not report the clipped ranges");
  Check(ranges.Contains(0x1000, 0x80) && !ranges.Intersects(0x1080, 0x300) &&
            ranges.Contains(0x1380, 0x80),
        "range set extract did not keep both tails");
  ranges.Subtract(0x1000, 0x80);
  Check(!ranges.Intersects(0x1000, 0x80), "range set subtract kept a range");
  ranges.Add(0x1000, 0x40);
  Check(ranges.Contains(0x1000, 0x40) && !ranges.Intersects(0x1040, 0x40),
        "range set re-add after a removal was skipped or grew");
  RangeSet moved = std::move(ranges);
  ranges.Add(0x1000, 0x40);
  Check(ranges.Contains(0x1000, 0x40) && moved.Contains(0x1380, 0x80),
        "range set add after a move was skipped");
}

// The set answers like a byte map under random adds, removals and queries: the ranges it recalls
// for repeated adds and positive queries never outlive a removal.
void TestRangeSetMatchesByteModel() {
  constexpr uint64_t base = 0x10000;
  constexpr uint64_t span = 0x4000;
  std::vector<uint8_t> model(span, 0);
  RangeSet ranges;
  uint64_t state = 0x2545f4914f6cdd1dull;
  const auto next = [&](uint64_t limit) {
    state ^= state << 13u;
    state ^= state >> 7u;
    state ^= state << 17u;
    return state % limit;
  };
  // Spans reused often, so repeated adds and queries meet the recalled ranges.
  std::vector<std::pair<uint64_t, uint64_t>> spans;
  for (int i = 0; i < 48; i++) {
    const auto offset = next(span - 0x100) & ~uint64_t{0xf};
    spans.emplace_back(offset, 0x10 + (next(0xf0) & ~uint64_t{0xf}));
  }
  for (int step = 0; step < 100000; step++) {
    const auto [offset, size] = next(4) != 0 ? spans[next(spans.size())]
                                             : std::pair<uint64_t, uint64_t>{
                                                   next(span - 0x200), 1 + next(0x1ff)};
    const auto op = next(16);
    if (op < 6) {
      ranges.Add(base + offset, size);
      std::fill(model.begin() + offset, model.begin() + offset + size, uint8_t{1});
    } else if (op < 7) {
      ranges.Subtract(base + offset, size);
      std::fill(model.begin() + offset, model.begin() + offset + size, uint8_t{0});
    } else if (op < 8) {
      uint64_t reported = 0;
      ranges.Extract(base + offset, size, [&](uint64_t from, uint64_t to) {
        for (auto at = from; at < to; at++) {
          Check(model[at - base] == 1, "range set extract reported an absent byte");
        }
        reported += to - from;
      });
      uint64_t present = 0;
      for (auto at = offset; at < offset + size; at++) {
        present += model[at];
        model[at] = 0;
      }
      Check(reported == present, "range set extract missed present bytes");
    } else {
      bool all = true;
      bool any = false;
      for (auto at = offset; at < offset + size; at++) {
        all = all && model[at] != 0;
        any = any || model[at] != 0;
      }
      Check(ranges.Contains(base + offset, size) == all,
            "range set containment disagrees with the byte model");
      Check(ranges.Intersects(base + offset, size) == any,
            "range set intersection disagrees with the byte model");
    }
  }
  uint64_t previous_end = 0;
  ranges.ForEach([&](uint64_t begin, uint64_t end) {
    Check(begin > previous_end || previous_end == 0, "range set kept touching ranges apart");
    for (auto at = begin; at < end; at++) {
      Check(model[at - base] == 1, "range set holds an absent byte");
    }
    previous_end = end;
  });
  RangeSet copy = ranges;
  RangeSet moved = std::move(ranges);
  for (const auto [offset, size] : spans) {
    bool all = true;
    for (auto at = offset; at < offset + size; at++) {
      all = all && model[at] != 0;
    }
    Check(copy.Contains(base + offset, size) == all && moved.Contains(base + offset, size) == all,
          "copied or moved range set disagrees with the byte model");
    Check(!ranges.Intersects(base + offset, size), "moved-from range set kept a range");
  }
}

// ForEachUploadRangeBatch reports each range's CPU-dirty pages as separate ForEachUploadRange
// calls do (a shared page once), and re-watches the pages of one region in one protection
// call when the pages between them are watched already.
void TestBatchedUploadProtection() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  Libs::Graphics::g_sticky_pages.store(false);
  {
    TrackerHarness harness;
    auto &tracker = harness.tracker;
    auto *memory = Allocate(8);
    const auto address = reinterpret_cast<uint64_t>(memory);
    tracker.ForEachUploadRange(
        address, page_size * 8, false, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
    tracker.InvalidateRegion(address + page_size, 8, [] {});
    tracker.InvalidateRegion(address + page_size * 5, 8, [] {});
    std::vector<std::pair<size_t, uint64_t>> reported;
    const auto record = [&](size_t index, uint64_t at, uint64_t bytes) noexcept {
      Check(bytes == page_size, "batched upload reported more than one page");
      reported.emplace_back(index, at);
    };
    const GuestRange sparse[] = {{address + page_size, page_size},
                                 {address + page_size * 5, page_size}};
    ResetProtectionLog();
    tracker.ForEachUploadRangeBatch(sparse, record);
    Check(reported.size() == 2 && reported[0].first == 0 &&
              reported[0].second == address + page_size && reported[1].first == 1 &&
              reported[1].second == address + page_size * 5,
          "batched upload did not report each range's dirty page");
    Check(g_protection_calls == 1 && g_protection_log[0].address == address + page_size &&
              g_protection_log[0].size == page_size * 5 &&
              g_protection_log[0].mode == Common::VirtualMemory::Mode::Read &&
              !tracker.IsRegionCpuModified(address, page_size * 8) &&
              Protection(memory + page_size) == PAGE_READONLY &&
              Protection(memory + page_size * 5) == PAGE_READONLY,
          "batched upload did not re-watch a region in one protection call");

    tracker.InvalidateRegion(address + page_size * 3, 8, [] {});
    const GuestRange shared[] = {{address + page_size * 3, 16},
                                 {address + page_size * 3 + 16, 16}};
    reported.clear();
    tracker.ForEachUploadRangeBatch(shared, record);
    Check(reported.size() == 1 && reported[0].first == 0 &&
              reported[0].second == address + page_size * 3,
          "batched upload reported a shared page twice");
    reported.clear();
    ResetProtectionLog();
    tracker.ForEachUploadRangeBatch(sparse, record);
    Check(reported.empty() && g_protection_calls == 0,
          "clean batched upload reported or protected pages");
    tracker.MarkRegionAsCpuModified(address, page_size * 8);
    Release(memory);
  }
  Libs::Graphics::g_sticky_pages.store(true);
}

void TestGuestRange() {
  constexpr GuestRange empty{};
  constexpr GuestRange first_byte{1, 1};
  constexpr uint64_t extended_end = Libs::LibKernel::Memory::kExtendedMemoryBase +
                                    Libs::LibKernel::Memory::kExtendedMemorySize;
  constexpr GuestRange last_byte{extended_end - 1, 1};

  static_assert(empty.Empty() && !empty.Valid() && empty.ValidOrEmpty());
  static_assert(!first_byte.Empty() && first_byte.Valid() &&
                first_byte.ValidOrEmpty() && first_byte.End() == 2);
  static_assert(last_byte.Valid() && last_byte.End() == extended_end);

  Check(!GuestRange{0, 1}.Empty() && !GuestRange{0, 1}.ValidOrEmpty(),
        "zero-address nonempty guest range is rejected");
  Check(!GuestRange{1, 0}.Empty() && !GuestRange{1, 0}.ValidOrEmpty(),
        "nonzero-address empty guest range is rejected");
  Check(GuestRange{Libs::LibKernel::Memory::kExtendedMemoryBase, 1}.Valid() &&
            !GuestRange{Libs::Graphics::LOWER_ADDRESS_SIZE, 1}.Valid() &&
            !GuestRange{Libs::LibKernel::Memory::kExtendedMemoryBase - 1, 2}.Valid() &&
            !GuestRange{extended_end - 1, 2}.Valid(),
        "extended range and gap boundaries are enforced");
  Check(!GuestRange{TRACKER_ADDRESS_SIZE, 1}.Valid(),
        "first address beyond the guest range is rejected");
  Check(!GuestRange{TRACKER_ADDRESS_SIZE - 1, 2}.Valid(),
        "guest range crossing the address-space end is rejected");
  Check(!GuestRange{UINT64_MAX, 2}.Valid(), "wrapping guest range is rejected");
}

void TestQueriesDoNotRequireMappedOwnership() {
  constexpr uint64_t address = 0x0000000203000000ull;
  TrackerHarness harness;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  Check(harness.tracker.IsRegionCpuModified(address, page_size) &&
            !harness.tracker.IsRegionGpuModified(address, page_size),
        "unowned tracker range did not expose its initial CPU-dirty state");
}

void TestConcurrentRegionPublication() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(1);
  const auto address = reinterpret_cast<uint64_t>(memory);

  std::binary_semaphore start_first{0};
  std::binary_semaphore start_second{0};
  std::atomic_uint32_t cpu_dirty_results{0};
  std::jthread first([&] {
    start_first.acquire();
    if (tracker.IsRegionCpuModified(address, page_size)) {
      cpu_dirty_results.fetch_add(1, std::memory_order_relaxed);
    }
  });
  std::jthread second([&] {
    start_second.acquire();
    if (tracker.IsRegionCpuModified(address, page_size)) {
      cpu_dirty_results.fetch_add(1, std::memory_order_relaxed);
    }
  });
  start_first.release();
  start_second.release();
  first.join();
  second.join();

  tracker.MarkRegionAsCpuModified(address, page_size);
  Release(memory);
  Check(cpu_dirty_results.load(std::memory_order_relaxed) == 2,
        "concurrent region publication lost initial CPU ownership");
}

void TestCpuDirtyUpload() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(2);
  const auto address = reinterpret_cast<uint64_t>(memory);
  Check(tracker.IsRegionCpuModified(address + 16, 32),
        "new region was not CPU dirty");

  uint32_t ranges = 0;
  bool uploaded = false;
  tracker.ForEachUploadRange(
      address + 16, 32, false,
      [&](uint64_t upload_address, uint64_t upload_size) noexcept {
        Check(upload_address == address && upload_size == page_size,
              "upload range was not page aligned");
        ranges++;
      },
      [&]() noexcept { uploaded = true; });
  Check(ranges == 1 && uploaded &&
            !tracker.IsRegionCpuModified(address, page_size) &&
            Protection(memory) == PAGE_READONLY,
        "upload did not clear CPU dirty state and arm protection");

  tracker.MarkRegionAsCpuModified(address + 16, 32);
  Check(tracker.IsRegionCpuModified(address, page_size) && IsWritable(memory),
        "explicit CPU dirtiness did not release write protection");
  tracker.MarkRegionAsCpuModified(address, page_size * 2);
  Release(memory);
}

// A repeated ForEachUploadRange is skipped only while its regions are
// unchanged: a CPU write, a GPU unmark or a download in between makes the next
// identical call run again, a new tracker does not take an old one's skip, and
// a page kept unwatched is uploaded again in the next submission.
void TestRepeatedUploadSkip() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(2);
  const auto address = reinterpret_cast<uint64_t>(memory);
  uint32_t ranges = 0;
  uint32_t uploads = 0;
  const auto upload = [&](MemoryTracker &tracker, bool written) {
    ranges = 0;
    tracker.ForEachUploadRange(
        address, page_size, written,
        [&](uint64_t, uint64_t) noexcept { ranges++; },
        [&]() noexcept { uploads++; });
  };
  {
    TrackerHarness harness;
    auto &tracker = harness.tracker;
    upload(tracker, false);
    Check(ranges == 1 && uploads == 1, "first upload did not copy the page");
    upload(tracker, false);
    Check(ranges == 0 && uploads == 2 && Protection(memory) == PAGE_READONLY,
          "repeated upload copied or skipped its upload callback");
    tracker.InvalidateRegion(address + 8, 8, [] {});
    upload(tracker, false);
    Check(ranges == 1, "upload after a CPU write was skipped");
    upload(tracker, false);
    Check(ranges == 0 && !tracker.IsRegionCpuModified(address, page_size),
          "upload after a CPU write left the page dirty");

    upload(tracker, true);
    Check(tracker.IsRegionGpuModified(address, page_size) &&
              Protection(memory) == PAGE_NOACCESS,
          "written upload did not mark the page GPU-dirty");
    tracker.UnmarkRegionAsGpuModified(address, page_size);
    upload(tracker, true);
    Check(tracker.IsRegionGpuModified(address, page_size) &&
              Protection(memory) == PAGE_NOACCESS,
          "written upload after an unmark was skipped");
    tracker.ForEachDownloadRange<true>(address, page_size,
                                       [](uint64_t, uint64_t) noexcept {});
    Check(!tracker.IsRegionGpuModified(address, page_size),
          "download did not clear the GPU-dirty page");
    upload(tracker, true);
    Check(tracker.IsRegionGpuModified(address, page_size) &&
              Protection(memory) == PAGE_NOACCESS,
          "written upload after a download was skipped");
    upload(tracker, true);
    Check(ranges == 0 && tracker.IsRegionGpuModified(address, page_size) &&
              uploads == 8,
          "repeated written upload changed the page's state");

    tracker.UnmarkRegionAsGpuModified(address, page_size * 2);
    tracker.MarkRegionAsCpuModified(address, page_size * 2);
  }
  {
    TrackerHarness harness;
    auto &tracker = harness.tracker;
    upload(tracker, false);
    Check(ranges == 1, "a new tracker skipped its first upload");

    // Two CPU writes after uploads keep the page unwatched; it is then current
    // only for the submission it was uploaded in.
    tracker.InvalidateRegion(address + 8, 8, [] {});
    upload(tracker, false);
    tracker.InvalidateRegion(address + 8, 8, [] {});
    Check(tracker.IsRegionUnwatched(address, page_size),
          "a page written after two uploads was not kept unwatched");
    upload(tracker, false);
    Check(ranges == 1, "an unwatched page was not uploaded");
    upload(tracker, false);
    Check(ranges == 0, "an unwatched page was uploaded twice in a submission");
    Libs::Graphics::AdvanceTrackerEpoch();
    upload(tracker, false);
    Check(ranges == 1,
          "an unwatched page was not uploaded again in the next submission");
    upload(tracker, false);
    Check(ranges == 0, "an unwatched page was uploaded twice in a submission");

    tracker.MarkRegionAsCpuModified(address, page_size * 2);
  }
  Release(memory);
}

// ForEachUploadRange reports the region generations it leaves (BufferCache's
// ObtainBuffer memo keys on them): the sum RegionGenerations reads afterwards,
// unchanged by a repeated call, changed by a CPU write, a GPU unmark or a
// written call that marks the range GPU-dirty.
void TestUploadReportsGenerations() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  Libs::Graphics::g_sticky_pages.store(false);
  auto *memory = Allocate(2);
  const auto address = reinterpret_cast<uint64_t>(memory);
  {
    TrackerHarness harness;
    auto &tracker = harness.tracker;
    uint32_t ranges = 0;
    const auto upload = [&](bool written) {
      uint64_t generations = 0;
      ranges = 0;
      tracker.ForEachUploadRange(
          address, page_size, written,
          [&](uint64_t, uint64_t) noexcept { ranges++; }, []() noexcept {},
          &generations);
      return generations;
    };
    const auto first = upload(false);
    Check(first != 0 && first == tracker.RegionGenerations(address, page_size),
          "upload reported other generations than it left");
    const auto repeated = upload(false);
    Check(ranges == 0 && repeated == first &&
              tracker.RegionGenerations(address, page_size) == first,
          "repeated clean upload changed the generations");
    tracker.InvalidateRegion(address + 8, 8, [] {});
    Check(tracker.RegionGenerations(address, page_size) != first,
          "a CPU write kept the generations");
    const auto after_write = upload(false);
    Check(ranges == 1 && after_write == tracker.RegionGenerations(address, page_size),
          "upload after a CPU write reported stale generations");
    const auto written = upload(true);
    Check(written != after_write &&
              written == tracker.RegionGenerations(address, page_size) &&
              tracker.IsRegionGpuModified(address, page_size),
          "written upload reported stale generations");
    Check(upload(true) == written && tracker.RegionGenerations(address, page_size) == written,
          "repeated written upload changed the generations");
    tracker.UnmarkRegionAsGpuModified(address, page_size);
    Check(tracker.RegionGenerations(address, page_size) != written,
          "a GPU unmark kept the generations");
    tracker.MarkRegionAsCpuModified(address, page_size * 2);
  }
  Release(memory);
  Libs::Graphics::g_sticky_pages.store(true);
}

// A page written (and faulting) in three frames in a row stays unwatched as an
// exact hot page: queries compare it with the bytes its last upload copied,
// so a write is seen at the next query without a fault, an unchanged page is
// clean, and a written upload or a CPU-modified mark makes it an ordinary
// watched page again.
void TestExactHotPages() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  using Libs::Graphics::HotPages;
  Libs::Graphics::g_sticky_pages.store(false);
  Libs::Graphics::g_exact_hot_pages.store(true);
  auto *memory = Allocate(2);
  const auto address = reinterpret_cast<uint64_t>(memory);
  {
    TrackerHarness harness;
    auto &tracker = harness.tracker;
    tracker.Hot().SetReader(
        [](uint64_t page) { return reinterpret_cast<const uint8_t *>(page); });
    std::vector<uint8_t> buffer(page_size);
    uint32_t ranges = 0;
    // The buffer cache's upload: hot pages go through their shadow.
    const auto upload = [&](bool written) {
      ranges = 0;
      tracker.ForEachUploadRange(
          address, page_size, written,
          [&](uint64_t at, uint64_t bytes) noexcept {
            ranges++;
            const auto slot = tracker.HotSlot(at);
            if (slot == HotPages::NO_SLOT ||
                !tracker.Hot().Refresh(slot, at, bytes, buffer.data())) {
              std::memcpy(buffer.data(), reinterpret_cast<const void *>(at), bytes);
            }
          },
          []() noexcept {});
    };
    upload(false);
    Check(ranges == 1 && Protection(memory) == PAGE_READONLY,
          "first upload did not watch the page");
    for (int frame = 0; frame < 3; frame++) {
      Libs::Graphics::AdvanceTrackerFrame();
      tracker.InvalidateRegion(address + 8, 8, [] {});
      memory[8] = static_cast<uint8_t>(frame + 1);
      upload(false);
      Check(ranges == 1 && buffer[8] == frame + 1, "a written page was not uploaded");
    }
    Check(tracker.HotSlot(address) != HotPages::NO_SLOT &&
              Protection(memory) == PAGE_READWRITE,
          "a page written in three frames in a row was not kept hot");
    Check(!tracker.IsRegionCpuModified(address, page_size) &&
              tracker.RegionGenerations(address, page_size) == 0,
          "an unchanged hot page read as modified, or kept a generation");
    upload(false);
    Check(ranges == 0, "an unchanged hot page was uploaded");
    memory[16] = 0x5a;
    Check(tracker.IsRegionCpuModified(address, page_size),
          "a write to a hot page was not seen");
    upload(false);
    Check(ranges == 1 && buffer[16] == 0x5a && Protection(memory) == PAGE_READWRITE,
          "a changed hot page was not uploaded, or got watched");
    Check(!tracker.IsRegionCpuModified(address, page_size),
          "an uploaded hot page still read as modified");
    memory[24] = 0x6b;
    upload(true);
    Check(ranges == 1 && buffer[24] == 0x6b && tracker.HotSlot(address) == HotPages::NO_SLOT &&
              tracker.IsRegionGpuModified(address, page_size) &&
              Protection(memory) == PAGE_NOACCESS,
          "a written upload kept the page hot");

    tracker.UnmarkRegionAsGpuModified(address, page_size * 2);
    Check(Protection(memory) == PAGE_READONLY, "GPU unmark left the page unwatched");
    for (int frame = 0; frame < 3; frame++) {
      Libs::Graphics::AdvanceTrackerFrame();
      tracker.InvalidateRegion(address + 8, 8, [] {});
      upload(false);
    }
    Check(tracker.HotSlot(address) != HotPages::NO_SLOT, "the page was not kept hot again");
    tracker.MarkRegionAsCpuModified(address, page_size * 2);
    Check(tracker.HotSlot(address) == HotPages::NO_SLOT &&
              tracker.IsRegionCpuModified(address, page_size),
          "a CPU-modified mark kept the page hot");
    upload(false);
    Check(ranges == 1 && Protection(memory) == PAGE_READONLY,
          "a demoted page was not uploaded and watched");
    tracker.MarkRegionAsCpuModified(address, page_size * 2);
  }
  Release(memory);
  Libs::Graphics::g_exact_hot_pages.store(false);
  Libs::Graphics::g_sticky_pages.store(true);
}

void TestCleanUploadPreservesOwnership() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = AllocateFixedGuestRange(region_size * 2, region_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  tracker.ForEachUploadRange(address, region_size * 2, false,
                             [](uint64_t, uint64_t) noexcept {},
                             []() noexcept {});

  for (const auto start : {address + 63 * page_size,
                           address + region_size - page_size}) {
    const auto before = start - page_size;
    const auto after = start + 2 * page_size;
    tracker.MarkRegionAsCpuModified(before, page_size);
    tracker.MarkRegionAsCpuModified(after, page_size);
    uint32_t ranges = 0;
    uint32_t completions = 0;
    ResetProtectionLog();
    tracker.ForEachUploadRange(
        start, 2 * page_size, false,
        [&](uint64_t, uint64_t) noexcept { ranges++; },
        [&]() noexcept { completions++; });
    Check(ranges == 0 && completions == 1 && g_protection_calls == 0 &&
              !tracker.IsRegionCpuModified(start, 2 * page_size),
          "clean read-only upload changed dirty state or protection");

    tracker.ForEachUploadRange(
        start, 2 * page_size, true,
        [&](uint64_t, uint64_t) noexcept { ranges++; },
        [&]() noexcept { completions++; });
    Check(ranges == 0 && completions == 2 &&
              tracker.IsRegionGpuModified(start, page_size) &&
              tracker.IsRegionGpuModified(start + page_size, page_size) &&
              Protection(reinterpret_cast<void *>(start)) == PAGE_NOACCESS &&
              Protection(reinterpret_cast<void *>(start + page_size)) ==
                  PAGE_NOACCESS &&
              tracker.IsRegionCpuModified(before, page_size) &&
              tracker.IsRegionCpuModified(after, page_size) &&
              !tracker.IsRegionGpuModified(before, page_size) &&
              !tracker.IsRegionGpuModified(after, page_size) &&
              IsWritable(reinterpret_cast<void *>(before)) &&
              IsWritable(reinterpret_cast<void *>(after)),
          "clean written upload lost GPU ownership or changed dirty neighbors");
    tracker.UnmarkRegionAsGpuModified(start, 2 * page_size);
  }
  tracker.MarkRegionAsCpuModified(address, region_size * 2);
  Release(memory);
}

void BenchmarkCleanUploads() {
  constexpr uint64_t size = 256ull * 1024 * 1024;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = AllocateFixedGuestRange(size, 0);
  const auto address = reinterpret_cast<uint64_t>(memory);
  tracker.ForEachUploadRange(address, size, false,
                             [](uint64_t, uint64_t) noexcept {},
                             []() noexcept {});
  std::puts("chunk_bytes,sweeps,calls,elapsed_ns,ns_per_sweep,ns_per_call");
  for (const uint64_t chunk : {size, Libs::Graphics::TRACKER_REGION_SIZE,
                               uint64_t{64 * 1024}}) {
    uint64_t ranges = 0;
    uint64_t completions = 0;
    uint64_t sweeps = 0;
    ResetProtectionLog();
    const auto start = std::chrono::steady_clock::now();
    std::chrono::nanoseconds elapsed{};
    do {
      for (uint64_t offset = 0; offset < size; offset += chunk) {
        tracker.ForEachUploadRange(
            address + offset, chunk, false,
            [&](uint64_t, uint64_t) noexcept { ranges++; },
            [&]() noexcept { completions++; });
      }
      sweeps++;
      elapsed = std::chrono::steady_clock::now() - start;
    } while (elapsed < std::chrono::milliseconds(250));
    Check(ranges == 0 && completions == sweeps * (size / chunk) &&
              g_protection_calls == 0,
          "clean upload benchmark changed ranges, completion, or protection");
    std::printf("%llu,%llu,%llu,%lld,%.2f,%.2f\n",
                static_cast<unsigned long long>(chunk),
                static_cast<unsigned long long>(sweeps),
                static_cast<unsigned long long>(completions),
                static_cast<long long>(elapsed.count()),
                static_cast<double>(elapsed.count()) / sweeps,
                static_cast<double>(elapsed.count()) / completions);
  }
  tracker.MarkRegionAsCpuModified(address, size);
  Release(memory);
}

void TestRangeInvalidation() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr uint64_t size = Libs::Graphics::TRACKER_REGION_SIZE * 2;
  auto *memory = AllocateFixedGuestRange(size, 0x1000000);
  const auto address = reinterpret_cast<uint64_t>(memory);

  tracker.ForEachUploadRange(
      address, size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(tracker.IsRegionGpuModified(address, size) && !IsWritable(memory),
        "range invalidation setup did not establish GPU ownership");
  uint32_t flushes = 0;
  tracker.InvalidateRegion(address + 16, size - 32, [&] {
    flushes++;
    tracker.ForEachDownloadRange<true>(address + 16, size - 32,
                                       [](uint64_t, uint64_t) noexcept {});
    tracker.MarkRegionAsCpuModified(address + 16, size - 32);
  });
  Check(flushes == 1 && !tracker.IsRegionGpuModified(address, size) &&
            tracker.IsRegionCpuModified(address, size) && IsWritable(memory) &&
            IsWritable(memory + size - 1),
        "range invalidation did not batch ownership transfer across regions");
  tracker.InvalidateRegion(address + 16, size - 32, [&] { flushes++; });
  Check(flushes == 1,
        "clean range invalidation unnecessarily requested a GPU flush");
  tracker.MarkRegionAsCpuModified(address, size);
  Release(memory);
}

void TestGpuReacquisitionAfterInvalidation() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(1);
  const auto address = reinterpret_cast<uint64_t>(memory);

  tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(tracker.IsRegionGpuModified(address, page_size) &&
            !tracker.IsRegionCpuModified(address, page_size),
        "reacquisition setup did not establish GPU ownership");

  uint32_t flushes = 0;
  uint32_t uploads = 0;
  std::binary_semaphore reacquire{0};
  std::binary_semaphore reacquired{0};
  std::jthread publisher([&] {
    reacquire.acquire();
    tracker.ForEachUploadRange(
        address + 16, 32, true,
        [&](uint64_t, uint64_t) noexcept { uploads++; }, []() noexcept {});
    reacquired.release();
  });
  tracker.InvalidateRegion(address + 16, 32, [&] {
    flushes++;
    tracker.ForEachDownloadRange<true>(address + 16, 32,
                                       [](uint64_t, uint64_t) noexcept {});
    tracker.MarkRegionAsCpuModified(address + 16, 32);
    reacquire.release();
    reacquired.acquire();
  });
  publisher.join();
  Check(flushes == 1 && uploads == 1 &&
            tracker.IsRegionGpuModified(address, page_size) &&
            !tracker.IsRegionCpuModified(address, page_size) &&
            !IsWritable(memory),
        "invalidation rejected a new generation of GPU ownership");

  tracker.UnmarkRegionAsGpuModified(address, page_size);
  tracker.MarkRegionAsCpuModified(address, page_size);
  Release(memory);
}

void TestGpuDirtyBits() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(2);
  const auto address = reinterpret_cast<uint64_t>(memory);

  tracker.ForEachUploadRange(
      address, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  Check(tracker.IsRegionGpuModified(address, page_size) &&
            !tracker.IsRegionGpuModified(address + page_size, page_size) &&
            Protection(memory) == PAGE_NOACCESS,
        "GPU dirty state escaped the requested range");
  // CPU-dirtying the next page does not conflict with the GPU-dirty one.
  tracker.MarkRegionAsCpuModified(address + page_size, page_size);
  tracker.UnmarkRegionAsGpuModified(address, page_size);
  Check(!tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_READONLY,
        "GPU dirty state did not restore write-only tracking");
  // Nor does CPU-dirtying the previous page.
  MarkGpuModified(tracker, address + page_size, page_size);
  tracker.MarkRegionAsCpuModified(address, page_size);
  tracker.UnmarkRegionAsGpuModified(address + page_size, page_size);
  tracker.MarkRegionAsCpuModified(address, page_size * 2);
  Release(memory);
}

void TestExactDirtyIntervalsSharingTrackerPage() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(1);
  const auto address = reinterpret_cast<uint64_t>(memory);

  tracker.ForEachUploadRange(
      address, page_size, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  RangeSet exact_dirty;
  exact_dirty.Add(address + 64, 16);
  exact_dirty.Add(address + 192, 32);

  ResetProtectionLog();
  MarkGpuModified(tracker, address + 64, 16);
  MarkGpuModified(tracker, address + 192, 32);
  Check(g_protection_calls == 1 &&
            tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_NOACCESS,
        "disjoint byte dirtiness duplicated the page watcher");

  exact_dirty.Subtract(address + 64, 16);
  if (!exact_dirty.Intersects(address, page_size)) {
    tracker.UnmarkRegionAsGpuModified(address, page_size);
  }
  Check(g_protection_calls == 1 &&
            tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_NOACCESS,
        "draining one exact interval prematurely released its shared page");

  exact_dirty.Subtract(address + 192, 32);
  if (!exact_dirty.Intersects(address, page_size)) {
    tracker.UnmarkRegionAsGpuModified(address, page_size);
  }
  Check(g_protection_calls == 2 &&
            !tracker.IsRegionGpuModified(address, page_size) &&
            Protection(memory) == PAGE_READONLY,
        "draining the final exact interval did not release its tracker page");

  tracker.MarkRegionAsCpuModified(address, page_size);
  Release(memory);
}

void TestGpuDownloadProtectionMirrors() {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(4);
  const auto address = reinterpret_cast<uint64_t>(memory);

  tracker.ForEachUploadRange(
      address, page_size * 4, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  MarkGpuModified(tracker, address + 16, 32);
  MarkGpuModified(tracker, address + page_size * 2 + 16, 32);

  std::vector<std::pair<uint64_t, uint64_t>> visited;
  ResetProtectionLog();
  tracker.ForEachDownloadRange<false>(
      address, page_size * 3,
      [&](uint64_t range_address, uint64_t range_size) noexcept {
        visited.push_back({range_address, range_size});
      });
  Check(visited.size() == 2 && visited[0].first == address &&
            visited[0].second == page_size &&
            visited[1].first == address + page_size * 2 &&
            visited[1].second == page_size && g_protection_calls == 0 &&
            tracker.IsRegionGpuModified(address, page_size * 3),
        "non-clearing download changed protection or lost sparse ranges");

  visited.clear();
  bool protected_during_download = false;
  tracker.ForEachDownloadRange<true>(
      address + 16, 32,
      [&](uint64_t range_address, uint64_t range_size) noexcept {
        protected_during_download = Protection(memory) == PAGE_NOACCESS;
        visited.push_back({range_address, range_size});
      });
  Check(protected_during_download && visited.size() == 1 &&
            visited[0].first == address &&
            visited[0].second == page_size && g_protection_log.size() == 1 &&
            g_protection_log[0].address == address &&
            g_protection_log[0].size == page_size &&
            g_protection_log[0].mode == Common::VirtualMemory::Mode::Read &&
            !tracker.IsRegionGpuModified(address, page_size) &&
            tracker.IsRegionGpuModified(address + page_size * 2, page_size) &&
            Protection(memory) == PAGE_READONLY &&
            Protection(memory + page_size * 2) == PAGE_NOACCESS,
        "partial download did not preserve the CPU/GPU protection mirrors");

  visited.clear();
  ResetProtectionLog();
  tracker.ForEachDownloadRange<true>(
      address + 16, 32,
      [&](uint64_t range_address, uint64_t range_size) noexcept {
        visited.push_back({range_address, range_size});
      });
  Check(visited.empty() && g_protection_calls == 0 &&
            tracker.IsRegionGpuModified(address + page_size * 2, page_size),
        "idempotent partial download disturbed another GPU-owned page");

  tracker.UnmarkRegionAsGpuModified(address, page_size * 3);
  Check(!tracker.IsRegionGpuModified(address, page_size * 3) &&
            Protection(memory + page_size * 2) == PAGE_READONLY,
        "broad final unmark did not restore write-only tracking");
  ResetProtectionLog();
  tracker.MarkRegionAsCpuModified(address + 16, 32);
  Check(
      g_protection_log.size() == 1 && g_protection_log[0].address == address &&
          g_protection_log[0].size == page_size &&
          g_protection_log[0].mode == Common::VirtualMemory::Mode::ReadWrite &&
          IsWritable(memory) && !IsWritable(memory + page_size),
      "CPU-dirty transition did not release only its write watcher");

  tracker.MarkRegionAsCpuModified(address, page_size * 4);
  Release(memory);
}

void TestCrossRegionUpload() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = AllocateFixedGuestRange(region_size * 2, 0x10000);
  const auto address = reinterpret_cast<uint64_t>(memory);
  const auto boundary = (address + region_size - 1) & ~(region_size - 1);
  for (const bool is_written : {false, true}) {
    TrackerHarness harness;
    auto &tracker = harness.tracker;
    uint32_t ranges = 0;
    tracker.ForEachUploadRange(
        boundary - page_size, page_size * 2, is_written,
        [&](uint64_t start, uint64_t size) noexcept {
          Check(start == boundary - page_size + ranges * page_size &&
                    size == page_size,
                "cold upload did not visit both region boundaries exactly");
          ranges++;
        },
        [&]() noexcept {
          Check(ranges == 2 &&
                    Protection(reinterpret_cast<void *>(boundary - page_size)) ==
                        PAGE_READONLY &&
                    Protection(reinterpret_cast<void *>(boundary)) == PAGE_READONLY,
                "cold upload completed before clearing and protecting both regions");
        });
    for (const auto page : {boundary - page_size, boundary}) {
      Check(!tracker.IsRegionCpuModified(page, page_size) &&
                tracker.IsRegionGpuModified(page, page_size) == is_written &&
                Protection(reinterpret_cast<void *>(page)) ==
                    (is_written ? PAGE_NOACCESS : PAGE_READONLY),
            "cross-region upload did not preserve final ownership and protection");
    }
    Check(IsWritable(reinterpret_cast<void *>(boundary - 2 * page_size)) &&
              IsWritable(reinterpret_cast<void *>(boundary + page_size)),
          "cross-region upload changed neighboring pages");
    tracker.UnmarkRegionAsGpuModified(boundary - page_size, 2 * page_size);
    tracker.MarkRegionAsCpuModified(address, region_size * 2);
  }
  Release(memory);
}

void TestConcurrentColdUploads() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = AllocateFixedGuestRange(region_size * 2, region_size);
  const auto address = reinterpret_cast<uint64_t>(memory);
  std::counting_semaphore<2> start{0};
  std::counting_semaphore<2> upload_entered{0};
  std::counting_semaphore<2> finish_upload{0};
  std::vector<std::jthread> workers;
  for (const auto page : {address, address + region_size}) {
    workers.emplace_back([&, page] {
      start.acquire();
      uint32_t ranges = 0;
      tracker.ForEachUploadRange(
          page, page_size, true,
          [&](uint64_t upload_address, uint64_t upload_size) noexcept {
            Check(upload_address == page && upload_size == page_size,
                  "concurrent cold upload lost its dirty page");
            ranges++;
          },
          [&]() noexcept {
            Check(ranges == 1, "concurrent cold upload skipped its dirty page");
            upload_entered.release();
            finish_upload.acquire();
          });
    });
  }
  start.release(2);
  const bool first_entered = upload_entered.try_acquire_for(std::chrono::seconds(5));
  const bool second_entered = upload_entered.try_acquire_for(std::chrono::seconds(5));
  finish_upload.release(2);
  for (auto &worker : workers) {
    worker.join();
  }
  Check(first_entered && second_entered,
        "cold writable uploads serialized disjoint regions");
  for (const auto page : {address, address + region_size}) {
    Check(!tracker.IsRegionCpuModified(page, page_size) &&
              tracker.IsRegionGpuModified(page, page_size) &&
              Protection(reinterpret_cast<void *>(page)) == PAGE_NOACCESS,
          "concurrent cold upload did not retain GPU ownership");
    tracker.UnmarkRegionAsGpuModified(page, page_size);
  }
  tracker.MarkRegionAsCpuModified(address, region_size * 2);
  Release(memory);
}

// Runs probe on a thread while an upload of the page at address is parked in
// its callback, holding that region's lock, and returns whether the probe
// finished within 5 s.
template <typename Probe>
bool RunDuringParkedUpload(MemoryTracker &tracker, uint64_t address,
                           Probe &&probe) {
  std::binary_semaphore upload_entered{0};
  std::binary_semaphore finish_upload{0};
  std::binary_semaphore probe_finished{0};
  std::jthread uploader([&] {
    tracker.ForEachUploadRange(
        address, Libs::Graphics::TRACKER_PAGE_SIZE, true,
        [](uint64_t, uint64_t) noexcept {},
        [&]() noexcept {
          upload_entered.release();
          finish_upload.acquire();
        });
  });
  upload_entered.acquire();
  std::jthread worker([&] {
    probe();
    probe_finished.release();
  });
  const bool finished = probe_finished.try_acquire_for(std::chrono::seconds(5));
  finish_upload.release();
  uploader.join();
  worker.join();
  return finished;
}

void TestUploadDoesNotSerializeDisjointRegion() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(region_size * 2 / page_size);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto second_region =
      (allocation_base & ~(region_size - 1)) + region_size;
  Check(second_region + page_size <= allocation_base + region_size * 2,
        "test allocation does not span two tracker regions");

  // Publish both managers before the concurrent section so this test measures
  // tracker access serialization rather than manager allocation.
  Check(tracker.IsRegionCpuModified(allocation_base, page_size) &&
            tracker.IsRegionCpuModified(second_region, page_size),
        "disjoint upload setup did not initialize both regions");

  const bool completed_while_upload_blocked =
      RunDuringParkedUpload(tracker, allocation_base, [&] {
        MarkGpuModified(tracker, second_region, page_size);
      });
  const bool second_marked =
      tracker.IsRegionGpuModified(second_region, page_size);

  tracker.UnmarkRegionAsGpuModified(allocation_base, page_size);
  tracker.UnmarkRegionAsGpuModified(second_region, page_size);
  tracker.MarkRegionAsCpuModified(allocation_base, region_size * 2);
  Release(memory);
  Check(completed_while_upload_blocked && second_marked,
        "upload callback serialized an unrelated tracker region");
}

void TestQueriesSkipRegionLock() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(1);
  const auto address = reinterpret_cast<uint64_t>(memory);

  bool cpu_modified = true;
  bool gpu_modified = true;
  // The upload holds the region lock until its writes are marked.
  const bool completed_while_upload_blocked =
      RunDuringParkedUpload(tracker, address, [&] {
        cpu_modified = tracker.IsRegionCpuModified(address, page_size);
        gpu_modified = tracker.IsRegionGpuModified(address, page_size);
        tracker.ForEachUploadRange(
            address, page_size, false, [](uint64_t, uint64_t) noexcept {},
            []() noexcept {});
      });

  const bool marked = tracker.IsRegionGpuModified(address, page_size);
  tracker.UnmarkRegionAsGpuModified(address, page_size);
  tracker.MarkRegionAsCpuModified(address, page_size);
  Release(memory);
  Check(completed_while_upload_blocked && !cpu_modified && !gpu_modified &&
            marked,
        "dirty queries or a clean read-only upload waited for the region lock");
}

void TestDownloadSkipsRegionLock() {
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(2);
  const auto address = reinterpret_cast<uint64_t>(memory);
  const auto clean = address + page_size;
  tracker.ForEachUploadRange(
      clean, page_size, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});

  uint32_t downloads = 0;
  // A GPU-clean page of a region held by an upload has nothing to download or unmark.
  const bool completed_while_upload_blocked =
      RunDuringParkedUpload(tracker, address, [&] {
        tracker.ForEachDownloadRange<true>(
            clean, page_size,
            [&](uint64_t, uint64_t) noexcept { downloads++; });
        tracker.UnmarkRegionAsGpuModified(clean, page_size);
      });

  uint32_t dirty_downloads = 0;
  tracker.ForEachDownloadRange<true>(
      address, page_size * 2,
      [&](uint64_t, uint64_t) noexcept { dirty_downloads++; });
  const bool cleared = !tracker.IsRegionGpuModified(address, page_size * 2);
  tracker.MarkRegionAsCpuModified(address, page_size * 2);
  Release(memory);
  Check(completed_while_upload_blocked && downloads == 0 &&
            dirty_downloads == 1 && cleared,
        "a GPU-clean download or unmark waited for the region lock, or a "
        "GPU-dirty page was not downloaded");
}

void TestDownloadDoesNotSerializeDisjointRegion() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(region_size * 2 / page_size);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto second_region =
      (allocation_base & ~(region_size - 1)) + region_size;
  Check(second_region + page_size <= allocation_base + region_size * 2,
        "test allocation does not span two tracker regions");

  tracker.ForEachUploadRange(
      allocation_base, page_size, true, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  tracker.ForEachUploadRange(
      second_region, page_size, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});

  std::binary_semaphore download_entered{0};
  std::binary_semaphore finish_download{0};
  std::binary_semaphore mutation_finished{0};
  std::jthread downloader([&] {
    tracker.ForEachDownloadRange<false>(
        allocation_base, second_region + page_size - allocation_base,
        [&](uint64_t address, uint64_t) noexcept {
          if (address == allocation_base) {
            download_entered.release();
            finish_download.acquire();
          }
        });
  });
  download_entered.acquire();
  std::jthread mutation([&] {
    MarkGpuModified(tracker, second_region, page_size);
    mutation_finished.release();
  });

  const bool completed_while_download_blocked =
      mutation_finished.try_acquire_for(std::chrono::seconds(5));
  finish_download.release();
  downloader.join();
  mutation.join();

  const bool both_gpu_owned =
      tracker.IsRegionGpuModified(allocation_base, page_size) &&
      tracker.IsRegionGpuModified(second_region, page_size);
  tracker.UnmarkRegionAsGpuModified(allocation_base, page_size);
  tracker.UnmarkRegionAsGpuModified(second_region, page_size);
  tracker.MarkRegionAsCpuModified(allocation_base, region_size * 2);
  Release(memory);
  Check(completed_while_download_blocked && both_gpu_owned,
        "download callback serialized an unrelated tracker region");
}

void TestGpuUnmarkUsesRegionMask() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(region_size * 2 / page_size);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  Check(region_base + region_size + page_size <=
            allocation_base + region_size * 2,
        "test allocation does not span two complete tracker regions");

  const auto sparse_begin = region_base + page_size;
  tracker.ForEachUploadRange(
      sparse_begin, page_size * 3, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  MarkGpuModified(tracker, sparse_begin, page_size);
  MarkGpuModified(tracker, sparse_begin + page_size * 2, page_size);
  ResetProtectionLog();
  tracker.UnmarkRegionAsGpuModified(sparse_begin, page_size * 3);
  Check(
      g_protection_calls == 1 && g_protection_log.size() == 1 &&
          g_protection_log[0].address == sparse_begin &&
          g_protection_log[0].size == page_size * 3 &&
          g_protection_log[0].mode == Common::VirtualMemory::Mode::Read &&
          !tracker.IsRegionGpuModified(sparse_begin, page_size * 3) &&
          Protection(reinterpret_cast<void *>(sparse_begin)) == PAGE_READONLY &&
          Protection(reinterpret_cast<void *>(sparse_begin + page_size)) ==
              PAGE_READONLY &&
          Protection(reinterpret_cast<void *>(sparse_begin + page_size * 2)) ==
              PAGE_READONLY,
      "GPU unmark did not coalesce a sparse 4 MiB region mask");
  ResetProtectionLog();
  tracker.UnmarkRegionAsGpuModified(sparse_begin, page_size * 3);
  Check(g_protection_calls == 0,
        "idempotent GPU unmark performed a protection call");

  const auto boundary = region_base + region_size;
  const auto cross_begin = boundary - page_size;
  tracker.ForEachUploadRange(
      cross_begin, page_size * 2, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  MarkGpuModified(tracker, cross_begin, page_size * 2);
  ResetProtectionLog();
  tracker.UnmarkRegionAsGpuModified(cross_begin, page_size * 2);
  Check(g_protection_calls == 2 && g_protection_log.size() == 2 &&
            g_protection_log[0].address == cross_begin &&
            g_protection_log[0].size == page_size &&
            g_protection_log[0].mode == Common::VirtualMemory::Mode::Read &&
            g_protection_log[1].address == boundary &&
            g_protection_log[1].size == page_size &&
            g_protection_log[1].mode == Common::VirtualMemory::Mode::Read &&
            !tracker.IsRegionGpuModified(cross_begin, page_size * 2),
        "cross-region GPU unmark did not use one update per 4 MiB region");

  tracker.MarkRegionAsCpuModified(allocation_base, region_size * 2);
  Release(memory);
}

// UnmarkRegionsAsGpuModified leaves what separate UnmarkRegionAsGpuModified calls leave, with one
// protection call per tracker region where the pages between the ranges are readable already.
void TestBatchedGpuUnmark() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(region_size * 2 / page_size);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  Check(region_base + region_size + page_size * 2 <=
            allocation_base + region_size * 2,
        "test allocation does not span two tracker regions");
  const auto page = [&](uint64_t index) { return region_base + page_size * index; };
  const auto protection = [&](uint64_t index) {
    return Protection(reinterpret_cast<void *>(page(index)));
  };

  tracker.ForEachUploadRange(
      page(0), page_size * 8, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  MarkGpuModified(tracker, page(1), page_size);
  MarkGpuModified(tracker, page(3), page_size);
  MarkGpuModified(tracker, page(5), page_size);
  const GuestRange outer[] = {{page(1), page_size}, {page(5), page_size}};
  ResetProtectionLog();
  tracker.UnmarkRegionsAsGpuModified(outer);
  Check(g_protection_calls == 2 &&
            g_protection_log[0].address == page(1) &&
            g_protection_log[0].size == page_size &&
            g_protection_log[1].address == page(5) &&
            g_protection_log[1].size == page_size &&
            !tracker.IsRegionGpuModified(page(1), page_size) &&
            tracker.IsRegionGpuModified(page(3), page_size) &&
            !tracker.IsRegionGpuModified(page(5), page_size) &&
            protection(1) == PAGE_READONLY && protection(3) == PAGE_NOACCESS &&
            protection(5) == PAGE_READONLY,
        "batched GPU unmark joined ranges across a GPU-dirty page");

  MarkGpuModified(tracker, page(1), page_size);
  MarkGpuModified(tracker, page(5), page_size);
  const GuestRange all[] = {{page(1), page_size}, {page(3), page_size}, {page(5), page_size}};
  ResetProtectionLog();
  tracker.UnmarkRegionsAsGpuModified(all);
  Check(g_protection_calls == 1 && g_protection_log[0].address == page(1) &&
            g_protection_log[0].size == page_size * 5 &&
            g_protection_log[0].mode == Common::VirtualMemory::Mode::Read &&
            !tracker.IsRegionGpuModified(page(0), page_size * 8) &&
            protection(1) == PAGE_READONLY && protection(2) == PAGE_READONLY &&
            protection(3) == PAGE_READONLY && protection(5) == PAGE_READONLY,
        "batched GPU unmark did not share one protection call in a region");
  ResetProtectionLog();
  tracker.UnmarkRegionsAsGpuModified(all);
  Check(g_protection_calls == 0, "idempotent batched GPU unmark protected pages");

  const auto boundary = region_base + region_size;
  tracker.ForEachUploadRange(
      boundary - page_size, page_size * 3, false,
      [](uint64_t, uint64_t) noexcept {}, []() noexcept {});
  MarkGpuModified(tracker, boundary - page_size, page_size * 3);
  const GuestRange cross[] = {{boundary - page_size, page_size * 2},
                              {boundary + page_size, page_size}};
  ResetProtectionLog();
  tracker.UnmarkRegionsAsGpuModified(cross);
  Check(g_protection_calls == 2 &&
            g_protection_log[0].address == boundary - page_size &&
            g_protection_log[0].size == page_size &&
            g_protection_log[1].address == boundary &&
            g_protection_log[1].size == page_size * 2 &&
            !tracker.IsRegionGpuModified(boundary - page_size, page_size * 3),
        "batched GPU unmark did not use one update per tracker region");

  tracker.MarkRegionAsCpuModified(allocation_base, region_size * 2);
  Release(memory);
}

void TestFullRegionGpuUnmarkBatching() {
  constexpr auto region_size = Libs::Graphics::TRACKER_REGION_SIZE;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  auto *memory = Allocate(region_size * 2 / page_size);
  const auto allocation_base = reinterpret_cast<uint64_t>(memory);
  const auto region_base =
      (allocation_base + region_size - 1) & ~(region_size - 1);
  Check(region_base + region_size <= allocation_base + region_size * 2,
        "test allocation does not contain a complete tracker region");

  tracker.ForEachUploadRange(
      region_base, region_size, false, [](uint64_t, uint64_t) noexcept {},
      []() noexcept {});
  MarkGpuModified(tracker, region_base, region_size);
  Check(tracker.IsRegionGpuModified(region_base, region_size) &&
            Protection(reinterpret_cast<void *>(region_base)) ==
                PAGE_NOACCESS &&
            Protection(reinterpret_cast<void *>(region_base + region_size -
                                                page_size)) == PAGE_NOACCESS,
        "full-region setup did not establish GPU read protection");

  ResetProtectionLog();
  tracker.UnmarkRegionAsGpuModified(region_base, region_size);
  Check(
      g_protection_log.size() == 1 &&
          g_protection_log[0].address == region_base &&
          g_protection_log[0].size == region_size &&
          g_protection_log[0].mode == Common::VirtualMemory::Mode::Read &&
          !tracker.IsRegionGpuModified(region_base, region_size) &&
          Protection(reinterpret_cast<void *>(region_base)) == PAGE_READONLY &&
          Protection(reinterpret_cast<void *>(region_base + region_size -
                                              page_size)) == PAGE_READONLY,
      "full-region GPU unmark did not use one exact 4 MiB protection request");

  tracker.MarkRegionAsCpuModified(allocation_base, region_size * 2);
  Release(memory);
}

[[noreturn]] void RunDeathCase(const char *name) {
  TrackerHarness harness;
  auto &tracker = harness.tracker;
  constexpr auto page_size = Libs::Graphics::TRACKER_PAGE_SIZE;
  auto *memory = Allocate(1);
  const auto address = reinterpret_cast<uint64_t>(memory);
  if (std::strcmp(name, "gpu-dirty-explicit-cpu") == 0) {
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        []() noexcept {});
    tracker.MarkRegionAsCpuModified(address, page_size);
  } else if (std::strcmp(name, "reentrant-upload") == 0) {
    tracker.ForEachUploadRange(
        address, page_size, true, [](uint64_t, uint64_t) noexcept {},
        [&]() noexcept {
          tracker.MarkRegionAsCpuModified(address, page_size);
        });
  } else if (std::strcmp(name, "recursive-tracking-lock") == 0) {
    Libs::Graphics::TrackingSpinLock lock;
    lock.lock();
    lock.lock();
  } else if (std::strcmp(name, "non-owner-tracking-unlock") == 0) {
    Libs::Graphics::TrackingSpinLock lock;
    lock.lock();
    std::thread worker([&] { lock.unlock(); });
    worker.join();
  }
  std::_Exit(0x7f);
}

void CheckDeathCase(const char *name) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
  char path[MAX_PATH]{};
  Check(GetModuleFileNameA(nullptr, path, MAX_PATH) != 0,
        "GetModuleFileName failed");
  std::string command = std::string("\"") + path + "\" --death " + name;
  std::vector<char> mutable_command(command.begin(), command.end());
  mutable_command.push_back('\0');
  STARTUPINFOA startup{sizeof(startup)};
  PROCESS_INFORMATION process{};
  Check(CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                       &process) != 0,
        "CreateProcess failed");
  Check(WaitForSingleObject(process.hProcess, 10000) == WAIT_OBJECT_0,
        "MemoryTracker death test timed out");
  DWORD exit_code = 0;
  Check(
      GetExitCodeProcess(process.hProcess, &exit_code) != 0 &&
          (exit_code == 321 || exit_code == EXCEPTION_NONCONTINUABLE_EXCEPTION),
      "MemoryTracker death path used the wrong exit");
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
#else
#if defined(__APPLE__)
  std::vector<char> path(PATH_MAX);
  uint32_t path_size = static_cast<uint32_t>(path.size());
  if (_NSGetExecutablePath(path.data(), &path_size) != 0) {
    path.resize(path_size);
    Check(_NSGetExecutablePath(path.data(), &path_size) == 0,
          "_NSGetExecutablePath failed");
  }
#endif
  const pid_t pid = ::fork();
  Check(pid >= 0, "fork failed");
  if (pid == 0) {
#if defined(__APPLE__)
    ::execl(path.data(), "MemoryTrackerTests", "--death", name, nullptr);
#else
    ::execl("/proc/self/exe", "MemoryTrackerTests", "--death", name, nullptr);
#endif
    std::_Exit(0x7e);
  }
  int status = 0;
  Check(::waitpid(pid, &status, 0) == pid, "waitpid failed");
  const bool fatal_exit =
      WIFEXITED(status) && WEXITSTATUS(status) == (321 & 0xff);
  Check(fatal_exit || WIFSIGNALED(status),
        "MemoryTracker death path used the wrong exit");
#endif
}

void TestFatalPaths() {
  for (const char *name : {"gpu-dirty-explicit-cpu", "reentrant-upload",
                           "recursive-tracking-lock", "non-owner-tracking-unlock"}) {
    CheckDeathCase(name);
  }
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
void *g_fault_stack = nullptr;
constexpr size_t FAULT_STACK_SIZE = 64 * 1024;
volatile sig_atomic_t g_stack_faults = 0;

bool HandleStackFault(const Common::HostException::ExceptionInfo &info) {
  using namespace Common::HostException;
  stack_t active_stack{};
  const auto fault_address = reinterpret_cast<uintptr_t>(g_fault_stack) +
                             FAULT_STACK_SIZE - sizeof(uintptr_t);
  if (info.type != ExceptionType::AccessViolation ||
      info.access_violation_type != AccessViolationType::Write ||
      info.access_violation_vaddr != fault_address ||
      ::sigaltstack(nullptr, &active_stack) != 0 ||
      (active_stack.ss_flags & SS_ONSTACK) == 0) {
    std::_Exit(1);
  }
  g_stack_faults = 1;
  return ::mprotect(g_fault_stack, FAULT_STACK_SIZE,
                    PROT_READ | PROT_WRITE) == 0;
}

// A stack write must fault before any signal frame can use the protected stack.
__attribute__((naked)) void WriteProtectedStack(void *) {
  asm volatile("mov %rsp, %rax\n"
               "mov %rdi, %rsp\n"
               "push %rax\n"
               "pop %rsp\n"
               "ret\n");
}

void TestFaultOnProtectedStack() {
  const pid_t pid = ::fork();
  Check(pid >= 0, "stack fault fork failed");
  if (pid == 0) {
    std::thread worker([] {
      Check(Common::HostException::InitializeThreadSignalStack(),
            "initialize thread signal stack failed");
      Check(Common::HostException::InstallHandler(HandleStackFault),
            "install stack fault handler failed");
      g_fault_stack = ::mmap(nullptr, FAULT_STACK_SIZE, PROT_READ,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      Check(g_fault_stack != MAP_FAILED, "allocate protected stack failed");
      WriteProtectedStack(static_cast<char *>(g_fault_stack) + FAULT_STACK_SIZE);
      Check(g_stack_faults == 1, "protected stack write did not resume");
      struct sigaction action{};
      Check(::sigaction(SIGSEGV, nullptr, &action) == 0 &&
                action.sa_handler != SIG_DFL,
            "stack fault reset the process handler");
      Check(::munmap(g_fault_stack, FAULT_STACK_SIZE) == 0,
            "release protected stack failed");
    });
    worker.join();
    std::_Exit(0);
  }
  int status = 0;
  Check(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
            WEXITSTATUS(status) == 0,
        "fault on a protected stack did not recover");
}
#endif

} // namespace

namespace Libs::LibKernel::Memory {

bool ProtectGuestHostMemory(uint64_t vaddr, uint64_t size,
                            Common::VirtualMemory::Mode mode) {
  return ProtectAddressSpace(vaddr, size, mode);
}

} // namespace Libs::LibKernel::Memory

int main(int argc, char **argv) {
  // The cases cover the rewritten-page mode too, which is off by default.
  Libs::Graphics::g_sticky_pages.store(true);
  if (argc == 3 && std::strcmp(argv[1], "--death") == 0) {
    RunDeathCase(argv[2]);
  }
  if (argc == 2 && std::strcmp(argv[1], "--benchmark-clean-upload") == 0) {
    BenchmarkCleanUploads();
    return 0;
  }
  TestGuestRange();
  TestRangeSet();
  TestRangeSetExtractAndReAdd();
  TestRangeSetMatchesByteModel();
  TestQueriesDoNotRequireMappedOwnership();
  TestConcurrentRegionPublication();
  TestCpuDirtyUpload();
  TestRepeatedUploadSkip();
  TestUploadReportsGenerations();
  TestExactHotPages();
  TestCleanUploadPreservesOwnership();
  TestRangeInvalidation();
  TestGpuReacquisitionAfterInvalidation();
  TestGpuDirtyBits();
  TestExactDirtyIntervalsSharingTrackerPage();
  TestGpuDownloadProtectionMirrors();
  TestCrossRegionUpload();
  TestConcurrentColdUploads();
  TestUploadDoesNotSerializeDisjointRegion();
  TestQueriesSkipRegionLock();
  TestDownloadSkipsRegionLock();
  TestDownloadDoesNotSerializeDisjointRegion();
  TestGpuUnmarkUsesRegionMask();
  TestFullRegionGpuUnmarkBatching();
  TestBatchedGpuUnmark();
  TestBatchedUploadProtection();
  TestFatalPaths();
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
  TestFaultOnProtectedStack();
#endif
  std::puts("MemoryTrackerTests: all cases passed");
  return 0;
}
