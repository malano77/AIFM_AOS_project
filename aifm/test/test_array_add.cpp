extern "C" {
#include <runtime/runtime.h>
}

#include "array.hpp"
#include "device.hpp"
#include "hotness_tracker.hpp"
#include "manager.hpp"
#include "stats.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <random>

using namespace far_memory;
using namespace std;

constexpr uint64_t kCacheSize = (128ULL << 20);
constexpr uint64_t kFarMemSize = (4ULL << 30);
constexpr uint32_t kNumGCThreads = 12;
constexpr uint32_t kNumEntries =
    (16ULL << 20); // So the array size is larger than the local cache size.

uint64_t raw_array_A[kNumEntries];
uint64_t raw_array_B[kNumEntries];
uint64_t raw_array_C[kNumEntries];

template <uint64_t N, typename T>
void copy_array(Array<T, N> *array, T *raw_array) {
  for (uint64_t i = 0; i < N; i++) {
    DerefScope scope;
    (*array).at_mut(scope, i) = raw_array[i];
  }
}

template <typename T, uint64_t N>
void add_array(Array<T, N> *array_C, Array<T, N> *array_A,
               Array<T, N> *array_B) {
  for (uint64_t i = 0; i < N; i++) {
    DerefScope scope;
    (*array_C).at_mut(scope, i) =
        (*array_A).at(scope, i) + (*array_B).at(scope, i);
  }
}

void gen_random_array(uint64_t num_entries, uint64_t *raw_array) {
  std::random_device rd;
  std::mt19937_64 eng(rd());
  std::uniform_int_distribution<uint64_t> distr;

  for (uint64_t i = 0; i < num_entries; i++) {
    raw_array[i] = distr(eng);
  }
}

void do_work(FarMemManager *manager) {
  cout << "Running " << __FILE__ "..." << endl;

  auto array_A = manager->allocate_array<uint64_t, kNumEntries>();
  auto array_B = manager->allocate_array<uint64_t, kNumEntries>();
  auto array_C = manager->allocate_array<uint64_t, kNumEntries>();

  gen_random_array(kNumEntries, raw_array_A);
  gen_random_array(kNumEntries, raw_array_B);
  copy_array(&array_A, raw_array_A);
  copy_array(&array_B, raw_array_B);
  add_array(&array_C, &array_A, &array_B);

  for (uint64_t i = 0; i < kNumEntries; i++) {
    DerefScope scope;
    if (array_C.at(scope, i) != raw_array_A[i] + raw_array_B[i]) {
      goto fail;
    }
  }

  cout << "Passed" << endl;
  HotnessTracker::dump("test_array_add");
  HotnessTracker::reset();
#ifdef MONITOR_READ_OBJECT_CYCLES
  std::cout << "[Stats] read_ops=" << Stats::get_num_read_object_ops()
            << " total_read_cycles=" << Stats::get_total_read_object_cycles()
            << " avg_read_cycles=" << Stats::get_avg_read_object_cycles()
            << std::endl;
#endif
#ifdef MONITOR_WRITE_OBJECT_CYCLES
  std::cout << "[Stats] write_ops=" << Stats::get_num_write_object_ops()
            << " total_write_cycles=" << Stats::get_total_write_object_cycles()
            << " avg_write_cycles=" << Stats::get_avg_write_object_cycles()
            << std::endl;
#endif
  return;

fail:
  cout << "Failed" << endl;
  HotnessTracker::dump("test_array_add");
  HotnessTracker::reset();
  return;
}

void _main(void *arg) {
  std::unique_ptr<FarMemManager> manager =
      std::unique_ptr<FarMemManager>(FarMemManagerFactory::build(
          kCacheSize, kNumGCThreads, new DRAMDevice(kFarMemSize)));
  Stats::reset_read_object_cycle_stats();
  Stats::reset_write_object_cycle_stats();
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
