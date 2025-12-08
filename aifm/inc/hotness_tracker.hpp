#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sync.h"

namespace far_memory {

class HotnessTracker {
public:
  struct Entry {
    uint64_t object_id;
    uint64_t accesses;
  };

  static void record(uint64_t object_id);
  static std::vector<Entry> snapshot();
  static void reset();
  static void dump_top(const char *label, size_t max_entries = 16);

private:
  struct ThreadLocalCounters {
    ThreadLocalCounters();
    ~ThreadLocalCounters();
    void inc(uint64_t object_id);
    void clear();
    rt::Spin lock;
    std::unordered_map<uint64_t, uint64_t> counts;
  };

  static ThreadLocalCounters &tls();
  static void register_tls(ThreadLocalCounters *tls_ptr);
  static void unregister_tls(ThreadLocalCounters *tls_ptr);

  static rt::Spin registry_lock_;
  static std::vector<ThreadLocalCounters *> registry_;
};

} // namespace far_memory
