#ifndef LSM_SRC_MEMTABLE_SKIPLIST_LOCKFREE_H_
#define LSM_SRC_MEMTABLE_SKIPLIST_LOCKFREE_H_

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>

#include "lsm/iterator.h"
#include "lsm/types.h"
#include "memtable/arena.h"

namespace lsm {

// Lock-free CAS-based concurrent SkipList (Section 3.2a).
// Zero mutexes anywhere in this file. Insert-only (deletes are tombstones).
class LockFreeSkipList {
 public:
  static constexpr int kMaxHeight = 12;
  static constexpr unsigned int kBranching = 4;

  struct Node {
    InternalKey key;
    uint32_t value_size;
    int height;
    std::atomic<Node*>* next;

    const char* ValueData() const noexcept {
      return reinterpret_cast<const char*>(next + height);
    }

    char* MutableValueData() noexcept {
      return reinterpret_cast<char*>(next + height);
    }

    std::string_view ValueView() const noexcept {
      return std::string_view(ValueData(), value_size);
    }

    Node* Next(int level) const noexcept {
      if (level < 0 || level >= height) return nullptr;
      return next[level].load(std::memory_order_acquire);
    }

    Node* NextRelaxed(int level) const noexcept {
      if (level < 0 || level >= height) return nullptr;
      return next[level].load(std::memory_order_relaxed);
    }

    void SetNextRelaxed(int level, Node* x) noexcept {
      if (level < 0 || level >= height) return;
      next[level].store(x, std::memory_order_relaxed);
    }

    bool CASNext(int level, Node* expected, Node* desired) noexcept {
      if (level < 0 || level >= height) return false;
      return next[level].compare_exchange_strong(
          expected, desired, std::memory_order_release,
          std::memory_order_acquire);
    }
  };

  explicit LockFreeSkipList(Arena* arena)
      : arena_(arena),
        head_(NewNode(InternalKey(), "", kMaxHeight)),
        max_height_(1),
        rng_state_(0xDEADBEEFCAFEBABEULL) {
    for (int i = 0; i < kMaxHeight; ++i) {
      head_->SetNextRelaxed(i, nullptr);
    }
  }

  LockFreeSkipList(const LockFreeSkipList&) = delete;
  LockFreeSkipList& operator=(const LockFreeSkipList&) = delete;

  void Insert(const InternalKey& key, std::string_view value) {
    Node* prev[kMaxHeight];
    Node* succ[kMaxHeight];
    FindSplicePositions(key, prev, succ);

    int height = RandomHeight();
    // Relaxed load of max_height_ to check if our node raises the skiplist height.
    int cur_max = max_height_.load(std::memory_order_relaxed);
    while (height > cur_max) {
      for (int i = cur_max; i < height; ++i) {
        prev[i] = head_;
        succ[i] = nullptr;
      }
      // Relaxed CAS on max_height_: readers synchronize on node next pointers
      // with acquire/release, while max_height_ is just the search start level.
      if (max_height_.compare_exchange_weak(cur_max, height,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
        break;
      }
    }

    Node* x = NewNode(key, value, height);
    for (int i = 0; i < height; ++i) {
      x->SetNextRelaxed(i, succ[i]);
    }

    // Step 1: Link at level 0 using CAS. Retry on contention without blocking.
    while (true) {
      x->SetNextRelaxed(0, succ[0]);
      if (prev[0]->CASNext(0, succ[0], x)) {
        break;
      }
      // Contention at level 0: recompute splice positions and retry immediately.
      FindSplicePositions(key, prev, succ);
      for (int i = 0; i < height; ++i) {
        x->SetNextRelaxed(i, succ[i]);
      }
    }

    // Step 2: Link upper levels 1 .. height - 1 bottom-up via CAS.
    for (int i = 1; i < height; ++i) {
      while (true) {
        x->SetNextRelaxed(i, succ[i]);
        if (prev[i]->CASNext(i, succ[i], x)) {
          break;
        }
        // Another writer modified level `i`; refresh `prev` and `succ`.
        FindSplicePositions(key, prev, succ);
      }
    }
  }

  bool LookupUserKey(std::string_view user_key, std::string* value,
                     ValueType* type, uint64_t* seq) const {
    // Seek to (user_key, UINT64_MAX, kPut) because higher sequence numbers sort
    // first for a given user_key.
    InternalKey target(user_key, UINT64_MAX, ValueType::kPut);
    Node* x = FindGreaterOrEqual(target, nullptr, nullptr);
    if (x != nullptr && x->key.UserKeyView() == user_key) {
      *type = x->key.type;
      *seq = x->key.seq;
      if (x->key.type == ValueType::kPut && value != nullptr) {
        value->assign(x->ValueData(), x->value_size);
      }
      return true;
    }
    return false;
  }

  class Iterator : public InternalIterator {
   public:
    explicit Iterator(const LockFreeSkipList* list)
        : list_(list), node_(nullptr) {}

    bool Valid() const override { return node_ != nullptr; }

    void SeekToFirst() override { node_ = list_->head_->Next(0); }

    void Seek(const InternalKey& target) override {
      node_ = list_->FindGreaterOrEqual(target, nullptr, nullptr);
    }

    void Next() override {
      assert(Valid());
      node_ = node_->Next(0);
    }

    InternalKey key() const override {
      assert(Valid());
      return node_->key;
    }

    std::string_view value() const override {
      assert(Valid());
      return node_->ValueView();
    }

   private:
    const LockFreeSkipList* list_;
    Node* node_;
  };

 private:
  int GetMaxHeight() const noexcept {
    // Relaxed load: safe because head_->Next(level) uses acquire semantics.
    return max_height_.load(std::memory_order_relaxed);
  }

  int RandomHeight() noexcept {
    // Lock-free xorshift64 via atomic fetch_add seed mixing
    // Relaxed fetch_add: RNG state needs only uniqueness/entropy, not ordering.
    uint64_t z =
        rng_state_.fetch_add(0x9E3779B97F4A7C15ULL, std::memory_order_relaxed);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);

    int height = 1;
    while (height < kMaxHeight && ((z & 3ULL) == 0ULL)) {
      ++height;
      z >>= 2;
    }
    return height;
  }

  Node* NewNode(const InternalKey& key, std::string_view value, int height) {
    const std::size_t ptr_count = static_cast<std::size_t>(height);
    const std::size_t value_bytes = value.size();
    const std::size_t node_bytes = sizeof(Node) + ptr_count * sizeof(std::atomic<Node*>) + value_bytes;
    void* mem = arena_->AllocateAligned(node_bytes, alignof(Node));
    Node* node = reinterpret_cast<Node*>(mem);
    node->key = key;
    node->value_size = static_cast<uint32_t>(value_bytes);
    node->height = height;
    node->next = reinterpret_cast<std::atomic<Node*>*>(reinterpret_cast<char*>(node) + sizeof(Node));
    for (int i = 0; i < height; ++i) {
      new (&node->next[i]) std::atomic<Node*>(nullptr);
    }
    if (!value.empty()) {
      std::memcpy(node->MutableValueData(), value.data(), value.size());
    }
    return node;
  }

  Node* FindGreaterOrEqual(const InternalKey& key, Node** prev,
                           Node** succ) const {
    Node* x = head_;
    int level = kMaxHeight - 1;
    while (true) {
      Node* next = x->Next(level);
      if (next != nullptr && next->key.Compare(key) < 0) {
        x = next;
      } else {
        if (prev != nullptr && level >= 0 && level < kMaxHeight) {
          prev[level] = x;
        }
        if (succ != nullptr && level >= 0 && level < kMaxHeight) {
          succ[level] = next;
        }
        if (level == 0) {
          return next;
        }
        --level;
      }
    }
  }

  void FindSplicePositions(const InternalKey& key, Node** prev,
                           Node** succ) const {
    for (int i = 0; i < kMaxHeight; ++i) {
      prev[i] = head_;
      succ[i] = nullptr;
    }
    FindGreaterOrEqual(key, prev, succ);
  }

  Arena* arena_;
  Node* head_;
  std::atomic<int> max_height_;
  std::atomic<uint64_t> rng_state_;
};

}  // namespace lsm

#endif  // LSM_SRC_MEMTABLE_SKIPLIST_LOCKFREE_H_
