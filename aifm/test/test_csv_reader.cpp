extern "C" {
#include <runtime/runtime.h>
}

#include "CSV/csv.hpp"
#include "dataframe_vector.hpp"
#include "deref_scope.hpp"
#include "device.hpp"
#include "helpers.hpp"
#include "manager.hpp"
#include "hotness_tracker.hpp"
#include "stats.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

using namespace far_memory;
using namespace std;

constexpr uint64_t kCacheSize = 512 * Region::kSize;
constexpr uint64_t kFarMemSize = (1ULL << 33); // 8 GB.
constexpr uint64_t kWorkSetSize = 1 << 30;
constexpr uint64_t kNumGCThreads = 12;
constexpr uint64_t kNumEntries = 64 << 20; // 64 million entries.

namespace far_memory {
class FarMemTest {
private:
public:
  void do_work(FarMemManager *manager) {
    auto my_tuple =
        io::parse_csv_to_vectors<int, SimpleTime, SimpleTime, int, double,
                                 double, double, int, char, double, double, int,
                                 double, double, double, double, double, double,
                                 double>(
            manager, "test/test_csv_reader.csv", "VendorID",
            "tpep_pickup_datetime", "tpep_dropoff_datetime", "passenger_count",
            "trip_distance", "pickup_longitude", "pickup_latitude",
            "RatecodeID", "store_and_fwd_flag", "dropoff_longitude",
            "dropoff_latitude", "payment_type", "fare_amount", "extra",
            "mta_tax", "tip_amount", "tolls_amount", "improvement_surcharge",
            "total_amount");

    DerefScope scope;
    TEST_ASSERT(std::get<0>(my_tuple).at(scope, 0) == 2);
    TEST_ASSERT(std::get<1>(my_tuple).at(scope, 0) ==
                SimpleTime(2016, 1, 1, 0, 0, 0));

    TEST_ASSERT(std::get<0>(my_tuple).at(scope, 7) == 1);
    TEST_ASSERT(std::get<1>(my_tuple).at(scope, 7) ==
                SimpleTime(2016, 1, 1, 0, 0, 1));

    TEST_ASSERT(std::get<0>(my_tuple).at(scope, 17) == 1);
    TEST_ASSERT(std::get<1>(my_tuple).at(scope, 17) ==
                SimpleTime(2016, 1, 1, 0, 0, 6));
    TEST_ASSERT(std::get<2>(my_tuple).at(scope, 17) ==
                SimpleTime(2016, 1, 1, 0, 7, 14));
    TEST_ASSERT(std::get<3>(my_tuple).at(scope, 17) == 1);
    TEST_ASSERT(std::abs(std::get<4>(my_tuple).at(scope, 17) - 1.7) < 1E-5);
    TEST_ASSERT(std::get<8>(my_tuple).at(scope, 17) == 'Y');
    TEST_ASSERT(
        std::abs(
            std::get<std::tuple_size<decltype(my_tuple)>::value - 1>(my_tuple)
                .at(scope, 17) -
            9.95) < 1E-5);

    cout << "Passed" << endl;
    HotnessTracker::dump("test_csv_reader");
    HotnessTracker::reset();
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
    return;
  }
};
} // namespace far_memory

void _main(void *arg) {
  auto manager = std::unique_ptr<FarMemManager>(FarMemManagerFactory::build(
      kCacheSize, kNumGCThreads, new DRAMDevice(kFarMemSize)));
  FarMemTest test;
  Stats::reset_read_object_cycle_stats();
  Stats::reset_write_object_cycle_stats();
  test.do_work(manager.get());
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
