#ifndef LSM_SRC_MEMTABLE_SKIPLIST_LOCKED_H_
#define LSM_SRC_MEMTABLE_SKIPLIST_LOCKED_H_

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

#include "lsm/iterator.h"
#include "lsm/types.h"
#include "memtable/arena.h"

namespace lsm {

// Mutex-protected SkipList used by GLOBAL_LOCK and SHARDED MemTables (Sec 3.2b/c).
class LockedSkipList {
 public:
  static constexpr int kMaxHeight = 12;

  struct Node {
    InternalKey key;
    uint32_t value_size;
    int height;
    Node* next[1];

    const char* ValueData() const noexcept {
      return reinterpret_cast<const char*>(&next[height]);
    }

    char* MutableValueData() noexcept {
      return reinterpret_cast<char*>(&next[height]);
    }

    std::string_view ValueView() const noexcept {
      return std::string_view(ValueData(), value_size);
    }
  };

  explicit LockedSkipList(Arena* arena)
      : arena_(arena),
        head_(NewNode(InternalKey(), "", kMaxHeight)),
        max_height_(1),
        rng_state_(0x123456789ABCDEF0ULL) {
    for (int i = 0; i < kMaxHeight; ++i) {
      head_->next[i] = nullptr;
    }
  }

  void Insert(const InternalKey& key, std::string_view value) {
    std::lock_guard<std::mutex> lock(mu_);
    Node* prev[kMaxHeight];
    FindGreaterOrEqualLocked(key, prev);

    int height = RandomHeightLocked();
    if (height > max_height_) {
      for (int i = max_height_; i < height; ++i) {
        prev[i] = head_;
      }
      max_height_ = height;
    }

    Node* x = NewNode(key, value, height);
    for (int i = 0; i < height; ++i) {
      x->next[i] = prev[i]->next[i];
      prev[i]->next[i] = x;
    }
  }

  bool LookupUserKey(std::string_view user_key, std::string* value,
                     ValueType* type, uint64_t* seq) const {
    std::lock_guard<std::mutex> lock(mu_);
    InternalKey target(user_key, UINT64_MAX, ValueType::kPut);
    Node* x = FindGreaterOrEqualLocked(target, nullptr);
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
    explicit Iterator(const LockedSkipList* list)
        : list_(list), node_(nullptr) {}

    bool Valid() const override { return node_ != nullptr; }

    void SeekToFirst() override {
      std::lock_guard<std::mutex> lock(list_->mu_);
      node_ = list_->head_->next[0];
    }

    void Seek(const InternalKey& target) override {
      std::lock_guard<std::mutex> lock(list_->mu_);
      node_ = list_->FindGreaterOrEqualLocked(target, nullptr);
    }

    void Next() override {
      std::lock_guard<std::mutex> lock(list_->mu_);
      assert(node_ != nullptr);
      node_ = node_->next[0];
    }

    InternalKey key() const override {
      assert(node_ != nullptr);
      return node_->key;
    }

    std::string_view value() const override {
      assert(node_ != nullptr);
      return node_->ValueView();
    }

   private:
    const LockedSkipList* list_;
    Node* node_;
  };

 private:
  int RandomHeightLocked() noexcept {
    rng_state_ ^= rng_state_ >> 12;
    rng_state_ ^= rng_state_ << 25;
    rng_state_ ^= rng_state_ >> 27;
    uint64_t z = rng_state_ * 2685821657736338717ULL;
    int height = 1;
    while (height < kMaxHeight && ((z & 3ULL) == 0ULL)) {
      ++height;
      z >>= 2;
    }
    return height;
  }

  Node* NewNode(const InternalKey& key, std::string_view value, int height) {
    const std::size_t node_bytes =
        offsetof(Node, next) + sizeof(Node*) * height + value.size();
    void* mem = arena_->AllocateAligned(node_bytes, alignof(Node));
    Node* node = reinterpret_cast<Node*>(mem);
    node->key = key;
    node->value_size = static_cast<uint32_t>(value.size());
    node->height = height;
    for (int i = 0; i < height; ++i) {
      node->next[i] = nullptr;
    }
    if (!value.empty()) {
      std::memcpy(node->MutableValueData(), value.data(), value.size());
    }
    return node;
  }

  Node* FindGreaterOrEqualLocked(const InternalKey& key, Node** prev) const {
    Node* x = head_;
    int level = max_height_ - 1;
    while (true) {
      Node* next = x->next[level];
      if (next != nullptr && next->key.Compare(key) < 0) {
        x = next;
      } else {
        if (prev != nullptr) prev[level] = x;
        if (level == 0) return next;
        --level;
      }
    }
  }

  mutable std::mutex mu_;
  Arena* arena_;
  Node* head_;
  int max_height_;
  uint64_t rng_state_;
};

}  // namespace lsm

#endif  // LSM_SRC_MEMTABLE_SKIPLIST_LOCKED_H_
