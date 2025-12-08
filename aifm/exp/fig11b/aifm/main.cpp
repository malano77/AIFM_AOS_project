extern "C" {
#include <runtime/runtime.h>
}

#include "snappy.h"

#include "array.hpp"
#include "deref_scope.hpp"
#include "device.hpp"
#include "helpers.hpp"
#include "hotness_tracker.hpp"
#include "manager.hpp"
#include "stats.hpp"

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <string>
#include <unistd.h>

constexpr uint64_t kCacheSize = 512 * Region::kSize;
constexpr uint64_t kFarMemSize = 20ULL << 30;
constexpr uint64_t kNumGCThreads = 15;
constexpr uint64_t kNumConnections = 600;
constexpr uint64_t kCompressedFileSize = 507860747;
constexpr uint64_t kCompressedFileNumBlocks =
    ((kCompressedFileSize - 1) / snappy::FileBlock::kSize) + 1;
constexpr uint32_t kNumCompressedFiles = 30;
constexpr bool kUseTpAPI = false;

using namespace std;

alignas(4096) snappy::FileBlock file_block;
std::unique_ptr<Array<snappy::FileBlock, kCompressedFileNumBlocks>>
    fm_array_ptrs[kNumCompressedFiles];

void write_file_to_string(const string &file_path, const string &str) {
  std::ofstream fs(file_path);
  fs << str;
  fs.close();
}

void flush_cache() {
  for (uint32_t k = 0; k < kNumCompressedFiles; k++) {
    fm_array_ptrs[k]->disable_prefetch();
  }
  for (uint32_t i = 0; i < kCompressedFileNumBlocks; i++) {
    for (uint32_t k = 0; k < kNumCompressedFiles; k++) {
      file_block = fm_array_ptrs[k]->read(i);
      ACCESS_ONCE(file_block.data[0]);
    }
  }
  for (uint32_t k = 0; k < kNumCompressedFiles; k++) {
    fm_array_ptrs[k]->enable_prefetch();
  }
}

void read_files_to_fm_array(const string &in_file_path) {
  int fd = open(in_file_path.c_str(), O_RDONLY | O_DIRECT);
  if (fd == -1) {
    helpers::dump_core();
  }
  // Read file and save data into the far-memory array.
  int64_t sum = 0, cur = snappy::FileBlock::kSize, tmp;
  while (sum != kCompressedFileSize) {
    BUG_ON(cur != snappy::FileBlock::kSize);
    cur = 0;
    while (cur < (int64_t)snappy::FileBlock::kSize) {
      tmp = read(fd, file_block.data + cur, snappy::FileBlock::kSize - cur);
      if (tmp <= 0) {
        break;
      }
      cur += tmp;
    }
    for (uint32_t i = 0; i < kNumCompressedFiles; i++) {
      DerefScope scope;
      fm_array_ptrs[i]->at_mut(scope, sum / snappy::FileBlock::kSize) =
          file_block;
    }
    sum += cur;
    if ((sum % (1 << 20)) == 0) {
      cerr << "Have read " << sum << " bytes." << endl;
    }
  }
  if (sum != kCompressedFileSize) {
    helpers::dump_core();
  }

  // Flush the cache to ensure there's no pending dirty data.
  flush_cache();

  close(fd);
}

void fm_uncompress_files_bench(const string &in_file_path,
                               const string &out_file_path) {
  string out_str;
  read_files_to_fm_array(in_file_path);
  auto start = chrono::steady_clock::now();
  for (uint32_t i = 0; i < kNumCompressedFiles; i++) {
    std::cout << "Uncompressing file " << i << std::endl;
    snappy::Uncompress<kCompressedFileNumBlocks, kUseTpAPI>(
        fm_array_ptrs[i].get(), kCompressedFileSize, &out_str);
  }
  auto end = chrono::steady_clock::now();
  cout << "Elapsed time in microseconds : "
       << chrono::duration_cast<chrono::microseconds>(end - start).count()
       << " µs" << endl;

  // write_file_to_string(out_file_path, out_str);
}

void do_work() {
  std::unique_ptr<FarMemManager> manager =
      std::unique_ptr<FarMemManager>(FarMemManagerFactory::build(
          kCacheSize, kNumGCThreads, new DRAMDevice(kFarMemSize)));
  for (uint32_t i = 0; i < kNumCompressedFiles; i++) {
    fm_array_ptrs[i].reset(
        manager->allocate_array_heap<snappy::FileBlock,
                                     kCompressedFileNumBlocks>());
  }

  fm_uncompress_files_bench("/mnt/enwik9.compressed",
                            "/mnt/enwik9.uncompressed.tmp");
  HotnessTracker::dump("snappy");
  HotnessTracker::reset();

  std::cout << "Force existing..." << std::endl;
  exit(0);
}

int argc;
void my_main(void *arg) {
  (void)arg;
  do_work();
}

int main(int _argc, char *argv[]) {
  if (_argc < 2) {
    std::cerr << "usage: [cfg_file]" << std::endl;
    return -EINVAL;
  }

  int ret = runtime_init(argv[1], my_main, argv);
  if (ret) {
    std::cerr << "failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
