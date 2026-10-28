#include "memtable/arena.h"
#include "memtable/memtable_iface.h"
#include "memtable/skiplist_lockfree.h"

namespace lsm {

// Section 3.2a: Lock-free CAS MemTable. No synchronization primitives other
// than C++17 atomics exist anywhere on this path.
class LockFreeMemTable final : public MemTableBase {
 public:
  LockFreeMemTable() : arena_(), table_(&arena_) {}
  ~LockFreeMemTable() override = default;

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
    return std::make_unique<LockFreeSkipList::Iterator>(&table_);
  }

 private:
  Arena arena_;
  LockFreeSkipList table_;
};

MemTable* CreateLockFreeMemTable() { return new LockFreeMemTable(); }

}  // namespace lsm
