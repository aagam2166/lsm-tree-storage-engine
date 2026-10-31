#ifndef LSM_INCLUDE_LSM_MEMTABLE_H_
#define LSM_INCLUDE_LSM_MEMTABLE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "lsm/iterator.h"
#include "lsm/options.h"
#include "lsm/status.h"
#include "lsm/types.h"

namespace lsm {

class MemTable {
 public:
  virtual ~MemTable() = default;

  virtual void Insert(uint64_t seq, ValueType type, std::string_view user_key,
                      std::string_view value) = 0;

  // Returns true if user_key was found (either kPut or kDelete tombstone).
  virtual bool Get(std::string_view user_key, std::string* value,
                   ValueType* type, uint64_t* seq) const = 0;

  virtual std::size_t ApproximateSize() const noexcept = 0;
  virtual std::unique_ptr<InternalIterator> NewIterator() const = 0;

  virtual void Ref() noexcept = 0;
  virtual void Unref() noexcept = 0;

  // In-flight writer tracking for lock-free MemTable-to-flush hand-off (Sec 3.3)
  virtual void BeginWrite() noexcept = 0;
  virtual void EndWrite() noexcept = 0;
  virtual uint64_t InFlightWriters() const noexcept = 0;
  virtual bool TryClaimSwap() noexcept = 0;

  virtual uint64_t GetWalSegmentId() const noexcept = 0;
  virtual void SetWalSegmentId(uint64_t id) noexcept = 0;

  static MemTable* Create(MemTableType type, std::size_t num_shards = 16);
};

}  // namespace lsm

#endif  // LSM_INCLUDE_LSM_MEMTABLE_H_
