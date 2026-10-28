#include "memtable/arena.h"
#include "memtable/memtable_iface.h"
#include "memtable/skiplist_locked.h"

namespace lsm {

// Section 3.2b: Global lock MemTable protected by a single std::mutex.
class GlobalLockMemTable final : public MemTableBase {
 public:
  GlobalLockMemTable() : arena_(), table_(&arena_) {}
  ~GlobalLockMemTable() override = default;

  void Insert(uint64_t seq, ValueType type, std::string_view user_key,
              std::string_view value) override {
    InternalKey ikey(user_key, seq, type);
    table_.Insert(ikey, value);
  }

  bool Get(std::string_view user_key, std::string* value, ValueType* type,
           uint64_t* seq) const override {
    return table_.LookupUserKey(user_key, value, type, seq);
  }

  std::size_t ApproximateSize() const noexcept override {
    return arena_.MemoryUsage();
  }

  std::unique_ptr<InternalIterator> NewIterator() const override {
    return std::make_unique<LockedSkipList::Iterator>(&table_);
  }

 private:
  Arena arena_;
  LockedSkipList table_;
};

MemTable* CreateGlobalLockMemTable() { return new GlobalLockMemTable(); }

}  // namespace lsm
