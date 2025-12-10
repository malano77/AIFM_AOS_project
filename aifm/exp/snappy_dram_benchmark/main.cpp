extern "C" {
#include <runtime/runtime.h>
}
#include "thread.h"

#include "array.hpp"
#include "deref_scope.hpp"
#include "device.hpp"
#include "helpers.hpp"
#include "manager.hpp"
#include "snappy.h"
#include "stats.hpp"
#include "zipf.hpp"

// crypto++
#include "cryptopp/aes.h"
#include "cryptopp/filters.h"
#include "cryptopp/modes.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <random>
#include <vector>

using namespace far_memory;

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

namespace far_memory {
class FarMemTest {
private:
  // FarMemManager.
  constexpr static uint64_t kCacheSize = 256 * Region::kSize; // 512MB (assuming 2MB regions)
  constexpr static uint64_t kFarMemSize = (2ULL << 30); // 2 GB
  constexpr static uint32_t kNumGCThreads = 12;

  // Hashtable.
  constexpr static uint32_t kKeyLen = 12;
  constexpr static uint32_t kValueLen = 4;
  constexpr static uint32_t kLocalHashTableNumEntriesShift = 25; // Reduced from 28
  constexpr static uint32_t kRemoteHashTableNumEntriesShift = 25; // Reduced from 28
  constexpr static uint64_t kRemoteHashTableSlabSize = (1ULL << 30) * 1.05; // Reduced from 4
  constexpr static uint32_t kNumKVPairs = 1 << 18; // Reduced 

  // Array.
  constexpr static uint32_t kNumArrayEntries = 1 << 18; // 256 K entries.
  constexpr static uint32_t kArrayEntrySize = 4096;     // 4 K
  // Total Array Size = 256K * 4KB = 1GB. Fits in 2GB FarMem.

  // Runtime.
  constexpr static uint32_t kNumMutatorThreads = 8;
  constexpr static double kZipfParamS = 0.85;
  constexpr static uint32_t kNumKeysPerRequest = 32;
  constexpr static uint32_t kNumReqs = kNumKVPairs / kNumKeysPerRequest;
  constexpr static uint32_t kLog10NumKeysPerRequest =
      helpers::static_log(10, kNumKeysPerRequest);
  constexpr static uint32_t kReqLen = kKeyLen - kLog10NumKeysPerRequest;
  constexpr static uint32_t kReqSeqLen = kNumReqs;

  // Output.
  constexpr static uint32_t kPrintPerIters = 8192;
  constexpr static uint32_t kMaxPrintIntervalUs = 1000 * 1000; // 1 second(s).
  constexpr static uint32_t kPrintTimes = 10; // Reduced print times

  struct Req {
    char data[kReqLen];
  };

  struct Key {
    char data[kKeyLen];
  };

  union Value {
    uint32_t num;
    char data[kValueLen];
  };

  struct ArrayEntry {
    uint8_t data[kArrayEntrySize];
  };

  struct alignas(64) Cnt {
    uint64_t c;
  };

  using AppArray = Array<ArrayEntry, kNumArrayEntries>;

  std::unique_ptr<std::mt19937> generators[helpers::kNumCPUs];
  alignas(helpers::kHugepageSize) Req all_gen_reqs[kNumReqs];
  uint32_t all_zipf_req_indices[helpers::kNumCPUs][kReqSeqLen];

  Cnt req_cnts[kNumMutatorThreads];
  Cnt per_core_req_idx[helpers::kNumCPUs];

  std::atomic_flag flag;
  uint64_t print_times = 0;
  uint64_t prev_sum_reqs = 0;
  uint64_t prev_us = 0;
  std::vector<double> mops_records;

  unsigned char key[CryptoPP::AES::DEFAULT_KEYLENGTH];
  unsigned char iv[CryptoPP::AES::BLOCKSIZE];
  std::unique_ptr<CryptoPP::CBC_Mode_ExternalCipher::Encryption> cbcEncryption;
  std::unique_ptr<CryptoPP::AES::Encryption> aesEncryption;

  inline void append_uint32_to_char_array(uint32_t n, uint32_t suffix_len,
                                          char *array) {
    uint32_t len = 0;
    while (n) {
      auto digit = n % 10;
      array[len++] = digit + '0';
      n = n / 10;
    }
    while (len < suffix_len) {
      array[len++] = '0';
    }
    std::reverse(array, array + suffix_len);
  }

  inline void random_string(char *data, uint32_t len) {
    BUG_ON(len <= 0);
    preempt_disable();
    auto guard = helpers::finally([&]() { preempt_enable(); });
    auto &generator = *generators[get_core_num()];
    std::uniform_int_distribution<int> distribution('a', 'z' + 1);
    for (uint32_t i = 0; i < len; i++) {
      data[i] = char(distribution(generator));
    }
  }

  inline void random_req(char *data, uint32_t tid) {
    auto tid_len = helpers::static_log(10, kNumMutatorThreads);
    random_string(data, kReqLen - tid_len);
    append_uint32_to_char_array(tid, tid_len, data + kReqLen - tid_len);
  }

  void prepare(GenericConcurrentHopscotch *hopscotch) {
    for (uint32_t i = 0; i < helpers::kNumCPUs; i++) {
      std::random_device rd;
      generators[i].reset(new std::mt19937(rd()));
    }
    memset(key, 0x00, CryptoPP::AES::DEFAULT_KEYLENGTH);
    memset(iv, 0x00, CryptoPP::AES::BLOCKSIZE);
    aesEncryption.reset(
        new CryptoPP::AES::Encryption(key, CryptoPP::AES::DEFAULT_KEYLENGTH));
    cbcEncryption.reset(
        new CryptoPP::CBC_Mode_ExternalCipher::Encryption(*aesEncryption, iv));
    std::vector<rt::Thread> threads;
    for (uint32_t tid = 0; tid < kNumMutatorThreads; tid++) {
      threads.emplace_back(rt::Thread([&, tid]() {
        auto num_reqs_per_thread = kNumReqs / kNumMutatorThreads;
        auto req_offset = tid * num_reqs_per_thread;
        auto *thread_gen_reqs = &all_gen_reqs[req_offset];
        for (uint32_t i = 0; i < num_reqs_per_thread; i++) {
          Req req;
          random_req(req.data, tid);
          Key key;
          memcpy(key.data, req.data, kReqLen);
          for (uint32_t j = 0; j < kNumKeysPerRequest; j++) {
            append_uint32_to_char_array(j, kLog10NumKeysPerRequest,
                                        key.data + kReqLen);
            Value value;
            value.num = (j ? 0 : req_offset + i);
            DerefScope scope;
            hopscotch->put(scope, kKeyLen, (const uint8_t *)key.data, kValueLen,
                           (uint8_t *)value.data);
          }
          thread_gen_reqs[i] = req;
        }
      }));
    }
    for (auto &thread : threads) {
      thread.Join();
    }
    preempt_disable();
    zipf_table_distribution<> zipf(kNumReqs, kZipfParamS);
    auto &generator = generators[get_core_num()];
    constexpr uint32_t kPerCoreWinInterval = kReqSeqLen / helpers::kNumCPUs;
    for (uint32_t i = 0; i < kReqSeqLen; i++) {
      auto rand_idx = zipf(*generator);
      for (uint32_t j = 0; j < helpers::kNumCPUs; j++) {
        all_zipf_req_indices[j][(i + (j * kPerCoreWinInterval)) % kReqSeqLen] =
            rand_idx;
      }
    }
    preempt_enable();
  }

  void prepare(AppArray *array) {
    // Initialization if needed
  }

  void consume_array_entry(const ArrayEntry &entry) {
    std::string ciphertext;
    CryptoPP::StreamTransformationFilter stfEncryptor(
        *cbcEncryption, new CryptoPP::StringSink(ciphertext));
    stfEncryptor.Put((const unsigned char *)&entry.data, sizeof(entry));
    stfEncryptor.MessageEnd();
    std::string compressed;
    snappy::Compress(ciphertext.c_str(), ciphertext.size(), &compressed);
    auto compressed_len = compressed.size();
    ACCESS_ONCE(compressed_len);
  }

  void print_perf() {
    if (!flag.test_and_set()) {
      preempt_disable();
      auto us = microtime();
      uint64_t sum_reqs = 0;
      for (uint32_t i = 0; i < kNumMutatorThreads; i++) {
        sum_reqs += ACCESS_ONCE(req_cnts[i].c);
      }
      if (us - prev_us > kMaxPrintIntervalUs) {
        auto mops =
            ((double)(sum_reqs - prev_sum_reqs) / (us - prev_us)) * 1.098;
        mops_records.push_back(mops);
        us = microtime();
        if (print_times++ >= kPrintTimes) {
          std::cout << "mops = "
                    << accumulate(mops_records.begin(), mops_records.end(),
                                  0.0) /
                           mops_records.size()
                    << std::endl;
          exit(0);
        }
        prev_us = us;
        prev_sum_reqs = sum_reqs;
      }
      preempt_enable();
      flag.clear();
    }
  }

  void bench(GenericConcurrentHopscotch *hopscotch, AppArray *array) {
    std::vector<rt::Thread> threads;
    prev_us = microtime();
    for (uint32_t tid = 0; tid < kNumMutatorThreads; tid++) {
      threads.emplace_back(rt::Thread([&, tid]() {
        uint32_t cnt = 0;
        while (1) {
          if (unlikely(cnt++ % kPrintPerIters == 0)) {
            preempt_disable();
            print_perf();
            preempt_enable();
          }
          preempt_disable();
          auto core_num = get_core_num();
          auto req_idx =
              all_zipf_req_indices[core_num][per_core_req_idx[core_num].c];
          if (unlikely(++per_core_req_idx[core_num].c == kReqSeqLen)) {
            per_core_req_idx[core_num].c = 0;
          }
          preempt_enable();

          auto &req = all_gen_reqs[req_idx];
          Key key;
          memcpy(key.data, req.data, kReqLen);
          uint32_t array_index = 0;
          {
            DerefScope scope;
            for (uint32_t i = 0; i < kNumKeysPerRequest; i++) {
              append_uint32_to_char_array(i, kLog10NumKeysPerRequest,
                                          key.data + kReqLen);
              Value value;
              uint16_t value_len;
              hopscotch->get(scope, kKeyLen, (const uint8_t *)key.data,
                             &value_len, (uint8_t *)value.data);
              array_index += value.num;
            }
          }
          {
            array_index %= kNumArrayEntries;
            DerefScope scope;
            const auto &array_entry =
                array->at</* NT = */ false>(scope, array_index);
            preempt_disable();
            consume_array_entry(array_entry);
            preempt_enable();
          }
          ACCESS_ONCE(req_cnts[tid].c)++;
        }
      }));
    }
    for (auto &thread : threads) {
      thread.Join();
    }
  }

public:
  void do_work(FarMemManager *manager) {
    auto hopscotch = std::unique_ptr<GenericConcurrentHopscotch>(
        manager->allocate_concurrent_hopscotch_heap(
            kLocalHashTableNumEntriesShift, kRemoteHashTableNumEntriesShift,
            kRemoteHashTableSlabSize));
    std::cout << "Prepare..." << std::endl;
    prepare(hopscotch.get());
    auto array_ptr = std::unique_ptr<AppArray>(
        manager->allocate_array_heap<ArrayEntry, kNumArrayEntries>());
    array_ptr->disable_prefetch();
    prepare(array_ptr.get());
    std::cout << "Bench..." << std::endl;
    bench(hopscotch.get(), array_ptr.get());
  }

  void run() {
    std::cout << "Starting run()" << std::endl;
    if (madvise(all_gen_reqs, sizeof(Req) * kNumReqs, MADV_HUGEPAGE) != 0) {
        std::cerr << "Madvise failed" << std::endl;
    }
    // Use DRAMDevice instead of TCPDevice.
    std::cout << "Building manager..." << std::endl;
    std::unique_ptr<FarMemManager> manager =
        std::unique_ptr<FarMemManager>(FarMemManagerFactory::build(
            kCacheSize, kNumGCThreads,
            new DRAMDevice(kFarMemSize)));
    std::cout << "Manager built. Doing work..." << std::endl;
    do_work(manager.get());
  }
};
} // namespace far_memory

int argc;
FarMemTest test;
void _main(void *arg) {
  test.run();
}

int main(int _argc, char *argv[]) {
  int ret;

  if (_argc < 2) {
    std::cerr << "usage: [cfg_file]" << std::endl;
    return -EINVAL;
  }

  char conf_path[strlen(argv[1]) + 1];
  strcpy(conf_path, argv[1]);
  argc = _argc - 1;

  ret = runtime_init(conf_path, _main, argv);
  if (ret) {
    std::cerr << "failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
