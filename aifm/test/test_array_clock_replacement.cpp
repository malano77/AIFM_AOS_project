extern "C" {
#include <runtime/runtime.h>
#include <runtime/timer.h>
}

#include "thread.h"

#include "deref_scope.hpp"
#include "device.hpp"
#include "helpers.hpp"
#include "hotness_tracker.hpp"
#include "manager.hpp"
#include "stats.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <unistd.h>

using namespace far_memory;
using namespace std;

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))
#define DEFINE_DATA_TYPE(width)                                                \
  struct Data_##width {                                                        \
    uint8_t buf[width];                                                        \
  };                                                                           \
  using Data_t = Data_##width;

DEFINE_DATA_TYPE(4096);

constexpr uint64_t kCacheSize = 1ULL << 30;   // 1 GiB.
constexpr uint64_t kFarMemSize = 20ULL << 30; // 20 GiB.
constexpr uint64_t kNumGCThreads = 4;
constexpr uint64_t kLargePtrsNumEntries = 1 << 20;         // 1 M entries.
constexpr uint64_t kSmallPtrsNumEntries = 1 << 4;          // 16 entries.
constexpr uint32_t kNumAccessedLargePtrsPerIter = 1 << 10; // 1 K entries.
constexpr uint64_t kTestTimes = 16;
constexpr uint64_t kMaxAllowedMissCnts = 5;

static_assert(kLargePtrsNumEntries % kNumAccessedLargePtrsPerIter == 0);

template <int64_t N> inline void read_raw_array(const uint8_t *ptr) {
  if constexpr (N >= 8) {
    ACCESS_ONCE(*reinterpret_cast<const uint64_t *>(ptr));
    read_raw_array<N - 8>(ptr + 8);
  } else if constexpr (N >= 4) {
    ACCESS_ONCE(*reinterpret_cast<const uint32_t *>(ptr));
    read_raw_array<N - 4>(ptr + 4);
  } else if constexpr (N >= 2) {
    ACCESS_ONCE(*reinterpret_cast<const uint16_t *>(ptr));
    read_raw_array<N - 2>(ptr + 2);
  } else if constexpr (N == 1) {
    ACCESS_ONCE(*reinterpret_cast<const uint8_t *>(ptr));
    read_raw_array<N - 1>(ptr + 1);
  }
}

namespace far_memory {
class FarMemTest {
public:
  UniquePtr<Data_t> large_ptrs[kLargePtrsNumEntries];
  UniquePtr<Data_t> small_ptrs[kSmallPtrsNumEntries];
  uint64_t small_ptrs_miss_cnt[kSmallPtrsNumEntries];

  void flush_core_local_regions(FarMemManager *mgr) {
    for (int i = 0; i < helpers::kNumCPUs; i++) {
      mgr->cache_region_manager_.try_refill_core_local_free_region(
          0, &(mgr->cache_region_manager_.core_local_free_regions_[i]));
      mgr->cache_region_manager_.try_refill_core_local_free_region(
          1, &(mgr->cache_region_manager_.core_local_free_nt_regions_[i]));
    }
  }

  void run(FarMemManager *manager) {
    for (uint64_t i = 0; i < kLargePtrsNumEntries; i++) {
      large_ptrs[i] = std::move(manager->allocate_unique_ptr<Data_t>());
    }
    flush_core_local_regions(manager);

    for (uint64_t i = 0; i < kSmallPtrsNumEntries; i++) {
      small_ptrs[i] = std::move(manager->allocate_unique_ptr<Data_t>());
    }

    for (uint8_t k = 0; k < kTestTimes; k++) {
      uint32_t large_ptrs_idx = 0;
      uint32_t small_ptrs_idx = 0;
      while (large_ptrs_idx != kLargePtrsNumEntries) {
        DerefScope scope;
        for (uint32_t i = 0; i < kNumAccessedLargePtrsPerIter; i++) {
          const auto const_large_ptr =
              large_ptrs[large_ptrs_idx++].deref(scope);
          read_raw_array<sizeof(Data_t)>(
              reinterpret_cast<const uint8_t *>(const_large_ptr));
        }
        small_ptrs_miss_cnt[small_ptrs_idx] +=
            !small_ptrs[small_ptrs_idx].meta().is_present();
        const auto const_small_ptr = small_ptrs[small_ptrs_idx].deref(scope);
        read_raw_array<sizeof(Data_t)>(
            reinterpret_cast<const uint8_t *>(const_small_ptr));
        small_ptrs_idx = (small_ptrs_idx + 1) % kSmallPtrsNumEntries;
      }
    }

    for (uint32_t i = 0; i < kSmallPtrsNumEntries; i++) {
      if (small_ptrs_miss_cnt[i] > kMaxAllowedMissCnts) {
        // When completely running out of space, the GC thread fails to
        // allocate "to space" for the hot objects, therefore the objects
        // have to be evacuated. This leads to few misses.
        std::cout << "Failed " << small_ptrs_miss_cnt[i] << std::endl;
        return;
      }
    }

    std::cout << "Passed" << std::endl;
    // HotnessTracker::dump("test_array_clock_replacement");
    // HotnessTracker::reset();
#ifdef MONITOR_READ_OBJECT_CYCLES
    std::cout << "[Stats] read_ops=" << Stats::get_num_read_object_ops()
              << " total_read_cycles=" << Stats::get_total_read_object_cycles()
              << " avg_read_cycles=" << Stats::get_avg_read_object_cycles()
              << std::endl;
#endif
#ifdef MONITOR_WRITE_OBJECT_CYCLES
    std::cout << "[Stats] write_ops=" << Stats::get_num_write_object_ops()
              << " total_write_cycles="
              << Stats::get_total_write_object_cycles()
              << " avg_write_cycles=" << Stats::get_avg_write_object_cycles()
              << std::endl;
#endif
  }
};
} // namespace far_memory

void do_work(FarMemManager *manager) {
  cout << "Running " << __FILE__ "..." << endl;
  auto test = std::make_unique<FarMemTest>();
  Stats::reset_read_object_cycle_stats();
  Stats::reset_write_object_cycle_stats();
  test->run(manager);
}

void _main(void *arg) {
  std::unique_ptr<FarMemManager> manager =
      std::unique_ptr<FarMemManager>(FarMemManagerFactory::build(
          kCacheSize, kNumGCThreads, new DRAMDevice(kFarMemSize)));
  do_work(manager.get());
}

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 2) {
    std::cerr << "usage: [cfg_file]" << std::endl;
    return -EINVAL;
  }

  ret = runtime_init(argv[1], _main, NULL);
  if (ret) {
    std::cerr << "failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
