#include "hotness_tracker.hpp"

#include <algorithm>
#include <cstdio>

namespace far_memory {

rt::Spin HotnessTracker::registry_lock_;
std::vector<HotnessTracker::ThreadLocalCounters *> HotnessTracker::registry_;
uint64_t HotnessTracker::far_mem_base_addr_ = 0;
bool HotnessTracker::far_mem_base_set_ = false;

HotnessTracker::ThreadLocalCounters::ThreadLocalCounters() { register_tls(this); }

HotnessTracker::ThreadLocalCounters::~ThreadLocalCounters() {
  unregister_tls(this);
}

void HotnessTracker::ThreadLocalCounters::inc(uint64_t object_id) {
  lock.Lock();
  counts[object_id]++;
  lock.Unlock();
}

void HotnessTracker::ThreadLocalCounters::clear() { 
  lock.Lock();
  counts.clear(); 
  lock.Unlock();
}

HotnessTracker::ThreadLocalCounters &HotnessTracker::tls() {
  thread_local ThreadLocalCounters tls_instance;
  return tls_instance;
}

void HotnessTracker::register_tls(ThreadLocalCounters *tls_ptr) {
  registry_lock_.Lock();
  registry_.push_back(tls_ptr);
  registry_lock_.Unlock();
}

void HotnessTracker::unregister_tls(ThreadLocalCounters *tls_ptr) {
  registry_lock_.Lock();
  registry_.erase(std::remove(registry_.begin(), registry_.end(), tls_ptr),
                  registry_.end());
  registry_lock_.Unlock();
}

void HotnessTracker::record(uint64_t object_id) { tls().inc(object_id); }

std::vector<HotnessTracker::Entry> HotnessTracker::snapshot() {
  std::vector<ThreadLocalCounters*> regs;
  registry_lock_.Lock();
  regs = registry_;
  registry_lock_.Unlock();

  std::unordered_map<uint64_t, uint64_t> aggregated;
  for (auto *tls_ptr : regs) {
    tls_ptr->lock.Lock();
    for (const auto &kv : tls_ptr->counts) {
      aggregated[kv.first] += kv.second;
    }
    tls_ptr->lock.Unlock();
  }

  std::vector<Entry> entries;
  entries.reserve(aggregated.size());
  for (const auto &kv : aggregated) {
    entries.push_back(Entry{kv.first, kv.second});
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry &lhs, const Entry &rhs) {
              return lhs.accesses > rhs.accesses;
            });
  return entries;
}

void HotnessTracker::reset() {
  std::vector<ThreadLocalCounters*> regs;
  registry_lock_.Lock();
  regs = registry_;
  registry_lock_.Unlock();
  for (auto *tls_ptr : regs) {
    tls_ptr->clear();
  }
}

void HotnessTracker::dump_top(const char *label, size_t max_entries) {
  auto entries = snapshot();
  if (entries.empty()) {
    printf("[HotnessTracker] %s: no accesses recorded.\n", label);
    return;
  }
  if (far_mem_base_set_) {
    printf("[HotnessTracker] %s: far_mem_base=0x%lx\n", label,
           far_mem_base_addr_);
  }
  printf("[HotnessTracker] %s: %zu total objects\n", label, entries.size());
  for (size_t i = 0; i < entries.size(); i++) {
    if (far_mem_base_set_) {
      printf("  #%zu offset=0x%lx actual=0x%lx accesses=%lu\n", i + 1,
             entries[i].object_id,
             far_mem_base_addr_ + entries[i].object_id,
             entries[i].accesses);
    } else {
      printf("  #%zu object_id=0x%lx accesses=%lu\n", i + 1,
             entries[i].object_id, entries[i].accesses);
    }
  }
}

void HotnessTracker::set_far_mem_base(uint8_t *base_addr) {
  if (base_addr) {
    far_mem_base_addr_ = reinterpret_cast<uint64_t>(base_addr);
    far_mem_base_set_ = true;
  } else {
    far_mem_base_set_ = false;
    far_mem_base_addr_ = 0;
  }
}

} // namespace far_memory
