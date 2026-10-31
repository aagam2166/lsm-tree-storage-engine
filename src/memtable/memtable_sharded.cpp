#include <queue>
#include <vector>

#include "memtable/arena.h"
#include "memtable/memtable_iface.h"
#include "memtable/skiplist_locked.h"

namespace lsm {

namespace {

inline std::size_t HashUserKey(std::string_view key) noexcept {
  uint64_t h = 14695981039346656037ULL;
  for (unsigned char c : key) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return static_cast<std::size_t>(h);
}

// k-way merge iterator across all N shards (Section 3.2c).
class ShardedMergingIterator final : public InternalIterator {
 public:
  explicit ShardedMergingIterator(
      std::vector<std::unique_ptr<InternalIterator>> children)
      : children_(std::move(children)), current_idx_(-1) {}

  bool Valid() const override { return current_idx_ >= 0; }

  void SeekToFirst() override {
    for (auto& child : children_) {
      child->SeekToFirst();
    }
    FindSmallest();
  }

  void Seek(const InternalKey& target) override {
    for (auto& child : children_) {
      child->Seek(target);
    }
    FindSmallest();
  }

  void Next() override {
    assert(Valid());
    children_[static_cast<std::size_t>(current_idx_)]->Next();
    FindSmallest();
  }

  InternalKey key() const override {
    assert(Valid());
    return children_[static_cast<std::size_t>(current_idx_)]->key();
  }

  std::string_view value() const override {
    assert(Valid());
    return children_[static_cast<std::size_t>(current_idx_)]->value();
  }

 private:
  void FindSmallest() {
    int smallest = -1;
    for (std::size_t i = 0; i < children_.size(); ++i) {
      if (children_[i]->Valid()) {
        if (smallest < 0 ||
            children_[i]->key().Compare(
                children_[static_cast<std::size_t>(smallest)]->key()) < 0) {
          smallest = static_cast<int>(i);
        }
      }
    }
    current_idx_ = smallest;
  }

  std::vector<std::unique_ptr<InternalIterator>> children_;
  int current_idx_;
};

}  // namespace

// Section 3.2c: Sharded MemTable with N independent mutex-protected skip lists.
class ShardedMemTable final : public MemTableBase {
 public:
  explicit ShardedMemTable(std::size_t num_shards)
      : num_shards_(num_shards == 0 ? 16 : num_shards) {
    shards_.reserve(num_shards_);
    for (std::size_t i = 0; i < num_shards_; ++i) {
      shards_.push_back(std::make_unique<Shard>());
    }
  }

  ~ShardedMemTable() override = default;

  void Insert(uint64_t seq, ValueType type, std::string_view user_key,
              std::string_view value) override {
    std::size_t idx = HashUserKey(user_key) % num_shards_;
    InternalKey ikey(user_key, seq, type);
    shards_[idx]->table.Insert(ikey, value);
  }

  bool Get(std::string_view user_key, std::string* value, ValueType* type,
           uint64_t* seq) const override {
    std::size_t idx = HashUserKey(user_key) % num_shards_;
    return shards_[idx]->table.LookupUserKey(user_key, value, type, seq);
  }

  std::size_t ApproximateSize() const noexcept override {
    std::size_t total = 0;
    for (const auto& s : shards_) {
      total += s->arena.MemoryUsage();
    }
    return total;
  }

  std::unique_ptr<InternalIterator> NewIterator() const override {
    std::vector<std::unique_ptr<InternalIterator>> iters;
    iters.reserve(num_shards_);
    for (const auto& s : shards_) {
      iters.push_back(std::make_unique<LockedSkipList::Iterator>(&s->table));
    }
    return std::make_unique<ShardedMergingIterator>(std::move(iters));
  }

 private:
  struct Shard {
    Arena arena{16 * 1024};
    LockedSkipList table{&arena};
  };

  std::size_t num_shards_;
  std::vector<std::unique_ptr<Shard>> shards_;
};

MemTable* CreateLockFreeMemTable();
MemTable* CreateGlobalLockMemTable();

MemTable* MemTable::Create(MemTableType type, std::size_t num_shards) {
  switch (type) {
    case MemTableType::LOCK_FREE:
      return CreateLockFreeMemTable();
    case MemTableType::GLOBAL_LOCK:
      return CreateGlobalLockMemTable();
    case MemTableType::SHARDED:
      return new ShardedMemTable(num_shards);
  }
  return CreateLockFreeMemTable();
}

}  // namespace lsm
